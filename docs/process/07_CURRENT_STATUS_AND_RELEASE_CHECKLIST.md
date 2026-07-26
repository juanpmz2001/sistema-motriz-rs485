# Estado Actual y Checklist de Cierre

Fecha de revisión: 2026-07-25
Rama revisada: `feature/mvp-svd48-register-editor`
Firmware fuente: build `19`, target `esp32s3`
Estado de release: **solo ingeniería con robot elevado; no apto para piso ni producto**

Este documento resume qué incluye el cambio actual, qué se verificó realmente y
qué sigue abierto. No reemplaza la API, las skills ni la evidencia fechada. La
vigencia y precedencia de toda la documentación están en
`../DOCUMENTATION_INDEX.md`.

## Alcance del cambio

### Firmware

- `FW_BUILD_NUMBER` sube de 18 a 19.
- El techo compilado de rueda baja de 300 a `15 RPM` en `main/main.c`.
- `robot_control_set_motor_speed()` aplica el techo antes de escribir al SVD48 y
  registra la consigna efectivamente aplicada.
- El gateway rechaza `SET_SPEED` fuera del techo con
  `ERR SET_SPEED_OUT_OF_RANGE`.
- La política `LAN_SAFE` permite temporalmente `SET_SPEED n rpm` y
  `STOP n|ALL`, además del acceso de diagnóstico/configuración ya existente.
- Los tests host de política cubren esa excepción temporal.

### Herramientas

- `tools/ota_announce.py` busca `BOTFARMS_OTA_TOKEN` en `--token`, entorno y
  finalmente `.env` ignorado por Git, sin imprimir el secreto.
- `tools/svd48_read_inventory.py` incorpora gear real, campos de placa, PID
  extendido/filtros, M2 Hall current y corrige MOS/bus/escala de velocidad.
- `tools/svd48_xml_inventory.py` carga el catálogo XML oficial, lee y decodifica
  todos sus parámetros, y exporta CSV/JSON.
- `tools/svd48_speed_sweep.py` ejecuta pasos acotados y registra palabra cruda
  `0.1 RPM` y RPM física por separado; exige `--confirm-elevated`.
- `tools/svd48_hall_capture.py` registra calibración Hall y ahora separa
  `actual_speed_raw_d10` de `actual_rpm`; exige `--confirm-elevated`.
- `tools/svd48_lan.py` centraliza lecturas directas autenticadas para las
  herramientas SVD48.

### Evidencia incluida

- Inventarios oficiales XML antes/después/restaurados: 339/339 parámetros.
- Capturas Hall M1/M2 con estado, corriente, ángulo, error y velocidad escalada.
- Barridos de velocidad/PID/gear con timestamps y stop de mejor esfuerzo.
- Diagnóstico completo y reporte de restauración al baseline original.
- `SESSION_LOG.md` conserva cronológicamente intentos, resultados y correcciones.

### Documentación

- Se añade un índice de vigencia para todos los Markdown del repositorio.
- Se corrigen techo de RPM, allowlist LAN, gear, Hall current, MOS/bus y escala
  de velocidad en README, API, skills, contratos y planes.
- Los informes históricos se mantienen como evidencia fechada y reciben notas de
  corrección/supersesión cuando una conclusión posterior cambió su interpretación.
- ADR-0004 registra la excepción temporal de actuación por maintenance LAN.
- `.obsidian/` queda ignorado como configuración local del editor.

## Evidencia ya obtenida

| Hecho | Evidencia | Nivel/limitación |
| --- | --- | --- |
| Límite de 15 RPM activo; 16 RPM rechazado | sesión y diagnóstico 2026-07-21 | Robot elevado; no prueba control productivo |
| ID 2/software `0x0131` leyó 339/339 parámetros XML | inventarios XML CSV/JSON | `E4`; el catálogo XML aún no está versionado en repo |
| Float observado es high-word-first | XML + read/write/readback/restauración | Solo familias observadas; runtime tipado pendiente |
| Gear real M1/M2 es `0x5030/31/34/35`; KK16 usa `1/5` | XML y pruebas live | `5/1` produjo overload y no debe repetirse |
| Hall M1/M2 recorrió sectores pero terminó en estado `2` | CSV Hall | Calibración fallida, no válida |
| Writes FC06/FC16 tuvieron old value y readback | CSV/JSON/reportes | Acceso raw provisional, sin job/dedupe/rollback |
| Baseline original RW fue restaurado donde era escribible | reporte de restauración | M2 Hall parcial y campos originalmente no leídos quedan desconocidos |
| `0x3100=1` recibió ACK | reporte de restauración | Registro WO; falta power-cycle final |
| Estado directo final observado fue stop/speed/error cero | inventario final | Estado fechado, no monitorización permanente |

## Bloqueos críticos antes de piso o producto

- [ ] `SAFE-001/002`: instanciar estado operacional y hacer cumplir el inhibit en
  un único coordinador inmediatamente antes de I/O.
- [ ] `SAFE-003/004`: conectar RC/LAN/Bluetooth al árbitro con TTL, dead-man,
  epochs, `RC > LAN > Bluetooth` y no stale fallback.
- [ ] `SAFE-011`: eliminar `maintenance_lan SET_SPEED` o pasarlo por ese mismo
  coordinador. Hoy una pérdida de Wi-Fi/cliente puede dejar la consigna activa.
- [ ] `SAFE-006`: dar clase de emergencia al stop y medir latencia con timeout,
  polling y un controlador ausente.
- [ ] `SAFE-008`: definir/probar E-stop o corte físico y reinicio desarmado.
- [ ] `SVD-009`: convertir velocidad cruda `0.1 RPM` a un contrato explícito y
  coherente en firmware, safety, API, backend/UI y herramientas.
- [ ] Sustituir `SAFE_FOR_OTA` como guard de writes por estado `MAINTENANCE`, job
  exclusivo, expiración, dedupe y resultados tipados.
- [ ] Confirmar persistencia del baseline restaurado mediante power-cycle del
  SVD48 y nuevo inventario, sin movimiento ni calibración.
- [ ] Resolver/aceptar formalmente la tabla Hall M2 no restaurable y validar el
  modelo eléctrico, UVW/Hall, dirección y gear antes de nuevo tuning.
- [ ] Medir diámetro/radio, RPM física, counts/rev, signos y mapeo de cada rueda.
- [ ] Completar pruebas de pérdida RC/dead-man/Wi-Fi, fault/stale/offline, partial
  apply, soak y coexistencia OTA del matrix 04.
- [ ] Definir firma de firmware, Secure Boot/flash/NVS encryption y
  aprovisionamiento de llaves antes de una entrega productiva (`AUTH-005`).
- [ ] Auditar/reducir la presión de IRAM: build 19 todavía reporta
  `16383/16384` bytes (1 byte libre), aunque DIRAM conserva margen. Bloquear
  nuevas opciones/atributos IRAM sin medición de map y prueba de build limpio.

## Validación de este commit

Los resultados deben actualizarse aquí antes de cerrar el commit:

| Verificación | Comando | Estado |
| --- | --- | --- |
| Contratos host | `./tools/run_host_tests.sh` | `PASS`, 7/7 |
| ASan/UBSan host | `BOTFARMS_HOST_TEST_SANITIZERS=ON ./tools/run_host_tests.sh` | `PASS`, 7/7 |
| Oráculo SVD48 | `python3 tools/test_svd48_protocol.py` | `PASS`, 3/3 |
| Sintaxis Python | `python3 -m py_compile tools/*.py` | `PASS` |
| Guarda de movimiento | herramientas sin `--confirm-elevated` | `PASS`; rechazo previo a red o archivo |
| Firmware ESP-IDF | fullclean + `idf.py build` | `PASS`, `0x101700`, slot `0x600000`, 83% libre |
| Memoria ESP-IDF | `idf.py size` | `PASS`; IRAM `16383/16384`, DIRAM `104991/341760` |
| JSON/CSV/documentación | parseo 16 JSON, shape 19 CSV, links e índice | `PASS`; `git diff --check` se repite al final |

No se requiere repetir movimiento ni writes físicos para documentar/compilar
este commit. La próxima sesión de hardware debe empezar por power-cycle/read-only
y no por una consigna.

## Checklist de próxima sesión de hardware

- [ ] Confirmar robot elevado, soportes, perímetro y persona dedicada al corte.
- [ ] Verificar build/partición con `VERSION`; no asumir que el binario fuente es
  el instalado.
- [ ] Ejecutar `PLATFORM_STATUS`, `SAFETY_STATUS`, `WIFI_STATUS` y
  `MAINT_LAN_STATUS` sin secretos.
- [ ] Leer ID/versiones y `0x5030/31/34/35`, comandos, given speed, actual speed
  raw y errores antes de cualquier write.
- [ ] Confirmar por power-cycle que el baseline restaurado persiste; guardar un
  inventario nuevo y compararlo palabra por palabra.
- [ ] No repetir gear `5/1`, Hall calibration ni elevar corriente sin un plan
  nuevo aprobado y datos eléctricos suficientes.
- [ ] No probar pérdida de Wi-Fi con una consigna activa mientras exista el
  bypass sin TTL. Primero implementar `SAFE-011`.

## Criterio de cierre de documentación

La documentación se considera coherente para este commit cuando:

- ningún documento vigente afirma 300 RPM, gear `0x2202/03`, bus en `0x5408`,
  MOS en `0x540C`, velocidad entera o movimiento LAN totalmente bloqueado;
- cada afirmación histórica contradictoria tiene fecha y nota de corrección;
- la API coincide con la allowlist y las unidades del código activo;
- las tareas abiertas tienen ID, evidencia requerida y riesgo explícito;
- los tests/build anteriores pasan y el commit no incluye secretos ni estado
  local de Obsidian.
