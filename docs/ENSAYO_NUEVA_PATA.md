# Ensayo nueva pata

Rama aislada de los proyectos de Toño y Rafa.

## Hardware

- ESP32-S3.
- DOCYKE S550 en modo motor: señal PWM en GPIO14.
- AS5600 configurado externamente para salida PWM: OUT en GPIO41.
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

1. `AS5600` y girar la rueda/pata manualmente; comprobar `VALID:1` y cambio de ángulo.
2. `SERVO STOP`; verificar que el actuador queda quieto. Ajustar el potenciómetro
   `Middle Angle/Offset` si 1500 us no es neutro.
3. Probar `SERVO 1490`, volver a `SERVO STOP`, probar `SERVO 1510`.
4. Aumentar la separación en pasos de 10 us solamente después de confirmar sentido
   y parada. El límite temporal del firmware es 1300–1700 us.

## Conversión AS5600

La ficha indica PWM seleccionable de 115, 230, 460 o 920 Hz, con ciclo útil de
2.9 % a 97.1 %. El firmware mide ambos flancos y aproxima:

```text
ángulo = clamp((duty - 2.9) / (97.1 - 2.9), 0, 1) * 360
```

Si `VALID:0`, confirmar que el AS5600 esté realmente programado con `OUTS=10`
(PWM), que haya imán válido y que OUT no exceda 3.3 V en el GPIO del ESP.
