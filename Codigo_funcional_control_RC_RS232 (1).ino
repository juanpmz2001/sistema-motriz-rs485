#include <Arduino.h>
#include <string.h>

// ------------------- CONFIGURACIÓN PPM -------------------
constexpr int PIN_PPM = 34;                 // Pin donde entra la señal PPM
constexpr int NUM_CHANNELS = 6;             // Número de canales PPM
constexpr unsigned long PPM_SYNC_THRESH = 3000UL;  // ~3 ms para detectar pulso de sync

volatile unsigned long lastRiseTime = 0;
volatile unsigned int channelValues[NUM_CHANNELS];
volatile uint8_t currentChannel = 0;

// ------------------- CLASE MOTORCONTROLLER -------------------
#define RXD2 16
#define TXD2 17
#define LED_BUILTIN 2

class MotorController {
public:
    MotorController(HardwareSerial &serial) : _serial(serial) {}

    void begin(unsigned long baudRate = 115200) {
      _serial.begin(baudRate, SERIAL_8N1, RXD2, TXD2);
      delay(1000);
    }

    //--------------------------------------------------------------------------
    // LECTURA (Ejemplo): Si deseas leer parámetros del motor
    //--------------------------------------------------------------------------
    int16_t getMotor1Speed() { return readRegister16(0x5410); }
    int16_t getMotor2Speed() { return readRegister16(0x5411); }

    //--------------------------------------------------------------------------
    // ESCRITURA: Control de M1 y M2
    //  - Se repite 3 veces cada comando para mayor confiabilidad
    //--------------------------------------------------------------------------
    void setM1ControlCommand(uint16_t cmd) {
      for(int i=0; i<3; i++) {
        writeRegister16(0x5300, cmd);
      }
    }
    void setM1GivenSpeed(int16_t speed) {
      for(int i=0; i<3; i++) {
        writeRegister16(0x5304, speed);
      }
    }
    void setM2ControlCommand(uint16_t cmd) {
      for(int i=0; i<3; i++) {
        writeRegister16(0x5301, cmd);
      }
    }
    void setM2GivenSpeed(int16_t speed) {
      for(int i=0; i<3; i++) {
        writeRegister16(0x5305, speed);
      }
    }

private:
    //--------------------------------------------------------------------------
    // Escribir 16 bits (función Modbus 0x06)
    //--------------------------------------------------------------------------
    void writeRegister16(uint16_t regAddress, int16_t value) {
      uint8_t cmd[6];
      cmd[0] = 0xEE;      // Drive address
      cmd[1] = 0x06;      // Función: Write Single Register
      cmd[2] = (regAddress >> 8) & 0xFF;
      cmd[3] = regAddress & 0xFF;
      cmd[4] = (value >> 8) & 0xFF;
      cmd[5] = value & 0xFF;
      sendCommand(cmd, 6);
    }

    //--------------------------------------------------------------------------
    // Leer 16 bits (función Modbus 0x03)
    //--------------------------------------------------------------------------
    int16_t readRegister16(uint16_t regAddress) {
      uint8_t cmd[6];
      cmd[0] = 0xEE;
      cmd[1] = 0x03;
      cmd[2] = (regAddress >> 8) & 0xFF;
      cmd[3] = regAddress & 0xFF;
      cmd[4] = 0x00;
      cmd[5] = 0x01;

      sendCommand(cmd, 6);

      uint8_t buffer[64];
      size_t len = readResponse(buffer, sizeof(buffer));
      if(len < 5) return 0;

      if(buffer[1] == 0x03) {
        uint8_t byteCount = buffer[2];
        if(byteCount == 2 && len >= 5) {
          int16_t rawValue = (buffer[3] << 8) | buffer[4];
          return rawValue;
        }
      }
      return 0;
    }

    //--------------------------------------------------------------------------
    // Enviar comando con CRC
    //--------------------------------------------------------------------------
    void sendCommand(const uint8_t *command, size_t cmdLength) {
      if(cmdLength > 62) return;
      uint8_t packet[64];
      memcpy(packet, command, cmdLength);

      uint16_t crc = computeCRC(command, cmdLength);
      // Byte alto, luego byte bajo
      packet[cmdLength]   = (crc >> 8) & 0xFF;
      packet[cmdLength+1] = crc & 0xFF;
      size_t packetLength = cmdLength + 2;

      // Debug opcional
      // Serial.print("Enviando comando: ");
      // for(size_t i=0; i<packetLength; i++){
      //   Serial.print("0x"); Serial.print(packet[i], HEX); Serial.print(" ");
      // }
      // Serial.println();

      _serial.write(packet, packetLength);
    }

    //--------------------------------------------------------------------------
    // Recibir respuesta
    //--------------------------------------------------------------------------
    size_t readResponse(uint8_t *buffer, size_t maxLength, unsigned long timeout = 3000) {
      size_t index = 0;
      unsigned long start = millis();
      while(millis() - start < timeout) {
        while(_serial.available()) {
          if(index < maxLength) {
            buffer[index++] = _serial.read();
          }
        }
        if(index>0 && (millis()-start>300)) break;
      }
      return index;
    }

    //--------------------------------------------------------------------------
    // CRC16 (Modbus)
    //--------------------------------------------------------------------------
    uint16_t computeCRC(const uint8_t *data, size_t length) {
      uint16_t crc = 0xFFFF;
      for (size_t pos = 0; pos < length; pos++) {
        crc ^= (uint16_t)data[pos];
        for (int i = 0; i < 8; i++) {
          if (crc & 0x0001) {
            crc >>= 1;
            crc ^= 0xA001;
          } else {
            crc >>= 1;
          }
        }
      }
      return crc;
    }

    HardwareSerial &_serial;
};

// ---------------- INSTANCIA DEL CONTROLADOR ----------------
MotorController motorController(Serial2);

// ---------------- FUNCIONES PARA MAPEO DE CANALES ----------------
/**
 * mapea x de [inMin..inMax] a [outMin..outMax] en float
 */
float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}


// Devuelve la velocidad máxima (10..60) segun CH5 (1000..2000)
float getMaxSpeed(unsigned int ch5Value) {
  if (ch5Value < 1000) ch5Value = 1000;
  if (ch5Value > 2000) ch5Value = 2000;
  // mapea [1000..2000] -> [10..60]
  return mapFloat(ch5Value, 1000, 2000, 10, 60);
}

/**
 * Convierte un canal (chVal) a una velocidad int16 según la lógica:
 *  - >1550 => mapea [1550..2000] a [3..maxSpd]
 *  - <1450 => mapea [1000..1450] a [-maxSpd..-3]
 *  - [1450..1550] => 0
 */
int16_t computeSpeed(unsigned int chVal, float maxSpd) {
  if (chVal < 1000) chVal = 1000;
  if (chVal > 2000) chVal = 2000;

  if (chVal > 1550) {
    float val = mapFloat(chVal, 1550, 2000, 3, maxSpd);
    return (int16_t) round(val);
  } else if (chVal < 1450) {
    float val = mapFloat(chVal, 1000, 1450, -maxSpd, -3);
    return (int16_t) round(val);
  } else {
    return 0;
  }
}

// ---------------- ISR DE PPM ----------------
void IRAM_ATTR handlePPMInterrupt() {
  unsigned long now = micros();
  unsigned long pulseWidth = now - lastRiseTime;
  lastRiseTime = now;
  
  if (pulseWidth > PPM_SYNC_THRESH) {
    // Pulso largo => reiniciamos canal
    currentChannel = 0;
  } else {
    if (currentChannel < NUM_CHANNELS) {
      channelValues[currentChannel] = pulseWidth;
      currentChannel++;
    }
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configurar pin PPM
  pinMode(PIN_PPM, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PPM), handlePPMInterrupt, RISING);

  // Inicializamos canales
  for (int i = 0; i < NUM_CHANNELS; i++) {
    channelValues[i] = 1500;
  }

  // LED (opcional)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // Inicializar comunicación con controlador (RS232)
  motorController.begin(115200);

  Serial.println("Sistema integrado: PPM -> MotorController listo.");
}

// ---------------- LOOP ----------------
void loop() {
  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  // Actualizamos cada 50 ms
  if (now - lastUpdate >= 50) {
    lastUpdate = now;

    // Copiamos canales localmente
    unsigned int localCh[NUM_CHANNELS];
    noInterrupts();
    for (int i = 0; i < NUM_CHANNELS; i++) {
      localCh[i] = channelValues[i];
    }
    interrupts();

    // CH5 => determina la velocidad máxima
    float maxSpeed = getMaxSpeed(localCh[5]);

    // CH1 => "given speed"
    int16_t givenSpeed = computeSpeed(localCh[1], maxSpeed);

    // CH3 => "diff speed"
    int16_t diffSpeed  = computeSpeed(localCh[3], maxSpeed);

    // CH4 => habilita (start) si >1500, de lo contrario stop
    bool enable = (localCh[4] > 1500);

    // CH0, CH2 => sin uso, pero si quieres puedes imprimirlos o ignorarlos

    // ---------------- LÓGICA DE CONTROL DE MOTORES ----------------
    if (enable) {
      // Start
      motorController.setM1ControlCommand(1);
      motorController.setM2ControlCommand(1);
    } else {
      // Stop
      motorController.setM1ControlCommand(0);
      motorController.setM2ControlCommand(0);
    }

    // Skid-steer: M1 = givenSpeed - diff, M2 = givenSpeed + diff
    int16_t m1Speed = givenSpeed - diffSpeed;
    int16_t m2Speed = givenSpeed + diffSpeed;

    // Enviamos la velocidad resultante
    motorController.setM1GivenSpeed(m1Speed);
    motorController.setM2GivenSpeed(m2Speed);

    // ---------------- IMPRESIÓN OPCIONAL ----------------
    // Aquí puedes imprimir el estado actual si deseas
    Serial.print("CH1=");
    Serial.print(localCh[1]);
    Serial.print(" -> givenSpeed=");
    Serial.print(givenSpeed);

    Serial.print(" | CH3=");
    Serial.print(localCh[3]);
    Serial.print(" -> diffSpeed=");
    Serial.print(diffSpeed);

    Serial.print(" | CH4=");
    Serial.print(localCh[4]);
    Serial.print(" -> enable=");
    Serial.print(enable ? 1 : 0);

    Serial.print(" | CH5=");
    Serial.print(localCh[5]);
    Serial.print(" -> maxSpeed=");
    Serial.print(maxSpeed);

    Serial.print(" || M1=");
    Serial.print(m1Speed);
    Serial.print(" rpm, M2=");
    Serial.print(m2Speed);
    Serial.println(" rpm");
  }
}
