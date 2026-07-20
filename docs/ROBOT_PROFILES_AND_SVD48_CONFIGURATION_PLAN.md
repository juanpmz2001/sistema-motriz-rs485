# Plan de Perfiles JSON, Cinemática y Configuración SVD48

Fecha: 2026-07-19

Estado: `IN_PROGRESS`. El schema/fixture, autoridad pura y cinemática
differential ya existen; parser/persistencia/runtime del perfil y las demás
estrategias siguen pendientes. Este documento reemplaza el diseño anterior basado
en una estructura C de cuatro motores. El contrato normativo del perfil está en
`docs/schemas/robot-profile.schema.json` y el primer fixture físico en
`docs/examples/robot-profile-differential-2wd-one-svd48.json`.

El avance, las pruebas y los handoffs se registran en `docs/process/`. Si este
documento contradice el código actual, el código representa el comportamiento
existente y esta documentación representa la migración pendiente; la diferencia
debe quedar anotada, no ocultarse.

## Decisiones Confirmadas

- El robot es un documento JSON canónico, versionado, autocontenido y validable.
- El JSON describe la placa, pines, buses, controladores, canales, motores,
  servos, ruedas pasivas, geometría, cinemática, fuentes de comando, límites,
  seguridad y configuración SVD48 deseada.
- No se recompila el firmware para cambiar una topología que cabe dentro de las
  capacidades anunciadas por el dispositivo.
- No existe una suposición fija de dos controladores, cuatro motores, cuatro
  servos ni posiciones `front-left..rear-right`.
- Un SVD48 con `M1` y `M2`, dos SVD48 con dos canales cada uno, o cuatro SVD48
  usando un canal cada uno se representan con las mismas listas de objetos y
  referencias por ID.
- Las fuentes pueden estar conectadas y recibiendo simultáneamente. La autoridad
  de movimiento cumple siempre `RC > LAN > Bluetooth`.
- LAN de movimiento usa un ingress compacto `control_lan`, separado de
  `maintenance_lan`. Ambos pueden reutilizar el token local del MVP, pero tienen
  puertos, payloads, colas y responsabilidades diferentes.
- La selección de fuente y la prioridad de tareas son conceptos distintos. RC se
  muestrea con baja latencia; LAN y Bluetooth jamás hacen I/O bloqueante dentro
  del ciclo de control.
- Las primeras estrategias cinemáticas son `differential`, `ackermann` e
  `independent_steer`. Crabwalk es una capacidad de `independent_steer`, no una
  topología codificada aparte.
- El frontend se difiere hasta que firmware y backend tengan contratos tipados,
  pruebas y comportamiento estable por USB y LAN.
- Las pruebas físicas de este programa se realizan con el robot elevado, ruedas
  libres y una persona junto al corte físico de potencia.

## Fuente de Verdad

### JSON canónico

El JSON completo es la fuente de verdad en herramientas, backend y ESP. El ESP:

1. recibe el documento completo o por fragmentos;
2. valida sintaxis, versión, referencias, recursos, rangos y compatibilidad;
3. guarda el JSON exacto en un slot `staged`;
4. lo normaliza a una representación C inmutable para ejecución;
5. activa esa representación solo en estado seguro;
6. conserva el JSON que produjo la configuración activa.

La estructura C no es un segundo formato editable ni una fuente independiente.
Es una vista compilada en memoria. Nunca se serializa una estructura C cruda
como perfil persistente, porque su layout depende del compilador y del firmware.

### Qué no pertenece al perfil

El perfil puede contener referencias como `nvs:wifi_credentials` o
`nvs:maintenance_token`, pero no contiene contraseñas, tokens ni llaves. Tampoco
contiene telemetría, estado transitorio, contadores de errores ni una orden de
movimiento. Esos datos tienen otro ciclo de vida y no describen el robot.

## Modelo por Composición

El perfil se construye con IDs estables y referencias, no con índices físicos.

### Placa y comunicaciones

- `board`: target, variante, revisión y pin de E-stop si existe.
- `io.rs485_buses[]`: UART, TX, RX, RTS/DE, baud y modo eléctrico.
- `io.rc_receivers[]`: protocolo, UART, pin RX, inversión, timeout y mapeo.
- Los actuadores PWM incluyen GPIO, timer, channel y calibración.

Los cambios de UART/GPIO/LEDC se validan y activan mediante reinicio. El perfil
factory debe conservar al menos una ruta de recuperación conocida.

### Controladores

`controllers[]` declara cada unidad física:

```json
{
  "id": "drive-main",
  "driver": "svd48",
  "bus_id": "rs485-main",
  "address": 1,
  "channels": [
    { "id": "M1", "actuator_id": "traction-left" },
    { "id": "M2", "actuator_id": "traction-right" }
  ],
  "configuration": {
    "apply_policy": "explicit_only",
    "shared_parameters": {
      "board.max_bus_voltage_v": 58.0
    },
    "channel_parameters": {
      "M1": {
        "motor.pole_pairs": 15,
        "motor.max_rpm": 300,
        "speed_pid.kp": 0.12
      },
      "M2": {
        "motor.pole_pairs": 15,
        "motor.max_rpm": 300,
        "speed_pid.kp": 0.12
      }
    }
  }
}
```

Los IDs `traction-left/right` del fragmento son ilustrativos. El mapping M1/M2
del robot físico se decide únicamente después de la prueba de identidad y se
guarda como configuración, no como convención del driver.

El catálogo SVD48 resuelve cada key tipada a registro, ancho, codec, unidad,
rango y política de persistencia. El JSON nunca contiene direcciones Modbus como
contrato principal. Un campo no verificado permanece presente como valor deseado
o evidencia, pero no se vuelve escribible hasta que el catálogo lo permita.

La genericidad no inventa canales que el hardware no tiene: un SVD48 ofrece M1 y
M2. Cuatro motores pueden usar dos SVD48 con ambos canales o cuatro SVD48 con el
canal que defina cada binding. Un controlador futuro de cuatro canales requerirá
otro driver/capability, pero no otro formato de robot.

`apply_policy: explicit_only` es obligatorio para el MVP: arrancar compara la
configuración live contra la deseada, pero nunca reescribe el controlador por su
cuenta.

### Actuadores y ruedas pasivas

`actuators.traction_motors[]` contiene:

- pose `{x_m, y_m, z_m, yaw_rad}` en el marco del cuerpo;
- binding a `controller_id/channel`;
- radio y ancho de rueda;
- relación `motor_revolutions_per_wheel_revolution`;
- signo de comando y signo de telemetría;
- límites mecánicos y de operación;
- si el actuador es obligatorio para armar.

`actuators.steering[]` contiene el driver del servo, ruedas controladas,
calibración de pulso/ángulo, inversión, trim, ángulo seguro y límites mecánicos.

`passive_wheels[]` representa casters u otros apoyos. Participan en inventario,
geometría y validación mecánica, pero no generan una salida de control.

Las posiciones se expresan en metros en un marco corporal único:

- `+x`: adelante;
- `+y`: izquierda;
- `+z`: arriba;
- `+wz`: giro antihorario visto desde arriba.

La cinemática usa esas poses. `track_width` y `wheelbase` pueden declararse como
medidas nominales para diagnóstico, pero no sustituyen las posiciones por rueda.

## Estrategias Cinemáticas

`robot_kinematics` es una estrategia pura: recibe perfil validado y velocidad de
cuerpo `{vx, vy, wz}`, y devuelve el conjunto completo de objetivos sin tocar
hardware. El coordinador valida todos los objetivos antes del primer write.

### `differential`

Para un par izquierdo/derecho separado por ancho `W`:

```text
velocidad_izquierda = vx - wz * W / 2
velocidad_derecha   = vx + wz * W / 2
```

La configuración identifica qué motores pertenecen a cada lado; puede haber uno
o varios por lado. Cada velocidad lineal se convierte con su radio de rueda y su
relación de transmisión. `vy` se rechaza salvo un deadband explícito.

El primer perfil físico usa:

- un SVD48;
- canales `M1` y `M2`, con lado físico definido por el JSON después de la prueba
  de identidad/dirección;
- dos ruedas locas;
- ninguna dirección por servo.

### `ackermann`

La configuración declara ejes, ruedas motrices y una de estas salidas de
dirección:

- `single_linkage`: un servo gobierna una barra mecánica Ackermann;
- `per_wheel`: cada rueda dirigida tiene su propio actuador;
- una extensión futura anunciada por capacidades.

Para `wz != 0`, el centro instantáneo de rotación parte de `R = vx / wz`. Con la
pose `(x_i, y_i)` de cada rueda, se calculan dirección y velocidad tangencial de
cada rueda. Así las ruedas interior y exterior reciben ángulos y velocidades
diferentes de acuerdo con radio de giro y geometría. En `single_linkage`, el
software comanda el ángulo central calibrado y la mecánica produce la diferencia
interior/exterior.

Ackermann rechaza desplazamiento lateral y giro puro si la geometría declarada
no puede producirlos. No aproxima silenciosamente una maniobra imposible.

### `independent_steer`

Cada módulo de rueda enlaza un motor de tracción y un actuador de dirección. Para
la rueda en `(x_i, y_i)`:

```text
vx_i = vx - wz * y_i
vy_i = vy + wz * x_i
speed_i = sqrt(vx_i^2 + vy_i^2)
angle_i = atan2(vy_i, vx_i)
```

La estrategia aplica límites, inversión de velocidad para reducir el recorrido
de dirección e histéresis cerca del cambio de 180 grados. Crabwalk corresponde a
`wz = 0` con `vy != 0`, habilitado por `allow_crab` en el JSON.

## Arbitraje Simultáneo de Fuentes

Cada adaptador escribe el último comando válido en su propio mailbox, con source
ID, secuencia, timestamp monotónico, validez, dead-man y expiración. Ningún
adaptador llama directamente a SVD48 o servos.

### Canal `control_lan`

`maintenance_lan` conserva discovery, diagnóstico, perfiles y configuración. No
se habilita `MOVE_VEL` dentro de su dispatcher ASCII. El movimiento LAN entra por
un servicio separado, por defecto UDP `32322`, con mensajes compactos:

```json
{
  "type": "botfarms_control_command",
  "protocol_version": "1.0",
  "request_id": "request-uuid",
  "stream_id": "backend-random-id",
  "sequence": 42,
  "token": "...",
  "action": "command",
  "command": {"vx_mps": 0.1, "vy_mps": 0.0, "wz_radps": 0.2, "deadman": true}
}
```

El ESP captura su hora de recepción antes de autenticar/parsear para que ese
trabajo no rejuvenezca el paquete. Rechaza secuencias repetidas o regresivas,
comandos fuera de los límites configurados y streams no armados. Un `stream_id`
nuevo empieza sin autoridad mediante `arm`, fuerza stop/cambio de epoch y luego
necesita un comando fresco. El task de socket no calcula cinemática ni toca
actuadores.

Las únicas acciones son `arm`, `command`, `disarm` y `stop`. `arm` sigue sujeto a
todos los guards del estado; `disarm` y `stop` solo reducen autoridad/movimiento.

El backend renueva el comando mientras el usuario mantiene control activo. Si se
cierra el backend, se corta Wi-Fi o deja de llegar heartbeat, el TTL vence y el
firmware detiene. La parada no depende de que el navegador alcance a enviar un
evento de desconexión.

En cada tick del control:

```text
leer snapshots no bloqueantes de RC, LAN y Bluetooth
descartar datos inválidos o expirados

si RC tiene una trama válida y fresh:
    seleccionar RC, incluso si ordena cero o su dead-man está liberado
si no, si LAN tiene un comando fresh:
    seleccionar LAN
si no, si Bluetooth tiene un comando fresh:
    seleccionar Bluetooth
si no:
    seleccionar NONE

si cambia la fuente seleccionada:
    emitir STOP
    invalidar comandos anteriores
    esperar un comando fresh posterior al cambio

si la fuente activa expira mientras existe movimiento:
    STOP inmediato
    latch de fault/inhibit
```

Reglas obligatorias:

- Prioridad semántica: `RC > LAN > Bluetooth`.
- RC válido preempta inmediatamente LAN/BT. Un RC neutral o sin dead-man bloquea
  fuentes inferiores en vez de dejarlas mover el robot por sorpresa.
- LAN preempta Bluetooth.
- No se reutiliza el último comando de una fuente inferior después de perder la
  superior. Se exige un comando posterior al cambio.
- Todos los comandos tienen TTL configurable dentro de techos seguros del
  firmware. Un valor `0`, infinito o fuera de rango es inválido.
- Cada fuente declara `availability_policy`: `optional`, `required_for_arm` o
  `required_while_armed`. La ausencia de RC no se presume fatal ni inocua; el
  perfil lo decide. Si RC es optional y está ausente, LAN/BT pueden operar; si
  aparece fresh, conserva su preempción obligatoria.
- `STOP ALL` es universal, no necesita autoridad y tiene prioridad de bus.
- USB de ingeniería también debe pasar por el árbitro o permanecer deshabilitado
  para movimiento; nunca conserva el bypass actual.
- Recibir Wi-Fi, hacer JSON, HTTP o logs no ocurre dentro del tick de control.

Las prioridades numéricas viven en el JSON para ser visibles y extensibles, pero
la validación del MVP exige la relación anterior. No son prioridades FreeRTOS.

## Persistencia en el ESP

El partition table actual incluye `storage` de `0x3d0000`, suficiente para
perfiles JSON y evidencia. La implementación propuesta usa slots A/B en esa
partición y NVS solo para metadatos pequeños:

```text
factory: JSON embebido, inmutable
slot_a.json / slot_b.json: candidatos completos
active_slot + revision + sha256: commit pequeño en NVS
known_good_slot + revision + sha256: recovery
```

Flujo de commit:

1. escribir el slot inactivo;
2. cerrar y releer;
3. validar JSON, semántica y SHA-256;
4. cambiar el puntero NVS en una sola operación;
5. conservar el slot anterior como rollback.

Se deben probar cortes de energía en cada paso. La implementación puede escoger
FATFS o una partición dedicada, pero no puede volver a convertir un blob C en la
fuente de verdad.

## Validación

Hay cuatro capas de validación:

1. JSON Schema: tipos, campos requeridos y forma.
2. Semántica: IDs únicos, referencias existentes y ninguna asignación duplicada.
3. Capacidades: drivers, estrategias, UART/LEDC/GPIO, memoria y máximos anunciados
   por este build/board.
4. Seguridad física: límites de corriente, voltaje, RPM, pulsos, ángulos,
   watchdog y tiempos dentro de techos compilados.

El schema no fija `maxItems` a cuatro. El ESP anuncia, por ejemplo,
`max_svd48_controllers`, `max_traction_motors` y `max_steering_actuators` según sus
recursos. Exceder una capacidad produce un error de validación explícito; no
cambia el significado del perfil ni exige otra variante de schema.

## Configuración SVD48 Mínima del MVP

El primer catálogo editable incluye, sujeto a evidencia del manual y hardware:

- identidad y versiones como solo lectura;
- polos magnéticos/pole pairs;
- sensor y dirección;
- RPM y corriente máximas;
- modo de velocidad;
- aceleración, desaceleración y smoothing;
- límite máximo de bus después de confirmar batería;
- PID de velocidad `Kp`, `Ki`, `Kd`;
- dead zone de velocidad;
- errores y telemetría requerida para readback.

Antes del primer write float se captura el orden de palabras desde SV-Config o
una lectura conocida. Los registros de relación de engranaje reportados como
sospechosos no bloquean el MVP: la relación mecánica del JSON gobierna la
conversión en el ESP hasta verificar el comportamiento del SVD48.

Cada write ejecuta:

```text
validar DISARMED/MAINTENANCE y RPM cero/fresh
leer old value
validar key, tipo, rango y controller capability
escribir una sola vez
leer de nuevo
clasificar APPLIED / NOT_APPLIED / READBACK_MISMATCH / OUTCOME_UNKNOWN
permanecer sin movimiento ante cualquier ambigüedad
```

No se requiere autenticación de operador, challenge/response ni ownership de
sesión para el MVP local. Sí se requiere exclusión de operación, estado
`MAINTENANCE`, request ID idempotente y token LAN ya provisionado. Esas son
garantías de seguridad y consistencia, no controles contra atacantes.

## Contrato Firmware y Backend Antes del Frontend

El firmware expone operaciones de dominio comunes a USB y LAN:

```text
capabilities.get
robot_profile.get
robot_profile.validate
robot_profile.stage.begin/chunk/finish
robot_profile.activate
robot_profile.rollback
control_sources.status
safety.status / arm / disarm / fault_ack / stop
svd48.schema / inventory / read / compare
svd48.change_set.validate / apply / status
```

El backend implementa después:

```text
GET  /api/v1/capabilities
GET  /api/v1/robot-profile
POST /api/v1/robot-profile/validate
POST /api/v1/robot-profile/stage
POST /api/v1/robot-profile/activate
POST /api/v1/robot-profile/rollback
GET  /api/v1/control-sources
POST /api/v1/control/arm
POST /api/v1/control/command
POST /api/v1/control/disarm
POST /api/v1/control/stop
GET  /api/v1/svd48/schema
GET  /api/v1/svd48/controllers
POST /api/v1/svd48/read
POST /api/v1/svd48/change-sets/validate
POST /api/v1/svd48/change-sets/apply
```

`SerialTransport` y `LanTransport` producen el mismo resultado tipado. El
backend valida JSON antes de enviarlo, pero el ESP vuelve a validar todo. Los
payloads grandes se transfieren por chunks con upload ID, número de chunk,
longitud y hash final; no se meten en un datagrama UDP gigante.

El frontend empieza únicamente cuando estas rutas tienen fixtures compartidos,
tests backend y pruebas reales por USB/LAN. No se diseñan formularios contra un
contrato todavía cambiante.

## Orden de Implementación

### P0 - Seguridad runtime y arbitraje

- conectar el modelo de estados al runtime;
- gatear todas las salidas en el actuator boundary;
- implementar mailboxes, árbitro `RC > LAN > BT`, TTL y stop-before-switch;
- tratar stale/offline/fault y pérdida de fuente durante movimiento;
- dar prioridad real de bus a `STOP ALL`;
- crear tests de reloj/bus/fuentes simuladas.

Salida: ninguna fuente ni pérdida de comunicación puede dejar movimiento sin
watchdog, y ninguna entrada actual conserva un bypass.

### P1 - JSON canónico y diferencial 2WD

- implementar parser, validador, capacidades y almacenamiento A/B;
- agregar fixture factory equivalente al hardware actual conocido;
- implementar la estrategia genérica `differential` por listas de lado;
- activar el perfil de un SVD48 `M1/M2`, dos ruedas motrices y dos locas;
- probar persistencia, corrupción, referencias y límites en host.

### P2 - Catálogo y writes SVD48 mínimos

- terminar codecs y catálogo tipado;
- exponer inventory/read/compare por USB y LAN;
- habilitar change sets con old value, un write y readback;
- verificar y habilitar los parámetros mínimos, incluido PID de velocidad cuando
  el float tenga evidencia.

### P3 - Cinemáticas genéricas restantes

- fixtures para dos controladores/cuatro motores y cuatro controladores;
- Ackermann con linkage y con dirección individual;
- independent steer y crabwalk;
- pruebas puras de geometría, inversión, saturación y maniobras imposibles.

### P4 - Backend

- modelo JSON y validación compartida;
- transportes USB/LAN tipados;
- endpoints de perfil, fuentes, safety y SVD48;
- jobs idempotentes para stage/apply y telemetría centralizada;
- tests unitarios, API, transport y fault injection.

### P5 - Frontend

- comenzar solo después de cerrar P0-P4;
- diseñar vistas desde capabilities/schema reales;
- separar perfil, inventario, configuración SVD48, tuning y telemetría;
- no generar registros, codecs ni reglas de seguridad en el navegador.

## Pruebas de Compatibilidad Obligatorias

Fixtures host:

- un SVD48, `M1/M2`, diferencial 2WD y dos casters;
- dos SVD48, dos canales por controlador y cuatro motores;
- cuatro SVD48, un motor usado por controlador;
- Ackermann con un servo/linkage;
- Ackermann con servos por rueda dirigida;
- cuatro módulos de dirección independiente con crabwalk;
- perfil con IDs/referencias duplicadas, canal inexistente, recurso GPIO
  conflictivo, capacidad excedida y límites peligrosos.

Arbitraje simulado:

- BT activo, luego LAN fresh: LAN preempta mediante stop y comando nuevo;
- LAN activo, luego RC fresh: RC preempta dentro del bound;
- RC neutral o sin dead-man: bloquea LAN/BT y comanda cero;
- pérdida de RC durante movimiento: stop/fault, sin fallback a LAN viejo;
- expiración LAN durante movimiento: stop/fault, sin fallback a BT viejo;
- todas las fuentes simultáneas: solo RC llega al actuator coordinator;
- `STOP ALL` desde cualquier transporte en todos los estados.

Hardware elevado:

- inventario y configuración read-only antes de energizar movimiento;
- identidad, signo y stop de cada rueda individual;
- differential straight/left/right con RPM calculadas;
- preempción RC/LAN/BT a velocidad mínima;
- pérdida de cada fuente y medición de latencia a frame cero/RPM cero;
- stale/offline/fault SVD48 y falla parcial de apply;
- write y readback de cada parámetro allowlisted sobre controlador respaldado;
- reinicio y verificación de persistencia solo cuando la política del registro se
  haya confirmado.

## Diferido, No Eliminado

- autenticación de usuarios, roles, TLS/HMAC, replay security y cifrado;
- Secure Boot, flash/NVS encryption y firma de perfiles;
- editor visual frontend antes de estabilizar backend;
- auto-tuning PID;
- calibraciones Hall/encoder que puedan girar motores;
- reescritura automática SVD48 al arrancar;
- operación degradada con un actuador obligatorio offline;
- control autónomo y pruebas de piso.

Se revisan antes de exposición a Internet, entrega a cliente, producción o
movimiento autónomo. Idempotencia, watchdog, readback, límites y parada física no
se difieren porque mitigan fallas accidentales del prototipo.
