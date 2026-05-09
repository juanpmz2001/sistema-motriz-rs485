void setup() {
  // put your setup code here, to run once:

}

void loop() {
  // put your main code here, to run repeatedly:

}
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

#define TX_PIN 17
#define RX_PIN 16

void enviarModbus(uint8_t id, uint8_t funcion, uint16_t reg, uint16_t valor);

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rx = String(pCharacteristic->getValue().c_str());
      if (rx.length() == 0) return; 

    int start = rx.indexOf('\x02');
    int end   = rx.indexOf('\x03');
    int sep   = rx.indexOf(',');

    if (start != -1 && end != -1 && sep != -1 && sep > start && end > sep) {
      int x = rx.substring(start + 1, sep).toInt();
      int y = rx.substring(sep + 1, end).toInt();

      int velocidad = map(y, 0, 1023, -50, 50);
      Serial.printf("Joystick Y=%d -> Velocidad=%d\n", y, velocidad);

      if (abs(velocidad) < 5) {
        velocidad = 0;
        Serial.println("ZONA MUERTA -> STOP");
        enviarModbus(0x01, 0x06, 0x5300, 0x0002);              // Modo
        delay(10);
        enviarModbus(0x01, 0x06, 0x5304, abs(velocidad));      // Velocidad
        delay(30);
        enviarModbus(0x01, 0x06, 0x5300, 0x0000);              // STOP
        delay(10);
        enviarModbus(0x01, 0x06, 0x5300, 0x0001);              // Arranque
        delay(10);
        
        return;
      }

      else {
        
        //enviarModbus(0x01, 0x06, 0x5100, (velocidad > 0) ? 0 : 1); // Dirección
        delay(10);
        enviarModbus(0x01, 0x06, 0x5304, abs(velocidad));      // Velocidad
        delay(10);
        
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  BLEDevice::init("ESP32-Joystick");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                          CHARACTERISTIC_UUID,
                                          BLECharacteristic::PROPERTY_WRITE
                                        );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->start();

  Serial.println("BLE listo y esperando comandos...");
}

void loop() {
  delay(100);
}

// Función para calcular CRC Modbus RTU (big endian)
uint16_t calcCRC(uint8_t *data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc = crc >> 1;
    }
  }
  return crc;
}

// Enviar comando Modbus RTU por UART
void enviarModbus(uint8_t id, uint8_t funcion, uint16_t reg, uint16_t valor) {
  uint8_t trama[8];
  trama[0] = id;
  trama[1] = funcion;
  trama[2] = reg >> 8;
  trama[3] = reg & 0xFF;
  trama[4] = valor >> 8;
  trama[5] = valor & 0xFF;
  uint16_t crc = calcCRC(trama, 6);
  trama[7] = crc & 0xFF;
  trama[6] = crc >> 8;

  Serial.print("TX -> ");
  for (int i = 0; i < 8; i++) {
    Serial.printf("%02X ", trama[i]);
    Serial2.write(trama[i]);
  }
  Serial.println();
}
