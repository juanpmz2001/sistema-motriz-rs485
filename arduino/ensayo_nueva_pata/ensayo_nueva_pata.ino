#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_system.h>
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define NUEVA_PATA_TALLER_SSID ""
#define NUEVA_PATA_TALLER_PASSWORD ""
#endif

// Ensayo independiente: una pata tipo Toño.
// Tracción SVD48 por RS485 y dirección externa DOCYKE + AS5600.
constexpr uint8_t RS485_RX_PIN = 16;
constexpr uint8_t RS485_TX_PIN = 17;
constexpr uint8_t STEER_PWM_PIN = 14;
constexpr uint8_t AS5600_I2C_ADDRESS = 0x36;
constexpr uint8_t AS5600_SDA_PIN = 5;
constexpr uint8_t AS5600_SCL_PIN = 7;
constexpr uint32_t AS5600_I2C_HZ = 50000;
constexpr uint32_t AS5600_I2C_HALF_PERIOD_US = 10;
constexpr uint32_t AS5600_SCL_TIMEOUT_US = 1000;
constexpr uint32_t AS5600_SAMPLE_MS = 100;
constexpr uint32_t AS5600_BOOT_GRACE_MS = 3000;
constexpr uint32_t AS5600_PANIC_GRACE_MS = 15000;
constexpr uint8_t DRIVE_ID = 2;
constexpr uint16_t DRIVE_CONTROL_REG = 0x5300;  // M1
constexpr uint16_t DRIVE_SPEED_REG = 0x5304;    // M1

constexpr uint32_t SERVO_HZ = 50;
constexpr uint16_t SERVO_NEUTRAL_US = 1500;
// Rango oficial DOCYKE: PWM 0.5–2.5 ms a 50 Hz.
constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_MAX_US = 2500;
constexpr uint32_t COMMAND_TIMEOUT_MS = 450;
constexpr uint32_t AUTO_TEST_RUN_MS = 30000;
constexpr uint32_t AUTO_TEST_PAUSE_MS = 5000;
constexpr uint32_t AUTO_TEST_SAMPLE_MS = 250;
constexpr size_t AUTO_TEST_MAX_SAMPLES = 280;

enum AutoTestState : uint8_t {
  AUTO_TEST_UNINITIALIZED = 0,
  AUTO_TEST_PENDING = 1,
  AUTO_TEST_RUNNING = 2,
  AUTO_TEST_COMPLETE = 3,
  AUTO_TEST_INTERRUPTED = 4,
};

struct AutoTestSample {
  uint16_t elapsed100ms;
  uint16_t adcRaw;
  uint16_t servoUs;
};

struct As5600Reading {
  bool valid;
  bool diagnosticsValid;
  uint16_t rawAngle;
  float angleDeg;
  uint8_t status;
  uint8_t agc;
  uint16_t magnitude;
};

WebServer server(80);
HardwareSerial rs485(2);
Preferences diagnostics;

bool armed = false;
int joyX = 0;
int joyY = 0;
int commandedRpm = 0;
uint16_t servoPulseUs = SERVO_NEUTRAL_US;
uint32_t lastCommandMs = 0;
uint32_t lastMotorWriteMs = 0;
uint32_t lastDiagnosticMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t lastTelemetryRequestMs = 0;
uint32_t diagnosticBootCount = 0;
uint32_t autoTestArmedBoot = 0;
AutoTestState autoTestState = AUTO_TEST_UNINITIALIZED;
bool autoTestScheduled = false;
AutoTestSample autoTestSamples[AUTO_TEST_MAX_SAMPLES];
size_t autoTestSampleCount = 0;
As5600Reading lastAs5600 = {};
uint32_t as5600ReadErrors = 0;
uint32_t lastAs5600SampleMs = 0;
uint32_t lastAs5600GoodMs = 0;
uint32_t as5600ConsecutiveErrors = 0;
uint32_t diagnosticLastResetReason = 0;
uint32_t as5600EnableAtMs = 0;
bool as5600BusInitAttempted = false;
bool as5600BusReady = false;
bool otaInProgress = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Nueva pata</title><style>
*{box-sizing:border-box;user-select:none}html,body,#p{margin:0;width:100%;height:100%;overflow:hidden}
body{font-family:Arial;background:#17202a;color:#fff}#p{display:grid;place-items:center;touch-action:none}
#s{width:86px;height:86px;border-radius:50%;background:#fff;box-shadow:0 5px 20px #000}
#a{position:absolute;top:28px;padding:14px 28px;border:2px solid #fff;border-radius:30px;background:#922b21;color:#fff;font-weight:bold}
#a.on{background:#148f77}#st{position:absolute;bottom:18px;left:12px;right:12px;padding:10px;background:#000a;border-radius:8px;font:13px monospace;white-space:pre}
</style></head><body><div id="p"><button id="a">ARM</button><div id="s"></div><div id="st">Conectando…</div></div>
<script>
const p=document.querySelector('#p'),s=document.querySelector('#s'),a=document.querySelector('#a'),st=document.querySelector('#st');
let arm=0,x=0,y=0,cx=0,cy=0,R=125;
function center(){cx=innerWidth/2;cy=innerHeight/2;s.style.transform='translate(0,0)'}
function send(){fetch(`/cmd?x=${x}&y=${y}&arm=${arm}`).then(r=>r.text()).then(t=>st.textContent=t).catch(()=>st.textContent='Sin conexión')}
function move(e){e.preventDefault();let t=e.touches[0],dx=t.clientX-cx,dy=t.clientY-cy,d=Math.hypot(dx,dy);if(d>R){dx*=R/d;dy*=R/d}s.style.transform=`translate(${dx}px,${dy}px)`;x=Math.round(dx/R*100);y=Math.round(-dy/R*100);send()}
function stop(){x=0;y=0;s.style.transform='translate(0,0)';send()}
a.onclick=()=>{arm=arm?0:1;a.textContent=arm?'ARMED':'ARM';a.classList.toggle('on',arm);if(!arm)stop();send()};
s.addEventListener('touchmove',move,{passive:false});s.addEventListener('touchend',stop);s.addEventListener('touchcancel',stop);
addEventListener('resize',center);center();setInterval(send,150);setInterval(()=>fetch('/status').then(r=>r.text()).then(t=>st.textContent=t),500);
</script></body></html>
)HTML";

const char TELEMETRY_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Telemetria AS5600</title><style>
:root{color-scheme:dark;--bg:#07111f;--card:#101f32;--line:#29435f;--cyan:#34d5ff;--lime:#66f2a3;--amber:#ffc857}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 50% -10%,#173454,#07111f 55%);
color:#eaf7ff;font-family:Inter,Segoe UI,Arial,sans-serif;min-height:100vh}
header{display:flex;justify-content:space-between;align-items:center;padding:18px 24px;border-bottom:1px solid #ffffff18}
h1{margin:0;font-size:clamp(20px,4vw,34px);letter-spacing:.03em}.tag{color:var(--lime);font:700 13px monospace}
main{display:grid;grid-template-columns:minmax(300px,520px) minmax(280px,1fr);gap:18px;padding:18px;max-width:1200px;margin:auto}
.card{background:#101f32e8;border:1px solid #ffffff16;border-radius:20px;padding:18px;box-shadow:0 18px 45px #0006}
.dial{position:relative;max-width:500px;margin:auto;aspect-ratio:1}.dial svg{width:100%;height:100%;display:block}
#needle{transform-origin:150px 150px;transition:transform 90ms linear}.angle{position:absolute;inset:auto 0 13%;text-align:center}
#angle{font-size:clamp(50px,10vw,82px);font-weight:800;line-height:.9}.unit{color:#8fb2ca;font-size:20px}
.stats{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}.stat{padding:15px;background:#071522;border:1px solid #ffffff12;border-radius:14px}
.label{color:#86a6bc;font-size:12px;text-transform:uppercase;letter-spacing:.12em}.value{font:700 28px/1.25 monospace;margin-top:5px}
.small{font-size:18px}.okText{color:var(--lime)}.warnText{color:var(--amber)}.badText{color:#ff637d}
.wide{grid-column:1/-1}.bar{height:10px;border-radius:9px;background:#21364a;overflow:hidden;margin-top:10px}
#bar{height:100%;width:0;background:linear-gradient(90deg,var(--cyan),var(--lime));transition:width 100ms}
canvas{width:100%;height:240px;background:#071522;border-radius:14px;margin-top:14px}
.row{display:flex;justify-content:space-between;gap:12px;align-items:center}.ok{color:var(--lime)}.bad{color:#ff637d}
button{border:1px solid #ffffff2a;background:#19334a;color:#fff;border-radius:10px;padding:9px 13px;cursor:pointer}
@media(max-width:780px){main{grid-template-columns:1fr;padding:10px}.card{padding:12px}header{padding:14px}}
</style></head><body>
<header><div><h1>Nueva Pata</h1><div class="label">AS5600 · I2C seguro 0x36 · SDA5/SCL7</div></div><div id="link" class="tag">CONECTANDO</div></header>
<main><section class="card"><div class="dial">
<svg viewBox="0 0 300 300" aria-label="Angulo de direccion">
<defs><filter id="glow"><feGaussianBlur stdDeviation="3" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>
<circle cx="150" cy="150" r="132" fill="#071522" stroke="#29435f" stroke-width="3"/>
<circle cx="150" cy="150" r="112" fill="none" stroke="#1d354b" stroke-width="18"/>
<g id="ticks"></g>
<g id="needle" filter="url(#glow)"><line x1="150" y1="150" x2="150" y2="40" stroke="#34d5ff" stroke-width="5" stroke-linecap="round"/>
<path d="M150 25l-10 25h20z" fill="#ffc857"/><circle cx="150" cy="150" r="13" fill="#34d5ff" stroke="#dffaff" stroke-width="3"/></g>
</svg><div class="angle"><span id="angle">---.-</span><span class="unit">°</span></div></div></section>
<section class="card"><div class="row"><div><div class="label">Telemetria en vivo</div><div id="time" class="tag">Esperando datos</div></div>
<button onclick="zeroTurns()">Poner vueltas en cero</button></div>
<div class="stats" style="margin-top:16px">
<div class="stat"><div class="label">Raw angle (12 bit)</div><div id="raw" class="value">----</div></div>
<div class="stat"><div class="label">Posicion</div><div id="pct" class="value">--.--%</div></div>
<div class="stat"><div class="label">Estado del iman</div><div id="magnet" class="value small">ESPERANDO</div></div>
<div class="stat"><div class="label">AGC</div><div id="agc" class="value">---</div></div>
<div class="stat"><div class="label">Magnitud</div><div id="magnitude" class="value">----</div></div>
<div class="stat"><div class="label">Bus I2C</div><div id="bus" class="value small">0x36</div></div>
<div class="stat"><div class="label">Giro acumulado</div><div id="total" class="value">0.0°</div></div>
<div class="stat"><div class="label">Vueltas</div><div id="turns" class="value">0.000</div></div>
<div class="stat wide"><div class="label">Recorrido electrico</div><div class="bar"><div id="bar"></div></div></div>
</div><canvas id="chart"></canvas></section></main>
<script>
const ns='http://www.w3.org/2000/svg',ticks=document.querySelector('#ticks');
for(let i=0;i<36;i++){let a=i*10*Math.PI/180,x1=150+Math.sin(a)*(i%3?119:113),y1=150-Math.cos(a)*(i%3?119:113),
x2=150+Math.sin(a)*128,y2=150-Math.cos(a)*128,l=document.createElementNS(ns,'line');
l.setAttribute('x1',x1);l.setAttribute('y1',y1);l.setAttribute('x2',x2);l.setAttribute('y2',y2);
l.setAttribute('stroke',i%3?'#45627a':'#ffc857');l.setAttribute('stroke-width',i%3?1.5:3);ticks.appendChild(l)}
const c=document.querySelector('#chart'),ctx=c.getContext('2d'),angleEl=document.querySelector('#angle'),
rawEl=document.querySelector('#raw'),pctEl=document.querySelector('#pct'),totalEl=document.querySelector('#total'),
turnsEl=document.querySelector('#turns'),barEl=document.querySelector('#bar'),linkEl=document.querySelector('#link'),
timeEl=document.querySelector('#time'),magnetEl=document.querySelector('#magnet'),agcEl=document.querySelector('#agc'),
magnitudeEl=document.querySelector('#magnitude'),busEl=document.querySelector('#bus');
let history=[],last=null,unwrapped=0,zero=0,rot=0,fail=0;
function zeroTurns(){zero=unwrapped}
function draw(){let dpr=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*dpr;c.height=h*dpr;ctx.setTransform(dpr,0,0,dpr,0,0);
ctx.clearRect(0,0,w,h);ctx.strokeStyle='#29435f';ctx.lineWidth=1;for(let i=0;i<=4;i++){let y=h*i/4;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}
if(history.length<2)return;ctx.strokeStyle='#34d5ff';ctx.lineWidth=2;ctx.beginPath();history.forEach((v,i)=>{let x=i*w/199,y=h-v/360*h;i?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke()}
async function poll(){let ctl=new AbortController(),timeout=setTimeout(()=>ctl.abort(),1000);try{
let r=await fetch('/telemetry.json',{cache:'no-store',signal:ctl.signal});if(!r.ok)throw 0;let d=await r.json();
if(!d.valid){linkEl.textContent='AS5600 NO DETECTADO';linkEl.className='tag bad';busEl.textContent=`0x36 NO RESPONDE | SDA${d.sda}=${d.sda_level} SCL${d.scl}=${d.scl_level}`;busEl.className='value small badText';
magnetEl.textContent='SIN DISPOSITIVO';magnetEl.className='value small badText';rawEl.textContent='----';pctEl.textContent='--.--%';agcEl.textContent='---';magnitudeEl.textContent='----';
angleEl.textContent='---.-';barEl.style.width='0%';document.querySelector('#needle').style.transform='rotate(0deg)';timeEl.textContent=new Date().toLocaleTimeString();fail=0;return}let a=Number(d.angle);
if(last===null){last=a;rot=a;unwrapped=a}else{let delta=((a-last+540)%360)-180;unwrapped+=delta;rot+=delta;last=a}
document.querySelector('#needle').style.transform=`rotate(${rot}deg)`;angleEl.textContent=a.toFixed(1);rawEl.textContent=d.raw;pctEl.textContent=d.pct.toFixed(2)+'%';
let magnet='NO DETECTADO',magnetClass='badText';if(d.ml){magnet='MUY DEBIL';magnetClass='warnText'}else if(d.mh){magnet='MUY FUERTE';magnetClass='warnText'}else if(d.md){magnet='DETECTADO';magnetClass='okText'}
magnetEl.textContent=magnet;magnetEl.className='value small '+magnetClass;agcEl.textContent=d.diag?d.agc:'---';magnitudeEl.textContent=d.diag?d.magnitude:'----';
busEl.textContent=`0x36 OK · SDA${d.sda}/SCL${d.scl}`;busEl.className='value small okText';
let rel=unwrapped-zero;totalEl.textContent=rel.toFixed(1)+'°';turnsEl.textContent=(rel/360).toFixed(3);barEl.style.width=d.pct+'%';
history.push(a);if(history.length>200)history.shift();draw();linkEl.textContent='EN LINEA · I2C · '+d.hz.toFixed(1)+' Hz';linkEl.className='tag ok';
timeEl.textContent=new Date().toLocaleTimeString();fail=0}catch(e){if(++fail>3){linkEl.textContent='SIN CONEXION';linkEl.className='tag bad'}}
finally{clearTimeout(timeout);setTimeout(poll,200)}}
addEventListener('resize',draw);poll();
</script></body></html>
)HTML";

uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t p = 0; p < length; ++p) {
    crc ^= data[p];
    for (int i = 0; i < 8; ++i) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void writeRegister(uint16_t reg, int16_t value) {
  uint8_t frame[8] = {DRIVE_ID, 0x06, uint8_t(reg >> 8), uint8_t(reg),
                      uint8_t(uint16_t(value) >> 8), uint8_t(value), 0, 0};
  uint16_t crc = crc16(frame, 6);
  // Los controladores UU Motor usados en Toño esperan CRC alto y luego bajo.
  frame[6] = uint8_t(crc >> 8);
  frame[7] = uint8_t(crc);
  while (rs485.available()) rs485.read();
  rs485.write(frame, sizeof(frame));
  rs485.flush();
}

void stopMotor() {
  commandedRpm = 0;
  writeRegister(DRIVE_SPEED_REG, 0);
  writeRegister(DRIVE_CONTROL_REG, 0);
}

void commandMotor(int rpm) {
  rpm = constrain(rpm, -30, 30);
  commandedRpm = rpm;
  writeRegister(DRIVE_SPEED_REG, rpm);
  writeRegister(DRIVE_CONTROL_REG, rpm == 0 ? 0 : 1);
}

void setServoPulse(uint16_t pulseUs) {
  pulseUs = constrain(pulseUs, SERVO_MIN_US, SERVO_MAX_US);
  servoPulseUs = pulseUs;
  const uint32_t maxDuty = (1UL << 14) - 1;
  ledcWrite(STEER_PWM_PIN, uint32_t(pulseUs) * maxDuty / 20000UL);
}

void softI2cDelay() {
  delayMicroseconds(AS5600_I2C_HALF_PERIOD_US);
}

void softI2cSetSda(bool releaseHigh) {
  gpio_set_level(GPIO_NUM_5, releaseHigh ? 1 : 0);
}

void softI2cSetScl(bool releaseHigh) {
  gpio_set_level(GPIO_NUM_7, releaseHigh ? 1 : 0);
}

bool softI2cWaitSclHigh() {
  softI2cSetScl(true);
  uint32_t startedUs = micros();
  while (gpio_get_level(GPIO_NUM_7) == 0) {
    if (uint32_t(micros() - startedUs) >= AS5600_SCL_TIMEOUT_US)
      return false;
  }
  return true;
}

bool as5600LinesIdle() {
  return gpio_get_level(GPIO_NUM_5) == 1 &&
         gpio_get_level(GPIO_NUM_7) == 1;
}

bool softI2cStop() {
  softI2cSetScl(false);
  softI2cSetSda(false);
  softI2cDelay();
  bool sclReleased = softI2cWaitSclHigh();
  softI2cDelay();
  softI2cSetSda(true);
  softI2cDelay();
  return sclReleased && as5600LinesIdle();
}

bool softI2cRecoverBus() {
  softI2cSetSda(true);
  if (!softI2cWaitSclHigh()) return false;
  softI2cDelay();
  for (uint8_t pulse = 0; pulse < 9 && gpio_get_level(GPIO_NUM_5) == 0;
       ++pulse) {
    softI2cSetScl(false);
    softI2cDelay();
    if (!softI2cWaitSclHigh()) return false;
    softI2cDelay();
  }
  return softI2cStop();
}

bool softI2cStart() {
  softI2cSetSda(true);
  softI2cDelay();
  if (!softI2cWaitSclHigh() || gpio_get_level(GPIO_NUM_5) == 0)
    return false;
  softI2cDelay();
  softI2cSetSda(false);
  softI2cDelay();
  softI2cSetScl(false);
  softI2cDelay();
  return true;
}

bool softI2cWriteByte(uint8_t value) {
  for (uint8_t bit = 0; bit < 8; ++bit) {
    softI2cSetScl(false);
    softI2cSetSda((value & 0x80) != 0);
    softI2cDelay();
    if (!softI2cWaitSclHigh()) return false;
    softI2cDelay();
    softI2cSetScl(false);
    softI2cDelay();
    value <<= 1;
  }
  softI2cSetSda(true);
  softI2cDelay();
  if (!softI2cWaitSclHigh()) return false;
  softI2cDelay();
  bool acknowledged = gpio_get_level(GPIO_NUM_5) == 0;
  softI2cSetScl(false);
  softI2cDelay();
  return acknowledged;
}

bool softI2cReadByte(uint8_t &value, bool acknowledge) {
  value = 0;
  softI2cSetSda(true);
  for (uint8_t bit = 0; bit < 8; ++bit) {
    softI2cSetScl(false);
    softI2cDelay();
    if (!softI2cWaitSclHigh()) return false;
    softI2cDelay();
    value = uint8_t((value << 1) | gpio_get_level(GPIO_NUM_5));
    softI2cSetScl(false);
    softI2cDelay();
  }
  softI2cSetSda(!acknowledge);
  softI2cDelay();
  if (!softI2cWaitSclHigh()) return false;
  softI2cDelay();
  softI2cSetScl(false);
  softI2cSetSda(true);
  softI2cDelay();
  return true;
}

bool as5600ReadBytes(uint8_t firstRegister, uint8_t *data, size_t length) {
  if (!as5600BusReady || !data || !length) return false;
  if (!as5600LinesIdle() && !softI2cRecoverBus()) return false;

  bool ok = softI2cStart() &&
            softI2cWriteByte(uint8_t(AS5600_I2C_ADDRESS << 1)) &&
            softI2cWriteByte(firstRegister) && softI2cStart() &&
            softI2cWriteByte(uint8_t((AS5600_I2C_ADDRESS << 1) | 1));
  if (!ok) {
    softI2cStop();
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!softI2cReadByte(data[i], i + 1 < length)) {
      softI2cStop();
      return false;
    }
  }
  return softI2cStop();
}

bool beginAs5600Bus() {
  gpio_set_level(GPIO_NUM_5, 1);
  gpio_set_level(GPIO_NUM_7, 1);
  gpio_config_t busConfig = {};
  busConfig.pin_bit_mask = (1ULL << AS5600_SDA_PIN) |
                           (1ULL << AS5600_SCL_PIN);
  busConfig.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  busConfig.pull_up_en = GPIO_PULLUP_ENABLE;
  busConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
  busConfig.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&busConfig) != ESP_OK) return false;
  softI2cSetSda(true);
  softI2cSetScl(true);
  softI2cDelay();
  softI2cRecoverBus();
  return true;
}

bool hardwareAs5600ReadBytes(uint8_t firstRegister, uint8_t *data,
                             size_t length) {
  Wire.beginTransmission(AS5600_I2C_ADDRESS);
  Wire.write(firstRegister);
  if (Wire.endTransmission(false) != 0) return false;
  size_t received = Wire.requestFrom(AS5600_I2C_ADDRESS, length);
  if (received != length) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < length; ++i) data[i] = Wire.read();
  return true;
}

bool sampleAs5600() {
  uint8_t primary[3] = {};     // STATUS, RAW_ANGLE_H, RAW_ANGLE_L
  uint8_t diagnostics[3] = {}; // AGC, MAGNITUDE_H, MAGNITUDE_L
  lastAs5600SampleMs = millis();

  if (!as5600ReadBytes(0x0B, primary, sizeof(primary))) {
    lastAs5600 = {};
    as5600ReadErrors++;
    as5600ConsecutiveErrors++;
    return false;
  }

  As5600Reading reading = {};
  reading.valid = true;
  reading.diagnosticsValid =
      as5600ReadBytes(0x1A, diagnostics, sizeof(diagnostics));
  if (!reading.diagnosticsValid) as5600ReadErrors++;
  reading.rawAngle =
      (uint16_t(primary[1] & 0x0F) << 8) | primary[2];
  reading.angleDeg = 360.0f * reading.rawAngle / 4096.0f;
  reading.status = primary[0] & 0x38;
  reading.agc = diagnostics[0];
  reading.magnitude =
      (uint16_t(diagnostics[1] & 0x0F) << 8) | diagnostics[2];
  lastAs5600 = reading;
  lastAs5600GoodMs = lastAs5600SampleMs;
  as5600ConsecutiveErrors = 0;
  return true;
}

bool readAngle(float &angle, uint32_t &raw, float &positionPct) {
  uint32_t sampleAge =
      lastAs5600SampleMs ? uint32_t(millis() - lastAs5600SampleMs) : UINT32_MAX;
  if (!lastAs5600.valid || sampleAge > 250) {
    angle = 0.0f;
    raw = 0;
    positionPct = 0.0f;
    return false;
  }
  angle = lastAs5600.angleDeg;
  raw = lastAs5600.rawAngle;
  positionPct = 100.0f * lastAs5600.rawAngle / 4095.0f;
  return true;
}

void serviceAs5600() {
  if (otaInProgress || int32_t(millis() - as5600EnableAtMs) < 0) return;
  if (!as5600BusInitAttempted) {
    as5600BusInitAttempted = true;
    as5600BusReady = beginAs5600Bus();
    if (!as5600BusReady) {
      as5600ReadErrors++;
      as5600ConsecutiveErrors++;
      Serial.println("AS5600 I2C INIT FAILED");
      return;
    }
    Serial.println("AS5600 SAFE I2C READY SDA5 SCL7");
  }
  if (!as5600BusReady) return;
  uint32_t intervalMs = lastAs5600.valid ? AS5600_SAMPLE_MS : 250;
  if (as5600ConsecutiveErrors >= 5) intervalMs = 1000;
  if (!lastAs5600SampleMs || millis() - lastAs5600SampleMs >= intervalMs)
    sampleAs5600();
}

const char *autoTestStateName() {
  switch (autoTestState) {
    case AUTO_TEST_PENDING: return "PENDING";
    case AUTO_TEST_RUNNING: return "RUNNING";
    case AUTO_TEST_COMPLETE: return "COMPLETE";
    case AUTO_TEST_INTERRUPTED: return "INTERRUPTED";
    default: return "UNINITIALIZED";
  }
}

const char *resetReasonName(uint32_t reason) {
  switch (static_cast<esp_reset_reason_t>(reason)) {
    case ESP_RST_POWERON: return "POWER_ON";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";
    default: return "OTHER";
  }
}

void persistAutoTestSamples() {
  diagnostics.putUShort("sample_count", autoTestSampleCount);
  if (autoTestSampleCount)
    diagnostics.putBytes("samples", autoTestSamples,
                         autoTestSampleCount * sizeof(AutoTestSample));
}

void recordAutoTestSample(uint32_t startedMs) {
  if (autoTestSampleCount >= AUTO_TEST_MAX_SAMPLES) return;
  AutoTestSample &sample = autoTestSamples[autoTestSampleCount++];
  sample.elapsed100ms = uint16_t((millis() - startedMs) / 100);
  sample.adcRaw = lastAs5600.valid ? lastAs5600.rawAngle : UINT16_MAX;
  sample.servoUs = servoPulseUs;
  if (autoTestSampleCount % 20 == 0) persistAutoTestSamples();
}

String autoTestLogText() {
  String out;
  out.reserve(9000);
  out += "STATE:" + String(autoTestStateName());
  out += "\nBOOT_COUNT:" + String(diagnosticBootCount);
  out += "\nLAST_RESET_REASON:" + String(diagnostics.getUInt("reset_reason", 0));
  out += "\nSAMPLES:" + String(autoTestSampleCount);
  out += "\nTIME_MS,ENCODER_RAW,ANGLE_DEG,SERVO_US\n";
  for (size_t i = 0; i < autoTestSampleCount; ++i) {
    const AutoTestSample &sample = autoTestSamples[i];
    out += String(uint32_t(sample.elapsed100ms) * 100);
    out += "," + String(sample.adcRaw);
    out += "," + String(360.0f * sample.adcRaw / 4095.0f, 2);
    out += "," + String(sample.servoUs) + "\n";
  }
  return out;
}

void runAutoTestPhase(uint16_t pulseUs, uint32_t durationMs,
                      uint32_t testStartedMs) {
  setServoPulse(pulseUs);
  uint32_t phaseStartedMs = millis();
  uint32_t lastSampleMs = 0;
  while (millis() - phaseStartedMs < durationMs) {
    ArduinoOTA.handle();
    server.handleClient();
    serviceAs5600();
    if (millis() - lastSampleMs >= AUTO_TEST_SAMPLE_MS) {
      recordAutoTestSample(testStartedMs);
      lastSampleMs = millis();
    }
    delay(2);
  }
}

void runAutonomousTest() {
  autoTestScheduled = false;
  autoTestState = AUTO_TEST_RUNNING;
  diagnostics.putUChar("state", autoTestState);
  diagnostics.putUShort("sample_count", 0);
  diagnostics.remove("samples");
  autoTestSampleCount = 0;
  armed = false;
  joyX = joyY = 0;
  stopMotor();
  setServoPulse(SERVO_NEUTRAL_US);
  Serial.println("AUTO_TEST START IN 15 SECONDS");
  uint32_t waitStartedMs = millis();
  while (millis() - waitStartedMs < 15000) {
    ArduinoOTA.handle();
    server.handleClient();
    serviceAs5600();
    delay(5);
  }

  uint32_t testStartedMs = millis();
  runAutoTestPhase(SERVO_MAX_US, AUTO_TEST_RUN_MS, testStartedMs);
  runAutoTestPhase(SERVO_NEUTRAL_US, AUTO_TEST_PAUSE_MS, testStartedMs);
  runAutoTestPhase(SERVO_MIN_US, AUTO_TEST_RUN_MS, testStartedMs);
  setServoPulse(SERVO_NEUTRAL_US);
  stopMotor();
  recordAutoTestSample(testStartedMs);
  persistAutoTestSamples();
  autoTestState = AUTO_TEST_COMPLETE;
  diagnostics.putUChar("state", autoTestState);
  Serial.println("AUTO_TEST COMPLETE - SERVO NEUTRAL");
}

String statusText() {
  float angle = 0, positionPct = 0;
  uint32_t raw = 0;
  bool valid = readAngle(angle, raw, positionPct);
  IPAddress activeIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  String s = "ROBOT:NUEVA_PATA\nFW:NUEVA_PATA_2.0_SAFE\nIP:" + activeIp.toString();
  s += "\nWIFI:" + String(WiFi.status() == WL_CONNECTED ? "STA" : "AP");
  if (WiFi.status() == WL_CONNECTED) s += " SSID:" + WiFi.SSID();
  s += "\nWIFI_POLICY:M_ZAPATA_ONLY";
  s += "\nAP_IP:" + WiFi.softAPIP().toString();
  s += " AP_CLIENTS:" + String(WiFi.softAPgetStationNum());
  s += "\nARMED:" + String(armed ? 1 : 0);
  s += "\nJOY_X:" + String(joyX) + " JOY_Y:" + String(joyY);
  s += "\nMOTOR_RPM:" + String(commandedRpm);
  s += "\nSERVO_US:" + String(servoPulseUs);
  s += "\nAS5600_VALID:" + String(valid ? 1 : 0);
  if (valid) {
    s += " ANGLE:" + String(angle, 1);
    s += " POSITION_PCT:" + String(positionPct, 2);
    s += " RAW_ANGLE:" + String(raw);
  }
  s += "\nAS5600_MODE:I2C_SAFE ADDR:0x36 SDA:" + String(AS5600_SDA_PIN);
  s += " SCL:" + String(AS5600_SCL_PIN);
  s += " HZ:" + String(AS5600_I2C_HZ);
  s += " BUS_READY:" + String(as5600BusReady ? 1 : 0);
  s += " SDA_LEVEL:" + String(gpio_get_level(GPIO_NUM_5));
  s += " SCL_LEVEL:" + String(gpio_get_level(GPIO_NUM_7));
  if (!as5600BusInitAttempted) {
    int32_t waitMs = int32_t(as5600EnableAtMs - millis());
    s += " START_IN_MS:" + String(max(int32_t(0), waitMs));
  }
  if (valid) {
    s += "\nMAGNET_DETECTED:" + String((lastAs5600.status & 0x20) ? 1 : 0);
    s += " TOO_WEAK:" + String((lastAs5600.status & 0x10) ? 1 : 0);
    s += " TOO_STRONG:" + String((lastAs5600.status & 0x08) ? 1 : 0);
    bool magnetHealthy = (lastAs5600.status & 0x20) &&
                         !(lastAs5600.status & 0x18);
    s += " HEALTHY:" + String(magnetHealthy ? 1 : 0);
    s += " STATUS:0x" + String(lastAs5600.status, HEX);
    s += " DIAGNOSTICS_VALID:" + String(lastAs5600.diagnosticsValid ? 1 : 0);
    s += " AGC:" + String(lastAs5600.agc);
    s += " MAGNITUDE:" + String(lastAs5600.magnitude);
  }
  s += "\nI2C_SAMPLE_AGE_MS:" +
       String(lastAs5600SampleMs ? uint32_t(millis() - lastAs5600SampleMs)
                                 : UINT32_MAX);
  s += " LAST_GOOD_AGE_MS:" +
       String(lastAs5600GoodMs ? uint32_t(millis() - lastAs5600GoodMs)
                               : UINT32_MAX);
  s += "\nI2C_READ_ERRORS:" + String(as5600ReadErrors);
  s += " CONSECUTIVE_ERRORS:" + String(as5600ConsecutiveErrors);
  s += "\nAUTO_TEST:" + String(autoTestStateName());
  s += " SAMPLES:" + String(autoTestSampleCount);
  s += " BOOT:" + String(diagnosticBootCount);
  s += "\nLAST_RESET_REASON:" + String(diagnosticLastResetReason);
  s += " " + String(resetReasonName(diagnosticLastResetReason));
  return s;
}

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/telemetry", [] {
    server.send_P(200, "text/html", TELEMETRY_HTML);
  });
  server.on("/telemetry.json", [] {
    float angle = 0, pct = 0;
    uint32_t raw = 0;
    bool valid = readAngle(angle, raw, pct);
    uint32_t now = millis();
    float requestHz =
        lastTelemetryRequestMs && now != lastTelemetryRequestMs
            ? 1000.0f / float(now - lastTelemetryRequestMs)
            : 0.0f;
    lastTelemetryRequestMs = now;
    bool magnetHealthy = valid && (lastAs5600.status & 0x20) &&
                         !(lastAs5600.status & 0x18);
    uint32_t sampleAge = lastAs5600SampleMs
                             ? uint32_t(now - lastAs5600SampleMs)
                             : UINT32_MAX;
    char json[448];
    snprintf(json, sizeof(json),
             "{\"valid\":%u,\"angle\":%.2f,\"raw\":%lu,\"pct\":%.3f,"
             "\"hz\":%.2f,\"status\":%u,\"md\":%u,\"ml\":%u,"
             "\"mh\":%u,\"healthy\":%u,\"diag\":%u,\"agc\":%u,"
             "\"magnitude\":%u,\"sda\":%u,\"scl\":%u,"
             "\"sda_level\":%u,\"scl_level\":%u,\"errors\":%lu,"
             "\"sample_age\":%lu,"
             "\"millis\":%lu}",
             valid ? 1U : 0U, angle, static_cast<unsigned long>(raw), pct,
             requestHz, static_cast<unsigned>(lastAs5600.status),
             (lastAs5600.status & 0x20) ? 1U : 0U,
             (lastAs5600.status & 0x10) ? 1U : 0U,
             (lastAs5600.status & 0x08) ? 1U : 0U,
             magnetHealthy ? 1U : 0U,
             lastAs5600.diagnosticsValid ? 1U : 0U,
             static_cast<unsigned>(lastAs5600.agc),
             static_cast<unsigned>(lastAs5600.magnitude),
             static_cast<unsigned>(AS5600_SDA_PIN),
             static_cast<unsigned>(AS5600_SCL_PIN),
             static_cast<unsigned>(gpio_get_level(GPIO_NUM_5)),
             static_cast<unsigned>(gpio_get_level(GPIO_NUM_7)),
             static_cast<unsigned long>(as5600ReadErrors),
             static_cast<unsigned long>(sampleAge),
             static_cast<unsigned long>(now));
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", json);
  });
  server.on("/i2c/scan", [] {
    String text = "I2C_SDA:" + String(AS5600_SDA_PIN);
    text += "\nI2C_SCL:" + String(AS5600_SCL_PIN);
    text += "\nI2C_HZ:" + String(AS5600_I2C_HZ);
    text += "\nAS5600_ADDR:0x36";
    text += "\nI2C_BUS_READY:" + String(as5600BusReady ? 1 : 0);
    text += "\nSDA_LEVEL:" + String(gpio_get_level(GPIO_NUM_5));
    text += "\nSCL_LEVEL:" + String(gpio_get_level(GPIO_NUM_7));
    text += "\nAS5600_FOUND:" + String(lastAs5600.valid ? 1 : 0);
    text += "\nI2C_READ_ERRORS:" + String(as5600ReadErrors);
    server.send(lastAs5600.valid ? 200 : 503, "text/plain", text);
  });
  server.on("/i2c/discover", [] {
    server.send(409, "text/plain",
                "DISCOVERY_DISABLED\nFIXED_SDA:5\nFIXED_SCL:7");
  });
  server.on("/i2c/hardware-test", [] {
    armed = false;
    joyX = joyY = 0;
    stopMotor();
    setServoPulse(SERVO_NEUTRAL_US);

    as5600BusReady = false;
    softI2cSetSda(true);
    softI2cSetScl(true);
    delay(2);

    bool beginOk =
        Wire.begin(AS5600_SDA_PIN, AS5600_SCL_PIN, AS5600_I2C_HZ);
    Wire.setTimeOut(20);
    uint8_t probeError = 255;
    uint8_t primary[3] = {};
    bool readOk = false;
    if (beginOk) {
      Wire.beginTransmission(AS5600_I2C_ADDRESS);
      probeError = Wire.endTransmission();
      if (probeError == 0)
        readOk = hardwareAs5600ReadBytes(0x0B, primary, sizeof(primary));
    }
    Wire.end();

    as5600BusReady = beginAs5600Bus();
    lastAs5600 = {};
    lastAs5600SampleMs = 0;

    uint16_t rawAngle =
        (uint16_t(primary[1] & 0x0F) << 8) | primary[2];
    String text = "HARDWARE_I2C_TEST:COMPLETE";
    text += "\nBEGIN_OK:" + String(beginOk ? 1 : 0);
    text += "\nPROBE_ERROR:" + String(probeError);
    text += "\nAS5600_FOUND:" + String(probeError == 0 ? 1 : 0);
    text += "\nREAD_OK:" + String(readOk ? 1 : 0);
    text += "\nSTATUS:" + String(primary[0]);
    text += "\nRAW_ANGLE:" + String(rawAngle);
    text += "\nANGLE_DEG:" + String(360.0f * rawAngle / 4096.0f, 2);
    text += "\nFINAL_MODE:I2C_SAFE";
    text += "\nSERVO_US:" + String(servoPulseUs);
    text += "\nMOTOR_RPM:" + String(commandedRpm);
    server.send(200, "text/plain", text);
  });
  server.on("/status", [] { server.send(200, "text/plain", statusText()); });
  server.on("/autotest/log", [] {
    server.send(200, "text/plain", autoTestLogText());
  });
  server.on("/autotest/arm", [] {
    armed = false;
    stopMotor();
    setServoPulse(SERVO_NEUTRAL_US);
    autoTestState = AUTO_TEST_PENDING;
    autoTestArmedBoot = diagnosticBootCount;
    diagnostics.putUChar("state", autoTestState);
    diagnostics.putUInt("armed_boot", autoTestArmedBoot);
    server.send(200, "text/plain",
                "ARMED FOR NEXT POWER CYCLE - SERVO NEUTRAL");
  });
  server.on("/pwm-loopback", [] {
    armed = false;
    stopMotor();
    setServoPulse(SERVO_NEUTRAL_US);
    server.send(409, "text/plain",
                "LOOPBACK_DISABLED\nDIAGNOSTIC_SAFETY\nSERVO_NEUTRAL");
  });
  server.on("/stop", [] { armed = false; joyX = joyY = 0; stopMotor(); setServoPulse(SERVO_NEUTRAL_US); server.send(200, "text/plain", "STOP"); });
  server.on("/cmd", [] {
    joyX = constrain(server.arg("x").toInt(), -100, 100);
    joyY = constrain(server.arg("y").toInt(), -100, 100);
    armed = server.arg("arm").toInt() == 1;
    lastCommandMs = millis();
    if (!armed) {
      joyX = joyY = 0;
      stopMotor();
      setServoPulse(SERVO_NEUTRAL_US);
    }
    server.send(200, "text/plain", statusText());
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  diagnostics.begin("np_diag1", false);
  diagnosticBootCount = diagnostics.getUInt("boot_count", 0) + 1;
  diagnostics.putUInt("boot_count", diagnosticBootCount);
  diagnosticLastResetReason = uint32_t(esp_reset_reason());
  diagnostics.putUInt("reset_reason", diagnosticLastResetReason);
  autoTestState =
      AutoTestState(diagnostics.getUChar("state", AUTO_TEST_UNINITIALIZED));
  autoTestArmedBoot = diagnostics.getUInt("armed_boot", 0);
  if (autoTestState == AUTO_TEST_RUNNING) {
    autoTestState = AUTO_TEST_INTERRUPTED;
    diagnostics.putUChar("state", autoTestState);
  }
  if (autoTestState == AUTO_TEST_UNINITIALIZED) {
    autoTestState = AUTO_TEST_PENDING;
    autoTestArmedBoot = diagnosticBootCount;
    diagnostics.putUChar("state", autoTestState);
    diagnostics.putUInt("armed_boot", autoTestArmedBoot);
  } else if (autoTestState == AUTO_TEST_PENDING &&
             diagnosticBootCount > autoTestArmedBoot) {
    autoTestScheduled = true;
  }
  autoTestSampleCount =
      min(size_t(diagnostics.getUShort("sample_count", 0)),
          AUTO_TEST_MAX_SAMPLES);
  if (autoTestSampleCount)
    diagnostics.getBytes("samples", autoTestSamples,
                         autoTestSampleCount * sizeof(AutoTestSample));

  rs485.begin(115200, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  if (!ledcAttach(STEER_PWM_PIN, SERVO_HZ, 14)) Serial.println("ERR LEDC");
  setServoPulse(SERVO_NEUTRAL_US);
  stopMotor();

  // AP de rescate siempre disponible; la unica red STA es M ZAPATA.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("NuevaPata", "nueva-pata");
  WiFi.setAutoReconnect(true);
  if (strlen(NUEVA_PATA_TALLER_SSID))
    WiFi.begin(NUEVA_PATA_TALLER_SSID, NUEVA_PATA_TALLER_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("AP http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("STA http://");
    Serial.println(WiFi.localIP());
  }
  setupWeb();
  ArduinoOTA.setHostname("ensayo-nueva-pata");
  ArduinoOTA.onStart([] {
    otaInProgress = true;
    autoTestScheduled = false;
    if (autoTestState == AUTO_TEST_PENDING ||
        autoTestState == AUTO_TEST_RUNNING) {
      autoTestState = AUTO_TEST_INTERRUPTED;
      diagnostics.putUChar("state", autoTestState);
    }
    armed = false;
    joyX = joyY = 0;
    stopMotor();
    setServoPulse(SERVO_NEUTRAL_US);
    Serial.println("OTA START - ACTUADORES DETENIDOS");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.println("OTA ERROR:" + String(static_cast<unsigned>(error)));
  });
  ArduinoOTA.begin();
  as5600EnableAtMs =
      millis() + (diagnosticLastResetReason == uint32_t(ESP_RST_PANIC)
                      ? AS5600_PANIC_GRACE_MS
                      : AS5600_BOOT_GRACE_MS);
  Serial.println("READY NUEVA_PATA");
  Serial.println(autoTestLogText());
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  serviceAs5600();
  uint32_t now = millis();
  if (autoTestScheduled) {
    runAutonomousTest();
    return;
  }
  if (WiFi.status() != WL_CONNECTED && WiFi.softAPgetStationNum() == 0 &&
      now - lastWifiRetryMs >= 60000) {
    if (strlen(NUEVA_PATA_TALLER_SSID))
      WiFi.begin(NUEVA_PATA_TALLER_SSID, NUEVA_PATA_TALLER_PASSWORD);
    lastWifiRetryMs = now;
  }
  if (now - lastDiagnosticMs >= 1000) {
    Serial.println(statusText());
    Serial.println("---");
    lastDiagnosticMs = now;
  }
  if (!armed || now - lastCommandMs > COMMAND_TIMEOUT_MS) {
    if (armed) Serial.println("WATCHDOG STOP");
    armed = false;
    if (commandedRpm != 0) stopMotor();
    setServoPulse(SERVO_NEUTRAL_US);
    delay(2);
    return;
  }

  // Modo manual abierto para caracterizar el DOCYKE en modo motor.
  // El AS5600 se registra, pero no participa todavía en el mando.
  if (abs(joyX) <= 2) setServoPulse(SERVO_NEUTRAL_US);
  else setServoPulse(uint16_t(map(joyX, -100, 100, SERVO_MIN_US, SERVO_MAX_US)));

  int rpm = joyY * 30 / 100;
  if (abs(rpm) < 8) rpm = 0;
  if (now - lastMotorWriteMs >= 100) {
    commandMotor(rpm);
    lastMotorWriteMs = now;
  }
  delay(2);
}
