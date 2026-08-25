#include <Arduino.h>
#include <driver/gpio.h>

// Diagnostico aislado del receptor FlySky FS-iA6B.
// GPIO14 es EXCLUSIVAMENTE una entrada. No se inicializan servo, Wi-Fi ni RS485.
constexpr uint8_t FS_IA6_SIGNAL_PIN = 14;
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t REPORT_INTERVAL_MS = 250;
constexpr uint32_t SIGNAL_STALE_US = 100000;

// Margenes deliberadamente amplios para reconocer una salida RC PWM.
constexpr uint32_t RC_PWM_MIN_US = 750;
constexpr uint32_t RC_PWM_MAX_US = 2250;
constexpr uint32_t RC_FRAME_MIN_US = 5000;
constexpr uint32_t RC_FRAME_MAX_US = 30000;

// Esta unidad FS-iA6B entrega ocho posiciones por trama PPM, aunque el FS-i6
// exponga seis canales principales. Se capturan las ocho para localizar tambien
// cualquier canal auxiliar. El tiempo entre flancos equivalentes representa
// cada posicion; un intervalo largo separa dos tramas.
constexpr uint8_t PPM_CHANNEL_COUNT = 8;
constexpr uint32_t PPM_CHANNEL_MIN_US = 750;
constexpr uint32_t PPM_CHANNEL_MAX_US = 2250;
constexpr uint32_t PPM_SYNC_MIN_US = 3000;
constexpr uint32_t PPM_SYNC_MAX_US = 30000;

portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t lastRiseUs = 0;
volatile uint32_t lastEdgeUs = 0;
volatile uint32_t lastValidUs = 0;
volatile uint32_t highUs = 0;
volatile uint32_t periodUs = 0;
volatile uint32_t effectivePulseUs = 0;
volatile uint32_t edgeCount = 0;
volatile uint32_t pulseCount = 0;
volatile uint32_t validPwmCount = 0;
volatile uint32_t windowPwmCount = 0;
volatile uint32_t windowPulseSumUs = 0;
volatile uint32_t windowPulseMinUs = UINT32_MAX;
volatile uint32_t windowPulseMaxUs = 0;
volatile uint32_t overallPulseMinUs = UINT32_MAX;
volatile uint32_t overallPulseMaxUs = 0;
volatile bool riseSeen = false;
volatile bool invertedPulse = false;
volatile uint16_t ppmWorking[PPM_CHANNEL_COUNT] = {};
volatile uint16_t ppmChannels[PPM_CHANNEL_COUNT] = {};
volatile uint16_t ppmOverallMin[PPM_CHANNEL_COUNT] = {
    UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX,
    UINT16_MAX, UINT16_MAX, UINT16_MAX, UINT16_MAX};
volatile uint16_t ppmOverallMax[PPM_CHANNEL_COUNT] = {};
volatile uint8_t ppmChannelIndex = 0;
volatile uint32_t ppmFrameCount = 0;
volatile uint32_t ppmInvalidFrameCount = 0;
volatile uint32_t ppmLastFrameUs = 0;
volatile uint32_t ppmLastSyncUs = 0;
volatile uint32_t ppmFramePeriodUs = 0;
volatile uint8_t ppmFrameSlotCount = 0;

struct PulseSnapshot {
  uint32_t lastEdgeUs;
  uint32_t lastValidUs;
  uint32_t highUs;
  uint32_t periodUs;
  uint32_t effectivePulseUs;
  uint32_t edgeCount;
  uint32_t pulseCount;
  uint32_t validPwmCount;
  uint32_t windowPwmCount;
  uint32_t windowPulseSumUs;
  uint32_t windowPulseMinUs;
  uint32_t windowPulseMaxUs;
  uint32_t overallPulseMinUs;
  uint32_t overallPulseMaxUs;
  bool invertedPulse;
  uint16_t ppmChannels[PPM_CHANNEL_COUNT];
  uint16_t ppmOverallMin[PPM_CHANNEL_COUNT];
  uint16_t ppmOverallMax[PPM_CHANNEL_COUNT];
  uint32_t ppmFrameCount;
  uint32_t ppmInvalidFrameCount;
  uint32_t ppmLastFrameUs;
  uint32_t ppmFramePeriodUs;
  uint8_t ppmFrameSlotCount;
};

bool IRAM_ATTR inRange(uint32_t value, uint32_t low, uint32_t high) {
  return value >= low && value <= high;
}

void IRAM_ATTR signalEdgeIsr() {
  const uint32_t now = micros();
  const bool levelHigh =
      gpio_get_level(static_cast<gpio_num_t>(FS_IA6_SIGNAL_PIN)) != 0;

  portENTER_CRITICAL_ISR(&pulseMux);
  edgeCount = edgeCount + 1U;
  lastEdgeUs = now;

  if (levelHigh) {
    if (riseSeen) {
      const uint32_t riseIntervalUs = now - lastRiseUs;
      const uint32_t measuredHighUs = highUs;
      periodUs = riseIntervalUs;

      // El ancho HIGH y el periodo deben pertenecer al mismo ciclo: el HIGH se
      // midio en la caida anterior y el periodo termina en este nuevo ascenso.
      uint32_t candidateUs = 0;
      bool candidateInverted = false;
      if (inRange(riseIntervalUs, RC_FRAME_MIN_US, RC_FRAME_MAX_US)) {
        if (inRange(measuredHighUs, RC_PWM_MIN_US, RC_PWM_MAX_US)) {
          candidateUs = measuredHighUs;
        } else if (riseIntervalUs > measuredHighUs) {
          const uint32_t measuredLowUs = riseIntervalUs - measuredHighUs;
          if (inRange(measuredLowUs, RC_PWM_MIN_US, RC_PWM_MAX_US)) {
            candidateUs = measuredLowUs;
            candidateInverted = true;
          }
        }
      }
      if (candidateUs != 0) {
        effectivePulseUs = candidateUs;
        invertedPulse = candidateInverted;
        lastValidUs = now;
        validPwmCount = validPwmCount + 1U;
        windowPwmCount = windowPwmCount + 1U;
        windowPulseSumUs += candidateUs;
        if (candidateUs < windowPulseMinUs) windowPulseMinUs = candidateUs;
        if (candidateUs > windowPulseMaxUs) windowPulseMaxUs = candidateUs;
        if (candidateUs < overallPulseMinUs) overallPulseMinUs = candidateUs;
        if (candidateUs > overallPulseMaxUs) overallPulseMaxUs = candidateUs;
      }

      if (inRange(riseIntervalUs, PPM_SYNC_MIN_US, PPM_SYNC_MAX_US)) {
        if (ppmLastSyncUs != 0) ppmFramePeriodUs = now - ppmLastSyncUs;
        ppmLastSyncUs = now;

        if (ppmChannelIndex == PPM_CHANNEL_COUNT) {
          for (uint8_t i = 0; i < PPM_CHANNEL_COUNT; ++i) {
            const uint16_t value = ppmWorking[i];
            ppmChannels[i] = value;
            if (value < ppmOverallMin[i]) ppmOverallMin[i] = value;
            if (value > ppmOverallMax[i]) ppmOverallMax[i] = value;
          }
          ppmFrameCount = ppmFrameCount + 1U;
          ppmLastFrameUs = now;
          ppmFrameSlotCount = ppmChannelIndex;
        } else if (ppmChannelIndex != 0) {
          ppmInvalidFrameCount = ppmInvalidFrameCount + 1U;
        }
        ppmChannelIndex = 0;
      } else if (inRange(riseIntervalUs, PPM_CHANNEL_MIN_US,
                         PPM_CHANNEL_MAX_US)) {
        if (ppmChannelIndex < PPM_CHANNEL_COUNT)
          ppmWorking[ppmChannelIndex] =
              static_cast<uint16_t>(riseIntervalUs);
        if (ppmChannelIndex < UINT8_MAX)
          ppmChannelIndex = ppmChannelIndex + 1U;
      } else {
        if (ppmChannelIndex != 0)
          ppmInvalidFrameCount = ppmInvalidFrameCount + 1U;
        ppmChannelIndex = 0;
      }
    }
    lastRiseUs = now;
    riseSeen = true;
  } else if (riseSeen) {
    const uint32_t measuredHighUs = now - lastRiseUs;
    highUs = measuredHighUs;
    pulseCount = pulseCount + 1U;
  }
  portEXIT_CRITICAL_ISR(&pulseMux);
}

PulseSnapshot takeSnapshotAndResetWindow() {
  PulseSnapshot s = {};
  portENTER_CRITICAL(&pulseMux);
  s.lastEdgeUs = lastEdgeUs;
  s.lastValidUs = lastValidUs;
  s.highUs = highUs;
  s.periodUs = periodUs;
  s.effectivePulseUs = effectivePulseUs;
  s.edgeCount = edgeCount;
  s.pulseCount = pulseCount;
  s.validPwmCount = validPwmCount;
  s.windowPwmCount = windowPwmCount;
  s.windowPulseSumUs = windowPulseSumUs;
  s.windowPulseMinUs = windowPulseMinUs;
  s.windowPulseMaxUs = windowPulseMaxUs;
  s.overallPulseMinUs = overallPulseMinUs;
  s.overallPulseMaxUs = overallPulseMaxUs;
  s.invertedPulse = invertedPulse;
  for (uint8_t i = 0; i < PPM_CHANNEL_COUNT; ++i) {
    s.ppmChannels[i] = ppmChannels[i];
    s.ppmOverallMin[i] = ppmOverallMin[i];
    s.ppmOverallMax[i] = ppmOverallMax[i];
  }
  s.ppmFrameCount = ppmFrameCount;
  s.ppmInvalidFrameCount = ppmInvalidFrameCount;
  s.ppmLastFrameUs = ppmLastFrameUs;
  s.ppmFramePeriodUs = ppmFramePeriodUs;
  s.ppmFrameSlotCount = ppmFrameSlotCount;
  windowPwmCount = 0;
  windowPulseSumUs = 0;
  windowPulseMinUs = UINT32_MAX;
  windowPulseMaxUs = 0;
  portEXIT_CRITICAL(&pulseMux);
  return s;
}

void resetMeasurements() {
  portENTER_CRITICAL(&pulseMux);
  effectivePulseUs = 0;
  validPwmCount = 0;
  windowPwmCount = 0;
  windowPulseSumUs = 0;
  windowPulseMinUs = UINT32_MAX;
  windowPulseMaxUs = 0;
  overallPulseMinUs = UINT32_MAX;
  overallPulseMaxUs = 0;
  lastValidUs = 0;
  ppmFrameCount = 0;
  ppmInvalidFrameCount = 0;
  ppmLastFrameUs = 0;
  ppmLastSyncUs = 0;
  ppmFramePeriodUs = 0;
  ppmFrameSlotCount = 0;
  ppmChannelIndex = 0;
  for (uint8_t i = 0; i < PPM_CHANNEL_COUNT; ++i) {
    ppmWorking[i] = 0;
    ppmChannels[i] = 0;
    ppmOverallMin[i] = UINT16_MAX;
    ppmOverallMax[i] = 0;
  }
  portEXIT_CRITICAL(&pulseMux);
  Serial.println("# MIN_MAX_REINICIADOS");
}

void printTelemetry() {
  const PulseSnapshot s = takeSnapshotAndResetWindow();
  // Leer el reloj despues del snapshot evita que un flanco nuevo produzca
  // temporalmente lastEdgeUs > nowUs y una edad falsa por underflow.
  const uint32_t nowUs = micros();
  const bool edgeFresh =
      s.lastEdgeUs != 0 && (nowUs - s.lastEdgeUs) <= SIGNAL_STALE_US;
  const bool pwmFresh =
      s.lastValidUs != 0 && (nowUs - s.lastValidUs) <= SIGNAL_STALE_US;
  const bool ppmFresh =
      s.ppmLastFrameUs != 0 && (nowUs - s.ppmLastFrameUs) <= SIGNAL_STALE_US;
  const uint32_t edgeAgeMs =
      s.lastEdgeUs != 0 ? (nowUs - s.lastEdgeUs) / 1000U : UINT32_MAX;
  const float frequencyHz =
      s.periodUs != 0 ? 1000000.0f / static_cast<float>(s.periodUs) : 0.0f;

  const char *state = "NO_SIGNAL";
  if (ppmFresh) {
    state = "PPM";
  } else if (pwmFresh) {
    state = "RC_PWM";
  } else if (edgeFresh && s.highUs >= 100 && s.highUs <= 700 &&
             s.periodUs >= 700 && s.periodUs <= 10000) {
    state = "PPM_LIKE";
  } else if (edgeFresh) {
    state = "PULSES_UNKNOWN";
  }

  Serial.printf("t=%lu state=%s level=%d edges=%lu pulses=%lu ",
                static_cast<unsigned long>(millis()), state,
                gpio_get_level(static_cast<gpio_num_t>(FS_IA6_SIGNAL_PIN)),
                static_cast<unsigned long>(s.edgeCount),
                static_cast<unsigned long>(s.pulseCount));

  if (ppmFresh) {
    const float frameHz =
        s.ppmFramePeriodUs != 0
            ? 1000000.0f / static_cast<float>(s.ppmFramePeriodUs)
            : 0.0f;
    Serial.printf("frame_hz=%.2f slots=%u frames=%lu invalid=%lu CH=[", frameHz,
                  static_cast<unsigned>(s.ppmFrameSlotCount),
                  static_cast<unsigned long>(s.ppmFrameCount),
                  static_cast<unsigned long>(s.ppmInvalidFrameCount));
    for (uint8_t i = 0; i < PPM_CHANNEL_COUNT; ++i) {
      const float positionPct = constrain(
          (static_cast<float>(s.ppmChannels[i]) - 1500.0f) / 5.0f, -100.0f,
          100.0f);
      if (i != 0) Serial.print(' ');
      Serial.printf("%u:%u(%+.1f%%)", static_cast<unsigned>(i + 1),
                    static_cast<unsigned>(s.ppmChannels[i]), positionPct);
    }
    Serial.print("] range=[");
    for (uint8_t i = 0; i < PPM_CHANNEL_COUNT; ++i) {
      if (i != 0) Serial.print(' ');
      Serial.printf("%u:%u..%u", static_cast<unsigned>(i + 1),
                    static_cast<unsigned>(s.ppmOverallMin[i]),
                    static_cast<unsigned>(s.ppmOverallMax[i]));
    }
    Serial.print(']');
  } else if (pwmFresh) {
    const float averageUs =
        s.windowPwmCount != 0
            ? static_cast<float>(s.windowPulseSumUs) /
                  static_cast<float>(s.windowPwmCount)
            : static_cast<float>(s.effectivePulseUs);
    const float positionPct =
        constrain((averageUs - 1500.0f) / 5.0f, -100.0f, 100.0f);
    const uint32_t windowMin =
        s.windowPwmCount != 0 ? s.windowPulseMinUs : s.effectivePulseUs;
    const uint32_t windowMax =
        s.windowPwmCount != 0 ? s.windowPulseMaxUs : s.effectivePulseUs;
    Serial.printf(
        "pulse=%.1fus pos=%+.1f%% high=%luus period=%luus hz=%.2f "
        "polarity=%s window=[%lu..%lu] overall=[%lu..%lu] valid=%lu",
        averageUs, positionPct, static_cast<unsigned long>(s.highUs),
        static_cast<unsigned long>(s.periodUs), frequencyHz,
        s.invertedPulse ? "LOW" : "HIGH",
        static_cast<unsigned long>(windowMin),
        static_cast<unsigned long>(windowMax),
        static_cast<unsigned long>(s.overallPulseMinUs),
        static_cast<unsigned long>(s.overallPulseMaxUs),
        static_cast<unsigned long>(s.validPwmCount));
  } else {
    Serial.printf("high=%luus rise_interval=%luus hz=%.2f age_ms=",
                  static_cast<unsigned long>(s.highUs),
                  static_cast<unsigned long>(s.periodUs), frequencyHz);
    if (edgeAgeMs == UINT32_MAX)
      Serial.print("NA");
    else
      Serial.print(edgeAgeMs);
  }
  Serial.println();
}

void setup() {
  // Esta es la primera accion de la aplicacion: GPIO14 queda en alta impedancia
  // con una resistencia debil a tierra, nunca como salida.
  pinMode(FS_IA6_SIGNAL_PIN, INPUT_PULLDOWN);
  Serial.begin(SERIAL_BAUD);
  delay(800);

  Serial.println();
  Serial.println("# FS_IA6B_GPIO14_DIAG_1.2.0");
  Serial.println("# GPIO14=INPUT_PULLDOWN; SERVO=OFF; WIFI=OFF; RS485=OFF");
  Serial.println("# Conecte S a GPIO14 solo despues de ver este mensaje.");
  Serial.println(
      "# Detecta PWM individual o PPM de 8 posiciones; envie r para min/max.");

  attachInterrupt(digitalPinToInterrupt(FS_IA6_SIGNAL_PIN), signalEdgeIsr,
                  CHANGE);
}

void loop() {
  static uint32_t lastReportMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = nowMs;
    printTelemetry();
  }

  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == 'r' || c == 'R') resetMeasurements();
  }
  delay(2);
}
