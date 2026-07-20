# Proceso de Perfiles JSON, Cinemática y Autoridad

Fecha: 2026-07-19

Estado: `IN_PROGRESS`. Fundaciones puras de autoridad y cinemática diferencial
implementadas con evidencia `E1`; integración runtime/perfiles/hardware pendiente.

Referencias:

- `../ROBOT_PROFILES_AND_SVD48_CONFIGURATION_PLAN.md`
- `../schemas/robot-profile.schema.json`
- `../examples/robot-profile-differential-2wd-one-svd48.json`
- `adr/0002-canonical-json-robot-profile.md`
- `adr/0003-simultaneous-command-source-arbitration.md`

## Brecha Actual

El firmware todavía contiene estas decisiones en compilación:

- dos SVD48 con IDs `{1, 2}` y cuatro canales lógicos;
- motores `0..3` en orden fijo;
- cuatro slots de servo y GPIO fijos;
- una sola geometría global;
- una cinemática de cuatro ruedas dirigibles aun cuando los servos están
  deshabilitados;
- comandos USB/LAN/RC sin un árbitro único de autoridad runtime.

La presencia de `robot_state_model`, su servicio mutex, `command_authority` y
`robot_kinematics` no corrige esto todavía: todos compilan, pero `main` no los
instancia como ruta de movimiento ni existe aún un coordinador único que haga el
gate inmediatamente antes del I/O.

## Implementación Disponible En Build 13

- `command_authority_model` implementa prioridad `RC > LAN > Bluetooth`, TTL,
  dead-man, secuencia, historial acotado fail-closed de streams retirados,
  stop-before-switch, barrera temporal fresh-after-switch y fault-stop por pérdida
  de una fuente que estaba moviendo.
- `robot_kinematics_differential_inverse` acepta arreglos variables con uno o más
  motores por lado y aplica radio, relación, signo, límite RPM y saturación global.
- `robot_state_service` serializa transiciones y snapshots sin ejecutar callbacks
  externos, pero no expone un supuesto permit check/use: ese gate debe vivir
  dentro del coordinador de
  actuadores para no crear una carrera entre validación y write.
- `control_lan` valida un protocolo UDP compacto en `32322`, versión, token,
  tamaño, stream/sequence y límites de velocidad; solo emite eventos tipados. El
  componente está compilado pero no se inicia hasta conectar gate, autoridad,
  perfil y coordinador.
- `main`, USB `MOVE_VEL`, `robot_control`, RC y Bluetooth siguen en la ruta
  heredada. Por tanto no hay autorización de movimiento LAN ni `GATE-SAFE`.

## Inventario Físico

No se activará un perfil con medidas inferidas. Cada robot debe completar:

| Profile ID | Controladores/canales | Motores | Dirección | Ruedas pasivas | Medidas | Motor/transmisión | Estado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `differential-2wd-one-svd48` | 1 SVD48; M1/M2, lado por verificar | 2 | diferencial | 2 casters | medir poses/radios/body | verificar polos, ratio, sensor, límites | `DRAFT` |
| `four-motor-two-svd48` | 2 SVD48; M1/M2 en ambos | 4 | por definir en JSON | por medir | por medir | por controlador/canal | `NOT_STARTED` |
| `four-motor-four-svd48` | 4 SVD48; canal usado por perfil | 4 | por definir en JSON | por medir | por medir | por controlador/canal | `NOT_STARTED` |
| `ackermann-linkage` | lista variable | variable | un servo/linkage | variable | wheelbase/track/poses | verificar | `NOT_STARTED` |
| `independent-steer-crab` | lista variable | variable | servo por módulo | variable | pose por módulo | verificar | `NOT_STARTED` |

Evidencia mínima antes de `activation_allowed:true`:

- foto/diagrama de cableado etiquetado;
- dirección física positiva por motor;
- radio efectivo de cada rueda;
- posición corporal `(x, y, z)` de cada rueda;
- relación motor-vuelta/rueda-vuelta;
- IDs Modbus y canales;
- límites eléctricos de batería, motor y controlador;
- calibración y tope mecánico de cada servo;
- pinout confirmado de la placa.

## Perfil Canónico

Existe un solo documento JSON autocontenido. Sus secciones internas conservan
responsabilidades claras, pero no son perfiles externos que puedan divergir:

```text
robot-profile.json
  metadata + lifecycle
  board
  io.rs485_buses[] + io.rc_receivers[]
  controllers[]
    channels[] -> actuator_id
    configuration.shared_parameters
    configuration.channel_parameters
  actuators.traction_motors[]
  actuators.steering[]
  passive_wheels[]
  geometry
  kinematics
  control_sources
  safety
  services (solo referencias a secretos)
```

El backend puede ofrecer templates y edición por secciones. Antes de enviar al
ESP debe resolver cualquier template/include y producir un JSON completo. El ESP
no depende del backend para interpretar el perfil activo.

### Capacidad frente a topología

El schema no limita semánticamente el robot a cuatro motores. El firmware sí
necesita límites de recursos para memoria y tiempos. Los publica como
capabilities:

```json
{
  "max_rs485_buses": 2,
  "max_svd48_controllers": 8,
  "max_traction_motors": 8,
  "max_steering_actuators": 8,
  "supported_kinematics": [
    "differential",
    "ackermann",
    "independent_steer"
  ]
}
```

Los números anteriores son ejemplo de contrato, no valores decididos. Deben
derivarse de presupuesto de heap, stack, UART/LEDC y periodo de bus. Cambiar una
topología dentro de esas capacidades no requiere recompilar.

## Validación y Activación

Orden obligatorio:

```text
parse JSON
-> validar schema/version
-> validar IDs únicos y referencias
-> validar topología/cinemática
-> validar GPIO/UART/LEDC/capabilities
-> validar límites contra hard ceilings
-> construir snapshot C inmutable
-> guardar STAGED y releer/hash
-> activar solo DISARMED/MAINTENANCE, RPM cero y telemetría fresh
```

Errores mínimos por campo:

- `DUPLICATE_ID`
- `REFERENCE_NOT_FOUND`
- `CHANNEL_ALREADY_BOUND`
- `GPIO_CONFLICT`
- `RESOURCE_CAPACITY_EXCEEDED`
- `UNSUPPORTED_DRIVER`
- `UNSUPPORTED_KINEMATICS`
- `KINEMATICS_TOPOLOGY_MISMATCH`
- `LIMIT_EXCEEDS_FIRMWARE_CEILING`
- `DRAFT_PROFILE_NOT_ACTIVATABLE`
- `CONTROLLER_CONFIGURATION_INCOMPLETE`

## Persistencia JSON

Slots:

- `factory`: JSON embebido e inmutable;
- `staged`: slot A/B escrito y validado, no activo;
- `active`: puntero NVS al JSON en uso;
- `known_good`: puntero a revisión promovida después de pruebas.

Cada slot conserva longitud, schema version, profile ID, revision y SHA-256. Se
escribe el slot inactivo, se relee y solo después se conmuta el puntero. Un corte
de energía no puede destruir simultáneamente `active` y `known_good`.

## Estados y Movimiento

| Estado | Movimiento | Lecturas | Configuración | OTA |
| --- | --- | --- | --- | --- |
| `BOOTING` | nunca | self-test | nunca | nunca |
| `DISARMED` | rechazado; stop sí | sí | validate/stage; apply mediante mantenimiento | check/download según política |
| `ARMED` | solo salida del árbitro | telemetría priorizada | nunca | nunca |
| `FAULTED` | rechazado; stop sí | sí | diagnóstico | nunca |
| `MAINTENANCE` | cero obligatorio | sí | operación exclusiva, tipada y con readback | no install |
| `OTA` | rechazado; cero obligatorio | diagnóstico | nunca | operación exclusiva |

El gate se consulta en el límite del actuador inmediatamente antes de escribir
SVD48/servo. Validarlo solo al recibir un paquete no es suficiente.

## Árbitro `RC > LAN > Bluetooth`

### Adaptadores

RC, LAN y Bluetooth se ejecutan simultáneamente y solo publican mailboxes:

```text
source_id
source_type
sequence
received_at_monotonic
expires_at_monotonic
valid
deadman
body_velocity {vx, vy, wz}
```

LAN de control no reutiliza el command dispatcher de `maintenance_lan`.
`control_lan` recibe datagramas compactos en un puerto separado, valida token,
`stream_id`, secuencia, finitud y tamaño, y publica el mailbox LAN. Un stream
nuevo o una secuencia regresiva no puede reactivar un comando anterior. La tarea
de socket queda por debajo de RC/control y por encima de management/OTA solo en
la medida necesaria para renovar el mailbox sin bloquear.

El control no espera sockets, UART parsing, JSON, Bluetooth callbacks ni locks de
red. Copia snapshots pequeños y selecciona la autoridad.

### Elegibilidad

- RC: trama válida y fresh. Si está neutral o sin dead-man, sigue siendo la
  fuente superior y su salida es cero; bloquea LAN/BT.
- LAN: último comando autenticado con el token existente, fresh y dentro de TTL.
- Bluetooth: conexión válida y último comando fresh.
- Toda fuente: secuencia no regresiva, números finitos y límites válidos.
- La presencia exigida por cada fuente se toma de `availability_policy`; no se
  presupone que RC ausente bloquee un robot que fue configurado para LAN/BT.

### Preempción y pérdida

```text
si aparece una fuente superior:
    bloquear nuevas salidas de la fuente anterior
    encolar STOP de emergencia
    marcar epoch nuevo de autoridad
    exigir comando recibido después del nuevo epoch
    aplicar solo entonces

si la fuente activa expira mientras movía:
    encolar STOP
    latchear SOURCE_LOST
    pasar a FAULTED
    no caer automáticamente a un comando viejo de otra fuente
```

LAN preempta Bluetooth con la misma secuencia. `STOP ALL` evita el árbitro solo
para detener; ningún transporte puede evitarlo o asignarse autoridad directa.

Las prioridades `300/200/100` pueden aparecer en el JSON para diagnóstico, pero
la validación exige estrictamente `RC > LAN > Bluetooth`. Las prioridades de
tareas FreeRTOS son una decisión interna: safety/control y RC por encima de
telemetría, Wi-Fi, LAN management, Bluetooth no crítico y OTA.

Prioridades iniciales para implementar y luego medir:

| Tarea/clase | Prioridad objetivo |
| --- | ---: |
| safety + control tick + arbiter | 10 |
| emergency RS485 dispatch | 9 |
| RC UART ingest/parser | 8 |
| SVD48 health/poll scheduler | 7 |
| serial gateway | 6 |
| `control_lan` socket ingress | 5 |
| Bluetooth ingress | 4 |
| telemetry fan-out | 3 |
| `maintenance_lan`, Wi-Fi reconnect, OTA check | 1-2 |

Estos números no son configuración del robot ni garantía por sí solos. Los tests
de latencia y stack/CPU pueden ajustarlos, preservando el orden safety/RC por
encima de ingress de red y evitando cualquier I/O bloqueante en prioridad 10.

## Estrategias Cinemáticas

Todas reciben listas por ID y poses físicas. Todas entregan un target completo
antes de I/O.

### Differential

El perfil declara `left_motors[]`, `right_motors[]` y `track_width_m`.

```text
left_mps  = vx - wz * track_width_m / 2
right_mps = vx + wz * track_width_m / 2
```

Cada motor usa su radio, relación y signo. Puede haber uno o varios motores por
lado. Un motor obligatorio offline inhibe el robot completo en el MVP.

### Ackermann

El perfil declara ruedas dirigidas, motores, wheelbase, track, radio mínimo y
layout `single_linkage` o `per_wheel`. La estrategia usa el centro instantáneo de
rotación para calcular velocidad por pose y ángulo interior/exterior. Giro puro y
velocidad lateral se rechazan si son físicamente imposibles.

### Independent steer / crabwalk

Cada módulo enlaza un motor y un actuador de dirección. La velocidad local usa:

```text
vx_i = vx - wz * y_i
vy_i = vy + wz * x_i
```

`allow_crab` permite `vy` con `wz=0`. Optimización de reversa, límites e histéresis
son parte de la estrategia y del perfil, no constantes globales.

## Apply de Actuadores

1. Verificar estado, epoch de autoridad, TTL y valores finitos.
2. Calcular todos los targets sin I/O.
3. Validar todos contra perfil y hard ceilings.
4. Reservar la secuencia de bus completa.
5. Escribir setpoints nuevos antes de `START`.
6. Aplicar steering y tracción en orden definido por estrategia.
7. Ante falla parcial: `STOP ALL`, `PARTIAL_APPLY` y `FAULTED`.
8. Registrar subset aplicado y resultado de stop.

No se promete atomicidad entre controladores físicos. Se promete detección,
inhibición y parada best effort con latencia medida.

## Work Items

### Perfiles

- `PROF-001`: schema JSON y fixtures topológicos.
- `PROF-002`: validador puro de schema/semántica/capabilities/ceilings.
- `PROF-003`: slots JSON A/B, NVS metadata y recuperación de power loss.
- `PROF-004`: factory JSON que reproduce el hardware actual sin literales de
  topología en `main`.
- `PROF-005`: APIs get/validate/stage/activate/rollback.
- `PROF-006`: migración de schema y corrupción.
- `PROF-008`: board/GPIO/UART/LEDC dentro del mismo JSON con reboot seguro.
- `PROF-009`: transferencia chunked del JSON completo por USB/LAN.

### Autoridad

- `SAFE-003A`: modelo de mailbox completo; adaptadores RC/LAN/BT pendientes.
- `SAFE-003B`: árbitro determinista y epoch implementados/probados en host.
- `SAFE-003C`: stop-before-switch, stream retirement y fresh-after-switch
  implementados/probados en host.
- `SAFE-003D`: TTL/fault implementados en el modelo; métricas/runtime pendientes.

### Cinemática

- `KIN-001`: target array genérico implementado para differential; selector de
  estrategias/perfil pendiente.
- `KIN-002`: differential con listas variables por lado implementado/probado.
- `KIN-003`: fixtures de uno/dos/cuatro controladores sin estrategia distinta.
- `KIN-004`: Ackermann linkage/per-wheel.
- `KIN-005`: independent steer/crab y servo calibration.

## Acceptance

Perfiles (`E1/E3`):

- [ ] El schema y todos los fixtures válidos pasan validación.
- [ ] No existe `ROBOT_PROFILE_MAX_MOTORS 4` como significado de topología.
- [ ] IDs/canales/pines duplicados y referencias inexistentes fallan por campo.
- [ ] Un draft nunca se activa.
- [ ] Exceder capabilities falla sin truncar arrays.
- [ ] Corte de energía conserva active o known-good válido.
- [ ] El JSON leído del ESP corresponde al JSON activo por hash/revision.

Autoridad (`E1/E5`):

Evidencia actual: las cinco primeras propiedades pasan en host (`E1`), pero sus
checkboxes permanecen abiertos hasta medir integración/latencia elevada (`E5`).

- [ ] RC preempta LAN y BT dentro del bound medido.
- [ ] RC neutral/sin dead-man produce cero y bloquea fuentes inferiores.
- [ ] LAN preempta BT.
- [ ] Pérdida de fuente en movimiento detiene y no hace stale fallback.
- [ ] Un comando recibido antes del cambio de epoch nunca se aplica después.
- [ ] Todas las entradas de movimiento, incluido USB legado, pasan por el gate.

Cinemática (`E1/E5`):

Evidencia actual: differential 1..N, rechazo de `vy`/NaN, radio/relación/signo y
saturación pasan en host (`E1`). Ackermann, crab, perfiles y `E5` siguen abiertos.

- [ ] Differential soporta 1..N motores por lado según capabilities.
- [ ] Ackermann produce diferencias interior/exterior coherentes con poses.
- [ ] Independent steer soporta crabwalk cuando el perfil lo habilita.
- [ ] Ejes no soportados se rechazan sin escribir un target parcial.
- [ ] Saturación, radio, relación y signo se aplican por actuador.

## Límites Deliberados del MVP

- Drivers iniciales: ESP32-S3 + SVD48 + servo LEDC + i-BUS/SBUS + LAN + BLE.
- Estrategias iniciales: differential, Ackermann e independent steer.
- No se soporta degradación automática con actuadores obligatorios offline.
- El perfil no contiene secretos ni estado transitorio.
- El perfil puede ser genérico y variable, pero solo activa drivers/capacidades
  anunciados por el build y la placa.
- Toda validación física se hace elevada antes de diseñar una prueba de piso.
