#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

// Diagnóstico mínimo de alimentación/arranque de la nueva pata.
// No configura GPIO14, RS485, Wi-Fi ni AS5600.
constexpr uint8_t LED_1_PIN = 5;
constexpr uint8_t LED_2_PIN = 6;
constexpr uint8_t LED_3_PIN = 7;
constexpr uint32_t STEP_MS = 1000;

Preferences diagnostics;
uint32_t bootCount = 0;
uint8_t stepIndex = 0;
uint32_t lastStepMs = 0;

void allOff() {
  digitalWrite(LED_1_PIN, LOW);
  digitalWrite(LED_2_PIN, LOW);
  digitalWrite(LED_3_PIN, LOW);
}

void showStep(uint8_t step) {
  allOff();
  switch (step) {
    case 0:
      digitalWrite(LED_1_PIN, HIGH);
      Serial.println("LED GPIO5");
      break;
    case 1:
      digitalWrite(LED_2_PIN, HIGH);
      Serial.println("LED GPIO6");
      break;
    case 2:
      digitalWrite(LED_3_PIN, HIGH);
      Serial.println("LED GPIO7");
      break;
    case 3:
      digitalWrite(LED_1_PIN, HIGH);
      digitalWrite(LED_2_PIN, HIGH);
      digitalWrite(LED_3_PIN, HIGH);
      Serial.println("LED TODOS");
      break;
    default:
      Serial.println("LED APAGADOS");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_1_PIN, OUTPUT);
  pinMode(LED_2_PIN, OUTPUT);
  pinMode(LED_3_PIN, OUTPUT);
  allOff();

  diagnostics.begin("np_leddiag", false);
  bootCount = diagnostics.getUInt("boots", 0) + 1;
  diagnostics.putUInt("boots", bootCount);
  diagnostics.putUInt("reset", uint32_t(esp_reset_reason()));

  delay(250);
  Serial.printf("LED_DIAG READY BOOT=%lu RESET_REASON=%u\n",
                static_cast<unsigned long>(bootCount),
                static_cast<unsigned>(esp_reset_reason()));
  showStep(stepIndex);
  lastStepMs = millis();
}

void loop() {
  if (millis() - lastStepMs >= STEP_MS) {
    stepIndex = (stepIndex + 1) % 5;
    showStep(stepIndex);
    lastStepMs += STEP_MS;
  }
  delay(2);
}
