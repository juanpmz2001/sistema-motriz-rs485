# Plan MVP de Escritura SVD48 y Activación del Perfil JSON

Fecha: 2026-07-19

Estado: `IN_PROGRESS`. El modelo/servicio de estados, el árbitro puro de fuentes,
la cinemática diferencial y FC `0x10` existen. También hay un vertical
provisional de banco: `WRITE_REG(S) ... CONFIRM` por USB/LAN, prelectura,
readback y editor web. Todavía faltan coordinador/gate runtime, adaptadores de
fuentes, catálogo tipado completo, operación exclusiva, perfil JSON y writes de
producción.

## Avance Verificado

- `components/robot_state/robot_state_model.c` implementa seis estados, inhibits,
  fault latch, revisión monotónica y políticas separadas de movimiento,
  configuración y OTA.
- `robot_state_service`, `command_authority` y `robot_kinematics` compilan; no
  están instanciados por `main` ni son todavía la ruta de actuadores.
- `tests/` aporta CMake/CTest, fake clock y event sink.
- `svd48_write_registers_by_id()` valida FC `0x10` y hace un solo intento para no
  repetir a ciegas un write cuyo ACK se perdió.
- `./tools/run_host_tests.sh` pasa 7/7 y ESP-IDF 5.4.1 compila build 14 para
  `esp32s3`.
- El write provisional exige `CONFIRM`, bloquea registros de actuación, consulta
  el stopped heuristic, captura old value, escribe y verifica readback. FC
  `0x10` no hace retry ciego.
- El backend web ya ofrece `/api/svd48/register/read` y
  `/api/svd48/register/write`; la UI de banco muestra words y ambas
  interpretaciones float. El orden float PID sigue sin evidencia física.

Esto no autoriza movimiento ni configura por sí solo un controlador físico. El
vertical raw es utilizable únicamente con ruedas elevadas y corte disponible;
no sustituye `MAINTENANCE`, catálogo/hard ceilings, dedupe, rollback o save.

## Objetivo MVP

Permitir que firmware y luego backend puedan:

- recibir y validar el robot JSON canónico;
- leer parámetros SVD48 por key, tipo, unidad, rango y freshness;
- comparar JSON desired contra controlador live;
- preparar un change set sin escribir;
- aplicar parámetros allowlisted con old value, un intento y readback;
- parametrizar polos, sensor, dirección, límites, rampas y PID de velocidad;
- cambiar pines/buses/servos a través del mismo JSON con activación reboot-only;
- operar el mismo contrato por USB y LAN local con el token existente;
- distinguir rechazo, exception, timeout, mismatch y resultado ambiguo.

El frontend final tipado sigue después del backend estable. El editor raw actual
es una herramienta técnica provisional para desbloquear la parametrización MVP.

## Prioridad de Riesgos

El MVP protege contra fallas accidentales y de comunicación, no contra un atacante
en la red local.

Obligatorio ahora:

- estado y gate de actuadores;
- watchdog/TTL de movimiento y árbitro `RC > LAN > Bluetooth`;
- stop universal y prioridad de bus;
- límites, validación y readback;
- dedupe por request ID para mutaciones LAN;
- robot elevado y corte físico durante HIL;
- recuperación de perfil corrupto o pinout inválido.

Diferido hasta producción/Internet:

- cuentas de usuario, roles, CSRF y sesiones de operador;
- challenge/response, HMAC, nonce/timestamp y replay security;
- TLS, cifrado de perfil y firma del perfil;
- audit de identidad humana.

El maintenance token ya provisionado sigue siendo el control de acceso suficiente
para el prototipo LAN. Los secretos nunca se incluyen en el robot JSON.

## Realidad Actual

| Superficie | Estado actual | Brecha |
| --- | --- | --- |
| Estado | modelo puro no integrado | una ruta puede mover mientras otra detiene |
| Fuentes | sin árbitro único | stale, prioridad y preempción no están definidas runtime |
| Movimiento | target-before-START y fail-stop best effort | no consulta gate; apply sigue secuencial |
| SVD48 write | raw confirmado FC `0x06/0x10`, old value y readback | sin catálogo completo, job, dedupe, rollback o save verificado |
| LAN | token; read/STOP y raw config confirmado | no transporta JSON grande ni change sets idempotentes |
| Perfil | literales en `main.c` | topología/pines/geometría no son configurables |
| Backend | transporte serial/LAN y endpoints raw de registros con tests LAN fake | faltan dominio de perfiles/jobs, tests serial/fault completos y HIL |

## Máquina de Estados

| Estado | Movimiento | Lecturas | Writes SVD48 | Perfil JSON | OTA |
| --- | --- | --- | --- | --- | --- |
| `BOOTING` | nunca | self-test | nunca | cargar/validar, no mover | nunca |
| `DISARMED` | no; STOP sí | sí | no | get/validate/stage | check/download |
| `ARMED` | solo árbitro válido | telemetría | nunca | no mutar/activar | nunca |
| `FAULTED` | no; STOP sí | diagnóstico | nunca | export/rollback seguro | nunca |
| `MAINTENANCE` | cero obligatorio | sí | job exclusivo | stage/activate según tipo | no install |
| `OTA` | no; cero obligatorio | diagnóstico | nunca | nunca | install exclusivo |

No existe `BOOTING -> ARMED`, reconexión -> `ARMED`, reinicio -> `ARMED` ni
fallback automático de una fuente de comando perdida.

### Invariantes

1. Ninguna salida llega al driver si `state != ARMED`.
2. Toda salida requiere authority epoch vigente, comando fresh, números finitos y
   ausencia de inhibits.
3. Todo write requiere `MAINTENANCE`, exclusión de job, RPM cero y telemetría
   fresh.
4. Configuración y OTA son mutuamente excluyentes.
5. `STOP ALL` se acepta en cualquier estado y tiene clase de bus de emergencia.
6. Un timeout post-TX no se reintenta ni se clasifica como "no escrito".
7. Stale/unknown nunca se convierte en cero ni sirve como old value.
8. Un perfil draft, inválido o que elimina recuperación nunca se activa.
9. Perder la fuente activa durante movimiento causa stop y fault; no activa un
   comando inferior anterior.

## Componentes Objetivo

```text
components/robot_state/
  robot_state_model.c
  robot_state_service.c

components/command_authority/
  command_mailbox.c
  command_arbiter.c          # RC > LAN > Bluetooth, epoch y TTL

components/control_lan/
  control_lan.c              # ingress compacto; solo publica mailbox LAN

components/robot_profile/
  robot_profile_json.c       # parse + schema shape
  robot_profile_validate.c   # refs/capabilities/ceilings
  robot_profile_store.c      # A/B JSON + NVS metadata
  robot_profile_runtime.c    # snapshot C inmutable

components/robot_kinematics/
  differential.c
  ackermann.c
  independent_steer.c

components/svd48_parameters/
  svd48_parameter_catalog.c
  svd48_codec.c
  svd48_change_set.c

components/rs485_scheduler/
  rs485_scheduler.c          # emergency > motion > config > telemetry
```

Nombres finales pueden adaptarse al patrón del repo. Las responsabilidades no se
deben volver a mezclar en `serial_gateway`.

## Lectura Tipada

Una lectura raw como `0x5018 = 0x0018` no informa tipo, unidad ni alcance. El
servicio tipado devuelve:

```json
{
  "controller_id": "drive-main",
  "channel": "M1",
  "key": "motor.pole_pairs",
  "type": "u16",
  "value": 24,
  "unit": "pole_pairs",
  "range": {"min": 1, "max": 128},
  "access": "read_write",
  "freshness": "fresh",
  "confidence": "verified"
}
```

El registro resuelto puede aparecer en diagnóstico, pero no es el contrato que
edita backend/frontend. El mismo descriptor decodifica, valida, codifica y
comprueba readback.

## Operación de Mantenimiento MVP

No hay login, owner humano ni autorización de sesión. Sí hay un job firmware
exclusivo porque dos writes simultáneos son una falla de consistencia.

### Apertura automática

Un request de configuración tipado intenta entrar a `MAINTENANCE` si:

- estado actual es `DISARMED`;
- no hay OTA ni otro job;
- el árbitro no tiene una orden de movimiento;
- STOP fue solicitado;
- todos los actuadores obligatorios reportan RPM fresh dentro de umbral;
- controladores objetivo están online/fresh;
- por LAN, el maintenance token es válido.

RC/LAN/BT siguen siendo leídos para diagnóstico durante mantenimiento, pero sus
órdenes de movimiento se descartan por estado. Al finalizar correctamente el job,
el robot vuelve a `DISARMED`, nunca a `ARMED`.

### Baseline

Antes de aplicar:

- leer identidad/version del controlador;
- leer cada key involucrada;
- guardar valor, timestamp, exception, descriptor revision y hash;
- rechazar unsupported, stale o undecodable.

### Dry run

Firmware valida:

- catálogo, access, confidence y capability;
- rango del parámetro y hard ceiling;
- controller/channel scope;
- baseline no modificado;
- dependencias entre campos;
- codec y persistencia;
- request ID no usado con otro payload.

No hay write durante `validate`.

### Apply

```text
recibir request_id + change_set + baseline_hash
si request_id ya terminó con el mismo payload:
    devolver el mismo resultado sin repetir
si request_id existe con otro payload:
    rechazar ID_CONFLICT

entrar MAINTENANCE
STOP y confirmar RPM cero/fresh
releer old values
validar nuevamente todo

por cada operación ordenada:
    escribir una sola vez
    validar ACK/exception
    leer el valor de nuevo
    clasificar resultado

si cualquier resultado es ambiguo o mismatch:
    permanecer inhibido
    FAULTED o recovery-required según severidad
si todo coincide:
    cerrar job -> DISARMED
```

Resultados mínimos:

- `APPLIED_VERIFIED`
- `REJECTED_STATE`
- `REJECTED_STALE`
- `REJECTED_RANGE`
- `UNSUPPORTED`
- `DEVICE_EXCEPTION`
- `NOT_APPLIED_VERIFIED`
- `READBACK_MISMATCH`
- `OUTCOME_UNKNOWN`
- `ID_CONFLICT`

`0x10` agrupa palabras contiguas de un controlador; no vuelve atómicos dos
controladores ni garantiza save en flash.

### Persistencia SVD48

Aplicar y guardar en flash son operaciones distintas. `SVD-022` sigue bloqueado
hasta capturar el comando save, reinicio y recuperación en un controlador
respaldado. El estado reporta `runtime`, `pending_save`, `persisted` o `unknown`.

## Catálogo Inicial

Primera allowlist, activada campo por campo con evidencia:

- pole pairs;
- sensor type;
- motor direction;
- max RPM/current;
- speed mode;
- acceleration/deceleration/smoothing;
- max bus voltage después de verificar batería;
- speed PID `Kp/Ki/Kd` después de verificar float word order;
- speed dead zone.

Identity/version/errors/telemetry son read-only. Address/baud, save,
Hall/encoder calibration, registros sospechosos de gear y límites no confirmados
siguen bloqueados hasta su experimento específico.

La relación mecánica motor/rueda vive en el JSON y permite convertir velocidad
aunque los registros gear del SVD48 sigan sin verificarse.

## Configuración de Pines Dentro del JSON

No existe un `board_profile` externo. `board`, `io` y los drivers de servo forman
parte del robot JSON autocontenido.

Política:

- flash/PSRAM, boot/recovery y E-stop reservado: no editable o solo valores
  permitidos por la variante de placa;
- UART/RS485/RC/GPIO/LEDC: staged y reboot-only;
- trim/ángulos/límites dentro de ceiling: activación detenida según capability.

Activación:

```text
backend envía JSON completo
-> firmware valida schema/semántica/recursos/ceilings
-> escribe y relee slot STAGED
-> solicita reboot explícito si cambia recursos
-> BOOTING inicializa snapshot sin permitir movimiento
-> self-test OK: DISARMED
-> promoción posterior y explícita a known_good
-> fallo/watchdog: recuperar known_good/factory y permanecer sin movimiento
```

## Contrato Firmware

```text
capabilities.get
safety.status / arm / disarm / fault_ack / stop
control_sources.status
robot_profile.get
robot_profile.validate
robot_profile.stage.begin / chunk / finish
robot_profile.activate / rollback
svd48.schema / inventory / read / compare
svd48.change_set.validate / apply / status
```

Cada resultado contiene protocol version, request ID, command/job ID, boot ID,
state revision, result code, timestamps y data tipada. Serial y LAN son adapters
de framing; no implementan reglas de dominio distintas.

## Backend Antes del Frontend

Endpoints backend objetivo:

```text
GET  /api/v1/capabilities
GET  /api/v1/device/safety
GET  /api/v1/control-sources
POST /api/v1/control/arm
POST /api/v1/control/command
POST /api/v1/control/disarm
POST /api/v1/control/stop
GET  /api/v1/robot-profile
POST /api/v1/robot-profile/validate
POST /api/v1/robot-profile/stage
POST /api/v1/robot-profile/activate
POST /api/v1/robot-profile/rollback
GET  /api/v1/svd48/schema
GET  /api/v1/svd48/controllers
POST /api/v1/svd48/read
POST /api/v1/svd48/change-sets/validate
POST /api/v1/svd48/change-sets/apply
GET  /api/v1/svd48/change-sets/:id
```

Backend MVP:

- valida el JSON con el schema compartido;
- mantiene `SerialTransport` y `LanTransport` equivalentes;
- usa token LAN sin agregar cuentas/sesiones/HMAC;
- preserva request/job IDs y errores del firmware;
- trocea payloads grandes y verifica hash;
- tiene unit, API, transport y fault-injection tests.

Solo cuando esto esté cerrado comienza el frontend. En esa fase el navegador no
genera registros, codecs, prioridades ni decisiones de seguridad.

## Secuencia Priorizada

### P0 - Estado, árbitro y stop

- integrar `robot_state_service`;
- gatear todas las salidas;
- implementar mailboxes y `RC > LAN > BT` con TTL/epoch;
- implementar `control_lan` separado de `maintenance_lan`;
- stop-before-switch, no stale fallback;
- scheduler de bus con emergencia;
- host tests y luego latencia con ruedas elevadas.

### P1 - Perfil JSON y diferencial

- parser/validador/capabilities;
- storage A/B y factory/known-good;
- upload chunked USB/LAN;
- estrategia differential genérica;
- perfil actual: un SVD48, M1/M2, dos motrices y dos casters;
- medir y reemplazar todo placeholder antes de activar.

### P2 - Lectura y writes SVD48

- catálogo/codecs/inventory/read/compare;
- change-set validate/apply/status;
- primera allowlist;
- captura float y PID de velocidad;
- pruebas sobre controlador respaldado y robot elevado.

### P3 - Cinemáticas genéricas

- fixtures de uno/dos/cuatro controladores;
- Ackermann single-linkage/per-wheel;
- independent steer/crab;
- tests geométricos y HIL elevado.

### P4 - Backend

- contratos y transportes tipados;
- endpoints completos;
- tests USB/LAN y fallas;
- telemetría centralizada.

### P5 - Frontend

- se planifica e implementa desde las capabilities y schemas ya probados;
- no bloquea P0-P4 ni el inicio de parametrización por CLI/backend.

## Gates del Primer Write

- `GATE-SAFE`: runtime state, arbiter, TTL e inhibit probados.
- `GATE-STOP`: stop frame/zero RPM medidos con robot elevado.
- `GATE-CONTRACT`: ningún error/timeout/ambigüedad aparece como success.
- `GATE-SVD-READ`: old value tipado y fresh.
- `GATE-MVP-LAN`: token, bounded payload, request ID y dedupe.
- `GATE-BENCH`: controlador respaldado, ruedas elevadas y corte físico.
- Campo allowlisted con request/response/readback y rango confirmados.

No existe `GATE-AUTH` de usuario para el prototipo local. Se agrega antes de una
red hostil, exposición a Internet, múltiples operadores no confiables o entrega a
cliente.

## Criterio de Éxito

El MVP de backend termina cuando un desarrollador puede, por USB o LAN:

1. leer el JSON activo y capabilities;
2. validar/stagear un JSON autocontenido;
3. leer baseline SVD48 tipado;
4. validar/aplicar un parámetro allowlisted o PID verificado;
5. observar old/new/readback y resultado exacto;
6. perder conexión sin repetir el write ni permitir movimiento;
7. reiniciar y recuperar perfil known-good/factory si el staged falla.

El robot nunca se arma automáticamente y todas las pruebas físicas se ejecutan
elevadas. El frontend es la siguiente capa, no el criterio de cierre de este MVP.
