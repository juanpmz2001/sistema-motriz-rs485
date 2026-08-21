# Ensayo nueva pata

Rama aislada de los proyectos de Toño y Rafa.

## Hardware

- ESP32-S3.
- DOCYKE S550 en modo motor: señal PWM en GPIO14.
- AS5600 alimentado a 3,3 V y leído por I2C software: SDA GPIO5 y SCL GPIO7.
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
un voltaje analógico radiométrico. La variante actual conecta OUT a GPIO15. Este
pin es ADC2_CH4 y su lectura puede verse afectada mientras Wi-Fi está activo. El
firmware aproxima:

```text
ángulo = ADC_RAW / 4095 * 360
```

OUT nunca debe exceder 3,3 V en el GPIO del ESP.

La entrada se trata exclusivamente como analógica; no se habilitan interrupciones
de flanco sobre OUT durante la telemetría.

> Esta conversión analógica queda conservada solo como antecedente. La variante
> activa usa I2C por GPIO5/GPIO7.

## Sketch anterior con joystick iPhone (referencia histórica)

Este sketch queda conservado como antecedente y ya no es el firmware activo.
Está en `arduino/ensayo_nueva_pata/ensayo_nueva_pata.ino`.
Controla una sola pata con:

- joystick web: eje Y a tracción M1 del SVD48 ID 2, limitado inicialmente a
  ±30 rpm;
- RS485 UART2: RX GPIO16, TX GPIO17, 115200 8N1;
- eje X a objetivo de dirección de ±45 grados;
- PWM de velocidad del DOCYKE en GPIO14, 50 Hz y neutro de 1500 us;
- realimentación AS5600 analógica en GPIO15;
- parada si se pierde el joystick durante 450 ms;
- durante la caracterización inicial, dirección manual abierta: eje X completo
  mapea al rango oficial DOCYKE de 500–2500 us a 50 Hz; el AS5600 solo se
  registra y todavía no realimenta.

La ficha oficial guardada como
`C:\Users\lmuno\Downloads\www_docyke_com_products_servo-motor.pdf` especifica:

```text
Magnetic encoder, PWM: 0.5-2.5ms/50HZ
```

La politica STA actual usa exclusivamente `M ZAPATA`; no existe respaldo al
hotspot del iPhone. En la red del taller el ESP responde actualmente en:

```text
http://192.168.1.184
```

El punto de acceso siguiente permanece disponible solo como rescate manual:

```text
SSID: NuevaPata
Clave: nueva-pata
URL: http://192.168.4.1
```

## Calibración angular 7+7 vueltas

La calibración vigente se obtuvo con siete vueltas en cada sentido, tomando el
ángulo crudo del AS5600 cada 40 ms. El sentido de ángulo decreciente usó 2000 us
y el regreso 1040 us; GPIO14 terminó siempre en el neutro de 1500 us. RS485 no
se inicializó durante el ensayo.

Resultados de la captura de calibración:

- error RMS crudo: 7,97 grados;
- error pico a pico: 28,53 grados;
- correlación de las curvas de ambos sentidos: 0,9995;
- histéresis RMS: 0,245 grados;
- LUT periódica de 128 nodos, un nodo cada 32 cuentas del AS5600;
- transformación verificada como estrictamente monótona en las 4096 cuentas.

La LUT se aplica al ángulo crudo antes del promedio circular de cinco muestras y
antes de la resolución visual de 0,5 grados. Una segunda captura independiente,
sin reajustar la tabla, redujo el error RMS de 8,17 a 3,60 grados. Esa segunda
captura tuvo variación importante de velocidad entre vueltas, por lo que el
residual incluye dinámica del motor/mecánica y no solo error del encoder.

Archivos principales:

```text
arduino/as5600_gpio5_7_diag/as5600_linearity_lut.h
tools/analyze_as5600_linearity.py
artifacts/as5600-linearity/as5600_linearity_report.html
artifacts/as5600-linearity-validation/as5600_linearity_report.html
```

El AS5600 continúa indicando `ML=1` (campo magnético débil). La tabla solo es
válida mientras no cambien el imán, su distancia, el eje o la geometría mecánica.

## Secuencia cerrada 0 -> 1080 -> 0

El firmware validado es `AS5600_POSITION_SEQUENCE_1.7`, en
`arduino/as5600_gpio5_7_diag/as5600_gpio5_7_diag.ino`. Busca primero el cero
corregido más cercano, espera 10 s y recorre objetivos absolutos cada 90 grados
hasta 1080 grados. Luego regresa por los mismos objetivos hasta cero y vuelve a
esperar 10 s. La secuencia contiene 24 movimientos y 25 pausas.

Parámetros de control obtenidos en la pata elevada:

- recorrido positivo: 1040 us lejos del objetivo y 1390 us en los últimos
  5 grados;
- recorrido negativo: 2000 us lejos del objetivo y 1680 us en los últimos
  5 grados;
- neutro: 1500 us;
- banda de llegada: desde 3 grados antes del objetivo hasta el cruce del punto;
- en regreso se obliga a cruzar el objetivo antes de soltar el motor, para
  compensar el juego elástico medido en la reducción;
- readquisición solo si el error supera 4,5 grados durante cinco muestras
  válidas consecutivas (200 ms);
- sin lectura aceptada durante 100 ms se fuerza neutro y a 400 ms se aborta;
- timeout de 45 s por movimiento y 720 s para la secuencia completa.

Resultado de la ejecución completa del 7 de agosto de 2026:

- duración: 459,090 s;
- estado final: `COMPLETE`, inactivo y GPIO14 en 1500 us;
- posición final: -0,3873 grados para objetivo 0 grados;
- 11.474 muestras aceptadas, 3 inválidas y 3 glitches filtrados;
- encoder válido al terminar y cero errores I2C acumulados;
- AS5600 con `MD=1`, `ML=1`; la advertencia de imán débil permanece;
- RS485 no fue inicializado durante la prueba.

Telemetría y control HTTP:

```text
GET  http://192.168.1.184/
GET  http://192.168.1.184/position/status.json
POST http://192.168.1.184/position/sequence/start
POST http://192.168.1.184/position/sequence/stop
POST http://192.168.1.184/position/fine-probe/start
```

`fine-probe` es un diagnóstico corto: avanza -5 grados, espera 10 s y termina
en neutro. Se usó para medir la zona muerta direccional antes de repetir la
secuencia larga.

## Joystick iPhone cerrado ±90 grados (historial 1.3)

La primera versión validada el 7 de agosto de 2026 fue
`AS5600_JOYSTICK_STEERING_1.3`, en
`arduino/as5600_gpio5_7_diag/as5600_gpio5_7_diag.ino`. Reutiliza la LUT, el
desenrollado angular, la compensación de holgura y los pulsos calibrados de la
secuencia cerrada. El encoder está en la rueda conducida de 104 dientes: el
objetivo ya se expresa en grados reales de la rueda y no se multiplica de nuevo
por la relación mecánica 15:104.

La página principal para el iPhone es:

```text
http://192.168.1.184/
```

Incluye joystick horizontal, retorno a cero al soltar, dial de -90 a +90
grados, valor real y objetivo, error, PWM de GPIO14, gráfica temporal y
deslizador de velocidad de 10 a 100 %. El diagnóstico técnico anterior se
conserva en `http://192.168.1.184/telemetry`.

Control cerrado:

- `X=-100..100` se convierte proporcionalmente en `-90..+90` grados;
- ángulo creciente usa hasta 1040 us y 1390 us en la zona fina;
- ángulo decreciente usa hasta 2000 us y 1680 us en la zona fina;
- el deslizador limita la separación máxima respecto de 1500 us, sin bajar de
  los offsets mínimos necesarios para vencer las zonas muertas;
- llegada y readquisición requieren cinco muestras válidas, igual que la
  secuencia calibrada;
- soltar el joystick manda objetivo 0 grados; PARAR, pérdida de red, OTA o
  fallo del encoder dejan el servo en neutro donde esté, sin retorno autónomo.

Seguridad de la versión 1.3:

- heartbeat HTTP cada 180 ms y watchdog a 650 ms;
- la respuesta de cada heartbeat contiene la telemetría, por lo que no se hace
  un sondeo HTTP paralelo mientras el mando está armado;
- una muestra del AS5600 atrasada más de 100 ms fuerza 1500 us; a 400 ms se
  desarma y enclava el fallo;
- los cortes de heartbeat y encoder se ejecutan dentro de la tarea I2C de
  40 ms, independientemente de que una página HTTP esté bloqueada;
- `MD=1` es obligatorio al armar. Pérdidas aisladas de MD durante la marcha se
  filtran con la ventana 100/400 ms; `ML=1` sigue visible como advertencia;
- WATCHDOG, SENSOR_FAULT, OTA y PARAR quedan enclavados. Un `arm=1` atrasado se
  rechaza con HTTP 409 y solo un `arm=0` explícito reconoce el fallo;
- el enclavamiento también impide iniciar las pruebas diagnósticas;
- RS485 permanece `NOT_INITIALIZED`.

Endpoints:

```text
POST /joystick/cmd?x=-100..100&speed=0..100&arm=0|1
POST /joystick/stop
GET  /joystick/status.json
```

Validación física de la versión 1.3, a velocidad 45 %:

- `+90`: llegó a +88,7688 grados en 11,726 s, error 1,2312 grados;
- primer centro: -0,5251 grados en 11,494 s, error 0,5251 grados;
- `-90`: llegó a -90,8319 grados en 11,334 s, error 0,8319 grados;
- segundo centro: entró en la banda de llegada en 12,719 s y quedó finalmente
  cerca de cero;
- 251 heartbeats aceptados, 2 peticiones HTTP perdidas, 0 trips de seguridad y
  0 glitches; hubo 3 pérdidas aisladas adicionales del encoder, toleradas por
  el filtro temporal;
- GPIO14 estuvo en 1500 us en los cuatro puntos de llegada.

El deslizador también se comprobó sobre el mismo objetivo de 27 grados: al
10 % produjo 1355 us y al 80 % produjo 1213 us en sentido creciente, con una
aceleración visible. La prueba terminó desarmada, cerca de cero y en 1500 us.

## Joystick a máxima velocidad y llegada corta (1.4.1)

El firmware activo es `AS5600_JOYSTICK_STEERING_1.4.1`. La página principal
continúa en `http://192.168.1.184/`, pero ya no presenta un deslizador de
velocidad: todas las órdenes usan el máximo calibrado del servo, 1040 us para
ángulo creciente y 2000 us para ángulo decreciente. El parámetro HTTP `speed`
se conserva, pero el firmware lo ignora y reporta siempre `speed_pct=100`.

La reducción de velocidad al aproximarse al objetivo cambió así:

- pulso máximo mientras el error absoluto sea igual o mayor a 6 grados;
- interpolación desde el pulso máximo al pulso fino solamente entre 6 y 3
  grados;
- offsets finos conservados en 110 us para ángulo creciente y 180 us para
  ángulo decreciente;
- neutro de 1500 us al entrar en la banda unilateral de llegada de 0 a 3
  grados;
- readquisición, pausa de inversión y todas las protecciones de encoder y
  heartbeat permanecen sin cambios.

La interfaz usa ahora una sesión aleatoria por pestaña. `claim=1` solo se
envía al pulsar ARMAR; los heartbeats posteriores usan `claim=0`. El primer
cliente aceptado es el único que puede modificar el objetivo o desarmar con
`force=0`. El botón rojo PARAR usa `force=1` y sigue siendo una parada global
deliberada. Una página antigua, una segunda pestaña o un heartbeat atrasado no
pueden tomar el mando ni refrescar el watchdog. Es necesario recargar una
página que hubiera quedado abierta con la versión 1.3.

Endpoints vigentes:

```text
POST /joystick/cmd?x=-100..100&speed=0..100&arm=0|1&client=1..2147483647&claim=0|1
POST /joystick/stop?client=1..2147483647&force=0|1
GET  /joystick/status.json
```

Validación física de la versión 1.4.1 a máxima velocidad:

- `+90`: llegada en 5,007 s; final +87,9622 grados y sin sobrepasar +90;
- centro desde `+90`: llegada en 5,868 s; final -0,6617 grados y sobrepaso
  máximo de 1,2743 grados;
- `-90`: llegada en 6,183 s; final -91,1357 grados y sobrepaso de 1,1357
  grados;
- centro desde `-90`: llegada en 5,175 s; final -2,7748 grados y sin
  sobrepaso en el sentido de marcha;
- se observaron efectivamente 1040 y 2000 us lejos del objetivo, más los
  pulsos interpolados únicamente dentro de la nueva ventana 6 -> 3 grados;
- 0 fallos HTTP y 0 glitches en los cuatro recorridos; cuatro muestras
  inválidas aisladas fueron toleradas por la ventana temporal del AS5600;
- el watchdog se verificó por separado: cortó y enclavó a los 676 ms, con
  GPIO14 en 1500 us;
- estado final `DISARMED`, GPIO14 en 1500 us, AS5600 con `MD=1`, aviso `ML=1`
  todavía presente y RS485 `NOT_INITIALIZED`.

## Joystick iPhone por la red del ESP (1.5.0)

El firmware activo es `AS5600_JOYSTICK_STEERING_1.5.0`. El ESP mantiene su
conexión de taller por `M ZAPATA` en `http://192.168.1.184/` y, al mismo tiempo,
expone este punto de acceso propio:

```text
Red Wi-Fi: NuevaPata
Contraseña: nueva-pata
Joystick: http://192.168.4.1/
```

La dirección del punto de acceso se fija explícitamente en `192.168.4.1`. Un
DNS cautivo y las rutas de detección de iOS, Android y Windows entregan la
página principal del joystick. En el iPhone puede aparecer automáticamente al
unirse a la red; si no aparece, se abre directamente en Safari. Para conservar
la mayor superficie posible también puede añadirse a la pantalla de inicio.

La zona táctil horizontal ocupa toda la pantalla. El indicador circular, el
historial, el valor real, el objetivo y el estado se dibujan como capas
semitransparentes que no bloquean el gesto. Los botones ARMAR y PARAR siguen
siendo controles independientes y respetan las zonas seguras del iPhone tanto
en orientación vertical como horizontal. Al soltar el dedo, el objetivo vuelve
a cero grados.

La versión conserva la sesión exclusiva por pestaña, el watchdog de 650 ms, el
enclavamiento de fallos y el neutro de 1500 us. La OTA queda protegida con la
misma contraseña `nueva-pata`. Después de la instalación se comprobó por la red
del taller: firmware 1.5.0, AP activo en `192.168.4.1`, DNS cautivo activo,
estado `DISARMED`, GPIO14 en 1500 us y RS485 `NOT_INITIALIZED`. No se armó el
servo porque el AS5600 no presentaba una muestra válida en esa verificación.

## Firmware final AS5600 y dirección (1.6.2)

El resultado consolidado queda en `AS5600_JOYSTICK_STEERING_1.6.2`. El AS5600
se lee por I2C software con SDA en GPIO5 y SCL en GPIO7. El reloj del bus es de
aproximadamente 20 kHz, pero la aplicación inicia una sola transacción cada
50 ms: **20 lecturas de ángulo por segundo**, no 20.000.

La medición usada por la dirección tiene estas propiedades:

- ventana circular deslizante de las últimas 5 lecturas corregidas por la LUT;
- separación nominal de 200 ms entre la lectura más antigua y la más reciente,
  con máximo admisible de 240 ms;
- centro circular robusto, con descarte de valores alejados más de 5 grados y
  requisito mínimo de 4 de 5 muestras coherentes;
- cruce correcto entre 359 y 0 grados y presentación visual en pasos de 0,5
  grados, sin cuantizar internamente el valor continuo usado por el control;
- reacquisición controlada de la referencia para que una muestra aislada no
  pueda desplazar el cero ni reanudar el movimiento.

El selector `ALARMAS AS5600 · ON / MODO PRUEBA · OFF` permite omitir durante un
ensayo los indicadores magnéticos `MD/ML/MH`. Nunca omite errores I2C, muestras
atrasadas, incoherencias angulares ni el watchdog. La salida se neutraliza a
1500 us cuando la lectura aceptada supera 100 ms de edad; el fallo se enclava a
500 ms y la pérdida del mando web se enclava a 650 ms. Los endpoints remotos de
movimiento diagnóstico continúan deshabilitados.

Validación real del 21 de agosto de 2026, con el AS5600 conectado:

- firmware accesible en `http://192.168.1.184/` y mediante el AP `NuevaPata` en
  `http://192.168.4.1/`;
- 203 lecturas correctas en 10,141 s: **20,018 Hz efectivos**;
- 0 errores I2C, 0 atrasos de planificación, filtro 5/5, 5 inliers y ventana de
  200 ms;
- AS5600 válido con `MD=1`; `ML=1` quedó visible como aviso de campo débil;
- estado final `DISARMED`, GPIO14 en 1500 us y RS485 `NOT_INITIALIZED`.

## Curva PWM bidireccional final del S550 (2026-08-21)

Con el S550 acoplado, el AS5600 instalado después de la reducción confirmada
108/15 = 7,2:1 y la pata elevada, se midieron los dos sentidos desde 500 hasta
2500 us. Cada nivel tuvo rampa de 0,6 s, 1 s de asentamiento y 3 s de meseta. La
velocidad se obtuvo por regresión lineal del ángulo corregido y desenvuelto
entre 0,25 y 2,80 s de cada meseta. Por ello, la velocidad del eje de salida es
una medición directa del AS5600; la velocidad del S550 es una inferencia
multiplicada por 7,2, no una lectura de tacómetro sobre el motor.

Resultados principales:

- sentido A, ángulo creciente: máximo de **+8,983 rpm en la salida** y
  **+64,68 rpm S550 inferidas** a 550 us;
- extremo A de 500 us: +8,927 rpm en la salida y +64,27 rpm S550;
- sentido B, ángulo decreciente: máximo de **−8,913 rpm en la salida** y
  **−64,17 rpm S550 inferidas** a 2450 us;
- extremo B de 2500 us: −8,822 rpm en la salida y −63,52 rpm S550;
- zona neutra observada aproximadamente entre 1450 y 1600 us: el movimiento
  claro comenzó a 1400 us en A y a 1650 us en B.

Los valores medidos conservan las variaciones reales del montaje: no se forzó
una curva monótona ni una simetría artificial. En particular, el lado B muestra
variación intermedia entre 2000 y 2300 us; antes de convertir esta tabla en una
LUT definitiva conviene repetir el barrido en ida y regreso para cuantificar
histéresis y dependencia de posición/carga.

La tabla y la metodología quedan en
`artifacts/S550_PWM_BIDIRECTIONAL_2026-08-21.md`; los datos consolidados, en
`artifacts/s550-pwm-bidirectional-summary-20260821.csv`; y la gráfica final, en
`artifacts/s550-pwm-bidirectional-20260821.svg` y su exportación PNG. Los
registros crudos usados son los dos barridos originales del sentido A, el
barrido válido hasta 2400 us del sentido B y la meseta completa separada de
2450/2500 us. El firmware temporal de medición no forma parte del resultado
consolidado: al terminar se restauró `AS5600_JOYSTICK_STEERING_1.6.2`, en estado
`DISARMED` y con GPIO14 en 1500 us.
