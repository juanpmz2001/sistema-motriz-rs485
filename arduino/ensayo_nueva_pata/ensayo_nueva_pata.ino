#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define NUEVA_PATA_WIFI_SSID ""
#define NUEVA_PATA_WIFI_PASSWORD ""
#endif

// Ensayo independiente: una pata tipo Toño.
// Tracción SVD48 por RS485 y dirección externa DOCYKE + AS5600.
constexpr uint8_t RS485_RX_PIN = 16;
constexpr uint8_t RS485_TX_PIN = 17;
constexpr uint8_t STEER_PWM_PIN = 14;
constexpr uint8_t AS5600_OUT_PIN = 4;
constexpr uint8_t DRIVE_ID = 2;
constexpr uint16_t DRIVE_CONTROL_REG = 0x5300;  // M1
constexpr uint16_t DRIVE_SPEED_REG = 0x5304;    // M1

constexpr uint32_t SERVO_HZ = 50;
constexpr uint16_t SERVO_NEUTRAL_US = 1500;
constexpr uint16_t SERVO_MIN_US = 1000;
constexpr uint16_t SERVO_MAX_US = 2000;
constexpr uint32_t COMMAND_TIMEOUT_MS = 450;

WebServer server(80);
HardwareSerial rs485(2);

volatile uint32_t gpio4LastRiseUs = 0;
volatile uint32_t gpio4PeriodUs = 0;
volatile uint32_t gpio4HighUs = 0;
volatile uint32_t gpio4SampleUs = 0;

bool armed = false;
int joyX = 0;
int joyY = 0;
int commandedRpm = 0;
uint16_t servoPulseUs = SERVO_NEUTRAL_US;
uint32_t lastCommandMs = 0;
uint32_t lastMotorWriteMs = 0;
uint32_t lastDiagnosticMs = 0;

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

void IRAM_ATTR gpio4Edge() {
  uint32_t now = micros();
  if (digitalRead(AS5600_OUT_PIN)) {
    if (gpio4LastRiseUs) gpio4PeriodUs = now - gpio4LastRiseUs;
    gpio4LastRiseUs = now;
  } else if (gpio4LastRiseUs) {
    gpio4HighUs = now - gpio4LastRiseUs;
    gpio4SampleUs = now;
  }
}

bool readAngle(float &angle, uint32_t &period, float &duty) {
  // OUTS=00 (valor de fábrica): OUT es analógico y radiométrico respecto a VCC.
  // GPIO4 sí pertenece al ADC del ESP32-S3; GPIO41 no.
  uint16_t raw = analogRead(AS5600_OUT_PIN);
  period = raw;
  duty = 100.0f * raw / 4095.0f;
  angle = 360.0f * raw / 4095.0f;
  return true;
}

String statusText() {
  float angle = 0, duty = 0;
  uint32_t period = 0;
  bool valid = readAngle(angle, period, duty);
  noInterrupts();
  uint32_t pwmPeriod = gpio4PeriodUs;
  uint32_t pwmHigh = gpio4HighUs;
  uint32_t pwmSample = gpio4SampleUs;
  interrupts();
  uint32_t pwmAge = pwmSample ? uint32_t(micros() - pwmSample) : UINT32_MAX;
  bool pwmValid = pwmSample && pwmAge < 250000 && pwmPeriod >= 900 &&
                  pwmPeriod <= 10000 && pwmHigh <= pwmPeriod;
  float pwmDuty = pwmValid ? 100.0f * pwmHigh / pwmPeriod : 0.0f;
  float pwmAngle = pwmValid ? constrain((pwmDuty - 2.9f) / 94.2f, 0.0f, 1.0f) * 360.0f : 0.0f;
  IPAddress activeIp = WiFi.getMode() == WIFI_MODE_AP ? WiFi.softAPIP() : WiFi.localIP();
  String s = "ROBOT:NUEVA_PATA\nFW:NUEVA_PATA_0.1\nIP:" + activeIp.toString();
  s += "\nARMED:" + String(armed ? 1 : 0);
  s += "\nJOY_X:" + String(joyX) + " JOY_Y:" + String(joyY);
  s += "\nMOTOR_RPM:" + String(commandedRpm);
  s += "\nSERVO_US:" + String(servoPulseUs);
  s += "\nAS5600_VALID:" + String(valid ? 1 : 0);
  if (valid) s += " ANGLE:" + String(angle, 1) + " ADC_PCT:" + String(duty, 2) + " ADC_RAW:" + String(period);
  s += "\nGPIO4_PWM_VALID:" + String(pwmValid ? 1 : 0);
  s += " PERIOD_US:" + String(pwmPeriod) + " HIGH_US:" + String(pwmHigh);
  s += " DUTY:" + String(pwmDuty, 2) + " ANGLE:" + String(pwmAngle, 1);
  return s;
}

void setupWeb() {
  server.on("/", [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/status", [] { server.send(200, "text/plain", statusText()); });
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
  rs485.begin(115200, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  pinMode(AS5600_OUT_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(AS5600_OUT_PIN, ADC_11db);
  attachInterrupt(digitalPinToInterrupt(AS5600_OUT_PIN), gpio4Edge, CHANGE);
  if (!ledcAttach(STEER_PWM_PIN, SERVO_HZ, 14)) Serial.println("ERR LEDC");
  setServoPulse(SERVO_NEUTRAL_US);
  stopMotor();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (strlen(NUEVA_PATA_WIFI_SSID)) WiFi.begin(NUEVA_PATA_WIFI_SSID, NUEVA_PATA_WIFI_PASSWORD);
  else WiFi.begin();  // Reutiliza credenciales almacenadas si no hay archivo local.
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("NuevaPata", "nueva-pata");
    Serial.print("AP http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.print("STA http://");
    Serial.println(WiFi.localIP());
  }
  setupWeb();
  Serial.println("READY NUEVA_PATA");
}

void loop() {
  server.handleClient();
  uint32_t now = millis();
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
