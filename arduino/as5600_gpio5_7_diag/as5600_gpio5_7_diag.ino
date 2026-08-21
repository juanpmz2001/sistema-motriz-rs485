#include <Arduino.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "as5600_linearity_lut.h"

#if __has_include("../ensayo_nueva_pata/wifi_secrets.h")
#include "../ensayo_nueva_pata/wifi_secrets.h"
#else
#define NUEVA_PATA_TALLER_SSID ""
#define NUEVA_PATA_TALLER_PASSWORD ""
#endif

// Comparacion aislada direccion-vs-encoder. No inicializa UART ni RS485.
constexpr uint8_t AS5600_ADDRESS = 0x36;
constexpr char NUEVA_PATA_AP_SSID[] = "NuevaPata";
constexpr char NUEVA_PATA_AP_PASSWORD[] = "nueva-pata";
constexpr uint16_t NUEVA_PATA_DNS_PORT = 53;
const IPAddress NUEVA_PATA_AP_IP(192, 168, 4, 1);
const IPAddress NUEVA_PATA_AP_GATEWAY(192, 168, 4, 1);
const IPAddress NUEVA_PATA_AP_SUBNET(255, 255, 255, 0);
constexpr uint8_t AS5600_SDA_PIN = 5;
constexpr uint8_t AS5600_SCL_PIN = 7;
// El AS5600 admite hasta 1 MHz. Se usan 20 kHz para conservar amplio margen
// con el cableado/divisores del banco sin mantener ocupado un nucleo completo.
constexpr uint32_t AS5600_CLOCK_HZ = 20000;
constexpr uint32_t AS5600_HALF_PERIOD_US = 25;
constexpr int64_t AS5600_EDGE_TIMEOUT_US = 5000;
// Cinco lecturas a 20 Hz abarcan 200 ms entre la mas antigua y la mas nueva.
constexpr uint8_t FILTER_SAMPLE_COUNT = 5;
constexpr uint32_t FILTER_CYCLE_MS = 250;
constexpr uint32_t FILTER_SAMPLE_PERIOD_MS =
    FILTER_CYCLE_MS / FILTER_SAMPLE_COUNT;
constexpr uint32_t FILTER_MAX_SPAN_MS = 240;
constexpr float FILTER_OUTLIER_DEG = 5.0f;
constexpr uint8_t FILTER_MIN_INLIER_COUNT = 4;
constexpr float DISPLAY_QUANTUM_DEG = 0.5f;
constexpr float DISPLAY_UPDATE_THRESHOLD_DEG = 0.5f;
// Las rutas historicas de movimiento automatico quedan disponibles para
// detener/consultar, pero no pueden iniciar movimiento en el build joystick.
constexpr bool REMOTE_DIAGNOSTIC_MOTION_ENABLED = false;
constexpr uint8_t SERVO_PWM_PIN = 14;
constexpr uint32_t SERVO_PWM_HZ = 50;
constexpr uint8_t SERVO_PWM_RESOLUTION_BITS = 14;
constexpr uint16_t SERVO_MIN_US = 500;
constexpr uint16_t SERVO_NEUTRAL_US = 1500;
constexpr uint16_t SERVO_MAX_US = 2500;
constexpr uint16_t SERVO_TEST_POSITIVE_US = 2000;
constexpr uint16_t SERVO_TEST_NEGATIVE_US = 1000;
constexpr uint32_t SERVO_SENSOR_TIMEOUT_MS = 400;
constexpr size_t SERVO_TEST_MAX_SAMPLES = 400;
constexpr uint16_t LINEARITY_RETURN_US = 1040;
constexpr int32_t LINEARITY_SEVEN_TURNS_RAW = 7L * 4096L;
constexpr float LINEARITY_MAX_SLOT_DELTA_DEG = 5.0f;
constexpr int32_t LINEARITY_MAX_SLOT_DELTA_RAW =
    int32_t(LINEARITY_MAX_SLOT_DELTA_DEG * 4096.0f / 360.0f);
constexpr uint32_t LINEARITY_PHASE_TIMEOUT_MS = 150000;
constexpr uint8_t LINEARITY_TARGET_CONFIRM_SAMPLES = 3;
// 7800 x 12 bytes = 93.6 kB: cubre 390 s a 20 Hz, incluso ambos
// timeouts de 150 s y las tres pausas, sin reservar Strings gigantes.
constexpr size_t LINEARITY_TEST_MAX_SAMPLES = 7800;
constexpr float POSITION_TOLERANCE_DEG = 3.0f;
constexpr float POSITION_REACQUIRE_DEG = 4.5f;
constexpr float POSITION_MAX_SLOT_DELTA_DEG = 5.0f;
constexpr uint8_t POSITION_STABLE_SAMPLES = 5;
constexpr uint8_t POSITION_REACQUIRE_SAMPLES = 5;
constexpr uint32_t POSITION_DWELL_MS = 10000;
constexpr uint32_t POSITION_MOVE_TIMEOUT_MS = 45000;
constexpr uint32_t POSITION_SEQUENCE_TIMEOUT_MS = 720000;
constexpr uint32_t POSITION_SENSOR_NEUTRAL_MS = 100;
constexpr uint32_t POSITION_REVERSAL_SETTLE_MS = 240;
constexpr uint8_t POSITION_MAX_STEP = 12;
constexpr float POSITION_STEP_DEG = 90.0f;
constexpr uint16_t POSITION_MIN_DRIVE_OFFSET_US = 90;
constexpr float POSITION_FINE_ZONE_DEG = 5.0f;
constexpr uint16_t POSITION_FINE_FORWARD_OFFSET_US = 110;
constexpr uint16_t POSITION_FINE_RETURN_OFFSET_US = 180;
// Control de direccion desde el joystick web. El AS5600 esta montado en la
// rueda final, por lo que los objetivos ya son grados reales de la rueda y no
// se multiplica aqui la reduccion mecanica 15:104.
constexpr float JOYSTICK_MAX_ANGLE_DEG = 90.0f;
// +1: X positivo ordena angulo AS5600 corregido positivo. Cambiar solo este
// signo si la verificacion fisica derecha/izquierda resulta invertida.
constexpr float JOYSTICK_X_DIRECTION = 1.0f;
constexpr int16_t JOYSTICK_DEADZONE = 2;
constexpr uint8_t JOYSTICK_FIXED_SPEED_PCT = 100;
// A 6 grados todavia se usa el pulso maximo calibrado. Solo entre 6 y la
// banda de llegada de 3 grados se interpola hacia el pulso fino.
constexpr float JOYSTICK_SLOW_ZONE_DEG = 6.0f;
// 650 ms tolera un unico POST perdido con heartbeat de 180 ms y reintento a
// 250 ms. Sigue siendo una parada corta frente a la velocidad mecanica medida.
constexpr uint32_t JOYSTICK_COMMAND_TIMEOUT_MS = 650;
constexpr uint32_t JOYSTICK_SENSOR_NEUTRAL_MS = 100;
// A 20 Hz, 500 ms deja reconstruir una ventana coherente despues de dos
// slots perdidos. La salida sigue yendo a neutro mucho antes, a los 100 ms.
constexpr uint32_t JOYSTICK_SENSOR_TIMEOUT_MS = 500;
constexpr uint32_t JOYSTICK_REVERSAL_SETTLE_MS = 240;
constexpr float JOYSTICK_MAX_SLOT_DELTA_DEG = 5.0f;
// Una referencia desplazada no se adopta por una sola lectura. Con el filtro
// a 20 Hz, cinco promedios coherentes representan aproximadamente 250 ms.
constexpr uint8_t JOYSTICK_REFERENCE_CONFIRM_SAMPLES = 5;
constexpr float JOYSTICK_REFERENCE_CLUSTER_DEG = 2.0f;
constexpr uint8_t JOYSTICK_ARMED_RETURN_SAMPLES = 2;
constexpr uint8_t JOYSTICK_ARMED_GLITCH_TRIP_SAMPLES = 3;
static_assert(FILTER_CYCLE_MS % FILTER_SAMPLE_COUNT == 0,
              "Filter cycle must divide into exact sample slots");
static_assert(FILTER_MIN_INLIER_COUNT <= FILTER_SAMPLE_COUNT,
              "Filter inliers cannot exceed the window");
static_assert(FILTER_MAX_SPAN_MS >=
                  (FILTER_SAMPLE_COUNT - 1U) * FILTER_SAMPLE_PERIOD_MS,
              "Filter span must hold one complete nominal window");
static_assert(SERVO_TEST_MAX_SAMPLES >= 375,
              "Servo test log must cover the complete 14 second test");
static_assert(LINEARITY_TEST_MAX_SAMPLES >= 6500,
              "Linearity log must hold at least 6500 slots");
static_assert(JOYSTICK_SLOW_ZONE_DEG > POSITION_TOLERANCE_DEG,
              "Joystick slow zone must exceed arrival tolerance");

enum ServoTestPhase : uint8_t {
  SERVO_TEST_IDLE = 0,
  SERVO_TEST_NEUTRAL_START = 1,
  SERVO_TEST_2000_US = 2,
  SERVO_TEST_NEUTRAL_MIDDLE = 3,
  SERVO_TEST_1000_US = 4,
  SERVO_TEST_NEUTRAL_END = 5,
  SERVO_TEST_COMPLETE = 6,
  SERVO_TEST_STOPPED = 7,
  SERVO_TEST_SENSOR_TIMEOUT = 8,
  SERVO_TEST_OTA_ABORTED = 9,
};

struct ServoTestState {
  bool active;
  ServoTestPhase phase;
  uint16_t pulseUs;
  uint32_t startedMs;
  uint32_t phaseStartedMs;
  uint32_t completedMs;
};

struct ServoTestSample {
  uint32_t elapsedMs;
  uint16_t rawAngle;
  uint16_t servoUs;
  uint8_t phase;
  uint8_t valid;
};

enum LinearityTestPhase : uint8_t {
  LINEARITY_IDLE = 0,
  LINEARITY_NEUTRAL_START = 1,
  LINEARITY_LEFT_2000_US = 2,
  LINEARITY_NEUTRAL_MIDDLE = 3,
  LINEARITY_RIGHT_1040_US = 4,
  LINEARITY_NEUTRAL_END = 5,
  LINEARITY_COMPLETE = 6,
  LINEARITY_STOPPED = 7,
  LINEARITY_SENSOR_TIMEOUT = 8,
  LINEARITY_PHASE_TIMEOUT = 9,
  LINEARITY_OTA_ABORTED = 10,
};

struct LinearityTestState {
  bool active;
  LinearityTestPhase phase;
  uint16_t pulseUs;
  uint32_t startedMs;
  uint32_t phaseStartedMs;
  uint32_t completedMs;
  int32_t unwrappedRaw;
  int32_t phaseOriginRaw;
  uint16_t lastAcceptedRaw;
  bool unwrapReady;
  uint8_t targetConfirmSamples;
  uint32_t lastAcceptedMs;
  uint32_t glitches;
  uint32_t acceptedSamples;
  uint32_t invalidSamples;
};

enum LinearitySampleFlags : uint8_t {
  LINEARITY_SAMPLE_I2C_VALID = 1U << 0,
  LINEARITY_SAMPLE_ACCEPTED = 1U << 1,
  LINEARITY_SAMPLE_GLITCH = 1U << 2,
};

// El pulso se reconstruye desde phase al exportar; asi cada fila ocupa 12 B.
struct LinearityTestSample {
  uint32_t elapsedMs;
  int32_t unwrappedRaw;
  uint16_t rawAngle;
  uint8_t phase;
  uint8_t flags;
};
static_assert(sizeof(LinearityTestSample) == 12,
              "Keep the long-test sample compact");

enum PositionSequencePhase : uint8_t {
  POSITION_IDLE = 0,
  POSITION_HOME_MOVE = 1,
  POSITION_HOME_DWELL = 2,
  POSITION_FORWARD_MOVE = 3,
  POSITION_FORWARD_DWELL = 4,
  POSITION_RETURN_MOVE = 5,
  POSITION_RETURN_DWELL = 6,
  POSITION_COMPLETE = 7,
  POSITION_STOPPED = 8,
  POSITION_SENSOR_TIMEOUT = 9,
  POSITION_MOVE_TIMEOUT = 10,
  POSITION_OTA_ABORTED = 11,
  POSITION_SEQUENCE_TIMEOUT = 12,
};

struct PositionSequenceState {
  bool active;
  PositionSequencePhase phase;
  uint16_t pulseUs;
  uint8_t targetStep;
  uint8_t stableSamples;
  uint8_t reacquireSamples;
  bool unwrapReady;
  uint32_t startedMs;
  uint32_t phaseStartedMs;
  uint32_t completedMs;
  uint32_t lastAcceptedMs;
  uint32_t acceptedSamples;
  uint32_t invalidSamples;
  uint32_t glitches;
  uint32_t transitions;
  uint32_t driveInhibitUntilMs;
  float lastCorrectedCyclicDeg;
  float unwrappedCorrectedDeg;
  float homeAbsoluteDeg;
  float targetAbsoluteDeg;
  float logicalPositionDeg;
  float errorDeg;
};

enum JoystickSteeringMode : uint8_t {
  JOYSTICK_DISARMED = 0,
  JOYSTICK_ACTIVE = 1,
  JOYSTICK_AT_TARGET = 2,
  JOYSTICK_SPEED_ZERO = 3,
  JOYSTICK_REVERSAL_SETTLE = 4,
  JOYSTICK_SENSOR_STALE = 5,
  JOYSTICK_SENSOR_FAULT = 6,
  JOYSTICK_WATCHDOG = 7,
  JOYSTICK_OTA_ABORTED = 8,
  JOYSTICK_MANUAL_STOP = 9,
};

struct JoystickSteeringState {
  bool armed;
  bool armReleaseRequired;
  bool sensorAlarmsEnabled;
  bool referenceRecoveryActive;
  JoystickSteeringMode mode;
  JoystickSteeringMode lastTripReason;
  int16_t x;
  uint8_t speedPct;
  uint16_t pulseUs;
  uint8_t stableSamples;
  uint8_t reacquireSamples;
  uint8_t referenceCandidateSamples;
  uint8_t armedOutlierSamples;
  uint8_t armedReturnSamples;
  bool unwrapReady;
  uint32_t lastCommandMs;
  uint32_t lastAcceptedMs;
  uint32_t driveInhibitUntilMs;
  uint32_t acceptedSamples;
  uint32_t invalidSamples;
  uint32_t glitches;
  uint32_t safetyTrips;
  uint32_t referenceRebases;
  uint32_t ownerIpKey;
  uint32_t ownerSessionKey;
  float lastCorrectedCyclicDeg;
  float referenceCandidateAnchorDeg;
  float armedReturnAnchorDeg;
  float unwrappedCorrectedDeg;
  float zeroAbsoluteDeg;
  float logicalAngleDeg;
  float targetDeg;
  float targetAbsoluteDeg;
  float errorDeg;
};

enum SoftI2cError : uint16_t {
  I2C_OK = 0,
  I2C_ERROR_ARGUMENT = 100,
  I2C_ERROR_GPIO_CONFIG = 101,
  I2C_ERROR_GPIO_ACCESS = 102,
  I2C_ERROR_SCL_IDLE_TIMEOUT = 110,
  I2C_ERROR_SCL_EDGE_TIMEOUT = 111,
  I2C_ERROR_SDA_STUCK = 112,
  I2C_ERROR_START = 120,
  I2C_ERROR_ADDRESS_WRITE_NACK = 121,
  I2C_ERROR_REGISTER_NACK = 122,
  I2C_ERROR_REPEATED_START = 123,
  I2C_ERROR_ADDRESS_READ_NACK = 124,
  I2C_ERROR_READ = 125,
  I2C_ERROR_STOP = 126,
};

struct As5600Snapshot {
  bool busStarted;
  bool valid;
  bool diagnosticsValid;
  uint16_t rawAngle;
  float angleDeg;
  float correctedAngleDeg;
  float meanAngleDeg;
  float displayAngleDeg;
  float filterDeltaDeg;
  bool filterReady;
  uint8_t filterSamples;
  uint8_t filterInliers;
  uint32_t filterSpanMs;
  uint32_t displayUpdates;
  uint8_t status;
  uint8_t agc;
  uint16_t magnitude;
  uint16_t lastError;
  uint32_t samples;
  uint32_t successes;
  uint32_t errors;
  uint32_t scheduleOverruns;
  uint32_t lastSampleMs;
  uint32_t lastGoodMs;
};

WebServer server(80);
DNSServer dnsServer;
bool softApReady = false;
bool captiveDnsReady = false;
portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
As5600Snapshot snapshot = {};
volatile bool otaInProgress = false;
SemaphoreHandle_t servoStateMutex = nullptr;
portMUX_TYPE servoLogMux = portMUX_INITIALIZER_UNLOCKED;
ServoTestState servoTest = {false, SERVO_TEST_IDLE, SERVO_NEUTRAL_US, 0, 0,
                            0};
ServoTestSample servoTestLog[SERVO_TEST_MAX_SAMPLES] = {};
size_t servoTestLogCount = 0;
uint32_t servoTestLogDropped = 0;
portMUX_TYPE linearityLogMux = portMUX_INITIALIZER_UNLOCKED;
LinearityTestState linearityTest = {};
LinearityTestSample linearityTestLog[LINEARITY_TEST_MAX_SAMPLES] = {};
size_t linearityTestLogCount = 0;
uint32_t linearityTestLogDropped = 0;
PositionSequenceState positionSequence = {};
JoystickSteeringState joystickSteering = {};
bool servoPwmReady = false;

const char DIAGNOSTIC_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="es"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ángulo AS5600</title><style>
:root{color-scheme:dark;--bg:#06101d;--card:#0e2237;--cyan:#38d8ff;--ok:#65efa1;--bad:#ff657d;--warn:#ffd166;--grid:#29465c}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -12%,#174868,var(--bg) 58%);color:#effaff;font-family:Segoe UI,Arial,sans-serif}
header{padding:16px 21px;border-bottom:1px solid #ffffff18;display:flex;justify-content:space-between;gap:12px;align-items:center}h1{margin:0;font-size:clamp(21px,5vw,33px)}
main{max-width:1060px;margin:auto;padding:16px;display:grid;grid-template-columns:minmax(320px,1.08fr) minmax(300px,.92fr);gap:15px}.card{background:#0e2237ed;border:1px solid #ffffff18;border-radius:18px;padding:15px;box-shadow:0 18px 46px #0007}
.dial{position:relative;max-width:480px;aspect-ratio:1;margin:auto}.dial svg{width:100%;height:100%;display:block}.face{fill:#061321;stroke:#35546b;stroke-width:4}.ring{fill:none;stroke:#17364c;stroke-width:18}.tick{stroke:#7596aa;stroke-width:3}.major{stroke:#d7f5ff;stroke-width:5}.needle{stroke:var(--cyan);stroke-width:7;stroke-linecap:round;filter:drop-shadow(0 0 7px #38d8ff)}.hub{fill:#eefaff;stroke:#38d8ff;stroke-width:5}.degree{fill:#9bb9ca;font:700 15px Consolas,monospace;text-anchor:middle}.center{position:absolute;left:50%;top:57%;transform:translate(-50%,-50%);text-align:center;pointer-events:none}.angle{font:800 clamp(45px,9vw,80px)/1 Consolas,monospace;color:var(--cyan);text-shadow:0 0 18px #38d8ff55;white-space:nowrap}.unit{font-size:.38em;color:#9bb9ca}.filter{font:700 12px Consolas,monospace;color:var(--warn);margin-top:8px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:9px}.stat{background:#071522;border-radius:11px;padding:11px;border:1px solid #ffffff12}.label{font-size:10px;letter-spacing:.12em;text-transform:uppercase;color:#8daec5}.value{font:700 19px/1.3 Consolas,monospace;margin-top:4px}.wide{grid-column:1/-1}.tag{font:700 13px Consolas,monospace;color:var(--warn)}.ok{color:var(--ok)}.bad{color:var(--bad)}.controls{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}.controls button,.controls a{border:0;border-radius:9px;padding:10px 12px;font:700 13px Segoe UI,Arial;cursor:pointer;text-decoration:none;color:#06101d;background:var(--cyan)}.controls .stop{background:var(--bad);color:white}.controls .log{background:#9bb9ca}
@media(max-width:760px){main{grid-template-columns:1fr;padding:8px}.card{padding:10px}.dial{max-width:390px}}
</style></head><body><header><div><h1>Ángulo AS5600 · Nueva Pata</h1><div class="label">LUT de linealidad aplicada · últimas 5 muestras / 200 ms · resolución visual 0.5°</div></div><div id="link" class="tag">CONECTANDO</div></header>
<main><section class="card"><div class="dial"><svg viewBox="0 0 300 300" aria-label="Dial angular"><circle class="face" cx="150" cy="150" r="133"/><circle class="ring" cx="150" cy="150" r="116"/>
<line class="tick major" x1="150" y1="20" x2="150" y2="43"/><line class="tick major" x1="280" y1="150" x2="257" y2="150"/><line class="tick major" x1="150" y1="280" x2="150" y2="257"/><line class="tick major" x1="20" y1="150" x2="43" y2="150"/>
<line class="tick" x1="215" y1="37" x2="204" y2="56"/><line class="tick" x1="263" y1="85" x2="244" y2="96"/><line class="tick" x1="263" y1="215" x2="244" y2="204"/><line class="tick" x1="215" y1="263" x2="204" y2="244"/><line class="tick" x1="85" y1="263" x2="96" y2="244"/><line class="tick" x1="37" y1="215" x2="56" y2="204"/><line class="tick" x1="37" y1="85" x2="56" y2="96"/><line class="tick" x1="85" y1="37" x2="96" y2="56"/>
<text class="degree" x="150" y="15">0°</text><text class="degree" x="286" y="155">90°</text><text class="degree" x="150" y="298">180°</text><text class="degree" x="14" y="155">270°</text>
<g id="needle" transform="rotate(0 150 150)"><line class="needle" x1="150" y1="160" x2="150" y2="48"/></g><circle class="hub" cx="150" cy="150" r="10"/></svg>
<div class="center"><div id="angle" class="angle">---<span class="unit">°</span></div><div id="filter" class="filter">CORREGIDO · FILTRO 0/5</div></div></div>
<div class="grid"><div class="stat"><div class="label">Raw código</div><div id="raw" class="value">----</div></div><div class="stat"><div class="label">Raw ángulo · sin LUT</div><div id="rawangle" class="value">---°</div></div><div class="stat"><div class="label">Ángulo corregido · LUT</div><div id="correctedangle" class="value">---°</div></div><div class="stat"><div class="label">Promedio circular corregido</div><div id="mean" class="value">---°</div></div><div class="stat"><div class="label">Delta circular</div><div id="delta" class="value">---°</div></div><div class="stat"><div class="label">Actualizaciones</div><div id="updates" class="value">0</div></div><div class="stat"><div class="label">Posición raw</div><div id="pct" class="value">--%</div></div><div class="stat wide"><div class="label">Imán</div><div id="magnet" class="value">ESPERANDO</div></div></div></section>
<section class="card"><div class="grid"><div class="stat"><div class="label">Bus</div><div id="bus" class="value">---</div></div><div class="stat"><div class="label">Último error</div><div id="error" class="value">---</div></div><div class="stat"><div class="label">Intentos I2C</div><div id="samples" class="value">0</div></div><div class="stat"><div class="label">Lecturas OK</div><div id="success" class="value">0</div></div><div class="stat"><div class="label">Errores</div><div id="errors" class="value">0</div></div><div class="stat"><div class="label">Atrasos 50 ms</div><div id="overruns" class="value">0</div></div><div class="stat"><div class="label">Diagnóstico inicial</div><div id="diag" class="value">---</div></div><div class="stat"><div class="label">AGC inicial</div><div id="agc" class="value">---</div></div><div class="stat"><div class="label">Magnitud inicial</div><div id="magnitude" class="value">---</div></div><div class="stat wide"><div class="label">Líneas en reposo</div><div id="lines" class="value">SDA? SCL?</div></div><div class="stat wide"><div class="label">Prueba servo vs encoder</div><div id="servo" class="value">NEUTRO 1500 us</div><div id="servotime" class="tag">IDLE</div><div class="controls"><button onclick="startTest()">Iniciar prueba 14 s</button><button class="stop" onclick="stopTest()">Parar / neutro</button><a class="log" href="/servo/test/log.csv">Descargar CSV</a></div></div><div class="stat wide"><div class="label">Linealidad · 7 vueltas por sentido</div><div id="linearity" class="value">IDLE · 1500 us</div><div id="linearitytime" class="tag">0.0° · 0% · 0 muestras</div><div class="controls"><button onclick="startLinearity()">Iniciar prueba larga</button><button class="stop" onclick="stopLinearity()">Parar / neutro</button><a class="log" href="/linearity/test/log.csv">Descargar CSV</a></div></div><div class="stat wide"><div class="label">Secuencia de posición corregida · 0 → 1080 → 0</div><div id="position" class="value">IDLE · 1500 us</div><div id="positiontime" class="tag">actual 0.0° · objetivo 0.0° · error 0.0°</div><div class="controls"><button onclick="startPosition()">Iniciar secuencia</button><button class="stop" onclick="stopPosition()">Parar / neutro</button></div></div></div></section></main>
<script>
const q=id=>document.getElementById(id);let fails=0;const err={0:'OK',100:'ARGUMENTO',101:'CONFIG GPIO',102:'ACCESO GPIO',110:'SCL BAJA',111:'TIMEOUT SCL',112:'SDA BAJA',120:'FALLO START',121:'NACK DIR-W',122:'NACK REG',123:'FALLO RESTART',124:'NACK DIR-R',125:'FALLO LECTURA',126:'FALLO STOP'};
function setDial(angle){q('needle').setAttribute('transform',`rotate(${angle} 150 150)`)}
async function postServo(path){try{const r=await fetch(path,{method:'POST'}),t=await r.text();if(!r.ok)alert(t)}catch(e){alert('Sin comunicacion con el ESP')}}
function startTest(){if(confirm('La direccion se movera a 2000 us y luego a 1000 us. Continuar?'))postServo('/servo/test/start')}
function stopTest(){postServo('/servo/stop')}
function startLinearity(){if(confirm('Prueba larga: 7 vueltas a 2000 us y 7 vueltas de regreso a 1040 us. Mantenga la pata elevada y libre. Continuar?'))postServo('/linearity/test/start')}
function stopLinearity(){postServo('/linearity/test/stop')}
async function pollLinearity(){try{const r=await fetch('/linearity/status.json?x='+Date.now(),{cache:'no-store'}),d=await r.json();q('linearity').textContent=`${d.phase} · ${d.servo_us} us`;q('linearitytime').textContent=`${Number(d.phase_displacement_deg).toFixed(1)}° · ${Number(d.progress_pct).toFixed(1)}% · ${d.log_samples} muestras · ${d.glitches} glitches`}catch(e){}finally{setTimeout(pollLinearity,250)}}pollLinearity();
function startPosition(){if(confirm('Secuencia cerrada: home corregido, 0 a 1080 grados en pasos de 90 y regreso a 0, con pausas de 10 s. Mantenga la pata elevada y libre. Continuar?'))postServo('/position/sequence/start')}
function stopPosition(){postServo('/position/sequence/stop')}
async function pollPosition(){try{const r=await fetch('/position/status.json?x='+Date.now(),{cache:'no-store'}),d=await r.json();q('position').textContent=`${d.phase} · ${d.servo_us} us · paso ${d.target_step}/12`;q('positiontime').textContent=`actual ${Number(d.logical_deg).toFixed(1)}° · objetivo ${Number(d.target_deg).toFixed(1)}° · error ${Number(d.error_deg).toFixed(2)}° · pausa ${(Number(d.dwell_remaining_ms)/1000).toFixed(1)} s`}catch(e){}finally{setTimeout(pollPosition,250)}}pollPosition();
async function poll(){const ctl=new AbortController(),timer=setTimeout(()=>ctl.abort(),900);try{const r=await fetch('/telemetry.json?x='+Date.now(),{cache:'no-store',signal:ctl.signal});if(!r.ok)throw Error('HTTP');const d=await r.json();
q('bus').textContent=d.started?`0x36 @ ${Number(d.i2c_hz||20000)/1000}k SW`:'NO INICIADO';q('error').textContent=err[d.last_error]||'COD '+d.last_error;q('samples').textContent=d.samples;q('success').textContent=d.successes;q('errors').textContent=d.errors;q('overruns').textContent=d.schedule_overruns;q('lines').textContent=`SDA5=${d.sda_level} · SCL7=${d.scl_level}`;q('raw').textContent=d.raw;q('rawangle').textContent=Number(d.raw_angle).toFixed(2)+'°';q('correctedangle').textContent=Number(d.corrected_angle).toFixed(2)+'°';q('pct').textContent=Number(d.pct).toFixed(2)+'%';q('mean').textContent=d.filter_ready?Number(d.mean_angle).toFixed(2)+'°':'---°';q('delta').textContent=d.filter_ready?Number(d.filter_delta).toFixed(2)+'°':'---°';q('updates').textContent=d.display_updates;q('filter').textContent=`CORREGIDO · ${d.filter_samples}/${d.filter_window||5} · INLIERS ${d.filter_inliers||0} · ${d.filter_span_ms||0} ms${d.filter_ready?' · LISTO':''}`;
if(d.filter_ready){q('angle').innerHTML=`${Number(d.angle).toFixed(1)}<span class="unit">°</span>`;setDial(Number(d.angle))}else{q('angle').innerHTML='---<span class="unit">°</span>';setDial(0)}
q('diag').textContent=d.diag?'VÁLIDO':'ESPERANDO';q('agc').textContent=d.diag?d.agc:'---';q('magnitude').textContent=d.diag?d.magnitude:'---';let m='NO DETECTADO',c='bad';if(d.ml){m='IMÁN MUY DÉBIL'}else if(d.mh){m='IMÁN MUY FUERTE'}else if(d.md){m='IMÁN DETECTADO';c='ok'}q('magnet').textContent=m;q('magnet').className='value '+c;
q('servo').textContent=`${d.servo_us} us${d.servo_test_active?' · EN MOVIMIENTO':' · NEUTRO'}`;q('servotime').textContent=`${d.servo_phase} · ${(Number(d.servo_elapsed_ms)/1000).toFixed(1)} s · ${d.servo_log_samples} muestras`;
if(d.valid){q('link').textContent=d.filter_ready?'EN LÍNEA · CORREGIDO + FILTRADO':'EN LÍNEA · LLENANDO FILTRO';q('link').className='tag ok'}else{q('link').textContent='EN LÍNEA · ERROR I2C';q('link').className='tag bad'}fails=0}catch(e){if(++fails>2){q('link').textContent='SIN CONEXIÓN';q('link').className='tag bad'}}finally{clearTimeout(timer);setTimeout(poll,160)}}poll();
</script></body></html>
)HTML";

const char TELEMETRY_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#06111f">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>Direcci&oacute;n &middot; Nueva Pata</title>
<style>
:root{color-scheme:dark;--bg:#06111f;--cyan:#38d8ff;--yellow:#ffd166;--green:#60eca0;--red:#ff617a;--muted:#8facbf;--axis-y:55%}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;width:100%;height:100%;overflow:hidden;overscroll-behavior:none;background:var(--bg);color:#edfaff;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Arial,sans-serif}
body{position:fixed;inset:0;padding:0;user-select:none;-webkit-user-select:none;background:radial-gradient(circle at 50% 38%,#164762 0,#091d2f 36%,var(--bg) 78%)}
button{font:inherit;-webkit-appearance:none}.app{position:fixed;inset:0;width:100%;max-width:none;height:100vh;height:100dvh;margin:0;padding:0;display:block;overflow:hidden;isolation:isolate}
header{position:absolute;z-index:3;top:calc(env(safe-area-inset-top) + 7px);left:calc(env(safe-area-inset-left) + 10px);right:calc(env(safe-area-inset-right) + 10px);display:flex;align-items:center;justify-content:space-between;gap:8px;pointer-events:none}
h1{font-size:clamp(17px,4.8vw,24px);margin:0;line-height:1}.sub{font-size:9px;color:var(--muted);letter-spacing:.08em;text-transform:uppercase;margin-top:4px}.badge{flex:0 0 auto;padding:7px 8px;border:1px solid #ffffff25;border-radius:999px;font:800 9px/1 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--yellow);background:#06111fba;backdrop-filter:blur(7px);-webkit-backdrop-filter:blur(7px)}.badge.ok{color:var(--green);border-color:#60eca055}.badge.bad{color:var(--red);border-color:#ff617a55}
.card{background:#071827a8;border:1px solid #ffffff18;border-radius:15px;box-shadow:0 10px 32px #0005;backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px)}
.control-card{position:absolute;inset:0;padding:0;border:0;border-radius:0;background:none;box-shadow:none;backdrop-filter:none;-webkit-backdrop-filter:none;pointer-events:none}.control-title,.safety{display:none}
.pad{position:absolute;inset:0;z-index:0;width:100%;height:100%;margin:0;border:0;border-radius:0;background:radial-gradient(circle at 50% var(--axis-y),#123b55 0,#081b2b 32%,#06111f 77%);box-shadow:none;touch-action:none;cursor:default;outline:none;pointer-events:auto}.pad:focus-visible{box-shadow:inset 0 0 0 2px #38d8ff55}.pad.dragging{cursor:grabbing}.axis{position:absolute;left:calc(env(safe-area-inset-left) + 42px);right:calc(env(safe-area-inset-right) + 42px);top:var(--axis-y);height:5px;transform:translateY(-50%);background:linear-gradient(90deg,var(--red),#6d8290 50%,var(--green));border-radius:5px;box-shadow:0 0 18px #38d8ff32}.zero{position:absolute;left:50%;top:calc(var(--axis-y) - 76px);width:2px;height:152px;background:#ffffff38}.endmark{position:absolute;top:calc(var(--axis-y) - 46px);font:900 12px ui-monospace,SFMono-Regular,Consolas,monospace;color:#b6cbd7}.endmark.left{left:calc(env(safe-area-inset-left) + 14px)}.endmark.right{right:calc(env(safe-area-inset-right) + 14px)}.knob{position:absolute;left:50%;top:var(--axis-y);width:clamp(88px,23vw,112px);height:clamp(88px,23vw,112px);border-radius:50%;transform:translate(-50%,-50%);background:radial-gradient(circle at 36% 30%,#fff,#bdefff 38%,#38bde4 72%,#147898);border:4px solid #eaffff;box-shadow:0 10px 25px #000a,0 0 28px #38d8ff55;display:grid;place-items:center;color:#082033;font:900 14px ui-monospace,SFMono-Regular,Consolas,monospace;will-change:left,transform}.pad.locked .axis,.pad.locked .zero,.pad.locked .endmark,.pad.locked .knob{opacity:.34}.pad.locked .knob{box-shadow:0 8px 20px #0009}
.setpoint{position:absolute;z-index:4;left:50%;top:calc(var(--axis-y) + 62px);transform:translateX(-50%);margin:0;padding:6px 9px;border-radius:999px;background:#06111fc7;color:var(--yellow);font:900 13px ui-monospace,SFMono-Regular,Consolas,monospace;text-align:center;white-space:nowrap;pointer-events:none}.fixed-speed{position:absolute;z-index:4;left:50%;top:calc(var(--axis-y) + 97px);transform:translateX(-50%);width:max-content;max-width:70vw;padding:5px 8px;border:1px solid #38d8ff3d;border-radius:999px;background:#061522bd;color:var(--cyan);font:900 9px ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:.05em;text-align:center;pointer-events:none}
.buttons{position:absolute;z-index:5;right:calc(env(safe-area-inset-right) + 10px);bottom:calc(env(safe-area-inset-bottom) + 10px);width:138px;display:grid;grid-template-columns:1fr;gap:8px;margin:0;pointer-events:auto}.btn{min-height:55px;border:0;border-radius:13px;font-weight:900;letter-spacing:.05em;color:#fff;box-shadow:0 7px 20px #0006}.arm{background:#16745c}.arm.on{background:var(--green);color:#042014;box-shadow:0 0 20px #60eca044}.stop{background:var(--red);box-shadow:0 6px 17px #ff617a42}.btn:active{transform:scale(.97)}.sensor-switch{grid-column:1/-1;min-height:42px;border:1px solid #60eca066;border-radius:12px;padding:6px 8px;background:#0b4338e8;color:var(--green);font:900 9px/1.15 ui-monospace,SFMono-Regular,Consolas,monospace;letter-spacing:.04em;box-shadow:0 6px 18px #0005}.sensor-switch.off{border-color:#ffd16699;background:#5b4316ed;color:var(--yellow)}.sensor-switch:disabled{opacity:.48}.sensor-switch:active:not(:disabled){transform:scale(.97)}
.gauge-card,.chart-card{position:absolute;z-index:3;padding:7px;pointer-events:none}.gauge-card *,.chart-card *{pointer-events:none}.gauge-card{top:calc(env(safe-area-inset-top) + 50px);left:calc(env(safe-area-inset-left) + 8px);width:min(61vw,260px)}.gauge{position:relative;margin:auto;width:100%;aspect-ratio:320/205}.gauge svg{display:block;width:100%;height:100%}.arc{fill:none;stroke:#1a394e;stroke-width:20;stroke-linecap:round}.arc-live{fill:none;stroke:url(#arcGradient);stroke-width:7;stroke-linecap:round}.tick{stroke:#6c8ea3;stroke-width:2}.tick.major{stroke:#dff8ff;stroke-width:4}.gauge-label{fill:#99b6c7;font:700 13px ui-monospace,SFMono-Regular,Consolas,monospace;text-anchor:middle}.needle-actual{stroke:var(--cyan);stroke-width:7;stroke-linecap:round;filter:drop-shadow(0 0 6px #38d8ffaa)}.needle-target{stroke:var(--yellow);stroke-width:4;stroke-linecap:round;stroke-dasharray:7 6}.hub{fill:#e9fbff;stroke:var(--cyan);stroke-width:4}.readout{position:absolute;left:50%;bottom:0;transform:translateX(-50%);text-align:center;white-space:nowrap}.actual{font:900 clamp(31px,10vw,54px)/.92 ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--cyan);text-shadow:0 0 16px #38d8ff55}.actual small{font-size:.35em;color:var(--muted)}.target{margin-top:4px;color:var(--yellow);font:800 10px ui-monospace,SFMono-Regular,Consolas,monospace}
.statebar{display:grid;grid-template-columns:repeat(3,1fr);gap:4px;margin-top:4px}.mini{min-width:0;background:#061522b8;border:1px solid #ffffff12;border-radius:8px;padding:5px 3px;text-align:center}.mini-label{font-size:7px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}.mini-value{font:800 10px/1.2 ui-monospace,SFMono-Regular,Consolas,monospace;margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.mini-value.warn{color:var(--yellow)}
.chart-card{left:calc(env(safe-area-inset-left) + 8px);bottom:calc(env(safe-area-inset-bottom) + 9px);width:min(57vw,250px)}.chart-head{display:flex;justify-content:space-between;gap:5px;align-items:center;margin-bottom:4px;font-size:10px}.legend{font:700 7px ui-monospace,SFMono-Regular,Consolas,monospace;color:var(--muted)}.dot{display:inline-block;width:6px;height:6px;border-radius:50%;margin:0 2px 0 5px}.dot:first-child{margin-left:0}.dot.actual-dot{background:var(--cyan)}.dot.target-dot{background:var(--yellow)}canvas{display:block;width:100%;height:62px;background:#06152280;border:1px solid #ffffff12;border-radius:8px}.fault{display:none;margin-top:5px;padding:6px 7px;border-radius:8px;background:#5e1b2ad9;color:#fff;font-size:9px;font-weight:800;line-height:1.25}.fault.show{display:block}.fault.notice{background:#5b4316e6;color:#fff4cf}.diaglink{display:none}
@media(max-height:700px) and (orientation:portrait){:root{--axis-y:53%}.gauge-card{width:min(55vw,225px)}.chart-card{width:min(55vw,225px)}canvas{height:48px}.actual{font-size:30px}.fixed-speed{top:calc(var(--axis-y) + 92px)}}
@media(orientation:landscape){:root{--axis-y:54%}header{top:calc(env(safe-area-inset-top) + 5px)}.gauge-card{top:calc(env(safe-area-inset-top) + 42px);width:min(32vw,270px)}.chart-card{top:calc(env(safe-area-inset-top) + 45px);right:calc(env(safe-area-inset-right) + 8px);left:auto;bottom:auto;width:min(31vw,270px)}canvas{height:56px}.buttons{width:min(30vw,270px);grid-template-columns:1fr 1fr}.btn{min-height:50px}.setpoint{top:calc(var(--axis-y) + 57px)}.fixed-speed{top:calc(var(--axis-y) + 88px)}}
</style>
</head>
<body>
<main class="app">
<header><div><h1>Direcci&oacute;n &middot; Nueva Pata</h1><div class="sub">Deslice en toda la pantalla &middot; suelte para 0&deg;</div></div><div id="link" class="badge" role="status" aria-live="polite">CONECTANDO</div></header>

<section class="card gauge-card">
  <div class="gauge" aria-label="&Aacute;ngulo real de la rueda">
    <svg viewBox="0 0 320 205" role="img">
      <defs><linearGradient id="arcGradient"><stop offset="0" stop-color="#ff617a"/><stop offset=".5" stop-color="#38d8ff"/><stop offset="1" stop-color="#60eca0"/></linearGradient></defs>
      <path class="arc" d="M40 160 A120 120 0 0 1 280 160"/><path class="arc-live" d="M40 160 A120 120 0 0 1 280 160"/>
      <g transform="translate(160 160)">
        <line class="tick major" x1="-120" y1="0" x2="-101" y2="0"/><line class="tick" x1="-111" y1="-46" x2="-96" y2="-40"/><line class="tick major" x1="-85" y1="-85" x2="-72" y2="-72"/><line class="tick" x1="-46" y1="-111" x2="-40" y2="-96"/><line class="tick major" x1="0" y1="-120" x2="0" y2="-100"/><line class="tick" x1="46" y1="-111" x2="40" y2="-96"/><line class="tick major" x1="85" y1="-85" x2="72" y2="-72"/><line class="tick" x1="111" y1="-46" x2="96" y2="-40"/><line class="tick major" x1="120" y1="0" x2="101" y2="0"/>
      </g>
      <text class="gauge-label" x="36" y="185">-90&deg;</text><text class="gauge-label" x="160" y="25">0&deg;</text><text class="gauge-label" x="284" y="185">+90&deg;</text>
      <g id="targetNeedle" transform="rotate(0 160 160)"><line class="needle-target" x1="160" y1="151" x2="160" y2="58"/></g>
      <g id="actualNeedle" transform="rotate(0 160 160)"><line class="needle-actual" x1="160" y1="153" x2="160" y2="48"/></g><circle class="hub" cx="160" cy="160" r="10"/>
    </svg>
    <div class="readout"><div id="actualAngle" class="actual">---<small>&deg;</small></div><div id="targetAngle" class="target">OBJETIVO 0.0&deg;</div></div>
  </div>
  <div class="statebar">
    <div class="mini"><div class="mini-label">Estado</div><div id="state" class="mini-value">ESPERA</div></div>
    <div class="mini"><div class="mini-label">Error</div><div id="error" class="mini-value">---&deg;</div></div>
    <div class="mini"><div class="mini-label">PWM GPIO14</div><div id="servo" class="mini-value">1500 us</div></div>
  </div>
  <div id="fault" class="fault" role="alert"></div>
</section>

<section class="card control-card">
  <div class="control-title"><strong>Control eje X</strong><span class="hint">Suelte para volver a 0&deg;</span></div>
  <div id="pad" class="pad locked" role="slider" tabindex="0" aria-label="Objetivo de direcci&oacute;n" aria-orientation="horizontal" aria-valuemin="-90" aria-valuemax="90" aria-valuenow="0">
    <div class="axis"></div><div class="zero"></div><span class="endmark left">-90&deg;</span><span class="endmark right">+90&deg;</span><div id="knob" class="knob"><span id="knobValue">0&deg;</span></div>
  </div>
  <div id="setpoint" class="setpoint">X 0% &middot; OBJETIVO 0.0&deg;</div>
  <div class="fixed-speed">VELOCIDAD FIJA &middot; M&Aacute;XIMA CALIBRADA</div>
  <div class="buttons"><button id="arm" class="btn arm" type="button">ARMAR</button><button id="stop" class="btn stop" type="button">PARAR</button><button id="sensorAlarms" class="sensor-switch" type="button" role="switch" aria-checked="true">ALARMAS AS5600 &middot; ON</button></div>
  <div class="safety"><b>La rueda sigue el objetivo realimentado por el AS5600.</b><br>PARAR desarma y lleva GPIO14 inmediatamente a neutro.</div>
</section>

<section class="card chart-card">
  <div class="chart-head"><strong>Grados de la rueda</strong><div class="legend"><span class="dot actual-dot"></span>REAL <span class="dot target-dot"></span>OBJETIVO</div></div>
  <canvas id="trend" aria-label="Historial de &aacute;ngulo real y objetivo"></canvas>
  <div class="statebar">
    <div class="mini"><div class="mini-label">AS5600 corregido</div><div id="corrected" class="mini-value">---&deg;</div></div>
    <div class="mini"><div class="mini-label">Mando</div><div id="commandAge" class="mini-value">--- ms</div></div>
    <div class="mini"><div class="mini-label">Muestra</div><div id="sampleAge" class="mini-value">--- ms</div></div>
  </div>
  <a class="diaglink" href="/telemetry">Abrir diagn&oacute;stico t&eacute;cnico</a>
</section>
</main>

<script>
const $=id=>document.getElementById(id),clamp=(v,a,b)=>Math.max(a,Math.min(b,v)),FIXED_SPEED_PCT=100;
const CLIENT_SESSION=(()=>{try{const v=new Uint32Array(1);crypto.getRandomValues(v);return(v[0]>>>1)||1}catch(e){return Math.floor(Math.random()*2147483646)+1}})();
const pad=$('pad'),axis=pad.querySelector('.axis'),knob=$('knob'),armButton=$('arm'),stopButton=$('stop'),sensorAlarmsButton=$('sensorAlarms'),canvas=$('trend'),ctx=canvas.getContext('2d');
let armed=false,armPending=false,releaseRequired=false,sensorAlarmsEnabled=true,sensorSettingsBusy=false,joyX=0,dragging=false,activePointerId=null,postBusy=false,postPending=false,activePost=null,postToken=0,failures=0,armTransitionUntil=0;
let history=[],lastStatus=null;

function translatedState(state){return({DISARMED:'DESARMADO',ACTIVE:'MOVIENDO',AT_TARGET:'EN OBJETIVO',SPEED_ZERO:'VELOCIDAD CERO',REVERSAL_SETTLE:'CAMBIO SENTIDO',SENSOR_STALE:'MUESTRA ATRASADA',SENSOR_FAULT:'FALLO ENCODER',WATCHDOG:'SIN MANDO',OTA_ABORTED:'ACTUALIZANDO',MANUAL_STOP:'PARADA MANUAL'})[state]||state||'ESPERA'}
function setLink(text,kind=''){$('link').textContent=text;$('link').className='badge'+(kind?' '+kind:'')}
function setKnob(){
  const p=pad.getBoundingClientRect(),a=axis.getBoundingClientRect();
  knob.style.left=`${a.left-p.left+(joyX+100)*a.width/200}px`;
  knob.style.transform='translate(-50%,-50%)';
  const deg=joyX*.9;$('knobValue').textContent=(deg>0?'+':'')+deg.toFixed(0)+'\u00b0';
  $('setpoint').textContent=`X ${joyX>0?'+':''}${joyX}% · OBJETIVO ${deg>0?'+':''}${deg.toFixed(1)}°`;
  pad.setAttribute('aria-valuenow',deg.toFixed(1));
  pad.setAttribute('aria-valuetext',`${deg.toFixed(1)} grados`);
}
function setXFromPointer(clientX){const a=axis.getBoundingClientRect();joyX=Math.round(clamp((clientX-a.left)/Math.max(1,a.width),0,1)*200-100);setKnob()}
function releaseJoystick(send=true){dragging=false;activePointerId=null;pad.classList.remove('dragging');joyX=0;setKnob();if(send)sendCommand()}

pad.addEventListener('pointerdown',e=>{if(!armed||armPending||activePointerId!==null)return;e.preventDefault();activePointerId=e.pointerId;dragging=true;pad.classList.add('dragging');pad.setPointerCapture(e.pointerId);setXFromPointer(e.clientX);sendCommand()});
pad.addEventListener('pointermove',e=>{if(!dragging||e.pointerId!==activePointerId||!armed||armPending)return;e.preventDefault();setXFromPointer(e.clientX)});
pad.addEventListener('pointerup',e=>{if(dragging&&e.pointerId===activePointerId){e.preventDefault();releaseJoystick()}});
pad.addEventListener('pointercancel',e=>{if(e.pointerId===activePointerId)releaseJoystick()});
pad.addEventListener('lostpointercapture',e=>{if(dragging&&e.pointerId===activePointerId)releaseJoystick()});
pad.addEventListener('keydown',e=>{if(!armed||armPending)return;if(e.key==='ArrowLeft'||e.key==='ArrowRight'){e.preventDefault();joyX=clamp(joyX+(e.key==='ArrowRight'?5:-5),-100,100);setKnob();sendCommand()}else if(e.key===' '||e.key==='Home'){e.preventDefault();releaseJoystick()}});

armButton.addEventListener('click',()=>{
  if(sensorSettingsBusy)return;
  joyX=0;setKnob();
  if(releaseRequired){armed=false;armPending=false;armTransitionUntil=0;paintArmed();sendCommand(true);return}
  armed=!armed;armPending=armed;armTransitionUntil=Date.now()+700;paintArmed();
  if(armed)sendCommand(true,true);else stopJoystick(false)
});
stopButton.addEventListener('click',()=>stopJoystick(true));
sensorAlarmsButton.addEventListener('click',()=>{if(armed||armPending||sensorSettingsBusy||postBusy)return;const next=!sensorAlarmsEnabled;if(!next&&!confirm('MODO DE BANCO: se ignorarán MD/ML/MH del AS5600. La pérdida real de I2C seguirá deteniendo la dirección. ¿Desactivar alarmas magnéticas?'))return;setSensorAlarms(next)});
function paintSensorAlarms(){sensorAlarmsButton.textContent=sensorSettingsBusy?'CAMBIANDO...':(sensorAlarmsEnabled?'ALARMAS AS5600 · ON':'MODO PRUEBA · OFF');sensorAlarmsButton.classList.toggle('off',!sensorAlarmsEnabled);sensorAlarmsButton.disabled=armed||armPending||sensorSettingsBusy||postBusy;sensorAlarmsButton.setAttribute('aria-checked',sensorAlarmsEnabled?'true':'false')}
function paintArmed(){armButton.textContent=armPending?'ARMANDO...':(armed?'ARMADO':(releaseRequired?'RECONOCER FALLO':'ARMAR'));armButton.disabled=armPending||sensorSettingsBusy;armButton.setAttribute('aria-pressed',armed&&!armPending?'true':'false');armButton.classList.toggle('on',armed&&!armPending);pad.classList.toggle('locked',!armed||armPending);if((!armed||armPending)&&dragging)releaseJoystick(false);paintSensorAlarms()}

async function sendCommand(priority=false,claimControl=false){
  if(priority&&activePost){postToken++;activePost.abort();activePost=null;postBusy=false;postPending=false}
  if(postBusy){postPending=true;return}postBusy=true;postPending=false;
  const requestArm=armed,requestX=requestArm?joyX:0;
  const ctl=new AbortController(),token=++postToken;activePost=ctl;const timer=setTimeout(()=>ctl.abort(),250);
  try{const url=`/joystick/cmd?x=${requestX}&speed=${FIXED_SPEED_PCT}&arm=${requestArm?1:0}&client=${CLIENT_SESSION}&claim=${claimControl?1:0}&_=${Date.now()}`;const r=await fetch(url,{method:'POST',cache:'no-store',signal:ctl.signal});const d=await r.json();if(!r.ok){armed=false;armPending=false;joyX=0;armTransitionUntil=0;postPending=false;setKnob();paintArmed();showStatus(d);throw Error('HTTP '+r.status)}if(claimControl)armPending=false;showStatus(d);paintArmed()}catch(e){if(token===postToken&&claimControl){armed=false;armPending=false;joyX=0;armTransitionUntil=0;setKnob();paintArmed()}if(token===postToken&&++failures>2)setLink('SIN CONEXIÓN','bad')}finally{clearTimeout(timer);if(token!==postToken)return;activePost=null;postBusy=false;if(postPending&&(!releaseRequired||armed))queueMicrotask(()=>sendCommand());else postPending=false}
}
async function stopJoystick(forceStop=false){
  armed=false;armPending=false;releaseRequired=true;joyX=0;armTransitionUntil=0;setKnob();paintArmed();postPending=false;
  if(activePost){postToken++;activePost.abort();activePost=null;postBusy=false}
  const ctl=new AbortController(),token=++postToken;activePost=ctl;postBusy=true;const timer=setTimeout(()=>ctl.abort(),250);
  try{const r=await fetch(`/joystick/stop?client=${CLIENT_SESSION}&force=${forceStop?1:0}&_=${Date.now()}`,{method:'POST',cache:'no-store',signal:ctl.signal});const d=await r.json();showStatus(d);if(!r.ok)throw Error('HTTP '+r.status)}catch(e){if(token===postToken&&++failures>2)setLink('SIN CONEXIÓN','bad')}finally{clearTimeout(timer);if(token!==postToken)return;activePost=null;postBusy=false;postPending=false}
}
async function setSensorAlarms(enabled){
  sensorSettingsBusy=true;paintArmed();
  const ctl=new AbortController(),timer=setTimeout(()=>ctl.abort(),900);
  try{const r=await fetch(`/joystick/sensor-alarms?enabled=${enabled?1:0}&client=${CLIENT_SESSION}&_=${Date.now()}`,{method:'POST',cache:'no-store',signal:ctl.signal});const d=await r.json();showStatus(d);if(!r.ok)throw Error('HTTP '+r.status)}catch(e){setLink('NO SE PUDO CAMBIAR','bad')}finally{clearTimeout(timer);sensorSettingsBusy=false;paintArmed()}
}
setInterval(()=>{if(armed&&!armPending)sendCommand()},180);

function setNeedles(actual,target){$('actualNeedle').setAttribute('transform',`rotate(${clamp(actual,-100,100)} 160 160)`);$('targetNeedle').setAttribute('transform',`rotate(${clamp(target,-90,90)} 160 160)`)}
function pushHistory(actual,target){history.push({actual,target});if(history.length>120)history.shift();drawChart()}
function drawSeries(w,h,key,color){if(history.length<2)return;ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();history.forEach((p,i)=>{const x=i*w/119,y=8+(98-clamp(p[key],-98,98))*((h-16)/196);i?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke()}
function drawChart(){
  const dpr=window.devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;if(!w||!h)return;canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,w,h);
  ctx.font='9px ui-monospace,monospace';ctx.textAlign='left';[-90,0,90].forEach(v=>{const y=8+(98-v)*((h-16)/196);ctx.strokeStyle=v===0?'#7595a955':'#294a6155';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke();ctx.fillStyle='#7898ab';ctx.fillText((v>0?'+':'')+v+'°',5,y-3)});drawSeries(w,h,'target','#ffd166');drawSeries(w,h,'actual','#38d8ff')
}

function showStatus(d){
  lastStatus=d;const actual=Number(d.angle_deg),target=Number(d.target_deg),error=Number(d.error_deg),valid=!!d.sensor_valid,hardValid=!!d.i2c_fresh;
  releaseRequired=!!d.arm_release_required;
  if(d.sensor_alarms_enabled!==undefined)sensorAlarmsEnabled=!!d.sensor_alarms_enabled;
  if(Number.isFinite(actual)){$('actualAngle').innerHTML=`${actual>0?'+':''}${actual.toFixed(1)}<small>°</small>`}else $('actualAngle').innerHTML='---<small>°</small>';
  $('targetAngle').textContent=`OBJETIVO ${target>0?'+':''}${Number.isFinite(target)?target.toFixed(1):'---'}°`;
  $('error').textContent=Number.isFinite(error)?`${error>0?'+':''}${error.toFixed(1)}°`:'---°';$('servo').textContent=`${d.servo_us||'---'} us`;
  $('state').textContent=translatedState(d.state);$('state').className='mini-value'+(['SENSOR_STALE','SENSOR_FAULT','WATCHDOG','OTA_ABORTED'].includes(d.state)?' warn':'');
  const shownCorrected=Number(d.display_corrected_angle);$('corrected').textContent=Number.isFinite(shownCorrected)?shownCorrected.toFixed(1)+'°':'---°';$('commandAge').textContent=(d.command_age_ms??'---')+' ms';$('sampleAge').textContent=(d.sample_age_ms??'---')+' ms';
  if(Number.isFinite(actual)&&Number.isFinite(target)){setNeedles(actual,target);pushHistory(actual,target)}
  const filterWindow=Number(d.filter_window||5);let fault='',notice=false;if(releaseRequired)fault='Parada de seguridad enclavada. Pulse RECONOCER FALLO antes de volver a armar.';else if(!hardValid)fault='Sin lecturas I2C recientes: dirección detenida.';else if(Number(d.sample_age_ms)>Number(d.sensor_neutral_ms||100))fault='Lectura I2C intermitente o atrasada: dirección detenida.';else if(d.magnetic_rejected)fault='Lectura presente, pero MD no está activo. Puede usar MODO PRUEBA en banco.';else if(d.reference_recovery)fault=`Revalidando referencia AS5600 (${d.reference_candidate_samples||0}/${5}).`;else if(!valid)fault=Number(d.filter_samples)<filterWindow?`Llenando el promedio de ${filterWindow} lecturas: dirección detenida.`:`Ventana AS5600 no coherente (${d.filter_inliers||0}/${filterWindow}, ${d.filter_span_ms||0} ms): dirección detenida.`;else if(!sensorAlarmsEnabled){fault='MODO PRUEBA: se ignoran MD/ML/MH; el corte por pérdida I2C sigue activo.';notice=true}else if(d.magnet_weak){fault='Aviso: el AS5600 reporta campo magnético débil.';notice=true}else if(d.magnet_strong){fault='Aviso: el AS5600 reporta campo magnético fuerte.';notice=true}const faultNode=$('fault'),faultClass='fault'+(fault?' show':'')+(notice?' notice':'');if(faultNode.textContent!==fault)faultNode.textContent=fault;if(faultNode.className!==faultClass)faultNode.className=faultClass;
  if(releaseRequired){armed=false;armPending=false;joyX=0;armTransitionUntil=0;postPending=false;setKnob()}else if(Date.now()>armTransitionUntil&&armed&&!d.armed){armed=false;armPending=false;joyX=0;setKnob()}
  paintArmed();
  if(releaseRequired)setLink('PARADA ENCLAVADA','bad');else if(valid){if(!sensorAlarmsEnabled)setLink('EN LÍNEA · ALARMAS OFF');else setLink(d.magnet_weak?'EN LÍNEA · IMÁN DÉBIL':'EN LÍNEA','ok')}else setLink('FALLO ENCODER','bad');failures=0;
}
async function pollStatus(){
  if(armed){setTimeout(pollStatus,500);return}
  const ctl=new AbortController(),timer=setTimeout(()=>ctl.abort(),450);
  try{const r=await fetch('/joystick/status.json?_='+Date.now(),{cache:'no-store',signal:ctl.signal});if(!r.ok)throw Error('HTTP');showStatus(await r.json())}catch(e){if(++failures>2)setLink('SIN CONEXIÓN','bad')}finally{clearTimeout(timer);setTimeout(pollStatus,500)}
}

function safePageExit(){if(!armed&&!armPending&&!postBusy)return;armed=false;armPending=false;releaseRequired=true;joyX=0;dragging=false;activePointerId=null;if(activePost){postToken++;activePost.abort();activePost=null;postBusy=false;postPending=false}const url=`/joystick/stop?client=${CLIENT_SESSION}&force=0`;if(navigator.sendBeacon)navigator.sendBeacon(url,new Blob([],{type:'text/plain'}));else fetch(url,{method:'POST',keepalive:true}).catch(()=>{})}
function relayout(){setKnob();drawChart()}
addEventListener('pagehide',safePageExit);addEventListener('resize',relayout);
addEventListener('orientationchange',()=>{if(dragging)releaseJoystick();setTimeout(relayout,120)});
document.addEventListener('visibilitychange',()=>{if(document.hidden)safePageExit()});
setKnob();paintArmed();drawChart();pollStatus();
</script>
</body>
</html>
)HTML";

As5600Snapshot copySnapshot() {
  portENTER_CRITICAL(&snapshotMux);
  As5600Snapshot copy = snapshot;
  portEXIT_CRITICAL(&snapshotMux);
  return copy;
}

void publishSnapshot(const As5600Snapshot &next) {
  portENTER_CRITICAL(&snapshotMux);
  snapshot = next;
  portEXIT_CRITICAL(&snapshotMux);
}

const char *servoTestPhaseName(ServoTestPhase phase) {
  switch (phase) {
    case SERVO_TEST_IDLE: return "IDLE";
    case SERVO_TEST_NEUTRAL_START: return "NEUTRAL_START";
    case SERVO_TEST_2000_US: return "PWM_2000_US";
    case SERVO_TEST_NEUTRAL_MIDDLE: return "NEUTRAL_MIDDLE";
    case SERVO_TEST_1000_US: return "PWM_1000_US";
    case SERVO_TEST_NEUTRAL_END: return "NEUTRAL_END";
    case SERVO_TEST_COMPLETE: return "COMPLETE";
    case SERVO_TEST_STOPPED: return "STOPPED";
    case SERVO_TEST_SENSOR_TIMEOUT: return "SENSOR_TIMEOUT";
    case SERVO_TEST_OTA_ABORTED: return "OTA_ABORTED";
    default: return "UNKNOWN";
  }
}

const char *linearityTestPhaseName(LinearityTestPhase phase) {
  switch (phase) {
    case LINEARITY_IDLE: return "IDLE";
    case LINEARITY_NEUTRAL_START: return "NEUTRAL_START";
    case LINEARITY_LEFT_2000_US: return "LEFT_2000_US";
    case LINEARITY_NEUTRAL_MIDDLE: return "NEUTRAL_MIDDLE";
    case LINEARITY_RIGHT_1040_US: return "RIGHT_1040_US";
    case LINEARITY_NEUTRAL_END: return "NEUTRAL_END";
    case LINEARITY_COMPLETE: return "COMPLETE";
    case LINEARITY_STOPPED: return "STOPPED";
    case LINEARITY_SENSOR_TIMEOUT: return "SENSOR_TIMEOUT";
    case LINEARITY_PHASE_TIMEOUT: return "PHASE_TIMEOUT";
    case LINEARITY_OTA_ABORTED: return "OTA_ABORTED";
    default: return "UNKNOWN";
  }
}

uint16_t linearityPhasePulseUs(LinearityTestPhase phase) {
  switch (phase) {
    case LINEARITY_LEFT_2000_US: return SERVO_TEST_POSITIVE_US;
    case LINEARITY_RIGHT_1040_US: return LINEARITY_RETURN_US;
    default: return SERVO_NEUTRAL_US;
  }
}

const char *positionSequencePhaseName(PositionSequencePhase phase) {
  switch (phase) {
    case POSITION_IDLE: return "IDLE";
    case POSITION_HOME_MOVE: return "HOME_MOVE";
    case POSITION_HOME_DWELL: return "HOME_DWELL";
    case POSITION_FORWARD_MOVE: return "FORWARD_MOVE";
    case POSITION_FORWARD_DWELL: return "FORWARD_DWELL";
    case POSITION_RETURN_MOVE: return "RETURN_MOVE";
    case POSITION_RETURN_DWELL: return "RETURN_DWELL";
    case POSITION_COMPLETE: return "COMPLETE";
    case POSITION_STOPPED: return "STOPPED";
    case POSITION_SENSOR_TIMEOUT: return "SENSOR_TIMEOUT";
    case POSITION_MOVE_TIMEOUT: return "MOVE_TIMEOUT";
    case POSITION_OTA_ABORTED: return "OTA_ABORTED";
    case POSITION_SEQUENCE_TIMEOUT: return "SEQUENCE_TIMEOUT";
    default: return "UNKNOWN";
  }
}

const char *joystickSteeringModeName(JoystickSteeringMode mode) {
  switch (mode) {
    case JOYSTICK_DISARMED: return "DISARMED";
    case JOYSTICK_ACTIVE: return "ACTIVE";
    case JOYSTICK_AT_TARGET: return "AT_TARGET";
    case JOYSTICK_SPEED_ZERO: return "SPEED_ZERO";
    case JOYSTICK_REVERSAL_SETTLE: return "REVERSAL_SETTLE";
    case JOYSTICK_SENSOR_STALE: return "SENSOR_STALE";
    case JOYSTICK_SENSOR_FAULT: return "SENSOR_FAULT";
    case JOYSTICK_WATCHDOG: return "WATCHDOG";
    case JOYSTICK_OTA_ABORTED: return "OTA_ABORTED";
    case JOYSTICK_MANUAL_STOP: return "MANUAL_STOP";
    default: return "UNKNOWN";
  }
}

bool positionPhaseIsMove(PositionSequencePhase phase) {
  return phase == POSITION_HOME_MOVE || phase == POSITION_FORWARD_MOVE ||
         phase == POSITION_RETURN_MOVE;
}

bool positionPhaseIsDwell(PositionSequencePhase phase) {
  return phase == POSITION_HOME_DWELL || phase == POSITION_FORWARD_DWELL ||
         phase == POSITION_RETURN_DWELL;
}

bool positionInArrivalBand(float errorDeg) {
  // Se acepta el punto al llegar desde abajo: [objetivo-3 grados, objetivo].
  // En el regreso obliga a cruzar el objetivo antes de soltar el motor, para
  // que el rebote medido del engranaje lo acerque y no lo aleje del punto.
  return errorDeg >= 0.0f && errorDeg <= POSITION_TOLERANCE_DEG;
}

uint16_t positionControlPulseUs(float errorDeg) {
  const float magnitude = fabsf(errorDeg);
  if (positionInArrivalBand(errorDeg)) return SERVO_NEUTRAL_US;
  const uint16_t maxOffset = errorDeg > 0.0f
      ? uint16_t(SERVO_NEUTRAL_US - LINEARITY_RETURN_US)
      : uint16_t(SERVO_TEST_POSITIVE_US - SERVO_NEUTRAL_US);
  const float ramp = constrain(
      (magnitude - POSITION_TOLERANCE_DEG) /
          (20.0f - POSITION_TOLERANCE_DEG),
      0.0f, 1.0f);
  const uint16_t offset = uint16_t(lroundf(
      POSITION_MIN_DRIVE_OFFSET_US +
      ramp * (maxOffset - POSITION_MIN_DRIVE_OFFSET_US)));
  return errorDeg > 0.0f
      ? uint16_t(SERVO_NEUTRAL_US - offset)
      : uint16_t(SERVO_NEUTRAL_US + offset);
}

uint16_t positionDrivePulseUs(float errorDeg) {
  const float magnitude = fabsf(errorDeg);
  if (positionInArrivalBand(errorDeg)) return SERVO_NEUTRAL_US;
  if (magnitude < POSITION_FINE_ZONE_DEG) {
    // La prueba corta midio zonas muertas distintas: 110 us hacia adelante
    // y al menos 157 us en regreso. Se usan 180 us en regreso para vencer el
    // juego; la lectura a 20 Hz corta el pulso en menos de 50 ms al llegar.
    return errorDeg > 0.0f
        ? uint16_t(SERVO_NEUTRAL_US - POSITION_FINE_FORWARD_OFFSET_US)
        : uint16_t(SERVO_NEUTRAL_US + POSITION_FINE_RETURN_OFFSET_US);
  }
  return positionControlPulseUs(errorDeg);
}

uint16_t joystickDrivePulseUs(float errorDeg) {
  if (positionInArrivalBand(errorDeg)) return SERVO_NEUTRAL_US;

  // Los extremos y las zonas muertas provienen de la secuencia 0->1080->0:
  // angulo creciente 1040/1390 us; angulo decreciente 2000/1680 us.
  const bool increasing = errorDeg > 0.0f;
  const uint16_t fineOffset = increasing
      ? POSITION_FINE_FORWARD_OFFSET_US
      : POSITION_FINE_RETURN_OFFSET_US;
  const uint16_t fullOffset = increasing
      ? uint16_t(SERVO_NEUTRAL_US - LINEARITY_RETURN_US)
      : uint16_t(SERVO_TEST_POSITIVE_US - SERVO_NEUTRAL_US);
  const float errorScale = constrain(
      (fabsf(errorDeg) - POSITION_TOLERANCE_DEG) /
          (JOYSTICK_SLOW_ZONE_DEG - POSITION_TOLERANCE_DEG),
      0.0f, 1.0f);
  const uint16_t offset = uint16_t(lroundf(
      fineOffset + errorScale * (fullOffset - fineOffset)));
  return increasing ? uint16_t(SERVO_NEUTRAL_US - offset)
                    : uint16_t(SERVO_NEUTRAL_US + offset);
}

void writeServoPulseLocked(uint16_t pulseUs) {
  pulseUs = constrain(pulseUs, SERVO_MIN_US, SERVO_MAX_US);
  servoTest.pulseUs = pulseUs;
  linearityTest.pulseUs = pulseUs;
  positionSequence.pulseUs = pulseUs;
  joystickSteering.pulseUs = pulseUs;
  if (!servoPwmReady) return;
  constexpr uint32_t maxDuty = (1UL << SERVO_PWM_RESOLUTION_BITS) - 1;
  ledcWrite(SERVO_PWM_PIN,
            uint32_t(pulseUs) * maxDuty / (1000000UL / SERVO_PWM_HZ));
}

ServoTestState copyServoTestState() {
  ServoTestState copy = {};
  if (!servoStateMutex) return copy;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
    copy = servoTest;
    xSemaphoreGive(servoStateMutex);
  }
  return copy;
}

LinearityTestState copyLinearityTestState() {
  LinearityTestState copy = {};
  if (!servoStateMutex) return copy;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
    copy = linearityTest;
    xSemaphoreGive(servoStateMutex);
  }
  return copy;
}

PositionSequenceState copyPositionSequenceState() {
  PositionSequenceState copy = {};
  if (!servoStateMutex) return copy;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
    copy = positionSequence;
    xSemaphoreGive(servoStateMutex);
  }
  return copy;
}

JoystickSteeringState copyJoystickSteeringState() {
  JoystickSteeringState copy = {};
  if (!servoStateMutex) return copy;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
    copy = joystickSteering;
    xSemaphoreGive(servoStateMutex);
  }
  return copy;
}

// Debe llamarse con servoStateMutex tomado. Estos contadores pertenecen a la
// validacion de la referencia del encoder, no a la readquisicion del objetivo.
void clearJoystickReferenceRecoveryLocked() {
  joystickSteering.referenceRecoveryActive = false;
  joystickSteering.referenceCandidateSamples = 0;
  joystickSteering.armedOutlierSamples = 0;
  joystickSteering.armedReturnSamples = 0;
  joystickSteering.referenceCandidateAnchorDeg = 0.0f;
  joystickSteering.armedReturnAnchorDeg = 0.0f;
}

// Debe llamarse con servoStateMutex tomado. Un fallo queda enclavado para
// impedir que un heartbeat arm=1 atrasado rearme la direccion al volver Wi-Fi.
void latchJoystickSafetyLocked(JoystickSteeringMode reason) {
  if (!joystickSteering.armReleaseRequired &&
      joystickSteering.safetyTrips < UINT32_MAX)
    joystickSteering.safetyTrips++;
  joystickSteering.armReleaseRequired = true;
  joystickSteering.lastTripReason = reason;
}

void stopServoTest(ServoTestPhase reason, bool onlyIfActive = false) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  const bool affected = !onlyIfActive || servoTest.active;
  if (affected) {
    servoTest.active = false;
    servoTest.phase = reason;
    servoTest.phaseStartedMs = millis();
    servoTest.completedMs = servoTest.phaseStartedMs;
  }
  if (affected) writeServoPulseLocked(SERVO_NEUTRAL_US);
  xSemaphoreGive(servoStateMutex);
}

void stopLinearityTest(LinearityTestPhase reason, bool onlyIfActive = false) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  const bool affected = !onlyIfActive || linearityTest.active;
  if (affected) {
    const uint32_t now = millis();
    linearityTest.active = false;
    linearityTest.phase = reason;
    linearityTest.phaseStartedMs = now;
    linearityTest.completedMs = now;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }
  xSemaphoreGive(servoStateMutex);
}

void stopPositionSequence(PositionSequencePhase reason,
                          bool onlyIfActive = false) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  const bool affected = !onlyIfActive || positionSequence.active;
  if (affected) {
    const uint32_t now = millis();
    positionSequence.active = false;
    positionSequence.phase = reason;
    positionSequence.phaseStartedMs = now;
    positionSequence.completedMs = now;
    positionSequence.stableSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }
  xSemaphoreGive(servoStateMutex);
}

void stopJoystickSteering(JoystickSteeringMode reason,
                          bool onlyIfArmed = false) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (reason == JOYSTICK_WATCHDOG || reason == JOYSTICK_SENSOR_FAULT ||
      reason == JOYSTICK_OTA_ABORTED || reason == JOYSTICK_MANUAL_STOP)
    latchJoystickSafetyLocked(reason);
  const bool affected = !onlyIfArmed || joystickSteering.armed;
  if (affected) {
    joystickSteering.armed = false;
    joystickSteering.ownerIpKey = 0;
    joystickSteering.ownerSessionKey = 0;
    joystickSteering.mode = reason;
    joystickSteering.x = 0;
    joystickSteering.targetDeg = 0.0f;
    joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg;
    joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                                joystickSteering.unwrappedCorrectedDeg;
    joystickSteering.driveInhibitUntilMs = 0;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    clearJoystickReferenceRecoveryLocked();
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }
  xSemaphoreGive(servoStateMutex);
}

// Estas funciones se ejecutan con servoStateMutex tomado. Tambien las llama
// la tarea I2C, por lo que los cortes no dependen de que WebServer regrese.
void tripJoystickSafetyLocked(JoystickSteeringMode reason) {
  joystickSteering.armed = false;
  joystickSteering.ownerIpKey = 0;
  joystickSteering.ownerSessionKey = 0;
  joystickSteering.mode = reason;
  joystickSteering.x = 0;
  joystickSteering.targetDeg = 0.0f;
  joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg;
  joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                              joystickSteering.unwrappedCorrectedDeg;
  joystickSteering.driveInhibitUntilMs = 0;
  joystickSteering.stableSamples = 0;
  joystickSteering.reacquireSamples = 0;
  clearJoystickReferenceRecoveryLocked();
  latchJoystickSafetyLocked(reason);
  writeServoPulseLocked(SERVO_NEUTRAL_US);
}

bool enforceJoystickCommandAgeLocked(uint32_t now) {
  if (!joystickSteering.armed) return false;
  const uint32_t age = joystickSteering.lastCommandMs
      ? uint32_t(now - joystickSteering.lastCommandMs)
      : UINT32_MAX;
  if (age <= JOYSTICK_COMMAND_TIMEOUT_MS) return false;
  tripJoystickSafetyLocked(JOYSTICK_WATCHDOG);
  return true;
}

bool enforceJoystickMissingSensorLocked(uint32_t now) {
  if (!joystickSteering.armed) return false;
  const uint32_t age = joystickSteering.lastAcceptedMs
      ? uint32_t(now - joystickSteering.lastAcceptedMs)
      : UINT32_MAX;
  if (age > JOYSTICK_SENSOR_TIMEOUT_MS) {
    tripJoystickSafetyLocked(JOYSTICK_SENSOR_FAULT);
    return true;
  }
  if (age > JOYSTICK_SENSOR_NEUTRAL_MS) {
    joystickSteering.mode = JOYSTICK_SENSOR_STALE;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    return true;
  }
  return false;
}

void recordServoTestSample(uint32_t sampleMs, bool valid,
                           uint16_t rawAngle) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (servoTest.active) {
    ServoTestSample sample = {};
    sample.elapsedMs = uint32_t(sampleMs - servoTest.startedMs);
    sample.rawAngle = valid ? rawAngle : UINT16_MAX;
    sample.servoUs = servoTest.pulseUs;
    sample.phase = static_cast<uint8_t>(servoTest.phase);
    sample.valid = valid ? 1 : 0;
    portENTER_CRITICAL(&servoLogMux);
    if (servoTestLogCount < SERVO_TEST_MAX_SAMPLES)
      servoTestLog[servoTestLogCount++] = sample;
    else
      servoTestLogDropped++;
    portEXIT_CRITICAL(&servoLogMux);
  }
  xSemaphoreGive(servoStateMutex);
}

void processLinearitySample(uint32_t sampleMs, bool i2cValid,
                            uint16_t rawAngle, uint8_t as5600Status) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!linearityTest.active) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  uint8_t flags = 0;
  if (i2cValid) flags |= LINEARITY_SAMPLE_I2C_VALID;
  if (i2cValid) flags |= as5600Status & 0x38;
  bool accepted = false;
  bool glitch = false;
  if (i2cValid) {
    if (!linearityTest.unwrapReady) {
      linearityTest.lastAcceptedRaw = rawAngle;
      linearityTest.unwrapReady = true;
      accepted = true;
    } else {
      int32_t deltaRaw = int32_t(rawAngle) -
                         int32_t(linearityTest.lastAcceptedRaw);
      if (deltaRaw > 2048) deltaRaw -= 4096;
      if (deltaRaw < -2048) deltaRaw += 4096;
      if (abs(deltaRaw) <= LINEARITY_MAX_SLOT_DELTA_RAW) {
        linearityTest.unwrappedRaw += deltaRaw;
        linearityTest.lastAcceptedRaw = rawAngle;
        accepted = true;
      } else {
        glitch = true;
        linearityTest.glitches++;
      }
    }
  }

  if (accepted) {
    flags |= LINEARITY_SAMPLE_ACCEPTED;
    linearityTest.acceptedSamples++;
    linearityTest.lastAcceptedMs = sampleMs;
    bool targetReached = false;
    if (linearityTest.phase == LINEARITY_LEFT_2000_US) {
      targetReached =
          linearityTest.unwrappedRaw <= -LINEARITY_SEVEN_TURNS_RAW;
    } else if (linearityTest.phase == LINEARITY_RIGHT_1040_US) {
      targetReached =
          linearityTest.unwrappedRaw - linearityTest.phaseOriginRaw >=
          LINEARITY_SEVEN_TURNS_RAW;
    }
    if (targetReached) {
      if (linearityTest.targetConfirmSamples < UINT8_MAX)
        linearityTest.targetConfirmSamples++;
    } else {
      linearityTest.targetConfirmSamples = 0;
    }
  } else {
    linearityTest.invalidSamples++;
  }
  if (glitch) flags |= LINEARITY_SAMPLE_GLITCH;

  LinearityTestSample sample = {};
  sample.elapsedMs = uint32_t(sampleMs - linearityTest.startedMs);
  sample.unwrappedRaw = linearityTest.unwrappedRaw;
  sample.rawAngle = i2cValid ? rawAngle : UINT16_MAX;
  sample.phase = static_cast<uint8_t>(linearityTest.phase);
  sample.flags = flags;
  portENTER_CRITICAL(&linearityLogMux);
  if (linearityTestLogCount < LINEARITY_TEST_MAX_SAMPLES)
    linearityTestLog[linearityTestLogCount++] = sample;
  else
    linearityTestLogDropped++;
  portEXIT_CRITICAL(&linearityLogMux);
  xSemaphoreGive(servoStateMutex);
}

PositionSequencePhase positionDwellForMove(PositionSequencePhase phase) {
  switch (phase) {
    case POSITION_HOME_MOVE: return POSITION_HOME_DWELL;
    case POSITION_FORWARD_MOVE: return POSITION_FORWARD_DWELL;
    case POSITION_RETURN_MOVE: return POSITION_RETURN_DWELL;
    default: return POSITION_STOPPED;
  }
}

PositionSequencePhase positionMoveForDwell(PositionSequencePhase phase) {
  switch (phase) {
    case POSITION_HOME_DWELL: return POSITION_HOME_MOVE;
    case POSITION_FORWARD_DWELL: return POSITION_FORWARD_MOVE;
    case POSITION_RETURN_DWELL: return POSITION_RETURN_MOVE;
    default: return POSITION_STOPPED;
  }
}

void processPositionSequenceSample(uint32_t sampleMs, bool i2cValid,
                                   uint16_t rawAngle) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!positionSequence.active) {
    xSemaphoreGive(servoStateMutex);
    return;
  }
  if (!i2cValid) {
    positionSequence.invalidSamples++;
    positionSequence.reacquireSamples = 0;
    if (positionPhaseIsMove(positionSequence.phase))
      positionSequence.stableSamples = 0;
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const float correctedCyclicDeg = as5600CorrectedAngleDeg(rawAngle);
  if (!positionSequence.unwrapReady) {
    positionSequence.unwrapReady = true;
    positionSequence.lastCorrectedCyclicDeg = correctedCyclicDeg;
    positionSequence.unwrappedCorrectedDeg = correctedCyclicDeg;
  } else {
    float deltaDeg = correctedCyclicDeg -
                     positionSequence.lastCorrectedCyclicDeg;
    if (deltaDeg > 180.0f) deltaDeg -= 360.0f;
    if (deltaDeg < -180.0f) deltaDeg += 360.0f;
    if (fabsf(deltaDeg) > POSITION_MAX_SLOT_DELTA_DEG) {
      positionSequence.glitches++;
      positionSequence.invalidSamples++;
      positionSequence.reacquireSamples = 0;
      if (positionPhaseIsMove(positionSequence.phase))
        positionSequence.stableSamples = 0;
      xSemaphoreGive(servoStateMutex);
      return;
    }
    positionSequence.unwrappedCorrectedDeg += deltaDeg;
    positionSequence.lastCorrectedCyclicDeg = correctedCyclicDeg;
  }

  positionSequence.acceptedSamples++;
  positionSequence.lastAcceptedMs = sampleMs;
  positionSequence.logicalPositionDeg =
      positionSequence.unwrappedCorrectedDeg -
      positionSequence.homeAbsoluteDeg;
  const float previousErrorDeg = positionSequence.errorDeg;
  positionSequence.errorDeg = positionSequence.targetAbsoluteDeg -
                              positionSequence.unwrappedCorrectedDeg;

  if (positionPhaseIsMove(positionSequence.phase)) {
    positionSequence.reacquireSamples = 0;
    if (positionInArrivalBand(positionSequence.errorDeg)) {
      if (positionSequence.stableSamples < UINT8_MAX)
        positionSequence.stableSamples++;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
    } else {
      positionSequence.stableSamples = 0;
      const bool crossedTarget =
          previousErrorDeg * positionSequence.errorDeg < 0.0f;
      if (crossedTarget)
        positionSequence.driveInhibitUntilMs =
            sampleMs + POSITION_REVERSAL_SETTLE_MS;
      if (positionSequence.driveInhibitUntilMs &&
          int32_t(sampleMs - positionSequence.driveInhibitUntilMs) < 0)
        writeServoPulseLocked(SERVO_NEUTRAL_US);
      else
        writeServoPulseLocked(positionDrivePulseUs(positionSequence.errorDeg));
    }
    if (positionSequence.stableSamples >= POSITION_STABLE_SAMPLES) {
      positionSequence.phase = positionDwellForMove(positionSequence.phase);
      positionSequence.phaseStartedMs = sampleMs;
      positionSequence.stableSamples = 0;
      positionSequence.transitions++;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
    }
  } else if (positionPhaseIsDwell(positionSequence.phase)) {
    if (fabsf(positionSequence.errorDeg) > POSITION_REACQUIRE_DEG) {
      if (positionSequence.reacquireSamples < UINT8_MAX)
        positionSequence.reacquireSamples++;
    } else {
      positionSequence.reacquireSamples = 0;
    }
    if (positionSequence.reacquireSamples >= POSITION_REACQUIRE_SAMPLES) {
      positionSequence.phase = positionMoveForDwell(positionSequence.phase);
      positionSequence.phaseStartedMs = sampleMs;
      positionSequence.stableSamples = 0;
      positionSequence.reacquireSamples = 0;
      positionSequence.transitions++;
      writeServoPulseLocked(positionDrivePulseUs(positionSequence.errorDeg));
    } else {
      writeServoPulseLocked(SERVO_NEUTRAL_US);
    }
  }
  xSemaphoreGive(servoStateMutex);
}

float signedCircularDelta(float currentDeg, float referenceDeg) {
  float deltaDeg = currentDeg - referenceDeg;
  if (deltaDeg > 180.0f) deltaDeg -= 360.0f;
  if (deltaDeg < -180.0f) deltaDeg += 360.0f;
  return deltaDeg;
}

// La referencia solo se siembra con el servo desarmado o al arrancar. El
// El promedio recibido aqui ya contiene cinco lecturas corregidas consecutivas.
void seedJoystickReferenceLocked(float correctedCyclicDeg, uint32_t sampleMs,
                                 bool countAsRebase) {
  joystickSteering.unwrapReady = true;
  joystickSteering.lastCorrectedCyclicDeg = correctedCyclicDeg;
  joystickSteering.unwrappedCorrectedDeg = correctedCyclicDeg;
  joystickSteering.zeroAbsoluteDeg =
      roundf(correctedCyclicDeg / 360.0f) * 360.0f;
  joystickSteering.logicalAngleDeg =
      correctedCyclicDeg - joystickSteering.zeroAbsoluteDeg;
  joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg +
                                       joystickSteering.targetDeg;
  joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                              joystickSteering.unwrappedCorrectedDeg;
  joystickSteering.lastAcceptedMs = sampleMs;
  joystickSteering.acceptedSamples++;
  if (countAsRebase && joystickSteering.referenceRebases < UINT32_MAX)
    joystickSteering.referenceRebases++;
  clearJoystickReferenceRecoveryLocked();
}

void processJoystickSteeringSample(uint32_t sampleMs, bool i2cValid,
                                   bool filteredValid, float correctedCyclicDeg,
                                   uint8_t as5600Status) {
  if (!servoStateMutex) return;
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  const uint32_t safetyNow = millis();
  if (enforceJoystickCommandAgeLocked(safetyNow)) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  // En modo estricto MD es obligatorio. El interruptor de banco puede omitir
  // MD/ML/MH, pero nunca convierte una lectura I2C fallida en una muestra.
  const bool magneticStatusAccepted =
      !joystickSteering.sensorAlarmsEnabled || (as5600Status & 0x20);
  if (!i2cValid || !magneticStatusAccepted) {
    joystickSteering.invalidSamples++;
    joystickSteering.referenceCandidateSamples = 0;
    joystickSteering.armedReturnSamples = 0;
    if (!joystickSteering.armed)
      joystickSteering.referenceRecoveryActive = false;
    enforceJoystickMissingSensorLocked(safetyNow);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  // Una transaccion I2C puede ser correcta y aun producir una ventana poco
  // densa o incoherente. En ese caso el neutro es inmediato; no se refresca
  // lastAcceptedMs con el promedio anterior.
  if (!filteredValid) {
    joystickSteering.invalidSamples++;
    joystickSteering.referenceCandidateSamples = 0;
    joystickSteering.armedReturnSamples = 0;
    if (joystickSteering.armed) {
      joystickSteering.referenceRecoveryActive = true;
      joystickSteering.mode = JOYSTICK_SENSOR_STALE;
      joystickSteering.stableSamples = 0;
      joystickSteering.reacquireSamples = 0;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      enforceJoystickMissingSensorLocked(safetyNow);
    } else {
      joystickSteering.referenceRecoveryActive = false;
    }
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const uint32_t previousSensorAge = joystickSteering.lastAcceptedMs
      ? uint32_t(safetyNow - joystickSteering.lastAcceptedMs)
      : UINT32_MAX;
  if (joystickSteering.armed &&
      previousSensorAge > JOYSTICK_SENSOR_TIMEOUT_MS) {
    tripJoystickSafetyLocked(JOYSTICK_SENSOR_FAULT);
    xSemaphoreGive(servoStateMutex);
    return;
  }
  const bool recoveringFromStale = joystickSteering.armed &&
      previousSensorAge > JOYSTICK_SENSOR_NEUTRAL_MS;
  if (recoveringFromStale) {
    joystickSteering.mode = JOYSTICK_SENSOR_STALE;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }

  if (!joystickSteering.unwrapReady) {
    seedJoystickReferenceLocked(correctedCyclicDeg, sampleMs, false);
  } else {
    const float deltaDeg = signedCircularDelta(
        correctedCyclicDeg, joystickSteering.lastCorrectedCyclicDeg);
    if (fabsf(deltaDeg) > JOYSTICK_MAX_SLOT_DELTA_DEG) {
      joystickSteering.glitches++;
      joystickSteering.invalidSamples++;
      joystickSteering.referenceRecoveryActive = true;

      if (joystickSteering.armed) {
        joystickSteering.armedReturnSamples = 0;
        if (joystickSteering.armedOutlierSamples < UINT8_MAX)
          joystickSteering.armedOutlierSamples++;
        joystickSteering.mode = JOYSTICK_SENSOR_STALE;
        joystickSteering.stableSamples = 0;
        joystickSteering.reacquireSamples = 0;
        writeServoPulseLocked(SERVO_NEUTRAL_US);
        if (joystickSteering.armedOutlierSamples >=
            JOYSTICK_ARMED_GLITCH_TRIP_SAMPLES)
          tripJoystickSafetyLocked(JOYSTICK_SENSOR_FAULT);
        else
          enforceJoystickMissingSensorLocked(safetyNow);
      } else {
        // Desarmado se permite adoptar una posicion nueva, pero solo si varios
        // promedios consecutivos forman un grupo compacto alrededor del ancla.
        if (!joystickSteering.referenceCandidateSamples ||
            fabsf(signedCircularDelta(
                correctedCyclicDeg,
                joystickSteering.referenceCandidateAnchorDeg)) >
                JOYSTICK_REFERENCE_CLUSTER_DEG) {
          joystickSteering.referenceCandidateAnchorDeg = correctedCyclicDeg;
          joystickSteering.referenceCandidateSamples = 1;
        } else if (joystickSteering.referenceCandidateSamples < UINT8_MAX) {
          joystickSteering.referenceCandidateSamples++;
        }
        if (joystickSteering.referenceCandidateSamples >=
            JOYSTICK_REFERENCE_CONFIRM_SAMPLES)
          seedJoystickReferenceLocked(correctedCyclicDeg, sampleMs, true);
      }
      xSemaphoreGive(servoStateMutex);
      return;
    }

    if (joystickSteering.armed &&
        joystickSteering.referenceRecoveryActive) {
      // Tras un salto aislado no se reanuda PWM con una unica muestra que
      // regrese: se exigen dos lecturas filtradas coherentes con la referencia.
      if (!joystickSteering.armedReturnSamples ||
          fabsf(signedCircularDelta(
              correctedCyclicDeg,
              joystickSteering.armedReturnAnchorDeg)) >
              JOYSTICK_REFERENCE_CLUSTER_DEG) {
        joystickSteering.armedReturnAnchorDeg = correctedCyclicDeg;
        joystickSteering.armedReturnSamples = 1;
      } else if (joystickSteering.armedReturnSamples < UINT8_MAX) {
        joystickSteering.armedReturnSamples++;
      }
      joystickSteering.mode = JOYSTICK_SENSOR_STALE;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      if (joystickSteering.armedReturnSamples <
          JOYSTICK_ARMED_RETURN_SAMPLES) {
        xSemaphoreGive(servoStateMutex);
        return;
      }
      clearJoystickReferenceRecoveryLocked();
    } else if (!joystickSteering.armed &&
               joystickSteering.referenceRecoveryActive) {
      clearJoystickReferenceRecoveryLocked();
    }

    joystickSteering.unwrappedCorrectedDeg += deltaDeg;
    joystickSteering.lastCorrectedCyclicDeg = correctedCyclicDeg;
    joystickSteering.acceptedSamples++;
    joystickSteering.lastAcceptedMs = sampleMs;
  }

  joystickSteering.logicalAngleDeg =
      joystickSteering.unwrappedCorrectedDeg -
      joystickSteering.zeroAbsoluteDeg;
  // La primera lectura buena despues de una pausa solo rearma la referencia;
  // se exige otra muestra valida (50 ms) antes de volver a aplicar PWM.
  if (recoveringFromStale && joystickSteering.armed) {
    xSemaphoreGive(servoStateMutex);
    return;
  }
  if (!joystickSteering.armed) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const float previousErrorDeg = joystickSteering.errorDeg;
  joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg +
                                       joystickSteering.targetDeg;
  joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                              joystickSteering.unwrappedCorrectedDeg;

  if (!joystickSteering.speedPct) {
    joystickSteering.mode = JOYSTICK_SPEED_ZERO;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  // Una vez asentado, se conserva la misma inmunidad al ruido de la secuencia
  // validada: solo se readquiere si |error| > 4.5 grados durante 5 muestras.
  if (joystickSteering.mode == JOYSTICK_AT_TARGET) {
    if (fabsf(joystickSteering.errorDeg) > POSITION_REACQUIRE_DEG) {
      if (joystickSteering.reacquireSamples < UINT8_MAX)
        joystickSteering.reacquireSamples++;
    } else {
      joystickSteering.reacquireSamples = 0;
    }
    if (joystickSteering.reacquireSamples < POSITION_REACQUIRE_SAMPLES) {
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      xSemaphoreGive(servoStateMutex);
      return;
    }
    joystickSteering.mode = JOYSTICK_ACTIVE;
    joystickSteering.reacquireSamples = 0;
    joystickSteering.stableSamples = 0;
  }

  // Se corta el PWM en la primera muestra dentro de banda, pero solo se
  // declara AT_TARGET tras 5 muestras consecutivas (250 ms a 20 Hz).
  if (positionInArrivalBand(joystickSteering.errorDeg)) {
    if (joystickSteering.stableSamples < UINT8_MAX)
      joystickSteering.stableSamples++;
    joystickSteering.reacquireSamples = 0;
    joystickSteering.driveInhibitUntilMs = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    if (joystickSteering.stableSamples >= POSITION_STABLE_SAMPLES)
      joystickSteering.mode = JOYSTICK_AT_TARGET;
    xSemaphoreGive(servoStateMutex);
    return;
  }

  joystickSteering.stableSamples = 0;
  const bool reversed =
      previousErrorDeg * joystickSteering.errorDeg < 0.0f &&
      fabsf(previousErrorDeg) > POSITION_TOLERANCE_DEG &&
      fabsf(joystickSteering.errorDeg) > POSITION_TOLERANCE_DEG;
  if (reversed)
    joystickSteering.driveInhibitUntilMs =
        sampleMs + JOYSTICK_REVERSAL_SETTLE_MS;

  if (joystickSteering.driveInhibitUntilMs &&
      int32_t(sampleMs - joystickSteering.driveInhibitUntilMs) < 0) {
    joystickSteering.mode = JOYSTICK_REVERSAL_SETTLE;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  } else {
    joystickSteering.driveInhibitUntilMs = 0;
    joystickSteering.mode = JOYSTICK_ACTIVE;
    writeServoPulseLocked(joystickDrivePulseUs(joystickSteering.errorDeg));
  }
  xSemaphoreGive(servoStateMutex);
}

bool commandJoystickSteering(int16_t x, uint8_t requestedSpeedPct, bool arm,
                             uint32_t clientIpKey, uint32_t clientSessionKey,
                             bool claimControl, String &message) {
  // Se conserva el argumento para clientes antiguos, pero desde 1.4 toda
  // orden usa la maxima velocidad calibrada.
  (void)requestedSpeedPct;
  const uint32_t now = millis();
  const As5600Snapshot sensor = copySnapshot();
  if (!servoPwmReady || !servoStateMutex) {
    message = "SERVO_PWM_NOT_READY";
    return false;
  }
  if (otaInProgress) {
    message = "OTA_IN_PROGRESS";
    return false;
  }

  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }

  if (!arm) {
    const bool ownedByOtherClient = joystickSteering.armed &&
        (joystickSteering.ownerIpKey != clientIpKey ||
         joystickSteering.ownerSessionKey != clientSessionKey);
    if (ownedByOtherClient) {
      xSemaphoreGive(servoStateMutex);
      message = "CONTROL_OWNED_BY_OTHER_CLIENT";
      return false;
    }
    const bool wasArmed = joystickSteering.armed;
    joystickSteering.armed = false;
    joystickSteering.ownerIpKey = 0;
    joystickSteering.ownerSessionKey = 0;
    joystickSteering.armReleaseRequired = false;
    joystickSteering.mode = JOYSTICK_DISARMED;
    joystickSteering.x = 0;
    joystickSteering.speedPct = JOYSTICK_FIXED_SPEED_PCT;
    joystickSteering.lastCommandMs = now;
    joystickSteering.targetDeg = 0.0f;
    joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg;
    joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                                joystickSteering.unwrappedCorrectedDeg;
    joystickSteering.driveInhibitUntilMs = 0;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    clearJoystickReferenceRecoveryLocked();
    // Una pagina abierta y desarmada puede seguir enviando keep-alives. No
    // debe neutralizar una prueba diagnostica que use el mismo GPIO14.
    if (wasArmed) writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    message = "JOYSTICK_DISARMED";
    return true;
  }

  if (joystickSteering.armReleaseRequired) {
    joystickSteering.armed = false;
    joystickSteering.ownerIpKey = 0;
    joystickSteering.ownerSessionKey = 0;
    joystickSteering.x = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    message = "ARM_RELEASE_REQUIRED";
    return false;
  }

  if (servoTest.active || linearityTest.active || positionSequence.active) {
    xSemaphoreGive(servoStateMutex);
    message = "DIAGNOSTIC_MOTION_ACTIVE";
    return false;
  }
  const bool newlyArmed = !joystickSteering.armed;
  if (newlyArmed && !claimControl) {
    xSemaphoreGive(servoStateMutex);
    message = "CONTROL_CLAIM_REQUIRED";
    return false;
  }
  const bool ownedByOtherClient = !newlyArmed &&
      (joystickSteering.ownerIpKey != clientIpKey ||
       joystickSteering.ownerSessionKey != clientSessionKey);
  if (ownedByOtherClient) {
    xSemaphoreGive(servoStateMutex);
    message = "CONTROL_OWNED_BY_OTHER_CLIENT";
    return false;
  }
  // El primer armado exige el promedio de cinco lecturas reciente. MD solo es
  // obligatorio con las alarmas AS5600 activas; la frescura I2C nunca se omite.
  const bool magneticStatusAccepted =
      !joystickSteering.sensorAlarmsEnabled || (sensor.status & 0x20);
  if (newlyArmed &&
      (!sensor.filterReady || !magneticStatusAccepted || !sensor.lastGoodMs ||
      uint32_t(now - sensor.lastGoodMs) > JOYSTICK_SENSOR_TIMEOUT_MS ||
      !joystickSteering.unwrapReady || !joystickSteering.lastAcceptedMs ||
      joystickSteering.referenceRecoveryActive ||
      uint32_t(now - joystickSteering.lastAcceptedMs) >
          JOYSTICK_SENSOR_NEUTRAL_MS)) {
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    joystickSteering.armed = false;
    joystickSteering.mode = JOYSTICK_SENSOR_FAULT;
    latchJoystickSafetyLocked(JOYSTICK_SENSOR_FAULT);
    xSemaphoreGive(servoStateMutex);
    message = "AS5600_NOT_FRESH";
    return false;
  }

  const float previousTargetDeg = joystickSteering.targetDeg;
  const float previousErrorDeg = joystickSteering.errorDeg;
  if (newlyArmed) {
    // El cero es la vuelta equivalente mas cercana del cero magnetico
    // corregido. Asi nunca se pide recuperar vueltas enteras de una prueba
    // anterior al volver a armar el mando.
    joystickSteering.zeroAbsoluteDeg =
        roundf(joystickSteering.unwrappedCorrectedDeg / 360.0f) * 360.0f;
    joystickSteering.logicalAngleDeg =
        joystickSteering.unwrappedCorrectedDeg -
        joystickSteering.zeroAbsoluteDeg;
    joystickSteering.driveInhibitUntilMs = 0;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    clearJoystickReferenceRecoveryLocked();
    joystickSteering.ownerIpKey = clientIpKey;
    joystickSteering.ownerSessionKey = clientSessionKey;
  }
  joystickSteering.armed = true;
  joystickSteering.x = abs(x) <= JOYSTICK_DEADZONE ? 0 : x;
  joystickSteering.speedPct = JOYSTICK_FIXED_SPEED_PCT;
  joystickSteering.lastCommandMs = now;
  joystickSteering.targetDeg =
      JOYSTICK_X_DIRECTION * JOYSTICK_MAX_ANGLE_DEG *
      float(joystickSteering.x) / 100.0f;
  joystickSteering.targetAbsoluteDeg = joystickSteering.zeroAbsoluteDeg +
                                       joystickSteering.targetDeg;
  joystickSteering.errorDeg = joystickSteering.targetAbsoluteDeg -
                              joystickSteering.unwrappedCorrectedDeg;

  const bool targetChanged = newlyArmed ||
      fabsf(joystickSteering.targetDeg - previousTargetDeg) > 0.01f;
  if (targetChanged) {
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
  }

  const bool commandReversed = !newlyArmed &&
      previousErrorDeg * joystickSteering.errorDeg < 0.0f &&
      fabsf(previousErrorDeg) > POSITION_TOLERANCE_DEG &&
      fabsf(joystickSteering.errorDeg) > POSITION_TOLERANCE_DEG;
  if (commandReversed) {
    joystickSteering.driveInhibitUntilMs =
        now + JOYSTICK_REVERSAL_SETTLE_MS;
    joystickSteering.mode = JOYSTICK_REVERSAL_SETTLE;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  } else if (targetChanged) {
    joystickSteering.mode = JOYSTICK_ACTIVE;
  }
  xSemaphoreGive(servoStateMutex);
  message = "JOYSTICK_COMMAND_ACCEPTED";
  return true;
}

bool setJoystickSensorAlarmsEnabled(bool enabled, String &message) {
  if (!servoStateMutex) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }
  if (otaInProgress) {
    message = "OTA_IN_PROGRESS";
    return false;
  }
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }
  if (joystickSteering.armed || servoTest.active || linearityTest.active ||
      positionSequence.active) {
    xSemaphoreGive(servoStateMutex);
    message = "DISARM_BEFORE_SENSOR_SETTINGS";
    return false;
  }

  if (joystickSteering.sensorAlarmsEnabled != enabled) {
    joystickSteering.sensorAlarmsEnabled = enabled;
    // El cambio de criterio nunca hereda una referencia antigua. El filtro de
    // cinco muestras vuelve a sembrarla con GPIO14 en neutro.
    joystickSteering.unwrapReady = false;
    joystickSteering.lastAcceptedMs = 0;
    joystickSteering.mode = JOYSTICK_DISARMED;
    joystickSteering.x = 0;
    joystickSteering.targetDeg = 0.0f;
    joystickSteering.driveInhibitUntilMs = 0;
    joystickSteering.stableSamples = 0;
    joystickSteering.reacquireSamples = 0;
    clearJoystickReferenceRecoveryLocked();
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }
  xSemaphoreGive(servoStateMutex);
  message = enabled ? "AS5600_ALARMS_ENABLED" : "AS5600_ALARMS_DISABLED";
  return true;
}

bool startServoTest(String &message) {
  if (!REMOTE_DIAGNOSTIC_MOTION_ENABLED) {
    message = "REMOTE_DIAGNOSTIC_MOTION_DISABLED";
    return false;
  }
  const As5600Snapshot sensor = copySnapshot();
  const uint32_t now = millis();
  if (!servoPwmReady || !servoStateMutex) {
    message = "SERVO_PWM_NOT_READY";
    return false;
  }
  if (otaInProgress) {
    message = "OTA_IN_PROGRESS";
    return false;
  }
  if (copyLinearityTestState().active) {
    message = "LINEARITY_TEST_ALREADY_ACTIVE";
    return false;
  }
  if (copyPositionSequenceState().active) {
    message = "POSITION_SEQUENCE_ALREADY_ACTIVE";
    return false;
  }
  const JoystickSteeringState joystick = copyJoystickSteeringState();
  if (joystick.armed || joystick.armReleaseRequired) {
    message = "JOYSTICK_INTERLOCK_ACTIVE";
    return false;
  }
  if (!sensor.valid || !sensor.lastGoodMs ||
      uint32_t(now - sensor.lastGoodMs) > SERVO_SENSOR_TIMEOUT_MS) {
    stopServoTest(SERVO_TEST_SENSOR_TIMEOUT);
    message = "AS5600_NOT_FRESH";
    return false;
  }

  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }
  if (servoTest.active || linearityTest.active || positionSequence.active ||
      joystickSteering.armed || joystickSteering.armReleaseRequired) {
    xSemaphoreGive(servoStateMutex);
    message = "TEST_ALREADY_ACTIVE";
    return false;
  }
  portENTER_CRITICAL(&servoLogMux);
  servoTestLogCount = 0;
  servoTestLogDropped = 0;
  portEXIT_CRITICAL(&servoLogMux);
  servoTest.active = true;
  servoTest.phase = SERVO_TEST_NEUTRAL_START;
  servoTest.startedMs = now;
  servoTest.phaseStartedMs = now;
  servoTest.completedMs = 0;
  writeServoPulseLocked(SERVO_NEUTRAL_US);
  xSemaphoreGive(servoStateMutex);
  message = "SERVO_TEST_STARTED";
  return true;
}

bool startLinearityTest(String &message) {
  if (!REMOTE_DIAGNOSTIC_MOTION_ENABLED) {
    message = "REMOTE_DIAGNOSTIC_MOTION_DISABLED";
    return false;
  }
  const As5600Snapshot sensor = copySnapshot();
  const uint32_t now = millis();
  if (!servoPwmReady || !servoStateMutex) {
    message = "SERVO_PWM_NOT_READY";
    return false;
  }
  if (otaInProgress) {
    message = "OTA_IN_PROGRESS";
    return false;
  }
  const JoystickSteeringState joystick = copyJoystickSteeringState();
  if (copyServoTestState().active || copyPositionSequenceState().active ||
      joystick.armed || joystick.armReleaseRequired) {
    message = "OTHER_TEST_ALREADY_ACTIVE";
    return false;
  }
  if (!sensor.valid || !sensor.lastGoodMs ||
      uint32_t(now - sensor.lastGoodMs) > SERVO_SENSOR_TIMEOUT_MS) {
    stopLinearityTest(LINEARITY_SENSOR_TIMEOUT);
    message = "AS5600_NOT_FRESH";
    return false;
  }
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }
  if (servoTest.active || linearityTest.active || positionSequence.active ||
      joystickSteering.armed || joystickSteering.armReleaseRequired) {
    xSemaphoreGive(servoStateMutex);
    message = "TEST_ALREADY_ACTIVE";
    return false;
  }

  portENTER_CRITICAL(&linearityLogMux);
  linearityTestLogCount = 0;
  linearityTestLogDropped = 0;
  portEXIT_CRITICAL(&linearityLogMux);
  linearityTest = {};
  linearityTest.active = true;
  linearityTest.phase = LINEARITY_NEUTRAL_START;
  linearityTest.pulseUs = SERVO_NEUTRAL_US;
  linearityTest.startedMs = now;
  linearityTest.phaseStartedMs = now;
  linearityTest.lastAcceptedMs = now;
  linearityTest.lastAcceptedRaw = sensor.rawAngle;
  linearityTest.unwrapReady = true;
  writeServoPulseLocked(SERVO_NEUTRAL_US);
  xSemaphoreGive(servoStateMutex);
  message = "LINEARITY_TEST_STARTED";
  return true;
}

bool startPositionSequence(String &message, bool fineProbe) {
  if (!REMOTE_DIAGNOSTIC_MOTION_ENABLED) {
    message = "REMOTE_DIAGNOSTIC_MOTION_DISABLED";
    return false;
  }
  const As5600Snapshot sensor = copySnapshot();
  const uint32_t now = millis();
  if (!servoPwmReady || !servoStateMutex) {
    message = "SERVO_PWM_NOT_READY";
    return false;
  }
  if (otaInProgress) {
    message = "OTA_IN_PROGRESS";
    return false;
  }
  const JoystickSteeringState joystick = copyJoystickSteeringState();
  if (copyServoTestState().active || copyLinearityTestState().active ||
      joystick.armed || joystick.armReleaseRequired) {
    message = "OTHER_TEST_ALREADY_ACTIVE";
    return false;
  }
  if (!sensor.valid || !sensor.lastGoodMs ||
      uint32_t(now - sensor.lastGoodMs) > SERVO_SENSOR_TIMEOUT_MS) {
    stopPositionSequence(POSITION_SENSOR_TIMEOUT);
    message = "AS5600_NOT_FRESH";
    return false;
  }
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) {
    message = "SERVO_STATE_UNAVAILABLE";
    return false;
  }
  if (servoTest.active || linearityTest.active || positionSequence.active ||
      joystickSteering.armed || joystickSteering.armReleaseRequired) {
    xSemaphoreGive(servoStateMutex);
    message = "TEST_ALREADY_ACTIVE";
    return false;
  }

  positionSequence = {};
  positionSequence.active = true;
  positionSequence.phase = fineProbe ? POSITION_RETURN_MOVE
                                     : POSITION_HOME_MOVE;
  positionSequence.startedMs = now;
  positionSequence.phaseStartedMs = now;
  positionSequence.lastAcceptedMs = now;
  positionSequence.unwrapReady = true;
  positionSequence.lastCorrectedCyclicDeg = sensor.correctedAngleDeg;
  positionSequence.unwrappedCorrectedDeg = sensor.correctedAngleDeg;
  if (fineProbe) {
    positionSequence.homeAbsoluteDeg = sensor.correctedAngleDeg;
    positionSequence.targetAbsoluteDeg = sensor.correctedAngleDeg - 5.0f;
    positionSequence.logicalPositionDeg = 0.0f;
    positionSequence.errorDeg = -5.0f;
  } else {
    positionSequence.homeAbsoluteDeg =
        roundf(sensor.correctedAngleDeg / 360.0f) * 360.0f;
    positionSequence.targetAbsoluteDeg = positionSequence.homeAbsoluteDeg;
    positionSequence.logicalPositionDeg =
        sensor.correctedAngleDeg - positionSequence.homeAbsoluteDeg;
    positionSequence.errorDeg = -positionSequence.logicalPositionDeg;
  }
  writeServoPulseLocked(positionDrivePulseUs(positionSequence.errorDeg));
  xSemaphoreGive(servoStateMutex);
  message = fineProbe ? "POSITION_FINE_PROBE_STARTED"
                      : "POSITION_SEQUENCE_STARTED";
  return true;
}

void serviceServoTest() {
  if (!servoStateMutex) return;
  const uint32_t now = millis();
  const As5600Snapshot sensor = copySnapshot();
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!servoTest.active) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (!sensor.lastGoodMs ||
      uint32_t(now - sensor.lastGoodMs) > SERVO_SENSOR_TIMEOUT_MS) {
    servoTest.active = false;
    servoTest.phase = SERVO_TEST_SENSOR_TIMEOUT;
    servoTest.phaseStartedMs = now;
    servoTest.completedMs = now;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const uint32_t elapsed = uint32_t(now - servoTest.startedMs);
  ServoTestPhase nextPhase;
  uint16_t nextPulseUs;
  if (elapsed < 2000) {
    nextPhase = SERVO_TEST_NEUTRAL_START;
    nextPulseUs = SERVO_NEUTRAL_US;
  } else if (elapsed < 6000) {
    nextPhase = SERVO_TEST_2000_US;
    nextPulseUs = SERVO_TEST_POSITIVE_US;
  } else if (elapsed < 8000) {
    nextPhase = SERVO_TEST_NEUTRAL_MIDDLE;
    nextPulseUs = SERVO_NEUTRAL_US;
  } else if (elapsed < 12000) {
    nextPhase = SERVO_TEST_1000_US;
    nextPulseUs = SERVO_TEST_NEGATIVE_US;
  } else if (elapsed < 14000) {
    nextPhase = SERVO_TEST_NEUTRAL_END;
    nextPulseUs = SERVO_NEUTRAL_US;
  } else {
    servoTest.active = false;
    servoTest.phase = SERVO_TEST_COMPLETE;
    servoTest.phaseStartedMs = now;
    servoTest.completedMs = now;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (nextPhase != servoTest.phase) {
    servoTest.phase = nextPhase;
    servoTest.phaseStartedMs = now;
    writeServoPulseLocked(nextPulseUs);
  }
  xSemaphoreGive(servoStateMutex);
}

void serviceLinearityTest() {
  if (!servoStateMutex) return;
  const uint32_t now = millis();
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!linearityTest.active) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const uint32_t phaseElapsed = uint32_t(now - linearityTest.phaseStartedMs);
  if (!linearityTest.lastAcceptedMs ||
      uint32_t(now - linearityTest.lastAcceptedMs) >
          SERVO_SENSOR_TIMEOUT_MS) {
    linearityTest.active = false;
    linearityTest.phase = LINEARITY_SENSOR_TIMEOUT;
    linearityTest.phaseStartedMs = now;
    linearityTest.completedMs = now;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }
  if (phaseElapsed > LINEARITY_PHASE_TIMEOUT_MS) {
    linearityTest.active = false;
    linearityTest.phase = LINEARITY_PHASE_TIMEOUT;
    linearityTest.phaseStartedMs = now;
    linearityTest.completedMs = now;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  switch (linearityTest.phase) {
    case LINEARITY_NEUTRAL_START:
      if (phaseElapsed >= 2000) {
        linearityTest.phase = LINEARITY_LEFT_2000_US;
        linearityTest.phaseStartedMs = now;
        linearityTest.targetConfirmSamples = 0;
        writeServoPulseLocked(SERVO_TEST_POSITIVE_US);
      }
      break;
    case LINEARITY_LEFT_2000_US:
      if (linearityTest.targetConfirmSamples >=
          LINEARITY_TARGET_CONFIRM_SAMPLES) {
        linearityTest.phase = LINEARITY_NEUTRAL_MIDDLE;
        linearityTest.phaseStartedMs = now;
        linearityTest.targetConfirmSamples = 0;
        writeServoPulseLocked(SERVO_NEUTRAL_US);
      }
      break;
    case LINEARITY_NEUTRAL_MIDDLE:
      if (phaseElapsed >= 3000) {
        linearityTest.phase = LINEARITY_RIGHT_1040_US;
        linearityTest.phaseStartedMs = now;
        // El origen de regreso se fija despues de la pausa para incluir en la
        // ida, pero no en la vuelta, cualquier inercia al neutralizar.
        linearityTest.phaseOriginRaw = linearityTest.unwrappedRaw;
        linearityTest.targetConfirmSamples = 0;
        writeServoPulseLocked(LINEARITY_RETURN_US);
      }
      break;
    case LINEARITY_RIGHT_1040_US:
      if (linearityTest.targetConfirmSamples >=
          LINEARITY_TARGET_CONFIRM_SAMPLES) {
        linearityTest.phase = LINEARITY_NEUTRAL_END;
        linearityTest.phaseStartedMs = now;
        linearityTest.targetConfirmSamples = 0;
        writeServoPulseLocked(SERVO_NEUTRAL_US);
      }
      break;
    case LINEARITY_NEUTRAL_END:
      if (phaseElapsed >= 2000) {
        linearityTest.active = false;
        linearityTest.phase = LINEARITY_COMPLETE;
        linearityTest.phaseStartedMs = now;
        linearityTest.completedMs = now;
        writeServoPulseLocked(SERVO_NEUTRAL_US);
      }
      break;
    default:
      linearityTest.active = false;
      linearityTest.phase = LINEARITY_STOPPED;
      linearityTest.phaseStartedMs = now;
      linearityTest.completedMs = now;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      break;
  }
  xSemaphoreGive(servoStateMutex);
}

void servicePositionSequence() {
  if (!servoStateMutex) return;
  const uint32_t now = millis();
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!positionSequence.active) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (uint32_t(now - positionSequence.startedMs) >
      POSITION_SEQUENCE_TIMEOUT_MS) {
    positionSequence.active = false;
    positionSequence.phase = POSITION_SEQUENCE_TIMEOUT;
    positionSequence.phaseStartedMs = now;
    positionSequence.completedMs = now;
    positionSequence.stableSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (!positionSequence.lastAcceptedMs ||
      uint32_t(now - positionSequence.lastAcceptedMs) >
          SERVO_SENSOR_TIMEOUT_MS) {
    positionSequence.active = false;
    positionSequence.phase = POSITION_SENSOR_TIMEOUT;
    positionSequence.phaseStartedMs = now;
    positionSequence.completedMs = now;
    positionSequence.stableSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (uint32_t(now - positionSequence.lastAcceptedMs) >
      POSITION_SENSOR_NEUTRAL_MS) {
    // Una muestra de control de mas de 100 ms nunca conserva movimiento.
    // Se permite recuperar el bus hasta el aborto duro de 400 ms.
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  const uint32_t phaseElapsed =
      uint32_t(now - positionSequence.phaseStartedMs);
  if (positionPhaseIsMove(positionSequence.phase) &&
      phaseElapsed > POSITION_MOVE_TIMEOUT_MS) {
    positionSequence.active = false;
    positionSequence.phase = POSITION_MOVE_TIMEOUT;
    positionSequence.phaseStartedMs = now;
    positionSequence.completedMs = now;
    positionSequence.stableSamples = 0;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (!positionPhaseIsDwell(positionSequence.phase) ||
      phaseElapsed < POSITION_DWELL_MS) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  PositionSequencePhase nextPhase = POSITION_COMPLETE;
  if (positionSequence.phase == POSITION_HOME_DWELL) {
    positionSequence.targetStep = 1;
    nextPhase = POSITION_FORWARD_MOVE;
  } else if (positionSequence.phase == POSITION_FORWARD_DWELL) {
    if (positionSequence.targetStep < POSITION_MAX_STEP) {
      positionSequence.targetStep++;
      nextPhase = POSITION_FORWARD_MOVE;
    } else {
      positionSequence.targetStep = POSITION_MAX_STEP - 1;
      nextPhase = POSITION_RETURN_MOVE;
    }
  } else if (positionSequence.phase == POSITION_RETURN_DWELL) {
    if (positionSequence.targetStep > 0) {
      positionSequence.targetStep--;
      nextPhase = POSITION_RETURN_MOVE;
    } else {
      positionSequence.active = false;
      positionSequence.phase = POSITION_COMPLETE;
      positionSequence.phaseStartedMs = now;
      positionSequence.completedMs = now;
      positionSequence.transitions++;
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      xSemaphoreGive(servoStateMutex);
      return;
    }
  }

  positionSequence.phase = nextPhase;
  positionSequence.phaseStartedMs = now;
  positionSequence.targetAbsoluteDeg = positionSequence.homeAbsoluteDeg +
      POSITION_STEP_DEG * positionSequence.targetStep;
  positionSequence.errorDeg = positionSequence.targetAbsoluteDeg -
                              positionSequence.unwrappedCorrectedDeg;
  positionSequence.stableSamples = 0;
  positionSequence.transitions++;
  writeServoPulseLocked(positionDrivePulseUs(positionSequence.errorDeg));
  xSemaphoreGive(servoStateMutex);
}

void serviceJoystickSteering() {
  if (!servoStateMutex) return;
  const uint32_t now = millis();
  if (xSemaphoreTake(servoStateMutex, portMAX_DELAY) != pdTRUE) return;
  if (!joystickSteering.armed) {
    xSemaphoreGive(servoStateMutex);
    return;
  }

  if (enforceJoystickCommandAgeLocked(now) ||
      enforceJoystickMissingSensorLocked(now)) {
    xSemaphoreGive(servoStateMutex);
    return;
  }
  if (!joystickSteering.speedPct) {
    joystickSteering.mode = JOYSTICK_SPEED_ZERO;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
  }
  xSemaphoreGive(servoStateMutex);
}

constexpr gpio_num_t sdaGpio() {
  return static_cast<gpio_num_t>(AS5600_SDA_PIN);
}

constexpr gpio_num_t sclGpio() {
  return static_cast<gpio_num_t>(AS5600_SCL_PIN);
}

inline void softI2cDelay() {
  esp_rom_delay_us(AS5600_HALF_PERIOD_US);
}

bool setSoftI2cLine(gpio_num_t pin, uint32_t level, uint16_t &errorCode) {
  if (gpio_set_level(pin, level) == ESP_OK) return true;
  errorCode = I2C_ERROR_GPIO_ACCESS;
  return false;
}

bool releaseScl(uint16_t &errorCode, uint16_t timeoutError) {
  if (!setSoftI2cLine(sclGpio(), 1, errorCode)) return false;
  const int64_t startedUs = esp_timer_get_time();
  while (!gpio_get_level(sclGpio())) {
    if (esp_timer_get_time() - startedUs >= AS5600_EDGE_TIMEOUT_US) {
      errorCode = timeoutError;
      return false;
    }
    esp_rom_delay_us(1);
  }
  return true;
}

bool softI2cStop(uint16_t &errorCode) {
  if (!setSoftI2cLine(sdaGpio(), 0, errorCode)) return false;
  softI2cDelay();
  if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
  softI2cDelay();
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  softI2cDelay();
  if (!gpio_get_level(sclGpio()) || !gpio_get_level(sdaGpio())) {
    errorCode = I2C_ERROR_STOP;
    return false;
  }
  return true;
}

bool softI2cRecover(uint16_t &errorCode) {
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  if (!releaseScl(errorCode, I2C_ERROR_SCL_IDLE_TIMEOUT)) return false;
  softI2cDelay();

  if (!gpio_get_level(sdaGpio())) {
    // Hasta nueve pulsos permiten que un esclavo interrumpido termine el byte
    // que estaba enviando y libere SDA.
    for (uint8_t pulse = 0; pulse < 9; ++pulse) {
      if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
      softI2cDelay();
      if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
      softI2cDelay();
    }
    uint16_t stopError = I2C_OK;
    if (!softI2cStop(stopError)) {
      errorCode = gpio_get_level(sdaGpio()) ? stopError
                                            : I2C_ERROR_SDA_STUCK;
      return false;
    }
  }

  if (!gpio_get_level(sclGpio())) {
    errorCode = I2C_ERROR_SCL_IDLE_TIMEOUT;
    return false;
  }
  if (!gpio_get_level(sdaGpio())) {
    errorCode = I2C_ERROR_SDA_STUCK;
    return false;
  }
  errorCode = I2C_OK;
  return true;
}

bool softI2cStart(uint16_t &errorCode) {
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  softI2cDelay();
  if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
  if (!gpio_get_level(sdaGpio())) {
    errorCode = I2C_ERROR_SDA_STUCK;
    return false;
  }
  softI2cDelay();
  if (!setSoftI2cLine(sdaGpio(), 0, errorCode)) return false;
  softI2cDelay();
  if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
  softI2cDelay();
  return true;
}

bool softI2cWriteByte(uint8_t value, bool &acknowledged,
                      uint16_t &errorCode) {
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    if (!setSoftI2cLine(sdaGpio(), (value & mask) ? 1 : 0, errorCode))
      return false;
    softI2cDelay();
    if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
    softI2cDelay();
    if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
  }

  // El noveno pulso pertenece al esclavo: SDA baja significa ACK.
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  softI2cDelay();
  if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
  softI2cDelay();
  acknowledged = !gpio_get_level(sdaGpio());
  if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
  return true;
}

bool softI2cReadByte(uint8_t &value, bool acknowledgeMore,
                     uint16_t &errorCode) {
  value = 0;
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    softI2cDelay();
    if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
    softI2cDelay();
    value = uint8_t((value << 1) | gpio_get_level(sdaGpio()));
    if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
  }

  // ACK para pedir otro byte; NACK para indicar el ultimo byte.
  if (!setSoftI2cLine(sdaGpio(), acknowledgeMore ? 0 : 1, errorCode))
    return false;
  softI2cDelay();
  if (!releaseScl(errorCode, I2C_ERROR_SCL_EDGE_TIMEOUT)) return false;
  softI2cDelay();
  if (!setSoftI2cLine(sclGpio(), 0, errorCode)) return false;
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode)) return false;
  return true;
}

bool softI2cBegin(uint16_t &errorCode) {
  gpio_config_t config = {};
  config.pin_bit_mask = (1ULL << AS5600_SDA_PIN) |
                        (1ULL << AS5600_SCL_PIN);
  config.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) {
    errorCode = I2C_ERROR_GPIO_CONFIG;
    return false;
  }
  if (!setSoftI2cLine(sdaGpio(), 1, errorCode) ||
      !setSoftI2cLine(sclGpio(), 1, errorCode))
    return false;
  esp_rom_delay_us(100);

  // El GPIO queda configurado aunque el cableado este reteniendo una linea.
  // Cada muestra vuelve a intentar la recuperacion, sin exigir otro reset.
  softI2cRecover(errorCode);
  return true;
}

void softI2cAbortTransaction() {
  uint16_t ignoredError = I2C_OK;
  softI2cStop(ignoredError);
  setSoftI2cLine(sdaGpio(), 1, ignoredError);
  setSoftI2cLine(sclGpio(), 1, ignoredError);
}

bool readRegisters(uint8_t firstRegister, uint8_t *data, size_t length,
                   uint16_t &errorCode) {
  if (!data || !length) {
    errorCode = I2C_ERROR_ARGUMENT;
    return false;
  }
  if (!softI2cRecover(errorCode)) return false;
  if (!softI2cStart(errorCode)) {
    if (errorCode == I2C_OK) errorCode = I2C_ERROR_START;
    softI2cAbortTransaction();
    return false;
  }

  bool acknowledged = false;
  if (!softI2cWriteByte(uint8_t(AS5600_ADDRESS << 1), acknowledged,
                        errorCode)) {
    softI2cAbortTransaction();
    return false;
  }
  if (!acknowledged) {
    errorCode = I2C_ERROR_ADDRESS_WRITE_NACK;
    softI2cAbortTransaction();
    return false;
  }

  if (!softI2cWriteByte(firstRegister, acknowledged, errorCode)) {
    softI2cAbortTransaction();
    return false;
  }
  if (!acknowledged) {
    errorCode = I2C_ERROR_REGISTER_NACK;
    softI2cAbortTransaction();
    return false;
  }

  if (!softI2cStart(errorCode)) {
    if (errorCode == I2C_OK) errorCode = I2C_ERROR_REPEATED_START;
    softI2cAbortTransaction();
    return false;
  }
  if (!softI2cWriteByte(uint8_t((AS5600_ADDRESS << 1) | 1), acknowledged,
                        errorCode)) {
    softI2cAbortTransaction();
    return false;
  }
  if (!acknowledged) {
    errorCode = I2C_ERROR_ADDRESS_READ_NACK;
    softI2cAbortTransaction();
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    if (!softI2cReadByte(data[i], i + 1 < length, errorCode)) {
      if (errorCode == I2C_OK) errorCode = I2C_ERROR_READ;
      softI2cAbortTransaction();
      return false;
    }
  }
  if (!softI2cStop(errorCode)) {
    if (errorCode == I2C_OK) errorCode = I2C_ERROR_STOP;
    return false;
  }
  errorCode = I2C_OK;
  return true;
}

float normalizeAngle(float angleDeg) {
  angleDeg = fmodf(angleDeg, 360.0f);
  if (angleDeg < 0.0f) angleDeg += 360.0f;
  if (angleDeg >= 360.0f) angleDeg -= 360.0f;
  return angleDeg;
}

float circularDistance(float firstDeg, float secondDeg) {
  float difference = normalizeAngle(firstDeg) - normalizeAngle(secondDeg);
  if (difference > 180.0f) difference -= 360.0f;
  if (difference < -180.0f) difference += 360.0f;
  return fabsf(difference);
}

float quantizeDisplayAngle(float angleDeg) {
  return normalizeAngle(roundf(normalizeAngle(angleDeg) /
                               DISPLAY_QUANTUM_DEG) *
                        DISPLAY_QUANTUM_DEG);
}

bool robustCircularMean(const float *anglesDeg, uint8_t count,
                        float &meanAngleDeg, uint8_t &inlierCount) {
  if (!anglesDeg || !count || count > FILTER_SAMPLE_COUNT) return false;

  // Se desenvuelven localmente alrededor de la primera muestra. Como el grupo
  // fisico ocupa pocos grados, una mediana lineal ya es circularmente correcta
  // aun cuando el conjunto cruza 359 -> 0.
  float unwrapped[FILTER_SAMPLE_COUNT] = {};
  float sorted[FILTER_SAMPLE_COUNT] = {};
  const float anchorDeg = anglesDeg[0];
  for (uint8_t index = 0; index < count; ++index) {
    unwrapped[index] = anchorDeg +
        signedCircularDelta(anglesDeg[index], anchorDeg);
    sorted[index] = unwrapped[index];
    for (uint8_t position = index; position > 0 &&
         sorted[position] < sorted[position - 1]; --position) {
      const float temporary = sorted[position - 1];
      sorted[position - 1] = sorted[position];
      sorted[position] = temporary;
    }
  }

  const float medianDeg = count & 1
      ? sorted[count / 2]
      : 0.5f * (sorted[count / 2 - 1] + sorted[count / 2]);
  float sumDeg = 0.0f;
  inlierCount = 0;
  for (uint8_t index = 0; index < count; ++index) {
    if (fabsf(unwrapped[index] - medianDeg) > FILTER_OUTLIER_DEG) continue;
    sumDeg += unwrapped[index];
    inlierCount++;
  }
  if (inlierCount < FILTER_MIN_INLIER_COUNT) return false;
  meanAngleDeg = normalizeAngle(sumDeg / inlierCount);
  return true;
}

void i2cTask(void *) {
  delay(3000);  // HTTP y OTA quedan disponibles antes de tocar el bus.
  As5600Snapshot state = copySnapshot();
  uint16_t beginError = I2C_OK;
  state.busStarted = softI2cBegin(beginError);
  state.lastError = beginError;
  if (state.busStarted) {
    // AGC y magnitud se toman una vez al iniciar. La advertencia MD/ML/MH
    // sigue actualizándose cada 50 ms con el registro STATUS; sacar esta
    // segunda transacción de la ruta periódica mantiene exacta la ventana.
    uint8_t diagnostics[3] = {};
    uint16_t diagnosticsError = I2C_OK;
    state.diagnosticsValid =
        readRegisters(0x1A, diagnostics, sizeof(diagnostics),
                      diagnosticsError);
    if (state.diagnosticsValid) {
      state.agc = diagnostics[0];
      state.magnitude =
          (uint16_t(diagnostics[1] & 0x0F) << 8) | diagnostics[2];
    }
  }
  publishSnapshot(state);

  TickType_t lastWake = xTaskGetTickCount();
  float filterAngles[FILTER_SAMPLE_COUNT] = {};
  uint32_t filterSampleTimes[FILTER_SAMPLE_COUNT] = {};
  uint8_t filterWriteIndex = 0;
  uint8_t filterCount = 0;
  uint8_t diagnosticDivider = 0;

  for (;;) {
    if (xTaskDelayUntil(&lastWake,
                        pdMS_TO_TICKS(FILTER_SAMPLE_PERIOD_MS)) == pdFALSE) {
      state.scheduleOverruns++;
      // No se permiten muestras de "recuperacion" pegadas: el siguiente
      // slot vuelve a quedar exactamente 50 ms despues del instante actual.
      lastWake = xTaskGetTickCount();
    }
    if (otaInProgress || !state.busStarted) {
      filterWriteIndex = 0;
      filterCount = 0;
      diagnosticDivider = 0;
      state.filterReady = false;
      state.filterSamples = 0;
      state.filterInliers = 0;
      state.filterSpanMs = 0;
      publishSnapshot(state);
      delay(50);
      lastWake = xTaskGetTickCount();
      continue;
    }

    uint8_t primary[3] = {};
    uint16_t errorCode = 0;
    state.lastSampleMs = millis();
    state.samples++;
    if (!readRegisters(0x0B, primary, sizeof(primary), errorCode)) {
      state.valid = false;
      state.lastError = errorCode;
      state.errors++;
    } else {
      state.valid = true;
      state.lastError = 0;
      state.status = primary[0] & 0x38;
      state.rawAngle = (uint16_t(primary[1] & 0x0F) << 8) | primary[2];
      state.angleDeg = 360.0f * state.rawAngle / 4096.0f;
      state.correctedAngleDeg = as5600CorrectedAngleDeg(state.rawAngle);
      state.successes++;

      // Una pausa larga invalida el historial: no se mezclan posiciones
      // anteriores a la desconexion con las lecturas recuperadas.
      if (state.lastGoodMs &&
          uint32_t(state.lastSampleMs - state.lastGoodMs) >
              JOYSTICK_SENSOR_TIMEOUT_MS) {
        filterWriteIndex = 0;
        filterCount = 0;
        state.filterReady = false;
        state.filterInliers = 0;
        state.filterSpanMs = 0;
      }
      state.lastGoodMs = state.lastSampleMs;

      // Ventana circular deslizante: siempre son las cinco ultimas lecturas
      // I2C validas corregidas por la LUT, incluso al cruzar 359 -> 0 grados.
      filterAngles[filterWriteIndex] = state.correctedAngleDeg;
      filterSampleTimes[filterWriteIndex] = state.lastSampleMs;
      filterWriteIndex = (filterWriteIndex + 1) % FILTER_SAMPLE_COUNT;
      if (filterCount < FILTER_SAMPLE_COUNT) filterCount++;
      state.filterSamples = filterCount;

      if (filterCount == FILTER_SAMPLE_COUNT) {
        // Con la ventana llena, writeIndex apunta a la muestra mas antigua.
        state.filterSpanMs = uint32_t(
            state.lastSampleMs - filterSampleTimes[filterWriteIndex]);
        float meanAngle = 0.0f;
        uint8_t inlierCount = 0;
        if (state.filterSpanMs <= FILTER_MAX_SPAN_MS &&
            robustCircularMean(filterAngles, FILTER_SAMPLE_COUNT, meanAngle,
                               inlierCount)) {
          state.filterInliers = inlierCount;
          state.meanAngleDeg = meanAngle;
          if (!state.filterReady) {
            state.filterDeltaDeg = 0.0f;
            state.displayAngleDeg = quantizeDisplayAngle(meanAngle);
            state.filterReady = true;
            state.displayUpdates++;
          } else {
            state.filterDeltaDeg =
                circularDistance(meanAngle, state.displayAngleDeg);
            if (state.filterDeltaDeg > DISPLAY_UPDATE_THRESHOLD_DEG) {
              state.displayAngleDeg = quantizeDisplayAngle(meanAngle);
              state.displayUpdates++;
            }
          }
        } else {
          state.filterInliers = inlierCount;
          state.filterReady = false;
        }
      }
    }

    // Diagnosticos y joystick reciben cada promedio nuevo a 20 Hz. Las rutas
    // de movimiento diagnostico continúan deshabilitadas en este firmware.
    if (++diagnosticDivider >= 1) {
      diagnosticDivider = 0;
      recordServoTestSample(state.lastSampleMs, state.valid, state.rawAngle);
      processLinearitySample(state.lastSampleMs, state.valid, state.rawAngle,
                             state.status);
      processPositionSequenceSample(state.lastSampleMs, state.valid,
                                    state.rawAngle);
    }
    processJoystickSteeringSample(
        state.lastSampleMs, state.valid, state.filterReady,
        state.meanAngleDeg, state.status);
    if (!state.lastGoodMs ||
        uint32_t(state.lastSampleMs - state.lastGoodMs) >
            SERVO_SENSOR_TIMEOUT_MS) {
      stopServoTest(SERVO_TEST_SENSOR_TIMEOUT, true);
    }

    if (!state.valid && state.lastGoodMs &&
        uint32_t(state.lastSampleMs - state.lastGoodMs) >
            JOYSTICK_SENSOR_TIMEOUT_MS) {
      filterWriteIndex = 0;
      filterCount = 0;
      state.filterReady = false;
      state.filterSamples = 0;
      state.filterInliers = 0;
      state.filterSpanMs = 0;
    }
    publishSnapshot(state);
  }
}

const char *softI2cErrorName(uint16_t errorCode) {
  switch (errorCode) {
    case I2C_OK: return "OK";
    case I2C_ERROR_ARGUMENT: return "ARGUMENT";
    case I2C_ERROR_GPIO_CONFIG: return "GPIO_CONFIG";
    case I2C_ERROR_GPIO_ACCESS: return "GPIO_ACCESS";
    case I2C_ERROR_SCL_IDLE_TIMEOUT: return "SCL_STUCK_LOW";
    case I2C_ERROR_SCL_EDGE_TIMEOUT: return "SCL_EDGE_TIMEOUT";
    case I2C_ERROR_SDA_STUCK: return "SDA_STUCK_LOW";
    case I2C_ERROR_START: return "START_FAILED";
    case I2C_ERROR_ADDRESS_WRITE_NACK: return "ADDRESS_WRITE_NACK";
    case I2C_ERROR_REGISTER_NACK: return "REGISTER_NACK";
    case I2C_ERROR_REPEATED_START: return "REPEATED_START_FAILED";
    case I2C_ERROR_ADDRESS_READ_NACK: return "ADDRESS_READ_NACK";
    case I2C_ERROR_READ: return "READ_FAILED";
    case I2C_ERROR_STOP: return "STOP_FAILED";
    default: return "UNKNOWN";
  }
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

void copyServoLogMetadata(size_t &count, uint32_t &dropped) {
  portENTER_CRITICAL(&servoLogMux);
  count = servoTestLogCount;
  dropped = servoTestLogDropped;
  portEXIT_CRITICAL(&servoLogMux);
}

bool servoTestLogCsv(String &out) {
  size_t count = 0;
  uint32_t dropped = 0;
  copyServoLogMetadata(count, dropped);
  ServoTestSample *copy = nullptr;
  if (count) {
    copy = static_cast<ServoTestSample *>(
        malloc(count * sizeof(ServoTestSample)));
    if (!copy) {
      out = "ERROR:OUT_OF_MEMORY\n";
      return false;
    }
  }

  portENTER_CRITICAL(&servoLogMux);
  // Si otro POST inicio una prueba entre ambas secciones, nunca se copia mas
  // de lo reservado. Si solo se agregaron muestras, el CSV es un prefijo
  // coherente del registro que existia al abrir la descarga.
  if (servoTestLogCount < count) count = servoTestLogCount;
  if (count) memcpy(copy, servoTestLog, count * sizeof(ServoTestSample));
  dropped = servoTestLogDropped;
  portEXIT_CRITICAL(&servoLogMux);

  out.reserve(96 + count * 48);
  out = "time_ms,raw_angle,servo_us,phase,valid\n";
  for (size_t i = 0; i < count; ++i) {
    const ServoTestSample &sample = copy[i];
    out += String(sample.elapsedMs);
    out += ',';
    if (sample.valid)
      out += String(sample.rawAngle);
    else
      out += "-1";
    out += ',' + String(sample.servoUs);
    out += ',' + String(servoTestPhaseName(
                         static_cast<ServoTestPhase>(sample.phase)));
    out += ',' + String(sample.valid ? 1 : 0) + '\n';
  }
  if (dropped) out += "# dropped," + String(dropped) + "\n";
  free(copy);
  return true;
}

void copyLinearityLogMetadata(size_t &count, uint32_t &dropped) {
  portENTER_CRITICAL(&linearityLogMux);
  count = linearityTestLogCount;
  dropped = linearityTestLogDropped;
  portEXIT_CRITICAL(&linearityLogMux);
}

float rawTravelDegrees(int32_t rawTravel) {
  return float(rawTravel) * 360.0f / 4096.0f;
}

float linearityPhaseTravelDegrees(const LinearityTestState &test) {
  if (test.phase == LINEARITY_RIGHT_1040_US ||
      test.phase == LINEARITY_NEUTRAL_END ||
      test.phase == LINEARITY_COMPLETE)
    return rawTravelDegrees(test.unwrappedRaw - test.phaseOriginRaw);
  return rawTravelDegrees(test.unwrappedRaw);
}

float linearityProgressPercent(const LinearityTestState &test) {
  float progress = 0.0f;
  switch (test.phase) {
    case LINEARITY_LEFT_2000_US:
      progress = -100.0f * test.unwrappedRaw /
                 float(LINEARITY_SEVEN_TURNS_RAW);
      break;
    case LINEARITY_NEUTRAL_MIDDLE:
      progress = 100.0f;
      break;
    case LINEARITY_RIGHT_1040_US:
    case LINEARITY_NEUTRAL_END:
      progress = 100.0f * (test.unwrappedRaw - test.phaseOriginRaw) /
                 float(LINEARITY_SEVEN_TURNS_RAW);
      break;
    case LINEARITY_COMPLETE:
      progress = 100.0f;
      break;
    default:
      progress = 0.0f;
      break;
  }
  return constrain(progress, 0.0f, 100.0f);
}

void streamLinearityLogCsv() {
  size_t count = 0;
  uint32_t dropped = 0;
  copyLinearityLogMetadata(count, dropped);
  const LinearityTestState test = copyLinearityTestState();
  const int32_t returnOriginRaw = test.phaseOriginRaw;

  server.sendHeader("Cache-Control", "no-store, max-age=0");
  server.sendHeader("Content-Disposition",
                    "attachment; filename=as5600-linearity.csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent(
      "time_ms,raw_angle,angle_deg,corrected_angle,servo_us,phase,"
      "i2c_valid,valid,glitch,status,unwrapped_deg,"
      "phase_displacement_deg\n");

  String chunk;
  chunk.reserve(1400);
  char line[224];
  for (size_t i = 0; i < count; ++i) {
    LinearityTestSample sample = {};
    portENTER_CRITICAL(&linearityLogMux);
    sample = linearityTestLog[i];
    portEXIT_CRITICAL(&linearityLogMux);
    const LinearityTestPhase phase =
        static_cast<LinearityTestPhase>(sample.phase);
    const bool i2cValid = sample.flags & LINEARITY_SAMPLE_I2C_VALID;
    const bool accepted = sample.flags & LINEARITY_SAMPLE_ACCEPTED;
    const bool glitch = sample.flags & LINEARITY_SAMPLE_GLITCH;
    const float angleDeg = i2cValid
        ? 360.0f * sample.rawAngle / 4096.0f
        : -1.0f;
    const float correctedAngleDeg = i2cValid
        ? as5600CorrectedAngleDeg(sample.rawAngle)
        : -1.0f;
    int32_t phaseRaw = sample.unwrappedRaw;
    if (phase == LINEARITY_RIGHT_1040_US ||
        phase == LINEARITY_NEUTRAL_END || phase == LINEARITY_COMPLETE)
      phaseRaw -= returnOriginRaw;
    snprintf(line, sizeof(line),
             "%lu,%d,%.4f,%.4f,%u,%s,%u,%u,%u,%u,%.4f,%.4f\n",
             static_cast<unsigned long>(sample.elapsedMs),
             i2cValid ? int(sample.rawAngle) : -1, angleDeg,
              correctedAngleDeg,
              static_cast<unsigned>(linearityPhasePulseUs(phase)),
             linearityTestPhaseName(phase), i2cValid ? 1U : 0U,
             accepted ? 1U : 0U, glitch ? 1U : 0U,
             static_cast<unsigned>(sample.flags & 0x38),
             rawTravelDegrees(sample.unwrappedRaw),
             rawTravelDegrees(phaseRaw));
    chunk += line;
    if (chunk.length() >= 1100) {
      server.sendContent(chunk);
      chunk = "";
      delay(0);
    }
  }
  if (dropped) {
    snprintf(line, sizeof(line), "# dropped,%lu\n",
             static_cast<unsigned long>(dropped));
    chunk += line;
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");
}

String statusText() {
  As5600Snapshot s = copySnapshot();
  ServoTestState servo = copyServoTestState();
  LinearityTestState linearity = copyLinearityTestState();
  PositionSequenceState position = copyPositionSequenceState();
  JoystickSteeringState joystick = copyJoystickSteeringState();
  size_t logCount = 0;
  uint32_t logDropped = 0;
  copyServoLogMetadata(logCount, logDropped);
  size_t linearityLogCount = 0;
  uint32_t linearityLogDropped = 0;
  copyLinearityLogMetadata(linearityLogCount, linearityLogDropped);
  const uint32_t servoElapsed = servo.startedMs
      ? uint32_t((servo.active ? millis() : servo.completedMs) -
                 servo.startedMs)
      : 0;
  const uint32_t linearityElapsed = linearity.startedMs
      ? uint32_t((linearity.active ? millis() : linearity.completedMs) -
                 linearity.startedMs)
      : 0;
  const uint32_t positionElapsed = position.startedMs
      ? uint32_t((position.active ? millis() : position.completedMs) -
                 position.startedMs)
      : 0;
  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP()
                                                : WiFi.softAPIP();
  String out = "FW:AS5600_JOYSTICK_STEERING_1.6.2";
  out += "\nIP:" + ip.toString();
  out += "\nWIFI:" + String(WiFi.status() == WL_CONNECTED ? "STA" : "AP");
  out += "\nSTA_IP:" + WiFi.localIP().toString();
  out += "\nAP_SSID:" + String(NUEVA_PATA_AP_SSID);
  out += " AP_IP:" + WiFi.softAPIP().toString();
  out += " AP_READY:" + String(softApReady ? 1 : 0);
  out += " DNS_READY:" + String(captiveDnsReady ? 1 : 0);
  out += " AP_CLIENTS:" + String(WiFi.softAPgetStationNum());
  out += "\nI2C_SDA:5 I2C_SCL:7 I2C_HZ:" + String(AS5600_CLOCK_HZ);
  out += " MODE:SOFTWARE";
  out += "\nI2C_STARTED:" + String(s.busStarted ? 1 : 0);
  out += " SDA_LEVEL:" + String(gpio_get_level(GPIO_NUM_5));
  out += " SCL_LEVEL:" + String(gpio_get_level(GPIO_NUM_7));
  out += "\nAS5600_VALID:" + String(s.valid ? 1 : 0);
  out += " RAW:" + String(s.rawAngle);
  out += " RAW_ANGLE:" + String(s.angleDeg, 2);
  out += " CORRECTED_ANGLE:" + String(s.correctedAngleDeg, 2);
  out += "\nMEAN_ANGLE:" + String(s.meanAngleDeg, 2);
  out += " DISPLAY_ANGLE:" + String(s.displayAngleDeg, 1);
  out += " DELTA:" + String(s.filterDeltaDeg, 2);
  out += "\nFILTER_READY:" + String(s.filterReady ? 1 : 0);
  out += " FILTER_SAMPLES:" + String(s.filterSamples);
  out += "/" + String(FILTER_SAMPLE_COUNT);
  out += " INLIERS:" + String(s.filterInliers);
  out += " SPAN_MS:" + String(s.filterSpanMs);
  out += "/" + String(FILTER_MAX_SPAN_MS);
  out += " FILTER_PERIOD_MS:" + String(FILTER_SAMPLE_PERIOD_MS);
  out += " DISPLAY_UPDATES:" + String(s.displayUpdates);
  out += "\nSTATUS:0x" + String(s.status, HEX);
  out += " MD:" + String((s.status & 0x20) ? 1 : 0);
  out += " ML:" + String((s.status & 0x10) ? 1 : 0);
  out += " MH:" + String((s.status & 0x08) ? 1 : 0);
  out += "\nLAST_ERROR:" + String(s.lastError);
  out += " " + String(softI2cErrorName(s.lastError));
  out += " SAMPLES:" + String(s.samples);
  out += " SUCCESSES:" + String(s.successes);
  out += " ERRORS:" + String(s.errors);
  out += " OVERRUNS:" + String(s.scheduleOverruns);
  out += "\nSERVO_PWM_READY:" + String(servoPwmReady ? 1 : 0);
  out += " PIN:14 HZ:50 BITS:14";
  out += "\nSERVO_US:" + String(servo.pulseUs);
  out += " SERVO_TEST_ACTIVE:" + String(servo.active ? 1 : 0);
  out += " PHASE:" + String(servoTestPhaseName(servo.phase));
  out += "\nSERVO_TEST_ELAPSED_MS:" + String(servoElapsed);
  out += " LOG_SAMPLES:" + String(logCount);
  out += " LOG_DROPPED:" + String(logDropped);
  out += "\nLINEARITY_ACTIVE:" + String(linearity.active ? 1 : 0);
  out += " PHASE:" + String(linearityTestPhaseName(linearity.phase));
  out += " SERVO_US:" + String(linearity.pulseUs);
  out += "\nLINEARITY_ELAPSED_MS:" + String(linearityElapsed);
  out += " PHASE_ELAPSED_MS:" +
         String(linearity.phaseStartedMs
                    ? uint32_t(millis() - linearity.phaseStartedMs)
                    : 0);
  out += "\nUNWRAPPED_DEG:" + String(rawTravelDegrees(linearity.unwrappedRaw), 3);
  out += " PHASE_TRAVEL_DEG:" +
         String(linearityPhaseTravelDegrees(linearity), 3);
  out += " PROGRESS_PCT:" + String(linearityProgressPercent(linearity), 1);
  out += "\nLINEARITY_ACCEPTED:" + String(linearity.acceptedSamples);
  out += " INVALID:" + String(linearity.invalidSamples);
  out += " GLITCHES:" + String(linearity.glitches);
  out += " LOG_SAMPLES:" + String(linearityLogCount);
  out += " LOG_DROPPED:" + String(linearityLogDropped);
  out += "\nPOSITION_ACTIVE:" + String(position.active ? 1 : 0);
  out += " PHASE:" + String(positionSequencePhaseName(position.phase));
  out += " SERVO_US:" + String(position.pulseUs);
  out += " STEP:" + String(position.targetStep);
  out += "\nPOSITION_ELAPSED_MS:" + String(positionElapsed);
  out += " LOGICAL_DEG:" + String(position.logicalPositionDeg, 3);
  out += " TARGET_DEG:" +
         String(position.targetAbsoluteDeg - position.homeAbsoluteDeg, 1);
  out += " ERROR_DEG:" + String(position.errorDeg, 3);
  out += "\nPOSITION_STABLE_SAMPLES:" + String(position.stableSamples);
  out += " ACCEPTED:" + String(position.acceptedSamples);
  out += " INVALID:" + String(position.invalidSamples);
  out += " GLITCHES:" + String(position.glitches);
  out += " TRANSITIONS:" + String(position.transitions);
  out += "\nJOYSTICK_ARMED:" + String(joystick.armed ? 1 : 0);
  out += " STATE:" + String(joystickSteeringModeName(joystick.mode));
  out += " RELEASE_REQUIRED:" +
         String(joystick.armReleaseRequired ? 1 : 0);
  out += " SAFETY_TRIPS:" + String(joystick.safetyTrips);
  out += " LAST_TRIP:" +
         String(joystickSteeringModeName(joystick.lastTripReason));
  out += " AS5600_ALARMS:" +
         String(joystick.sensorAlarmsEnabled ? "ON" : "OFF");
  out += " REF_RECOVERY:" +
         String(joystick.referenceRecoveryActive ? 1 : 0);
  out += " REF_REBASES:" + String(joystick.referenceRebases);
  out += " X:" + String(joystick.x);
  out += " SPEED_PCT:" + String(joystick.speedPct);
  out += "\nSTEERING_ANGLE_DEG:" + String(joystick.logicalAngleDeg, 3);
  out += " TARGET_DEG:" + String(joystick.targetDeg, 1);
  out += " ERROR_DEG:" + String(joystick.errorDeg, 3);
  out += " SERVO_US:" + String(joystick.pulseUs);
  esp_reset_reason_t resetReason = esp_reset_reason();
  out += "\nRESET_REASON:" + String(uint32_t(resetReason));
  out += " " + String(resetReasonName(resetReason));
  out += "\nUPTIME_MS:" + String(millis());
  out += "\nRS485:NOT_INITIALIZED";
  return out;
}

bool parseIntegerArgument(const String &text, long minimum, long maximum,
                          long &value) {
  if (!text.length()) return false;
  char *end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || *end != '\0' || parsed < minimum ||
      parsed > maximum)
    return false;
  value = parsed;
  return true;
}

String joystickStatusJson() {
  const JoystickSteeringState control = copyJoystickSteeringState();
  const As5600Snapshot sensor = copySnapshot();
  const uint32_t now = millis();
  const uint32_t sampleAge = control.lastAcceptedMs
      ? uint32_t(now - control.lastAcceptedMs) : UINT32_MAX;
  const uint32_t i2cAge = sensor.lastGoodMs
      ? uint32_t(now - sensor.lastGoodMs) : UINT32_MAX;
  const uint32_t commandAge = control.lastCommandMs
      ? uint32_t(now - control.lastCommandMs) : UINT32_MAX;
  const bool i2cFresh = sensor.lastGoodMs &&
      i2cAge <= JOYSTICK_SENSOR_TIMEOUT_MS;
  const bool magneticStatusAccepted = !control.sensorAlarmsEnabled ||
      (sensor.status & 0x20);
  const bool sensorValid = i2cFresh && sensor.filterReady &&
      magneticStatusAccepted && control.unwrapReady &&
      !control.referenceRecoveryActive &&
      sampleAge <= JOYSTICK_SENSOR_NEUTRAL_MS;
  char json[2048];
  snprintf(
      json, sizeof(json),
      "{\"fw\":\"AS5600_JOYSTICK_STEERING_1.6.2\","
      "\"armed\":%u,\"state\":\"%s\","
      "\"arm_release_required\":%u,\"last_trip\":\"%s\","
      "\"safety_trips\":%lu,\"x\":%d,\"speed_pct\":%u,"
      "\"angle_deg\":%.4f,\"target_deg\":%.4f,\"error_deg\":%.4f,"
      "\"corrected_angle\":%.4f,\"filtered_corrected_angle\":%.4f,"
      "\"display_corrected_angle\":%.1f,"
      "\"unwrapped_corrected_deg\":%.4f,\"zero_absolute_deg\":%.4f,"
      "\"servo_us\":%u,"
      "\"stable_samples\":%u,\"stable_required\":%u,"
      "\"reacquire_samples\":%u,\"reacquire_required\":%u,"
      "\"sensor_valid\":%u,\"sample_age_ms\":%lu,"
      "\"i2c_latest_valid\":%u,\"i2c_fresh\":%u,\"i2c_age_ms\":%lu,"
      "\"filter_ready\":%u,\"filter_samples\":%u,\"filter_window\":%u,"
      "\"filter_inliers\":%u,\"filter_span_ms\":%lu,"
      "\"filter_max_span_ms\":%lu,"
      "\"command_age_ms\":%lu,\"md\":%u,\"ml\":%u,\"mh\":%u,"
      "\"magnet_weak\":%u,\"magnet_strong\":%u,"
      "\"sensor_alarms_enabled\":%u,"
      "\"magnetic_status_accepted\":%u,\"magnetic_rejected\":%u,"
      "\"reference_recovery\":%u,\"reference_candidate_samples\":%u,"
      "\"armed_outlier_samples\":%u,\"reference_rebases\":%lu,"
      "\"accepted\":%lu,\"invalid\":%lu,"
      "\"glitches\":%lu,\"angle_limit_deg\":%.1f,"
      "\"sensor_neutral_ms\":%lu,\"sensor_timeout_ms\":%lu,"
      "\"command_timeout_ms\":%lu,\"rs485\":\"NOT_INITIALIZED\"}",
      control.armed ? 1U : 0U, joystickSteeringModeName(control.mode),
      control.armReleaseRequired ? 1U : 0U,
      joystickSteeringModeName(control.lastTripReason),
      static_cast<unsigned long>(control.safetyTrips),
      static_cast<int>(control.x), static_cast<unsigned>(control.speedPct),
      control.logicalAngleDeg, control.targetDeg, control.errorDeg,
      sensor.correctedAngleDeg, sensor.meanAngleDeg, sensor.displayAngleDeg,
      control.unwrappedCorrectedDeg, control.zeroAbsoluteDeg,
      static_cast<unsigned>(control.pulseUs),
      static_cast<unsigned>(control.stableSamples),
      static_cast<unsigned>(POSITION_STABLE_SAMPLES),
      static_cast<unsigned>(control.reacquireSamples),
      static_cast<unsigned>(POSITION_REACQUIRE_SAMPLES),
      sensorValid ? 1U : 0U, static_cast<unsigned long>(sampleAge),
      sensor.valid ? 1U : 0U, i2cFresh ? 1U : 0U,
      static_cast<unsigned long>(i2cAge), sensor.filterReady ? 1U : 0U,
      static_cast<unsigned>(sensor.filterSamples),
      static_cast<unsigned>(FILTER_SAMPLE_COUNT),
      static_cast<unsigned>(sensor.filterInliers),
      static_cast<unsigned long>(sensor.filterSpanMs),
      static_cast<unsigned long>(FILTER_MAX_SPAN_MS),
      static_cast<unsigned long>(commandAge),
      (sensor.status & 0x20) ? 1U : 0U,
      (sensor.status & 0x10) ? 1U : 0U,
      (sensor.status & 0x08) ? 1U : 0U,
      (sensor.status & 0x10) ? 1U : 0U,
      (sensor.status & 0x08) ? 1U : 0U,
      control.sensorAlarmsEnabled ? 1U : 0U,
      magneticStatusAccepted ? 1U : 0U,
      (control.sensorAlarmsEnabled && !(sensor.status & 0x20)) ? 1U : 0U,
      control.referenceRecoveryActive ? 1U : 0U,
      static_cast<unsigned>(control.referenceCandidateSamples),
      static_cast<unsigned>(control.armedOutlierSamples),
      static_cast<unsigned long>(control.referenceRebases),
      static_cast<unsigned long>(control.acceptedSamples),
      static_cast<unsigned long>(control.invalidSamples),
      static_cast<unsigned long>(control.glitches), JOYSTICK_MAX_ANGLE_DEG,
      static_cast<unsigned long>(JOYSTICK_SENSOR_NEUTRAL_MS),
      static_cast<unsigned long>(JOYSTICK_SENSOR_TIMEOUT_MS),
      static_cast<unsigned long>(JOYSTICK_COMMAND_TIMEOUT_MS));
  return String(json);
}

void setupWeb() {
  const auto serveJoystickPortal = [] {
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("Captive-Portal", "http://192.168.4.1/");
    server.send_P(200, "text/html; charset=utf-8", TELEMETRY_HTML);
  };
  server.on("/", HTTP_GET, serveJoystickPortal);
  // Rutas consultadas por iOS, Android y Windows al detectar una red sin
  // Internet. Entregar la interfaz (en vez del resultado de conectividad)
  // abre el asistente cautivo y mantiene todas las llamadas con rutas relativas.
  server.on("/hotspot-detect.html", HTTP_GET, serveJoystickPortal);
  server.on("/library/test/success.html", HTTP_GET, serveJoystickPortal);
  server.on("/generate_204", HTTP_GET, serveJoystickPortal);
  server.on("/gen_204", HTTP_GET, serveJoystickPortal);
  server.on("/connecttest.txt", HTTP_GET, serveJoystickPortal);
  server.on("/ncsi.txt", HTTP_GET, serveJoystickPortal);
  server.on("/redirect", HTTP_GET, serveJoystickPortal);
  server.on("/telemetry", [] {
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send_P(200, "text/html; charset=utf-8", DIAGNOSTIC_HTML);
  });
  server.on("/status", [] { server.send(200, "text/plain", statusText()); });
  server.on("/joystick/status.json", HTTP_GET, [] {
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", joystickStatusJson());
  });
  server.on("/joystick/sensor-alarms", HTTP_POST, [] {
    if (!server.hasArg("enabled") || !server.hasArg("client")) {
      server.send(400, "text/plain", "REQUIRED_ARGS:enabled,client");
      return;
    }
    long enabled = 0;
    long clientSession = 0;
    if (!parseIntegerArgument(server.arg("enabled"), 0, 1, enabled) ||
        !parseIntegerArgument(server.arg("client"), 1, INT32_MAX,
                              clientSession)) {
      server.send(400, "text/plain",
                  "INVALID_ARGS:enabled[0,1],client[1,2147483647]");
      return;
    }
    (void)clientSession;  // Identifica la pagina v2; el cambio exige desarmado.
    String message;
    const bool accepted =
        setJoystickSensorAlarmsEnabled(enabled == 1, message);
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("X-Joystick-Result", message);
    server.send(accepted ? 200 : 409, "application/json",
                joystickStatusJson());
  });
  server.on("/joystick/cmd", HTTP_POST, [] {
    if (!server.hasArg("x") || !server.hasArg("speed") ||
        !server.hasArg("arm")) {
      server.send(400, "text/plain", "REQUIRED_ARGS:x,speed,arm");
      return;
    }
    long x = 0;
    long speed = 0;
    long arm = 0;
    if (!parseIntegerArgument(server.arg("x"), -100, 100, x) ||
        !parseIntegerArgument(server.arg("speed"), 0, 100, speed) ||
        !parseIntegerArgument(server.arg("arm"), 0, 1, arm)) {
      server.send(400, "text/plain", "INVALID_ARGS:x[-100,100],speed[0,100],arm[0,1]");
      return;
    }
    long clientSession = 0;
    long claimControl = 0;
    if (!server.hasArg("client") || !server.hasArg("claim")) {
      server.sendHeader("Cache-Control", "no-store, max-age=0");
      server.sendHeader("X-Joystick-Result", "CLIENT_SESSION_REQUIRED");
      server.send(409, "application/json", joystickStatusJson());
      return;
    }
    if (server.hasArg("client") &&
        !parseIntegerArgument(server.arg("client"), 1, INT32_MAX,
                              clientSession)) {
      server.send(400, "text/plain", "INVALID_ARG:client[1,2147483647]");
      return;
    }
    if (server.hasArg("claim") &&
        !parseIntegerArgument(server.arg("claim"), 0, 1, claimControl)) {
      server.send(400, "text/plain", "INVALID_ARG:claim[0,1]");
      return;
    }
    String message;
    const IPAddress remoteIp = server.client().remoteIP();
    const uint32_t clientKey =
        (uint32_t(remoteIp[0]) << 24) | (uint32_t(remoteIp[1]) << 16) |
        (uint32_t(remoteIp[2]) << 8) | uint32_t(remoteIp[3]);
    const bool accepted = commandJoystickSteering(
        static_cast<int16_t>(x), static_cast<uint8_t>(speed), arm == 1,
        clientKey, static_cast<uint32_t>(clientSession), claimControl == 1,
        message);
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("X-Joystick-Result", message);
    // La respuesta del heartbeat tambien es la telemetria. Asi la pagina no
    // compite con GET /status mientras esta armada y baja la carga HTTP.
    server.send(accepted ? 200 : 409, "application/json",
                joystickStatusJson());
  });
  server.on("/joystick/stop", HTTP_POST, [] {
    if (!server.hasArg("client") || !server.hasArg("force")) {
      server.sendHeader("Cache-Control", "no-store, max-age=0");
      server.sendHeader("X-Joystick-Result", "CLIENT_SESSION_REQUIRED");
      server.send(409, "application/json", joystickStatusJson());
      return;
    }
    long clientSession = 0;
    long forceStop = 0;
    if (!parseIntegerArgument(server.arg("client"), 1, INT32_MAX,
                              clientSession) ||
        !parseIntegerArgument(server.arg("force"), 0, 1, forceStop)) {
      server.send(400, "text/plain",
                  "INVALID_ARGS:client[1,2147483647],force[0,1]");
      return;
    }
    const IPAddress remoteIp = server.client().remoteIP();
    const uint32_t clientIpKey =
        (uint32_t(remoteIp[0]) << 24) | (uint32_t(remoteIp[1]) << 16) |
        (uint32_t(remoteIp[2]) << 8) | uint32_t(remoteIp[3]);
    const JoystickSteeringState control = copyJoystickSteeringState();
    const bool ownsControl = control.armed &&
        control.ownerIpKey == clientIpKey &&
        control.ownerSessionKey == static_cast<uint32_t>(clientSession);
    if (!forceStop && control.armed && !ownsControl) {
      server.sendHeader("Cache-Control", "no-store, max-age=0");
      server.sendHeader("X-Joystick-Result",
                        "CONTROL_OWNED_BY_OTHER_CLIENT");
      server.send(409, "application/json", joystickStatusJson());
      return;
    }
    if (!forceStop && !control.armed) {
      server.sendHeader("Cache-Control", "no-store, max-age=0");
      server.sendHeader("X-Joystick-Result", "NO_ACTIVE_CONTROL");
      server.send(200, "application/json", joystickStatusJson());
      return;
    }
    // PARAR queda enclavado para que un POST arm=1 anterior y atrasado no
    // pueda rearmar. El usuario lo reconoce luego con arm=0.
    stopServoTest(SERVO_TEST_STOPPED, true);
    stopLinearityTest(LINEARITY_STOPPED, true);
    stopPositionSequence(POSITION_STOPPED, true);
    stopJoystickSteering(JOYSTICK_MANUAL_STOP, false);
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("X-Joystick-Result", "MANUAL_STOP_ACCEPTED");
    server.send(200, "application/json", joystickStatusJson());
  });
  server.on("/telemetry.json", [] {
    As5600Snapshot s = copySnapshot();
    ServoTestState servo = copyServoTestState();
    size_t logCount = 0;
    uint32_t logDropped = 0;
    copyServoLogMetadata(logCount, logDropped);
    const uint32_t servoElapsed = servo.startedMs
        ? uint32_t((servo.active ? millis() : servo.completedMs) -
                   servo.startedMs)
        : 0;
    char json[1280];
    snprintf(
        json, sizeof(json),
        "{\"started\":%u,\"valid\":%u,\"diag\":%u,"
        "\"i2c_hz\":%lu,\"sample_period_ms\":%lu,\"filter_window\":%u,"
        "\"angle\":%.1f,"
        "\"raw\":%u,\"raw_angle\":%.3f,"
        "\"corrected_angle\":%.3f,\"mean_angle\":%.3f,"
        "\"filter_delta\":%.3f,\"filter_ready\":%u,"
        "\"filter_samples\":%u,\"filter_inliers\":%u,"
        "\"filter_span_ms\":%lu,\"filter_max_span_ms\":%lu,"
        "\"display_updates\":%lu,"
        "\"pct\":%.3f,\"status\":%u,\"md\":%u,"
        "\"ml\":%u,\"mh\":%u,\"agc\":%u,\"magnitude\":%u,"
        "\"last_error\":%u,\"samples\":%lu,\"successes\":%lu,"
        "\"errors\":%lu,\"schedule_overruns\":%lu,"
        "\"sda_level\":%u,\"scl_level\":%u,"
        "\"sample_age\":%lu,\"servo_pwm_ready\":%u,"
        "\"servo_us\":%u,\"servo_test_active\":%u,"
        "\"servo_phase\":\"%s\",\"servo_elapsed_ms\":%lu,"
        "\"servo_log_samples\":%u,\"servo_log_dropped\":%lu,"
        "\"millis\":%lu}",
        s.busStarted ? 1U : 0U, s.valid ? 1U : 0U,
        s.diagnosticsValid ? 1U : 0U,
        static_cast<unsigned long>(AS5600_CLOCK_HZ),
        static_cast<unsigned long>(FILTER_SAMPLE_PERIOD_MS),
        static_cast<unsigned>(FILTER_SAMPLE_COUNT), s.displayAngleDeg,
        static_cast<unsigned>(s.rawAngle), s.angleDeg, s.correctedAngleDeg,
        s.meanAngleDeg,
        s.filterDeltaDeg, s.filterReady ? 1U : 0U,
        static_cast<unsigned>(s.filterSamples),
        static_cast<unsigned>(s.filterInliers),
        static_cast<unsigned long>(s.filterSpanMs),
        static_cast<unsigned long>(FILTER_MAX_SPAN_MS),
        static_cast<unsigned long>(s.displayUpdates),
        100.0f * s.rawAngle / 4095.0f,
        static_cast<unsigned>(s.status), (s.status & 0x20) ? 1U : 0U,
        (s.status & 0x10) ? 1U : 0U, (s.status & 0x08) ? 1U : 0U,
        static_cast<unsigned>(s.agc), static_cast<unsigned>(s.magnitude),
        static_cast<unsigned>(s.lastError),
        static_cast<unsigned long>(s.samples),
        static_cast<unsigned long>(s.successes),
        static_cast<unsigned long>(s.errors),
        static_cast<unsigned long>(s.scheduleOverruns),
        static_cast<unsigned>(gpio_get_level(GPIO_NUM_5)),
        static_cast<unsigned>(gpio_get_level(GPIO_NUM_7)),
        static_cast<unsigned long>(
            s.lastSampleMs ? uint32_t(millis() - s.lastSampleMs) : UINT32_MAX),
        servoPwmReady ? 1U : 0U, static_cast<unsigned>(servo.pulseUs),
        servo.active ? 1U : 0U, servoTestPhaseName(servo.phase),
        static_cast<unsigned long>(servoElapsed),
        static_cast<unsigned>(logCount),
        static_cast<unsigned long>(logDropped),
        static_cast<unsigned long>(millis()));
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", json);
  });
  server.on("/position/status.json", HTTP_GET, [] {
    const PositionSequenceState test = copyPositionSequenceState();
    const As5600Snapshot sensor = copySnapshot();
    const uint32_t now = millis();
    const uint32_t elapsed = test.startedMs
        ? uint32_t((test.active ? now : test.completedMs) - test.startedMs)
        : 0;
    const uint32_t phaseElapsed = test.phaseStartedMs
        ? uint32_t((test.active ? now : test.completedMs) -
                   test.phaseStartedMs)
        : 0;
    const uint32_t dwellRemaining =
        positionPhaseIsDwell(test.phase) && phaseElapsed < POSITION_DWELL_MS
            ? POSITION_DWELL_MS - phaseElapsed : 0;
    const uint32_t moveRemaining =
        positionPhaseIsMove(test.phase) &&
        phaseElapsed < POSITION_MOVE_TIMEOUT_MS
            ? POSITION_MOVE_TIMEOUT_MS - phaseElapsed : 0;
    const uint32_t sequenceRemaining = test.active &&
        elapsed < POSITION_SEQUENCE_TIMEOUT_MS
            ? POSITION_SEQUENCE_TIMEOUT_MS - elapsed : 0;
    char json[1152];
    snprintf(
        json, sizeof(json),
        "{\"fw\":\"AS5600_JOYSTICK_STEERING_1.6.2\","
        "\"active\":%u,\"phase\":\"%s\",\"servo_us\":%u,"
        "\"corrected_angle\":%.4f,\"unwrapped_corrected_deg\":%.4f,"
        "\"logical_deg\":%.4f,\"home_absolute_deg\":%.4f,"
        "\"target_deg\":%.1f,\"error_deg\":%.4f,\"target_step\":%u,"
        "\"stable_samples\":%u,\"stable_required\":%u,"
        "\"reacquire_samples\":%u,\"reacquire_required\":%u,"
        "\"tolerance_deg\":%.1f,\"reacquire_deg\":%.1f,"
        "\"elapsed_ms\":%lu,\"phase_elapsed_ms\":%lu,"
        "\"dwell_remaining_ms\":%lu,\"move_timeout_remaining_ms\":%lu,"
        "\"sequence_timeout_remaining_ms\":%lu,"
        "\"accepted\":%lu,\"invalid\":%lu,\"glitches\":%lu,"
        "\"transitions\":%lu,\"last_accepted_age_ms\":%lu}",
        test.active ? 1U : 0U, positionSequencePhaseName(test.phase),
        static_cast<unsigned>(test.pulseUs), sensor.correctedAngleDeg,
        test.unwrappedCorrectedDeg, test.logicalPositionDeg,
        test.homeAbsoluteDeg,
        test.targetAbsoluteDeg - test.homeAbsoluteDeg, test.errorDeg,
        static_cast<unsigned>(test.targetStep),
        static_cast<unsigned>(test.stableSamples),
        static_cast<unsigned>(POSITION_STABLE_SAMPLES),
        static_cast<unsigned>(test.reacquireSamples),
        static_cast<unsigned>(POSITION_REACQUIRE_SAMPLES),
        POSITION_TOLERANCE_DEG, POSITION_REACQUIRE_DEG,
        static_cast<unsigned long>(elapsed),
        static_cast<unsigned long>(phaseElapsed),
        static_cast<unsigned long>(dwellRemaining),
        static_cast<unsigned long>(moveRemaining),
        static_cast<unsigned long>(sequenceRemaining),
        static_cast<unsigned long>(test.acceptedSamples),
        static_cast<unsigned long>(test.invalidSamples),
        static_cast<unsigned long>(test.glitches),
        static_cast<unsigned long>(test.transitions),
        static_cast<unsigned long>(test.lastAcceptedMs
            ? uint32_t(now - test.lastAcceptedMs) : UINT32_MAX));
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", json);
  });
  server.on("/position/sequence/start", HTTP_POST, [] {
    String message;
    const bool started = startPositionSequence(message, false);
    server.send(started ? 202 : 409, "text/plain", message);
  });
  server.on("/position/fine-probe/start", HTTP_POST, [] {
    String message;
    const bool started = startPositionSequence(message, true);
    server.send(started ? 202 : 409, "text/plain", message);
  });
  server.on("/position/sequence/stop", HTTP_POST, [] {
    stopServoTest(SERVO_TEST_STOPPED, true);
    stopLinearityTest(LINEARITY_STOPPED, true);
    stopPositionSequence(POSITION_STOPPED, true);
    stopJoystickSteering(JOYSTICK_DISARMED, true);
    if (servoStateMutex &&
        xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      xSemaphoreGive(servoStateMutex);
    }
    server.send(200, "text/plain", "ALL_TESTS_STOPPED_NEUTRAL");
  });
  server.on("/linearity/status.json", HTTP_GET, [] {
    const LinearityTestState test = copyLinearityTestState();
    const As5600Snapshot sensor = copySnapshot();
    size_t count = 0;
    uint32_t dropped = 0;
    copyLinearityLogMetadata(count, dropped);
    const uint32_t now = millis();
    const uint32_t elapsed = test.startedMs
        ? uint32_t((test.active ? now : test.completedMs) - test.startedMs)
        : 0;
    const uint32_t phaseElapsed = test.phaseStartedMs
        ? uint32_t((test.active ? now : test.completedMs) -
                   test.phaseStartedMs)
        : 0;
    char json[768];
    snprintf(
        json, sizeof(json),
        "{\"fw\":\"AS5600_JOYSTICK_STEERING_1.6.2\","
        "\"active\":%u,\"phase\":\"%s\",\"servo_us\":%u,"
        "\"corrected_angle\":%.3f,"
        "\"elapsed_ms\":%lu,\"phase_elapsed_ms\":%lu,"
        "\"unwrapped_deg\":%.4f,\"phase_displacement_deg\":%.4f,"
        "\"progress_pct\":%.2f,\"target_deg\":2520.0,"
        "\"accepted\":%lu,\"invalid\":%lu,\"glitches\":%lu,"
        "\"last_accepted_age_ms\":%lu,\"log_samples\":%u,"
        "\"log_capacity\":%u,\"log_dropped\":%lu}",
        test.active ? 1U : 0U, linearityTestPhaseName(test.phase),
        static_cast<unsigned>(test.pulseUs),
        sensor.correctedAngleDeg,
        static_cast<unsigned long>(elapsed),
        static_cast<unsigned long>(phaseElapsed),
        rawTravelDegrees(test.unwrappedRaw),
        linearityPhaseTravelDegrees(test), linearityProgressPercent(test),
        static_cast<unsigned long>(test.acceptedSamples),
        static_cast<unsigned long>(test.invalidSamples),
        static_cast<unsigned long>(test.glitches),
        static_cast<unsigned long>(test.lastAcceptedMs
            ? uint32_t(now - test.lastAcceptedMs) : UINT32_MAX),
        static_cast<unsigned>(count),
        static_cast<unsigned>(LINEARITY_TEST_MAX_SAMPLES),
        static_cast<unsigned long>(dropped));
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", json);
  });
  server.on("/linearity/test/start", HTTP_POST, [] {
    String message;
    const bool started = startLinearityTest(message);
    server.send(started ? 202 : 409, "text/plain", message);
  });
  server.on("/linearity/test/stop", HTTP_POST, [] {
    stopServoTest(SERVO_TEST_STOPPED, true);
    stopLinearityTest(LINEARITY_STOPPED, true);
    stopPositionSequence(POSITION_STOPPED, true);
    stopJoystickSteering(JOYSTICK_DISARMED, true);
    if (servoStateMutex &&
        xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      xSemaphoreGive(servoStateMutex);
    }
    server.send(200, "text/plain", "ALL_TESTS_STOPPED_NEUTRAL");
  });
  server.on("/linearity/test/log.csv", HTTP_GET, [] {
    if (copyServoTestState().active || copyLinearityTestState().active ||
        copyPositionSequenceState().active ||
        copyJoystickSteeringState().armed) {
      server.send(409, "text/plain",
                  "STOP_ALL_MOTION_BEFORE_DOWNLOADING_LOG");
      return;
    }
    streamLinearityLogCsv();
  });
  server.on("/servo/test/start", HTTP_POST, [] {
    String message;
    const bool started = startServoTest(message);
    server.send(started ? 202 : 409, "text/plain", message);
  });
  server.on("/servo/stop", HTTP_POST, [] {
    stopServoTest(SERVO_TEST_STOPPED, true);
    stopLinearityTest(LINEARITY_STOPPED, true);
    stopPositionSequence(POSITION_STOPPED, true);
    stopJoystickSteering(JOYSTICK_DISARMED, true);
    // Parada general: aun sin prueba activa se reafirma el neutro una vez.
    if (servoStateMutex &&
        xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
      writeServoPulseLocked(SERVO_NEUTRAL_US);
      xSemaphoreGive(servoStateMutex);
    }
    server.send(200, "text/plain", "ALL_TESTS_STOPPED_NEUTRAL");
  });
  server.on("/servo/test/log.csv", HTTP_GET, [] {
    if (copyServoTestState().active || copyLinearityTestState().active ||
        copyPositionSequenceState().active ||
        copyJoystickSteeringState().armed) {
      server.send(409, "text/plain",
                  "STOP_ALL_MOTION_BEFORE_DOWNLOADING_LOG");
      return;
    }
    String csv;
    const bool ok = servoTestLogCsv(csv);
    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("Content-Disposition",
                      "attachment; filename=servo-vs-encoder.csv");
    server.send(ok ? 200 : 500, "text/csv; charset=utf-8", csv);
  });
  server.on("/i2c/scan", [] {
    As5600Snapshot s = copySnapshot();
    String out = "CACHED_ONLY:1";
    out += "\nADDRESS:0x36";
    out += "\nFOUND:" + String(s.valid ? 1 : 0);
    out += "\nLAST_ERROR:" + String(s.lastError);
    out += " " + String(softI2cErrorName(s.lastError));
    out += "\nSDA_LEVEL:" + String(gpio_get_level(GPIO_NUM_5));
    out += "\nSCL_LEVEL:" + String(gpio_get_level(GPIO_NUM_7));
    server.send(200, "text/plain", out);
  });
  server.onNotFound([serveJoystickPortal] {
    if (server.method() == HTTP_GET) {
      serveJoystickPortal();
      return;
    }
    server.send(404, "text/plain", "NOT_FOUND");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  servoStateMutex = xSemaphoreCreateMutex();
  servoPwmReady = ledcAttach(SERVO_PWM_PIN, SERVO_PWM_HZ,
                             SERVO_PWM_RESOLUTION_BITS);
  if (servoStateMutex &&
      xSemaphoreTake(servoStateMutex, portMAX_DELAY) == pdTRUE) {
    joystickSteering.mode = JOYSTICK_DISARMED;
    joystickSteering.speedPct = JOYSTICK_FIXED_SPEED_PCT;
    // Siempre vuelve al modo estricto despues de un reinicio. El modo de banco
    // debe seleccionarse de forma visible en cada sesion.
    joystickSteering.sensorAlarmsEnabled = true;
    writeServoPulseLocked(SERVO_NEUTRAL_US);
    xSemaphoreGive(servoStateMutex);
  }
  delay(300);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  softApReady =
      WiFi.softAPConfig(NUEVA_PATA_AP_IP, NUEVA_PATA_AP_GATEWAY,
                        NUEVA_PATA_AP_SUBNET) &&
      WiFi.softAP(NUEVA_PATA_AP_SSID, NUEVA_PATA_AP_PASSWORD);
  WiFi.setAutoReconnect(true);
  if (strlen(NUEVA_PATA_TALLER_SSID))
    WiFi.begin(NUEVA_PATA_TALLER_SSID, NUEVA_PATA_TALLER_PASSWORD);
  else
    WiFi.begin();

  uint32_t wifiStartedMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartedMs < 8000)
    delay(100);

  setupWeb();
  if (softApReady)
    captiveDnsReady =
        dnsServer.start(NUEVA_PATA_DNS_PORT, "*", NUEVA_PATA_AP_IP);
  ArduinoOTA.setHostname("ensayo-nueva-pata");
  ArduinoOTA.setPassword(NUEVA_PATA_AP_PASSWORD);
  ArduinoOTA.onStart([] {
    otaInProgress = true;
    stopServoTest(SERVO_TEST_OTA_ABORTED, true);
    stopLinearityTest(LINEARITY_OTA_ABORTED, true);
    stopPositionSequence(POSITION_OTA_ABORTED, true);
    stopJoystickSteering(JOYSTICK_OTA_ABORTED, true);
    Serial.println("OTA START");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.println("OTA ERROR:" + String(static_cast<unsigned>(error)));
  });
  ArduinoOTA.begin();

  xTaskCreate(i2cTask, "as5600_i2c", 4096, nullptr, 1, nullptr);
  Serial.println("READY AS5600 GPIO5/7 JOYSTICK STEERING 1.6.2");
}

void loop() {
  ArduinoOTA.handle();
  if (captiveDnsReady) dnsServer.processNextRequest();
  server.handleClient();
  serviceJoystickSteering();
  serviceServoTest();
  serviceLinearityTest();
  servicePositionSequence();
  if (WiFi.status() != WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
    static uint32_t lastReconnectMs = 0;
    if (millis() - lastReconnectMs >= 60000) {
      WiFi.reconnect();
      lastReconnectMs = millis();
    }
  }
  delay(2);
}
