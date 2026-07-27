# Ensayo nueva pata

Rama aislada de los proyectos de Toño y Rafa.

## Hardware

- ESP32-S3.
- DOCYKE S550 en modo motor: señal PWM en GPIO14.
- AS5600 con salida analógica OUT en GPIO4.
- El servo requiere alimentación independiente de 16–24 V capaz de entregar hasta
  25 A. La tierra de esa fuente, la tierra del servo y GND del ESP deben ser comunes.
- No alimentar el servo desde el ESP.

## Comportamiento seguro

Al arrancar, GPIO14 entrega 50 Hz con pulso neutro de 1500 us. El firmware no inicia
movimiento automáticamente. Los comandos quedan limitados a 1300–1700 us mientras se
identifica el sentido, el neutro real y la mecánica.

## Comandos por consola

```text
PING
VERSION
AS5600
SERVO STOP
SERVO 1490
SERVO 1510
WIFI_CONNECT
WIFI_STATUS
OTA_CHECK
HELP
```

Secuencia inicial recomendada, con la rueda elevada:

1. Consultar el estado y girar la rueda/pata manualmente; comprobar el cambio de
   `ANGLE` y `ADC_RAW`.
2. `SERVO STOP`; verificar que el actuador queda quieto. Ajustar el potenciómetro
   `Middle Angle/Offset` si 1500 us no es neutro.
3. Probar `SERVO 1490`, volver a `SERVO STOP`, probar `SERVO 1510`.
4. Aumentar la separación en pasos de 10 us solamente después de confirmar sentido
   y parada. El límite temporal del firmware es 1300–1700 us.

## Conversión AS5600

El módulo conserva la configuración de fábrica `OUTS=00`, por lo que OUT entrega
un voltaje analógico radiométrico. GPIO41 no tiene ADC en el ESP32-S3; OUT debe
conectarse a GPIO4. El firmware aproxima:

```text
ángulo = ADC_RAW / 4095 * 360
```

OUT nunca debe exceder 3,3 V en el GPIO del ESP.

## Sketch con joystick iPhone

El sketch autónomo está en `arduino/ensayo_nueva_pata/ensayo_nueva_pata.ino`.
Controla una sola pata con:

- joystick web: eje Y a tracción M1 del SVD48 ID 2, limitado inicialmente a
  ±30 rpm;
- RS485 UART2: RX GPIO16, TX GPIO17, 115200 8N1;
- eje X a objetivo de dirección de ±45 grados;
- PWM de velocidad del DOCYKE en GPIO14, 50 Hz y neutro de 1500 us;
- realimentación AS5600 analógica en GPIO4;
- parada si se pierde el joystick durante 450 ms;
- durante la caracterización inicial, dirección manual abierta: eje X completo
  mapea a 1000–2000 us; el AS5600 solo se registra y todavía no realimenta.

Si no puede reutilizar credenciales Wi-Fi almacenadas, crea:

```text
SSID: NuevaPata
Clave: nueva-pata
URL: http://192.168.4.1
```
