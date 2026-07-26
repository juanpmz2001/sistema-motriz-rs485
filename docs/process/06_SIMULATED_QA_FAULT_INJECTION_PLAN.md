# Plan de QA Simulado y Fault Injection

Fecha: 2026-07-19

Estado: `IN_PROGRESS`. Complementa `04_OFF_GROUND_TEST_MATRIX.md`: ya existe el arnes CMake/CTest, fake clock/event sink y suites puras de estado, autoridad simultanea `RC > LAN > Bluetooth` y cinemática diferencial. Faltan integración del coordinador/gate, fake SVD48/bus, change sets, perfil JSON canonico, fuzzing y las capas ESP/backend/HIL.

Actualización 2026-07-25: build 19 y la campaña física añadieron evidencia XML,
raw write/readback, Hall, gear y velocidad, pero no sustituyen las capas
simuladas pendientes. La política LAN ahora contiene una excepción temporal
`SET_SPEED`; los tests deben demostrar explícitamente que se elimina/gatea y que
una pérdida de cliente no puede dejar consigna activa (`SAFE-011`).

## Objetivo

Detectar durante desarrollo, sin motores reales, regresiones que puedan:

- permitir movimiento fuera de `ARMED`;
- dejar una consigna activa despues de perdida de RC, lease, bus o Wi-Fi;
- confundir stale/unknown con cero/healthy;
- aceptar una respuesta corrupta, tardia, duplicada o de otro dispositivo;
- repetir una escritura cuyo resultado es ambiguo;
- dejar actuadores parcialmente configurados sin fault/stop;
- activar un robot JSON corrupto o perder la ruta de recuperacion;
- permitir OTA, configuracion y movimiento al mismo tiempo.

Los tests simulados reducen riesgo, pero no prueban latencia electrica, inercia, cableado, servo binding, regeneracion, watchdog del controlador ni corte fisico. Esas propiedades siguen requiriendo `E3..E5`.

## Principio de Testabilidad

La logica de decision debe depender de interfaces pequenas, no llamar directamente FreeRTOS/UART/NVS dentro del nucleo:

```c
typedef struct {
    uint32_t (*now_ms)(void *ctx);
    result_t (*submit_bus)(void *ctx, const bus_request_t *request);
    result_t (*request_stop)(void *ctx, stop_reason_t reason);
    result_t (*persist)(void *ctx, const void *record);
    void (*emit_event)(void *ctx, const robot_event_t *event);
    void *ctx;
} robot_dependencies_t;
```

Produccion conecta esas interfaces a ESP-IDF. Host tests conectan reloj virtual, fake RS485, fake actuators, fake NVS y event recorder. El mismo modelo de estado/guards/codecs se compila en ambos casos.

## Implementacion Actual

```text
tests/
  CMakeLists.txt
  host/
    host_test.c/.h
    host_test_support_test.c
    robot_state_model_test.c
    command_authority_model_test.c
    robot_kinematics_test.c
  fakes/
    fake_clock.c/.h
    fake_event_sink.c/.h
cmake/host_tests/BotfarmsHostTest.cmake
tools/run_host_tests.sh
```

`./tools/run_host_tests.sh` configura un build temporal, compila con `-Wall -Wextra -Wpedantic -Werror`, ejecuta CTest y conserva el oráculo Python independiente de frames SVD48. Build 19 pasó 7/7 normal y 7/7 con ASan/UBSan el 2026-07-25 (`detect_leaks=0` por restricción `ptrace` del entorno).

Cobertura parcial actual:

- `QA-STATE-001`, `003`, `005`, `009`, `010`, `011`, `012` y `013` tienen casos puros del modelo.
- `command_authority_model_test` cubre prioridad triple, dead-man RC, preempción LAN/BT, TTL en movimiento, no stale fallback, streams retirados, secuencia, barrera temporal y stop explícito. Aún falta conectarlo al fake clock/coordinador runtime.
- `robot_kinematics_test` cubre dos/cuatro motores, giro puro, radio/ratio/signo por actuador, rechazo de `vy`/NaN/config inválida y saturación proporcional acotada. Ackermann/crab/perfiles aún no existen.
- `QA-SVD-011` cubre ACK `0x10` valido y rechazo por start/count/slave/function/CRC/longitud; aun falta fake UART para timeout post-TX y readback.
- Ninguna prueba actual demuestra que las APIs runtime de movimiento consulten el modelo. Se eliminó el API de permit check/use del servicio por su carrera inherente; `SAFE-002` debe resolverse dentro de un único coordinador de actuadores y mantiene `INV-001/002/003` abiertos.

## Capas de QA

| Nivel | Entorno | Que prueba | Evidencia |
| --- | --- | --- | --- |
| L0 static | compilador/scripts | warnings, formato, limites, secretos, docs/IDs | `E0/E1` |
| L1 pure host | ejecutable C en laptop | estado, guards, codecs, catalogo, perfiles, change sets | `E1` |
| L2 component host | fakes + reloj virtual | secuencias de bus, retries, deadlines, partial failure | `E1` |
| L3 backend | Node con fake Serial/UDP ESP | auth, contratos, correlation, UI policy, audit | `E2` |
| L4 firmware build | ESP-IDF | integracion/target/particiones/tamano | `E2` |
| L5 ESP sin bus | ESP real | tareas, NVS, USB/LAN, reboot, watchdog | `E3` |
| L6 controlador restringido | SVD48 respaldado, motores aislados/elevados | frames reales, exceptions, readback/persistencia | `E4` |
| L7 robot elevado | placa de Toño completa | mapping, servos, stop latency, RC, carga | `E5` |

Todo PR ejecuta L0-L4. Un cambio de safety, bus, robot JSON/I-O, write o OTA no se libera a hardware completo sin su campaña L5/L6 aplicable.

## Estructura Propuesta

```text
tests/
  host/
    test_support.h
    test_robot_state.c
    test_command_authority.c
    test_safety_invariants.c
    test_svd48_protocol.c
    test_svd48_catalog.c
    test_configuration_change_set.c
    test_robot_profile.c
    test_rs485_scheduler.c
  fakes/
    fake_clock.c
    fake_svd48_bus.c
    fake_actuator.c
    fake_profile_store.c
    fake_event_sink.c
  fixtures/
    controllers/
    profiles/
    event_sequences/
    transport_contract/
  fuzz/
    fuzz_svd48_response.c
    fuzz_management_envelope.c

tools/run_qa.sh
```

Usar CTest con ejecutables C y un soporte de assertions pequeno evita agregar una dependencia externa al inicio. Mantener ESP-IDF Unity para tests que necesiten target real. Los fakes son explicitos y revisables; no deben esconder orden de llamadas importante para seguridad.

Comandos objetivo, a implementar en `TEST-001/003`:

```bash
./tools/run_qa.sh host
./tools/run_qa.sh firmware-build
./tools/run_qa.sh web
./tools/run_qa.sh all
```

Estos comandos siguen siendo el objetivo del runner integral. Actualmente `./tools/run_host_tests.sh` ya ejecuta el proyecto CTest para contratos, estados, autoridad, differential y fakes; `tools/run_qa.sh` y los modos firmware/web todavia no existen.

## Modelo del Fake SVD48

El fake debe permitir un script por request:

```text
EXPECT drive=1 func=0x03 reg=0x5410 -> RESPONSE valid after 4ms
EXPECT drive=1 func=0x06 reg=0x5018 -> DROP_AFTER_TX
EXPECT drive=1 func=0x03 reg=0x5018 -> RESPONSE value=24
EXPECT any STOP -> RESPONSE valid after 2ms
```

Resultados configurables:

- success con delay;
- timeout antes de transmitir;
- request transmitido y response perdida (`DROP_AFTER_TX`);
- CRC incorrecto;
- slave/function/register/count incorrectos;
- exception `0x83/0x86/0x90` y code;
- frame truncado/oversized;
- bus ocupado/deadline vencido;
- respuesta duplicada/tardia/reordenada;
- desconexion de un drive y recuperacion;
- valor de readback distinto;
- write aplicado aunque se perdio response;
- write no aplicado aunque se perdio response.

El fake registra orden, timestamp virtual, request ID y bytes. Esto permite comprobar que `STOP` adelanta telemetry/config y que un write ambiguo no se repite.

## Reloj Virtual

Ningun test debe dormir tiempo real para simular stale o leases:

```text
clock = 1000ms
inject RC_FRAME_VALID
advance 149ms -> sigue valido
advance 2ms -> RC_STALE event
expect state=FAULTED
expect one emergency stop request
```

El reloj virtual cubre wraparound de `uint32_t`, command TTL, authority epochs,
job deadlines, retry backoff y freshness sin tests lentos/flaky.

## Invariantes Automatizadas

Cada secuencia de eventos debe comprobar al menos:

```text
INV-001 hardware_motion_output => state == ARMED
INV-002 hardware_motion_output => authority.epoch_valid && command.not_expired
INV-003 config_write_output => state == MAINTENANCE && exclusive_job.valid
INV-004 state == OTA => no config_write_output && no motion_output
INV-005 fault_latched => no transition direct to ARMED
INV-006 stale_or_unknown critical value => never interpreted as healthy zero
INV-007 write_response_missing_after_tx => result != SUCCESS && no blind retry
INV-008 partial actuator/config apply => fault/inhibit + emergency stop requested
INV-009 reboot/reconnect => state != ARMED
INV-010 STOP request always accepted by policy; delivery result remains truthful
INV-011 invalid robot JSON/I-O profile => active/known_good unchanged
INV-012 duplicate request ID/body => at most one mutation
INV-013 same request ID/different body => explicit ID_REUSE error
INV-014 every state/result transition has monotonic sequence and audit event
INV-015 RC fresh => selected_source == RC, even neutral/deadman released
INV-016 source_switch => STOP precedes output && command.received_in_new_epoch
INV-017 source_loss_while_moving => FAULTED && no stale lower-source fallback
INV-018 active runtime snapshot => hash/revision of canonical active JSON
```

La propiedad critica no es "el software siempre logra detener el motor"; con bus/cable/controlador averiado eso es imposible. La propiedad comprobable es: inhibe nuevos comandos, prioriza stop, reporta si no fue confirmado y exige corte fisico en HIL.

## Suite de Estado y Seguridad

| ID | Secuencia simulada | Resultado obligatorio |
| --- | --- | --- |
| `QA-STATE-001` | boot nominal | `BOOTING -> DISARMED`, nunca `ARMED` |
| `QA-STATE-002` | RC ausente al boot | optional permite LAN/BT; required produce inhibit exacto segun JSON |
| `QA-STATE-003` | arm sin dead-man/controller fresh | rechazo exacto; cero I/O de movimiento |
| `QA-STATE-004` | arm valido | authority epoch creado antes de aceptar movimiento |
| `QA-STATE-005` | RC stale mientras ARMED | fault latch + emergency stop dentro del deadline logico |
| `QA-STATE-006` | TTL de fuente expira | fault/stop, nunca conserva movimiento ni hace stale fallback |
| `QA-STATE-007` | motor/controller critical stale | `FAULTED`, stop, stale no se transforma en zero |
| `QA-STATE-008` | error_code aparece y luego muestra cero | fault permanece hasta ACK y causa clear |
| `QA-STATE-009` | ACK mientras causa sigue activa | rechazo, continua `FAULTED` |
| `QA-STATE-010` | causa clear + ACK | solo `FAULTED -> DISARMED` |
| `QA-STATE-011` | open maintenance desde ARMED | rechazo y ningun write |
| `QA-STATE-012` | maintenance valida | movimiento rechazado en cada entry point |
| `QA-STATE-013` | OTA durante maintenance | conflicto; ninguna operacion inicia |
| `QA-STATE-014` | reboot/reconnect Wi-Fi/backend | permanece/retorna `DISARMED` |
| `QA-STATE-015` | STOP en cada estado | policy acepta, action/audit se emite una vez/idempotente |

## Suite de Movimiento y Stop

| ID | Falla | Resultado obligatorio |
| --- | --- | --- |
| `QA-MOTION-001` | prior target no cero + enable | target seguro/nuevo precede `START` |
| `QA-MOTION-002` | falla target motor N | no START posterior; stop all; `PARTIAL_APPLY` latched |
| `QA-MOTION-003` | falla START motor N | stop de todos los ya tocados; resultado parcial exacto |
| `QA-MOTION-004` | telemetry ocupa bus | emergency stop se agenda antes de siguiente telemetry frame |
| `QA-MOTION-005` | drive 1 ausente, drive 2 disponible | stop drive 2 no queda detras de retries ilimitados |
| `QA-MOTION-006` | bus totalmente caido | no nuevas consignas; `STOP_UNCONFIRMED`; permanece faulted |
| `QA-MOTION-007` | NaN/Inf/extremo | rechazo antes de kinematics/I/O |
| `QA-MOTION-008` | RC/LAN/BT concurrentes | RC seleccionado; lower sources siguen visibles pero sin I/O |
| `QA-MOTION-009` | queue saturation | emergency reservado/admitido; low priority rechazado/coalesced |
| `QA-MOTION-010` | LAN aparece durante BT | STOP, epoch nuevo, solo comando LAN posterior al switch |
| `QA-MOTION-011` | RC aparece durante LAN/BT | RC preempta dentro del bound mediante STOP/epoch nuevo |
| `QA-MOTION-012` | RC fresh neutral o dead-man off | cero; bloquea LAN/BT no cero |
| `QA-MOTION-013` | RC se pierde con LAN/BT viejo | STOP + FAULTED; no fallback automatico |
| `QA-MOTION-014` | LAN TTL expira con BT viejo | STOP + FAULTED; no fallback automatico |
| `QA-MOTION-015` | comando USB legado | entra al arbitro/gate o se rechaza; nunca I/O directo |

## Suite de Comunicacion SVD48

| ID | Falla | Resultado obligatorio |
| --- | --- | --- |
| `QA-SVD-001` | CRC incorrecto | dato descartado; error/metric; no cache fresh |
| `QA-SVD-002` | wrong slave/function/count | BAD_RESPONSE; no valor publicado |
| `QA-SVD-003` | frame truncado/oversized | rechazo bounded; parser recupera siguiente frame |
| `QA-SVD-004` | exception `0x83/86/90` | function/code preservados y result tipado |
| `QA-SVD-005` | timeout antes de TX | write no enviado; retry solo segun politica/idempotencia |
| `QA-SVD-006` | response perdida despues de write TX | `OUTCOME_UNKNOWN`; readback; no blind retry |
| `QA-SVD-007` | readback coincide tras timeout | resultado `APPLIED_AFTER_AMBIGUOUS_RESPONSE`, audit completo |
| `QA-SVD-008` | readback difiere tras timeout | fault/recovery required, no falso rollback |
| `QA-SVD-009` | un drive stale y otro fresh | valores separados; critical policy inhibe movimiento |
| `QA-SVD-010` | response tardia de request anterior | ignorada por correlation/deadline |
| `QA-SVD-011` | `0x10` echo start/count incorrecto | BAD_RESPONSE; change set falla |
| `QA-SVD-012` | poll retorna fallos de ambos drives | health no puede reportar success global |

## Suite de Maintenance y Writes

| ID | Secuencia | Resultado obligatorio |
| --- | --- | --- |
| `QA-WRITE-001` | write fuera de maintenance job | rechazado antes de bus |
| `QA-WRITE-002` | segundo job concurrente | busy/conflict; no segundo flujo de bus |
| `QA-WRITE-003` | descriptor RO/unknown/suspect | field error; no bus |
| `QA-WRITE-004` | fuera de rango/hard ceiling | field error exacto |
| `QA-WRITE-005` | baseline stale/cambio concurrente | revision conflict; releer |
| `QA-WRITE-006` | dry run | plan completo; cero writes |
| `QA-WRITE-007` | apply nominal | old/request/response/readback iguales; success |
| `QA-WRITE-008` | field N falla | per-field partial, inhibit/fault, stop result |
| `QA-WRITE-009` | job deadline expira antes de TX | cancelado; cero writes |
| `QA-WRITE-010` | job deadline expira despues de TX | completar clasificacion/readback seguro; no segunda mutacion |
| `QA-WRITE-011` | cliente/backend desconecta | job sigue authoritative en ESP; reconecta por job ID |
| `QA-WRITE-012` | request duplicado | mismo resultado; una mutacion |
| `QA-WRITE-013` | rollback falla | estado explicitamente partial/recovery required |
| `QA-WRITE-014` | save/power loss fixture | active/runtime/persisted nunca se confunden |
| `QA-WRITE-015` | raw mode en release/LAN | capability ausente y rechazo antes de bus |

`QA-WRITE-015` remains the production target. Build 19 carries a temporary
authenticated-LAN raw editor plus the ADR-0004 direct `SET_SPEED` exception. Its
automated subset must additionally
cover confirmation, actuation denylist, known one-word ranges, exact target
correlation, pre-read/readback parsing and `unknown_do_not_retry`. It must be
removed or placed behind an explicit engineering capability; direct speed must
be rejected or routed through TTL/state/authority before
`QA-WRITE-015` can pass.

## Suite PPM GPIO14

| ID | Secuencia | Resultado obligatorio |
| --- | --- | --- |
| `QA-PPM-001` | config invalida/canales fuera de capacidad | init rechazado; sin ISR activa |
| `QA-PPM-002` | sync + 10 pulsos validos + sync | snapshot atomico con 10 canales y frame valido |
| `QA-PPM-003` | pulso fuera de `750..2250 us` | contador invalid incrementa; no valor corrupto publicado |
| `QA-PPM-004` | menos de 4 canales antes de sync | frame incompleto, señal no renovada |
| `QA-PPM-005` | mas de 10 canales | overflow contado, escritura siempre bounded |
| `QA-PPM-006` | wrap de timestamp de 32 bits | edad y sync siguen correctos |
| `QA-PPM-007` | cesan frames por mas de `300 ms` | `STATUS:NO_SIGNAL`; safety solicita stop si antes vio RC valido |
| `QA-PPM-008` | OTA/escritura flash mientras PPM activo y robot detenido | sin crash; ausencia temporal de frames no arma ni mueve |

## Suite de Perfil y Pines

| ID | Fixture/falla | Resultado obligatorio |
| --- | --- | --- |
| `QA-PIN-001` | GPIO reservado/strapping/output invalido | field error; staged no cambia |
| `QA-PIN-002` | pin duplicado UART/servo/RC | conflicto exacto |
| `QA-PIN-003` | LEDC timer/channel duplicado | conflicto exacto |
| `QA-PIN-004` | JSON/schema/hash/marker corrupto | no activar; known_good/factory |
| `QA-PIN-005` | power loss durante stage | active y known_good intactos |
| `QA-PIN-006` | staged inicia recursos correctamente | `BOOTING -> DISARMED`, aun no auto-armed |
| `QA-PIN-007` | staged no inicia UART/LEDC | rollback siguiente boot; diagnostico persistido |
| `QA-PIN-008` | perfil elimina recovery | validacion rechaza |
| `QA-PIN-009` | revision stale desde dos clientes | conflict, no lost update |
| `QA-PIN-010` | activacion mientras ARMED/OTA | rechazo, no reboot/profile swap |
| `QA-PIN-011` | draft `activation_allowed:false` | schema puede pasar; activacion rechazada |
| `QA-PIN-012` | IDs/controller channels duplicados | field error; no snapshot parcial |
| `QA-PIN-013` | capacidad variable excedida | error de capability; ningun array truncado |
| `QA-PIN-014` | chunks drop/reorder/duplicate/hash mismatch | no commit staged; retry idempotente |
| `QA-PIN-015` | fixtures 1/2/4 SVD48 | mismo schema/validador; bindings exactos |

## Suite de Transporte LAN/Backend

El web repo conserva sus IDs `WEB-TEST-*`; como minimo deben cubrir:

- timeout, drop, duplicate, reorder, spoofed source y boot-ID change;
- token local ausente/incorrecto y redaccion;
- duplicate request, mismo ID/body, mismo ID/body distinto y boot-ID change;
- disconnect con request pendiente y backend restart;
- dos clientes y un solo firmware maintenance job;
- `ERR`/exception/readback mismatch nunca traducidos a HTTP `ok:true`;
- body/queue limits, command injection y redaction;
- UI stale/unknown/null nunca renderizado como cero saludable;
- job reconnect por ID y audit aun cuando desaparece el browser.

La suite de produccion diferida agrega HMAC, nonce replay, timestamp, key
rotation, cuentas/roles y CSRF/origin antes de exponer el sistema a una red hostil.

## Event-Sequence Tests y Model Checking Ligero

Guardar fixtures declarativos:

```json
{
  "initial": "DISARMED",
  "events": [
    {"at_ms": 0, "type": "ARM_REQUEST"},
    {"at_ms": 1, "type": "RC_VALID"},
    {"at_ms": 2, "type": "CONTROLLERS_FRESH"},
    {"at_ms": 3, "type": "ARM_REQUEST"},
    {"at_ms": 200, "type": "RC_STALE"}
  ],
  "expect": {
    "final_state": "FAULTED",
    "motion_before_arm": 0,
    "emergency_stops": 1
  }
}
```

Ademas de casos manuales, generar secuencias cortas de eventos validos/invalidos y comprobar invariantes. Un seed fallido se guarda como fixture permanente. Fuzzing de bytes se limita a parsers/envelopes; nunca ejecuta hardware real.

## Gates de CI

### Cada Commit/PR

- compilacion host con `-Wall -Wextra -Werror`;
- pure state/authority/protocol/catalog/profile/change-set tests;
- fixtures de fallos criticos;
- Python tools tests;
- ESP-IDF build/tamano/partitions;
- backend unit/API/fake transport tests;
- secret/log redaction y diff whitespace.

### Antes de Flashear ESP de Desarrollo

- todo lo anterior;
- compatibility matrix actualizada;
- build number/manifest coherentes;
- rollback path identificado;
- ningun test safety critico skipped sin motivo.

### Antes de Placa de Toño

- L5 USB/LAN sin SVD48 aprobado;
- fault scenarios aplicables pasan en fake;
- lista exacta de HIL tests, limites, cutoff y operador;
- profile/controller backup;
- primer write limitado a un parametro aprobado y reversible.

## Evidencia y Reproducibilidad

Cada suite produce:

- commit/build/toolchain;
- seed/fixture y virtual timeline;
- eventos/transiciones/actions;
- bus requests en orden con IDs, sin secretos;
- assertions e invariantes evaluadas;
- JUnit/JSON summary para CI;
- failures conservados como fixtures.

No guardar passwords, tokens, cookies, HMAC keys ni `.env`. Tests generan secretos efimeros.

## Secuencia de Implementacion QA

1. `QA0`: CMake/CTest, fake clock/event sink y migrar contratos actuales.
2. `QA1`: pure `robot_state` + invariantes `INV-001..010` antes de integrar la maquina real.
3. `QA2`: fake SVD48/bus scheduler y suites communication/motion.
4. `QA3`: fake store, perfiles/pines/change sets y power-loss state fixtures.
5. `QA4`: app factory/test runner/fake transports en web repo.
6. `QA5`: un comando `run_qa.sh all`, reportes y CI.
7. `QA6`: ESP component tests y campañas `E3/E4/E5` vinculadas a la matriz off-ground.

## Criterio de Salida

El QA MVP esta listo cuando cada cambio puede ejecutar una sola orden reproducible que demuestre los invariantes de estado, simule perdidas/errores/ambiguedad de bus y valide firmware/backend; ningun escenario simulado deja movimiento o write autorizado fuera de su estado, y la matriz separa claramente lo probado en host de lo pendiente en hardware.
