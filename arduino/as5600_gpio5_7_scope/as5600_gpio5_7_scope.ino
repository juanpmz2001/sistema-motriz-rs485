#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_continuous.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/adc_types.h>
#include <soc/adc_channel.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#if __has_include("../ensayo_nueva_pata/wifi_secrets.h")
#include "../ensayo_nueva_pata/wifi_secrets.h"
#else
#define NUEVA_PATA_TALLER_SSID ""
#define NUEVA_PATA_TALLER_PASSWORD ""
#endif

// Osciloscopio pasivo: no inicializa I2C, servo, PWM ni RS485.
constexpr uint8_t ANALOG_PIN = 5;
constexpr uint8_t DIGITAL_PIN = 7;
constexpr gpio_num_t DIGITAL_GPIO = GPIO_NUM_7;
constexpr adc_channel_t ANALOG_CHANNEL = ADC_CHANNEL_4;
constexpr uint16_t SAMPLE_COUNT = 2048;
constexpr uint16_t ADC_MAX_RAW = 4095;
constexpr uint32_t ADC_SAMPLE_RATE_HZ = 80000;
constexpr uint16_t TIMEBASE_SAMPLES[] = {128, 256, 512, 1024, 2048};
constexpr uint8_t TIMEBASE_STEPS =
    sizeof(TIMEBASE_SAMPLES) / sizeof(TIMEBASE_SAMPLES[0]);
constexpr uint8_t DEFAULT_TIMEBASE_INDEX = TIMEBASE_STEPS - 1;
constexpr size_t DMA_FRAME_BYTES = 1024;
constexpr size_t DMA_POOL_BYTES = 8192;
constexpr uint16_t MAX_DIGITAL_EDGES = 4096;
constexpr size_t PACKET_HEADER_BYTES = 32;
constexpr uint32_t PACKET_MAGIC = 0x31504353;  // "SCP1" little-endian.

struct ScopeFrame {
  bool started;
  uint32_t sequence;
  uint32_t capturedAtMs;
  uint32_t durationUs;
  uint32_t captureWallUs;
  uint32_t sampleRateHz;
  uint16_t minRaw;
  uint16_t maxRaw;
  uint16_t averageRaw;
  uint16_t calibratedMv;
  uint16_t digitalEdges;
  uint8_t firstDigital;
  bool digitalOverflow;
  uint16_t analog[SAMPLE_COUNT];
  uint8_t digital[SAMPLE_COUNT];
};

enum ScopeMode : uint8_t {
  SCOPE_CLOSED = 0,
  SCOPE_OPEN = 1,
  SCOPE_SINGLE = 2,
};

struct ScopeControlSnapshot {
  ScopeMode mode;
  uint32_t generation;
  bool capturing;
  uint8_t timebaseIndex;
};

WebServer server(80);
SemaphoreHandle_t scopeMutex = nullptr;
ScopeFrame scopeFrame = {};
ScopeFrame httpFrame = {};
volatile bool otaInProgress = false;
volatile int32_t lastAdcError = ESP_OK;
portMUX_TYPE scopeControlMux = portMUX_INITIALIZER_UNLOCKED;
ScopeMode requestedScopeMode = SCOPE_SINGLE;
uint32_t scopeControlGeneration = 1;
bool scopeCaptureInProgress = false;
uint8_t scopeTimebaseIndex = DEFAULT_TIMEBASE_INDEX;

adc_continuous_handle_t adcHandle = nullptr;
adc_cali_handle_t adcCalibration = nullptr;
alignas(4) uint8_t dmaReadBuffer[DMA_FRAME_BYTES] = {};

volatile bool digitalCaptureActive = false;
volatile bool digitalEdgeOverflow = false;
volatile uint16_t digitalEdgeCount = 0;
volatile uint32_t digitalCaptureStartUs = 0;
volatile uint32_t digitalEdgeTimeUs[MAX_DIGITAL_EDGES] = {};
volatile uint8_t digitalEdgeLevel[MAX_DIGITAL_EDGES] = {};

const char SCOPE_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Osciloscopio GPIO5 / GPIO7</title><style>
:root{color-scheme:dark;--bg:#06101d;--card:#0d2033;--grid:#26435a;--cyan:#37d8ff;--yellow:#ffd166;--ok:#66f2a3;--bad:#ff637d}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 50% -15%,#164467,var(--bg) 58%);color:#eefaff;font-family:Segoe UI,Arial,sans-serif;min-height:100vh}
header{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:16px 20px;border-bottom:1px solid #ffffff18}h1{font-size:clamp(20px,4vw,32px);margin:0}.sub{color:#91b3c9;font-size:13px;margin-top:4px}.tag{font:700 13px Consolas,monospace;color:var(--yellow)}
main{max-width:1120px;margin:auto;padding:14px}.card{background:#0d2033ed;border:1px solid #ffffff18;border-radius:16px;padding:12px;box-shadow:0 20px 50px #0007}.scope-wrap{width:100%;overflow:hidden;border-radius:10px;background:#020912}canvas{display:block;width:100%;height:430px}
.stats{display:grid;grid-template-columns:repeat(6,minmax(120px,1fr));gap:9px;margin-top:12px}.stat{background:#071522;border:1px solid #ffffff12;border-radius:10px;padding:10px}.label{text-transform:uppercase;letter-spacing:.1em;color:#86a8be;font-size:10px}.value{font:700 20px/1.3 Consolas,monospace;margin-top:4px}.cyan{color:var(--cyan)}.yellow{color:var(--yellow)}.ok{color:var(--ok)}.bad{color:var(--bad)}
.controls{display:flex;flex-wrap:wrap;align-items:center;gap:12px;margin:0 0 12px}.elevator{display:grid;grid-template-columns:1fr;gap:6px;padding:8px;border:1px solid #ffffff20;border-radius:14px;background:#071522}.acquisition{display:flex;flex-wrap:wrap;align-items:center;gap:7px;padding:8px;border:1px solid #ffffff20;border-radius:14px;background:#071522}.controls button{border:1px solid #ffffff28;border-radius:9px;padding:10px 18px;background:#16334c;color:#eefaff;font:700 14px Segoe UI,Arial;cursor:pointer}.elevator button{min-width:142px}.controls button:hover{filter:brightness(1.18)}.controls button.active{border-color:var(--cyan);box-shadow:0 0 0 2px #37d8ff30;background:#164c67}.controls button:disabled{opacity:.45;cursor:not-allowed}.timebase{text-align:center;font:700 18px Consolas,monospace;color:var(--cyan);padding:2px}.group-label{width:100%;text-align:center;text-transform:uppercase;letter-spacing:.12em;color:#86a8be;font-size:10px}.mode{margin-left:auto;font:700 14px Consolas,monospace;color:var(--yellow)}
@media(max-width:780px){main{padding:7px}.stats{grid-template-columns:repeat(2,1fr)}canvas{height:390px}.card{padding:7px}}
</style></head><body><header><div><h1>Osciloscopio · Nueva Pata</h1><div class="sub">GPIO5 analógico · GPIO7 digital · captura pasiva</div></div><div id="link" class="tag">CONECTANDO</div></header>
<main><section class="card"><div class="controls"><div class="elevator"><div class="group-label">Base de tiempo</div><button id="time_open" onclick="setTimebase('open')">▲ ABRIR</button><div id="timebase" class="timebase">25.6 ms</div><button id="time_close" onclick="setTimebase('close')">▼ CERRAR</button></div><div class="acquisition"><div class="group-label">Adquisicion</div><button id="continuous" onclick="setMode('open')">CONTINUO</button><button id="freeze" onclick="setMode('closed')">CONGELAR</button><button id="single" onclick="setMode('single')">CAPTURA UNICA</button></div><div id="mode" class="mode">ESTADO: ESPERANDO</div></div><div class="scope-wrap"><canvas id="scope"></canvas></div><div class="stats">
<div class="stat"><div class="label">GPIO5 promedio</div><div id="voltage" class="value cyan">--- V</div></div>
<div class="stat"><div class="label">GPIO5 min / max</div><div id="range" class="value cyan">--- / ---</div></div>
<div class="stat"><div class="label">GPIO7 actual</div><div id="digital" class="value yellow">-</div></div>
<div class="stat"><div class="label">Flancos GPIO7</div><div id="edges" class="value yellow">0</div></div>
<div class="stat"><div class="label">Muestreo real</div><div id="rate" class="value">---</div></div>
<div class="stat"><div class="label">Ventana</div><div id="window" class="value">---</div></div>
</div></section></main>
<script>
const q=id=>document.getElementById(id),canvas=q('scope'),ctx=canvas.getContext('2d');let fails=0,lastSeq=0,commandBusy=false,timebaseBusy=false;
function showMode(id,busy=false){const names=['CONGELADA','CONTINUA','CAPTURA UNICA'],buttons=['freeze','continuous','single'];q('mode').textContent='ADQUISICION: '+(names[id]||'DESCONOCIDA')+(busy?' · CAPTURANDO':'');buttons.forEach((name,i)=>{q(name).classList.toggle('active',i===id);q(name).disabled=commandBusy})}
function showTimebase(index,samples,durationUs){q('timebase').textContent=(durationUs/1000).toFixed(1)+' ms';q('window').textContent=(durationUs/1000).toFixed(1)+' ms · '+samples;q('time_open').disabled=timebaseBusy||index>=4;q('time_close').disabled=timebaseBusy||index<=0}
async function refreshState(){try{const r=await fetch('/scope/state?x='+Date.now(),{cache:'no-store'}),d=await r.json();showMode(d.mode_id,!!d.capturing);showTimebase(d.timebase_index,d.window_samples,d.window_us)}catch(e){}}
async function setMode(mode){if(commandBusy)return;commandBusy=true;showMode(-1);try{const r=await fetch('/scope/control?mode='+mode,{method:'POST',cache:'no-store'});if(!r.ok)throw Error('CONTROL');const d=await r.json();showMode(d.mode_id,!!d.capturing)}catch(e){q('mode').textContent='ERROR DE CONTROL'}finally{commandBusy=false;await refreshState()}}
async function setTimebase(action){if(timebaseBusy)return;timebaseBusy=true;try{const r=await fetch('/scope/timebase?action='+action,{method:'POST',cache:'no-store'});if(!r.ok)throw Error('TIMEBASE');const d=await r.json();showTimebase(d.timebase_index,d.window_samples,d.window_us)}catch(e){q('timebase').textContent='ERROR'}finally{timebaseBusy=false;await refreshState()}}
function resize(){const dpr=Math.max(1,Math.min(2,devicePixelRatio||1)),w=canvas.clientWidth,h=canvas.clientHeight;canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);return{w,h}}
function line(x1,y1,x2,y2,color,width=1){ctx.strokeStyle=color;ctx.lineWidth=width;ctx.beginPath();ctx.moveTo(x1,y1);ctx.lineTo(x2,y2);ctx.stroke()}
function draw(a,d,rate,duration,minRaw,maxRaw,avgRaw,calMv,edgeCount){const {w,h}=resize(),left=52,right=12,top=20,aBottom=Math.round(h*.63),dTop=Math.round(h*.72),dBottom=h-30,plotW=w-left-right;
ctx.clearRect(0,0,w,h);ctx.fillStyle='#020912';ctx.fillRect(0,0,w,h);ctx.font='11px Consolas';ctx.textAlign='right';ctx.textBaseline='middle';
const scale=(avgRaw&&calMv)?calMv/avgRaw:3300/4095;
for(const mv of [0,1000,2000,3000,3300]){const y=aBottom-(mv/3300)*(aBottom-top);line(left,y,w-right,y,'#1d384d');ctx.fillStyle='#789bb2';ctx.fillText((mv/1000).toFixed(mv===3300?1:0)+'V',left-6,y)}
ctx.fillStyle='#37d8ff';ctx.textAlign='left';ctx.fillText('GPIO5 ADC',left,10);ctx.strokeStyle='#37d8ff';ctx.lineWidth=1.5;ctx.beginPath();for(let i=0;i<a.length;i++){const x=left+(i/(a.length-1))*plotW,y=aBottom-Math.min(3300,a[i]*scale)/3300*(aBottom-top);i?ctx.lineTo(x,y):ctx.moveTo(x,y)}ctx.stroke();
line(left,dTop,w-right,dTop,'#1d384d');line(left,dBottom,w-right,dBottom,'#1d384d');ctx.fillStyle='#789bb2';ctx.textAlign='right';ctx.fillText('1',left-8,dTop);ctx.fillText('0',left-8,dBottom);ctx.fillStyle='#ffd166';ctx.textAlign='left';ctx.fillText('GPIO7 DIGITAL',left,dTop-12);
ctx.strokeStyle='#ffd166';ctx.lineWidth=1.7;ctx.beginPath();let py=d[0]?dTop:dBottom;ctx.moveTo(left,py);for(let i=1;i<d.length;i++){const x=left+(i/(d.length-1))*plotW,ny=d[i]?dTop:dBottom;ctx.lineTo(x,py);if(ny!==py)ctx.lineTo(x,ny);py=ny}ctx.stroke();
ctx.fillStyle='#789bb2';ctx.textAlign='left';ctx.fillText('0 ms',left,h-12);ctx.textAlign='right';ctx.fillText((duration/1000).toFixed(2)+' ms',w-right,h-12);
const minV=minRaw*scale/1000,maxV=maxRaw*scale/1000,avgV=avgRaw*scale/1000;q('voltage').textContent=avgV.toFixed(3)+' V';q('range').textContent=minV.toFixed(3)+' / '+maxV.toFixed(3);q('digital').textContent=d[d.length-1]?'1 · ALTO':'0 · BAJO';q('edges').textContent=edgeCount;q('rate').textContent=(rate/1000).toFixed(1)+' kS/s';q('window').textContent=(duration/1000).toFixed(2)+' ms'}
async function poll(){const ctl=new AbortController(),timer=setTimeout(()=>ctl.abort(),1200);try{const r=await fetch('/scope.bin?x='+Date.now(),{cache:'no-store',signal:ctl.signal});if(!r.ok)throw Error('HTTP');const b=await r.arrayBuffer(),v=new DataView(b);if(v.byteLength<32||v.getUint32(0,true)!==0x31504353)throw Error('FORMATO');const seq=v.getUint32(4,true),rate=v.getUint32(8,true),duration=v.getUint32(12,true),n=v.getUint16(16,true),min=v.getUint16(20,true),max=v.getUint16(22,true),avg=v.getUint16(24,true),mv=v.getUint16(26,true),edges=v.getUint16(28,true),flags=v.getUint8(31);if(![128,256,512,1024,2048].includes(n)||v.byteLength!==32+n*3)throw Error('LONGITUD');const a=new Uint16Array(n),d=new Uint8Array(n);for(let i=0;i<n;i++)a[i]=v.getUint16(32+i*2,true);const doff=32+n*2;for(let i=0;i<n;i++)d[i]=v.getUint8(doff+i);draw(a,d,rate,duration,min,max,avg,mv,edges);showMode((flags>>1)&3,!!(flags&8));showTimebase((flags>>4)&7,n,duration);lastSeq=seq;fails=0;q('link').textContent='EN LINEA · CAPTURA '+seq;q('link').className='tag ok'}catch(e){await refreshState();if(++fails>2){q('link').textContent='ESPERANDO CAPTURA';q('link').className='tag'}}finally{clearTimeout(timer);setTimeout(poll,160)}}
addEventListener('resize',()=>{canvas.width=canvas.width});refreshState();poll();
</script></body></html>
)HTML";

void putU16(uint8_t *buffer, size_t offset, uint16_t value) {
  buffer[offset] = uint8_t(value);
  buffer[offset + 1] = uint8_t(value >> 8);
}

void putU32(uint8_t *buffer, size_t offset, uint32_t value) {
  buffer[offset] = uint8_t(value);
  buffer[offset + 1] = uint8_t(value >> 8);
  buffer[offset + 2] = uint8_t(value >> 16);
  buffer[offset + 3] = uint8_t(value >> 24);
}

inline uint8_t ARDUINO_ISR_ATTR readDigitalPinFast() {
  return uint8_t((REG_READ(GPIO_IN_REG) >> DIGITAL_PIN) & 1U);
}

void ARDUINO_ISR_ATTR digitalEdgeIsr() {
  if (!digitalCaptureActive) return;
  const uint16_t index = digitalEdgeCount;
  if (index >= MAX_DIGITAL_EDGES) {
    digitalEdgeOverflow = true;
    return;
  }
  digitalEdgeTimeUs[index] = uint32_t(micros() - digitalCaptureStartUs);
  digitalEdgeLevel[index] = readDigitalPinFast();
  digitalEdgeCount = uint16_t(index + 1);
}

bool initContinuousAdc() {
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = DMA_POOL_BYTES;
  handleConfig.conv_frame_size = DMA_FRAME_BYTES;
  esp_err_t error = adc_continuous_new_handle(&handleConfig, &adcHandle);
  if (error != ESP_OK) {
    lastAdcError = error;
    return false;
  }

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = ANALOG_CHANNEL;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t config = {};
  config.sample_freq_hz = ADC_SAMPLE_RATE_HZ;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  error = adc_continuous_config(adcHandle, &config);
  if (error != ESP_OK) {
    lastAdcError = error;
    adc_continuous_deinit(adcHandle);
    adcHandle = nullptr;
    return false;
  }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t calibrationConfig = {};
  calibrationConfig.unit_id = ADC_UNIT_1;
  calibrationConfig.atten = ADC_ATTEN_DB_12;
  calibrationConfig.bitwidth = ADC_BITWIDTH_12;
  adc_cali_create_scheme_curve_fitting(&calibrationConfig,
                                       &adcCalibration);
#endif
  lastAdcError = ESP_OK;
  return true;
}

void beginDigitalCapture(uint8_t &firstLevel) {
  gpio_intr_disable(DIGITAL_GPIO);
  digitalCaptureActive = false;
  digitalEdgeCount = 0;
  digitalEdgeOverflow = false;
  firstLevel = readDigitalPinFast();
  digitalCaptureStartUs = micros();
  digitalCaptureActive = true;
  gpio_intr_enable(DIGITAL_GPIO);
}

uint16_t endDigitalCapture(bool &overflow) {
  gpio_intr_disable(DIGITAL_GPIO);
  digitalCaptureActive = false;
  const uint16_t count = digitalEdgeCount;
  overflow = digitalEdgeOverflow;
  return count;
}

void reconstructDigitalSamples(ScopeFrame &frame, uint16_t edgeCount) {
  uint16_t edgeIndex = 0;
  uint8_t level = frame.firstDigital;
  for (uint16_t sample = 0; sample < SAMPLE_COUNT; ++sample) {
    const uint32_t sampleTimeUs = uint32_t(
        (uint64_t(sample) * 1000000ULL) / ADC_SAMPLE_RATE_HZ);
    while (edgeIndex < edgeCount &&
           digitalEdgeTimeUs[edgeIndex] <= sampleTimeUs) {
      level = digitalEdgeLevel[edgeIndex];
      ++edgeIndex;
    }
    frame.digital[sample] = level;
  }
}

bool captureScopeFrame(ScopeFrame &frame) {
  if (!adcHandle) return false;
  adc_continuous_flush_pool(adcHandle);
  beginDigitalCapture(frame.firstDigital);
  const int64_t wallStartedUs = esp_timer_get_time();
  esp_err_t error = adc_continuous_start(adcHandle);
  if (error != ESP_OK) {
    endDigitalCapture(frame.digitalOverflow);
    lastAdcError = error;
    return false;
  }

  uint16_t sampleCount = 0;
  uint32_t rawSum = 0;
  frame.minRaw = ADC_MAX_RAW;
  frame.maxRaw = 0;
  while (sampleCount < SAMPLE_COUNT) {
    uint32_t bytesRead = 0;
    error = adc_continuous_read(adcHandle, dmaReadBuffer,
                                sizeof(dmaReadBuffer), &bytesRead, 100);
    if (error != ESP_OK) break;
    for (uint32_t offset = 0;
         offset + sizeof(adc_digi_output_data_t) <= bytesRead &&
         sampleCount < SAMPLE_COUNT;
         offset += sizeof(adc_digi_output_data_t)) {
      const auto *sample = reinterpret_cast<const adc_digi_output_data_t *>(
          dmaReadBuffer + offset);
      if (sample->type2.unit != 0 ||
          sample->type2.channel != ANALOG_CHANNEL)
        continue;
      const uint16_t raw = uint16_t(sample->type2.data);
      frame.analog[sampleCount++] = raw;
      rawSum += raw;
      if (raw < frame.minRaw) frame.minRaw = raw;
      if (raw > frame.maxRaw) frame.maxRaw = raw;
    }
  }
  const esp_err_t stopError = adc_continuous_stop(adcHandle);
  const uint16_t edgeCount = endDigitalCapture(frame.digitalOverflow);
  frame.captureWallUs = uint32_t(esp_timer_get_time() - wallStartedUs);

  if (error != ESP_OK || stopError != ESP_OK ||
      sampleCount != SAMPLE_COUNT) {
    lastAdcError = error != ESP_OK ? error : stopError;
    return false;
  }

  frame.durationUs = uint32_t(
      (uint64_t(SAMPLE_COUNT) * 1000000ULL) / ADC_SAMPLE_RATE_HZ);
  frame.sampleRateHz = ADC_SAMPLE_RATE_HZ;
  frame.averageRaw = uint16_t(rawSum / SAMPLE_COUNT);
  int averageMv = 0;
  if (adcCalibration &&
      adc_cali_raw_to_voltage(adcCalibration, frame.averageRaw,
                              &averageMv) == ESP_OK)
    frame.calibratedMv = uint16_t(constrain(averageMv, 0, 65535));
  else
    frame.calibratedMv = 0;
  frame.digitalEdges = edgeCount;
  reconstructDigitalSamples(frame, edgeCount);
  frame.capturedAtMs = millis();
  frame.sequence++;
  frame.started = true;
  lastAdcError = ESP_OK;
  return true;
}

ScopeControlSnapshot copyScopeControl() {
  portENTER_CRITICAL(&scopeControlMux);
  const ScopeControlSnapshot copy = {
      requestedScopeMode, scopeControlGeneration, scopeCaptureInProgress,
      scopeTimebaseIndex};
  portEXIT_CRITICAL(&scopeControlMux);
  return copy;
}

void requestScopeMode(ScopeMode mode) {
  portENTER_CRITICAL(&scopeControlMux);
  requestedScopeMode = mode;
  ++scopeControlGeneration;
  portEXIT_CRITICAL(&scopeControlMux);
}

void changeScopeTimebase(bool openWindow) {
  portENTER_CRITICAL(&scopeControlMux);
  if (openWindow) {
    if (scopeTimebaseIndex + 1 < TIMEBASE_STEPS) ++scopeTimebaseIndex;
  } else if (scopeTimebaseIndex > 0) {
    --scopeTimebaseIndex;
  }
  portEXIT_CRITICAL(&scopeControlMux);
}

uint16_t timebaseSamples(uint8_t index) {
  if (index >= TIMEBASE_STEPS) index = DEFAULT_TIMEBASE_INDEX;
  return TIMEBASE_SAMPLES[index];
}

uint32_t timebaseDurationUs(uint8_t index) {
  return uint32_t((uint64_t(timebaseSamples(index)) * 1000000ULL) /
                  ADC_SAMPLE_RATE_HZ);
}

void setScopeCaptureInProgress(bool active) {
  portENTER_CRITICAL(&scopeControlMux);
  scopeCaptureInProgress = active;
  portEXIT_CRITICAL(&scopeControlMux);
}

void closeSingleCapture(uint32_t generation) {
  portENTER_CRITICAL(&scopeControlMux);
  if (requestedScopeMode == SCOPE_SINGLE &&
      scopeControlGeneration == generation) {
    requestedScopeMode = SCOPE_CLOSED;
    ++scopeControlGeneration;
  }
  portEXIT_CRITICAL(&scopeControlMux);
}

const char *scopeModeName(ScopeMode mode) {
  switch (mode) {
    case SCOPE_OPEN: return "OPEN";
    case SCOPE_SINGLE: return "SINGLE";
    default: return "CLOSED";
  }
}

uint8_t scopePacketFlags(const ScopeControlSnapshot &control,
                         bool digitalOverflow) {
  uint8_t flags = digitalOverflow ? 0x01 : 0x00;
  flags |= uint8_t(control.mode & 0x03) << 1;
  if (control.capturing) flags |= 0x08;
  flags |= uint8_t(control.timebaseIndex & 0x07) << 4;
  return flags;
}

void sendScopeControlState() {
  const ScopeControlSnapshot control = copyScopeControl();
  char json[256];
  snprintf(json, sizeof(json),
           "{\"mode\":\"%s\",\"mode_id\":%u,\"capturing\":%u,"
           "\"generation\":%lu,\"timebase_index\":%u,"
           "\"window_samples\":%u,\"window_us\":%lu}",
           scopeModeName(control.mode), unsigned(control.mode),
           control.capturing ? 1U : 0U,
           static_cast<unsigned long>(control.generation),
           unsigned(control.timebaseIndex),
           unsigned(timebaseSamples(control.timebaseIndex)),
           static_cast<unsigned long>(
               timebaseDurationUs(control.timebaseIndex)));
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.send(200, "application/json", json);
}

void handleScopeControl() {
  if (!server.hasArg("mode")) {
    server.send(400, "text/plain", "MISSING_MODE");
    return;
  }
  const String mode = server.arg("mode");
  if (mode == "open")
    requestScopeMode(SCOPE_OPEN);
  else if (mode == "closed")
    requestScopeMode(SCOPE_CLOSED);
  else if (mode == "single")
    requestScopeMode(SCOPE_SINGLE);
  else {
    server.send(400, "text/plain", "INVALID_MODE");
    return;
  }
  sendScopeControlState();
}

void handleScopeTimebase() {
  if (!server.hasArg("action")) {
    server.send(400, "text/plain", "MISSING_ACTION");
    return;
  }
  const String action = server.arg("action");
  if (action == "open")
    changeScopeTimebase(true);
  else if (action == "close")
    changeScopeTimebase(false);
  else {
    server.send(400, "text/plain", "INVALID_ACTION");
    return;
  }
  sendScopeControlState();
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default: return "OTHER";
  }
}

bool copyScopeFrame(ScopeFrame &destination) {
  if (!scopeMutex || xSemaphoreTake(scopeMutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return false;
  destination = scopeFrame;
  xSemaphoreGive(scopeMutex);
  return destination.started;
}

String statusText() {
  ScopeFrame frame = {};
  copyScopeFrame(frame);
  const ScopeControlSnapshot control = copyScopeControl();
  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP()
                                                : WiFi.softAPIP();
  String out = "FW:AS5600_GPIO5_7_SCOPE_2.2_TIMEBASE";
  out += "\nIP:" + ip.toString();
  out += "\nWIFI:" + String(WiFi.status() == WL_CONNECTED ? "STA" : "AP");
  out += "\nGPIO5:ADC_INPUT GPIO7:DIGITAL_INPUT";
  out += "\nACQUISITION:" + String(scopeModeName(control.mode));
  out += " CAPTURING:" + String(control.capturing ? 1 : 0);
  out += " CONTROL_GEN:" + String(control.generation);
  out += "\nTIMEBASE_INDEX:" + String(control.timebaseIndex);
  out += " WINDOW_SAMPLES:" + String(timebaseSamples(control.timebaseIndex));
  out += " WINDOW_US:" + String(timebaseDurationUs(control.timebaseIndex));
  out += "\nSCOPE_STARTED:" + String(frame.started ? 1 : 0);
  out += " SEQUENCE:" + String(frame.sequence);
  out += " RATE_HZ:" + String(frame.sampleRateHz);
  out += " DURATION_US:" + String(frame.durationUs);
  out += " WALL_US:" + String(frame.captureWallUs);
  out += "\nADC_RAW_MIN:" + String(frame.minRaw);
  out += " MAX:" + String(frame.maxRaw);
  out += " AVG:" + String(frame.averageRaw);
  out += " CAL_MV:" + String(frame.calibratedMv);
  out += "\nGPIO7_LEVEL:" + String(gpio_get_level(DIGITAL_GPIO));
  out += " EDGES:" + String(frame.digitalEdges);
  out += " OVERFLOW:" + String(frame.digitalOverflow ? 1 : 0);
  out += "\nADC_DRIVER_ERROR:" + String(lastAdcError);
  out += "\nFRAME_AGE_MS:" +
         String(frame.capturedAtMs ? uint32_t(millis() - frame.capturedAtMs)
                                    : UINT32_MAX);
  esp_reset_reason_t resetReason = esp_reset_reason();
  out += "\nRESET_REASON:" + String(uint32_t(resetReason));
  out += " " + String(resetReasonName(resetReason));
  out += "\nACTUATORS:NOT_INITIALIZED";
  return out;
}

void sendScopePacket() {
  if (!copyScopeFrame(httpFrame)) {
    server.send(503, "text/plain", "CAPTURE_NOT_READY");
    return;
  }

  const ScopeControlSnapshot control = copyScopeControl();
  const uint16_t visibleCount = timebaseSamples(control.timebaseIndex);
  const uint16_t firstSample = SAMPLE_COUNT - visibleCount;
  uint16_t visibleMin = ADC_MAX_RAW;
  uint16_t visibleMax = 0;
  uint16_t visibleEdges = 0;
  uint32_t visibleSum = 0;
  for (uint16_t i = firstSample; i < SAMPLE_COUNT; ++i) {
    const uint16_t raw = httpFrame.analog[i];
    visibleSum += raw;
    if (raw < visibleMin) visibleMin = raw;
    if (raw > visibleMax) visibleMax = raw;
    if (i > firstSample &&
        httpFrame.digital[i] != httpFrame.digital[i - 1])
      ++visibleEdges;
  }
  const uint16_t visibleAverage = uint16_t(visibleSum / visibleCount);
  const uint16_t visibleAverageMv =
      (httpFrame.averageRaw && httpFrame.calibratedMv)
          ? uint16_t((uint32_t(visibleAverage) * httpFrame.calibratedMv) /
                     httpFrame.averageRaw)
          : 0;

  uint8_t header[PACKET_HEADER_BYTES] = {};
  putU32(header, 0, PACKET_MAGIC);
  putU32(header, 4, httpFrame.sequence);
  putU32(header, 8, httpFrame.sampleRateHz);
  putU32(header, 12, timebaseDurationUs(control.timebaseIndex));
  putU16(header, 16, visibleCount);
  putU16(header, 18, ADC_MAX_RAW);
  putU16(header, 20, visibleMin);
  putU16(header, 22, visibleMax);
  putU16(header, 24, visibleAverage);
  putU16(header, 26, visibleAverageMv);
  putU16(header, 28, visibleEdges);
  header[30] = httpFrame.digital[firstSample];
  header[31] = scopePacketFlags(control, httpFrame.digitalOverflow);

  const size_t analogBytes = size_t(visibleCount) * sizeof(uint16_t);
  const size_t digitalBytes = size_t(visibleCount) * sizeof(uint8_t);
  const size_t packetBytes = PACKET_HEADER_BYTES + analogBytes + digitalBytes;
  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.setContentLength(packetBytes);
  server.send(200, "application/octet-stream", "");
  WiFiClient client = server.client();
  client.write(header, sizeof(header));
  client.write(reinterpret_cast<const uint8_t *>(
                   &httpFrame.analog[firstSample]),
               analogBytes);
  client.write(&httpFrame.digital[firstSample], digitalBytes);
}

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", SCOPE_HTML); });
  server.on("/scope", [] { server.send_P(200, "text/html", SCOPE_HTML); });
  server.on("/scope.bin", sendScopePacket);
  server.on("/scope/state", HTTP_GET, sendScopeControlState);
  server.on("/scope/control", HTTP_POST, handleScopeControl);
  server.on("/scope/timebase", HTTP_POST, handleScopeTimebase);
  server.on("/status", [] { server.send(200, "text/plain", statusText()); });
  server.begin();
}

void scopeTask(void *) {
  delay(1200);  // HTTP y OTA quedan arriba antes de iniciar las capturas.
  static ScopeFrame captured = {};
  if (!initContinuousAdc()) {
    Serial.println("ADC CONTINUOUS INIT FAILED:" + String(lastAdcError));
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    if (otaInProgress) {
      setScopeCaptureInProgress(false);
      delay(50);
      continue;
    }

    const ScopeControlSnapshot control = copyScopeControl();
    if (control.mode == SCOPE_CLOSED) {
      setScopeCaptureInProgress(false);
      delay(25);
      continue;
    }

    setScopeCaptureInProgress(true);
    const bool captureOk = captureScopeFrame(captured);
    setScopeCaptureInProgress(false);
    if (captureOk) {
      if (xSemaphoreTake(scopeMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        scopeFrame = captured;
        xSemaphoreGive(scopeMutex);
      }
    }
    if (control.mode == SCOPE_SINGLE)
      closeSingleCapture(control.generation);
    else
      delay(120);
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  // Los pines se mantienen pasivos desde el primer instante.
  pinMode(ANALOG_PIN, INPUT);
  pinMode(DIGITAL_PIN, INPUT);
  gpio_set_pull_mode(GPIO_NUM_5, GPIO_FLOATING);
  gpio_set_pull_mode(DIGITAL_GPIO, GPIO_FLOATING);
  attachInterrupt(DIGITAL_PIN, digitalEdgeIsr, CHANGE);
  gpio_intr_disable(DIGITAL_GPIO);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("NuevaPata", "nueva-pata");
  WiFi.setAutoReconnect(true);
  if (strlen(NUEVA_PATA_TALLER_SSID))
    WiFi.begin(NUEVA_PATA_TALLER_SSID, NUEVA_PATA_TALLER_PASSWORD);
  else
    WiFi.begin();

  const uint32_t wifiStartedMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartedMs < 8000)
    delay(100);

  scopeMutex = xSemaphoreCreateMutex();
  setupWeb();
  ArduinoOTA.setHostname("ensayo-nueva-pata-scope");
  ArduinoOTA.onStart([] {
    otaInProgress = true;
    Serial.println("OTA START");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.println("OTA ERROR:" + String(static_cast<unsigned>(error)));
  });
  ArduinoOTA.begin();

  xTaskCreate(scopeTask, "gpio5_7_scope", 8192, nullptr, 1, nullptr);
  Serial.println("READY GPIO5 ANALOG / GPIO7 DIGITAL SCOPE");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
    static uint32_t lastReconnectMs = 0;
    if (millis() - lastReconnectMs >= 60000) {
      WiFi.reconnect();
      lastReconnectMs = millis();
    }
  }
  delay(2);
}
