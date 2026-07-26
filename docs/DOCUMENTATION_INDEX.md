# Índice de Vigencia de la Documentación

Última revisión integral: 2026-07-25
Código revisado: firmware fuente build `19`, rama
`feature/mvp-svd48-register-editor`

Este índice clasifica todos los archivos Markdown del repositorio. “Vigente” no
significa “feature terminada”: un plan objetivo puede ser vigente y describir
trabajo todavía no integrado. Los reportes fechados conservan lo observado en su
momento y no deben leerse como estado live del hardware.

## Precedencia

Ante una contradicción, usar este orden y corregir el documento en el mismo
cambio:

1. Código/configuración activa y tests que realmente se ejecutan.
2. Contratos vigentes: `API.md`, safety contract, README y skills.
3. Checklist actual `process/07_CURRENT_STATUS_AND_RELEASE_CHECKLIST.md`.
4. Planes objetivo y ADRs; describen la arquitectura aprobada, no necesariamente
   el runtime actual.
5. Evidencia fechada y `SESSION_LOG`; preservan historia y pueden contener una
   interpretación luego corregida mediante una nota posterior.
6. Auditorías/planes históricos; sirven como contexto, no como instrucciones
   actuales.

No inferir el firmware instalado desde `main/app_version.h`: comprobarlo con
`VERSION`. No inferir el estado actual del SVD48 desde una captura fechada:
leerlo de nuevo.

## Cambios de Interpretación Que Aplican Globalmente

- Build fuente actual: `19`; techo temporal de comandos: `15 RPM`, no 300.
- `maintenance_lan` build 19 permite temporalmente `SET_SPEED` y `STOP n|ALL`.
  No tiene TTL/dead-man y no es control LAN productivo (ADR-0004).
- Gear real: M1 `0x5030/0x5034`, M2 `0x5031/0x5035`; KK16 observado `1/5`.
  `0x2202/0x2203` son candidatos legacy inválidos en software `0x0131`.
- MOS: `0x5408/09`; bus voltage: `0x540C/0D`.
- Actual speed `0x5410/11` es signed `0.1 RPM`. El firmware build 19 todavía
  imprime el raw como `RPM`; defecto abierto `SVD-009`.
- M2 Hall calibration current es `0x5625`; `0x5605/0x5609` son inválidos en la
  revisión observada.
- Ambas calibraciones Hall del 2026-07-21 terminaron en estado `2` (failed).
- La restauración del baseline original tuvo readback, pero su persistencia final
  tras power-cycle sigue pendiente.

## Documentos Raíz y Contratos

| Documento | Clasificación | Vigencia/uso |
| --- | --- | --- |
| [`README.md`](../README.md) | Vigente, entrada operativa | Arquitectura activa, límites, LAN, build/flash y tutorial OTA resumido |
| [`README_COMPONENTS.md`](../README_COMPONENTS.md) | Vigente, mapa de componentes | Diferencia componentes activos, compilados-inactivos y legacy |
| [`ota_documentation_for_dummies.md`](../ota_documentation_for_dummies.md) | Vigente, tutorial OTA | Procedimiento humano; usar números de build como placeholders, no copiarlos literalmente |
| [`API.md`](API.md) | Normativo vigente | Contrato ASCII/UDP real, allowlist y defectos conocidos de unidades |
| [`FIRMWARE_LOGIC_HUMAN_FRIENDLY.md`](FIRMWARE_LOGIC_HUMAN_FRIENDLY.md) | Vigente, explicación | Separa runtime build 19 de arquitectura objetivo |
| [`TONO_PLATFORM_SAFETY_CONTRACT.md`](TONO_PLATFORM_SAFETY_CONTRACT.md) | Normativo de prototipo | Declara explícitamente limitaciones que bloquean piso/producto |
| [`ROBOT_PROFILES_AND_SVD48_CONFIGURATION_PLAN.md`](ROBOT_PROFILES_AND_SVD48_CONFIGURATION_PLAN.md) | Plan objetivo vigente | JSON/autoridad/kinemática objetivo; perfil runtime aún no implementado |
| [`FUTURE_CONTROL_TUNING_UI_NOTES.md`](FUTURE_CONTROL_TUNING_UI_NOTES.md) | Plan futuro vigente | Requisitos de UI tipada; editor raw actual no satisface el objetivo |
| [`TONO_COMM_TELEMETRY_ROADMAP.md`](TONO_COMM_TELEMETRY_ROADMAP.md) | Histórico/supersedido parcialmente | Roadmap de mayo; usar correcciones superiores y planes `process/` actuales |
| [`RC_IBUS_EXPERIMENT_NOTES.md`](RC_IBUS_EXPERIMENT_NOTES.md) | Evidencia histórica con follow-up vigente | i-BUS GPIO18 falló; PPM GPIO14 es el default actual de diagnóstico/safety |
| [`OTA_IMPLEMENTATION_PLAN.md`](OTA_IMPLEMENTATION_PLAN.md) | Histórico | Registro de iteraciones OTA; no usar sus secciones “current” antiguas como estado actual |
| [`OTA_MEMORY_AUDIT.md`](OTA_MEMORY_AUDIT.md) | Histórico | Auditoría de memoria de Iteración 9.5; repetir mediciones para build actual |

## Skills y Conocimiento del Controlador

| Documento | Clasificación | Vigencia/uso |
| --- | --- | --- |
| [`skills/SVD48B50A_SKILL.md`](skills/SVD48B50A_SKILL.md) | Skill operativa vigente | Leer antes de tocar protocolo, registros, tuning o calibración |
| [`skills/OTA_UPDATE_SKILL.md`](skills/OTA_UPDATE_SKILL.md) | Skill operativa vigente | Leer antes de preparar, anunciar, instalar o recuperar OTA |
| [`controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md`](controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md) | Nota técnica vigente con historia | Integra XML/live findings y marca hipótesis anteriores |
| [`controllers/SVD48B50A/OBSERVED_TOÑO_CONFIG.md`](controllers/SVD48B50A/OBSERVED_TOÑO_CONFIG.md) | Evidencia resumida | Valores observados y correcciones posteriores; verificar live antes de escribir |
| [`controllers/SVD48B50A/FULLING_MANUAL_AUDIT.md`](controllers/SVD48B50A/FULLING_MANUAL_AUDIT.md) | Auditoría vigente con follow-up | Delimita la familia Fulling incompatible; gear resuelto luego por XML dual |
| [`controllers/SVD48B50A/sources.md`](controllers/SVD48B50A/sources.md) | Bibliografía/snapshot | Origen y trazabilidad de fuentes descargadas |
| [`controllers/SVD48B50A/product-page.md`](controllers/SVD48B50A/product-page.md) | Snapshot de fuente | Contenido histórico del producto, no contrato firmware |

## Proceso Activo

| Documento | Clasificación | Vigencia/uso |
| --- | --- | --- |
| [`process/README.md`](process/README.md) | Normativo de proceso | Estados, IDs, niveles de evidencia y workflow |
| [`process/00_MASTER_PLAN.md`](process/00_MASTER_PLAN.md) | Plan maestro vigente | Workstreams, gates y estado build 19 |
| [`process/01_SVD48_REGISTER_COVERAGE.md`](process/01_SVD48_REGISTER_COVERAGE.md) | Inventario/plan vigente | Cobertura, confianza, XML y experimentos pendientes |
| [`process/02_ROBOT_PROFILES_KINEMATICS_SAFETY.md`](process/02_ROBOT_PROFILES_KINEMATICS_SAFETY.md) | Diseño objetivo vigente | Estado/autoridad/perfiles/kinemática; integración runtime pendiente |
| [`process/03_TRANSPORT_AND_API_CONTRACT.md`](process/03_TRANSPORT_AND_API_CONTRACT.md) | Contrato objetivo + gap audit | V1 real versus gestión v2; incluye desviación LAN build 19 |
| [`process/04_OFF_GROUND_TEST_MATRIX.md`](process/04_OFF_GROUND_TEST_MATRIX.md) | Matriz normativa vigente | Requisitos antes de crear un plan de piso |
| [`process/05_SAFE_CONFIGURATION_WRITE_PLAN.md`](process/05_SAFE_CONFIGURATION_WRITE_PLAN.md) | Plan vigente | Migra acceso raw a jobs tipados/guardados |
| [`process/06_SIMULATED_QA_FAULT_INJECTION_PLAN.md`](process/06_SIMULATED_QA_FAULT_INJECTION_PLAN.md) | Plan vigente | QA host/fakes/fault injection aún incompleto |
| [`process/07_CURRENT_STATUS_AND_RELEASE_CHECKLIST.md`](process/07_CURRENT_STATUS_AND_RELEASE_CHECKLIST.md) | Estado vigente | Alcance de este commit, validación y bloqueos críticos |
| [`process/COMPATIBILITY_MATRIX.md`](process/COMPATIBILITY_MATRIX.md) | Registro vigente | Solo combinaciones con evidencia; no borrar filas históricas |
| [`process/SESSION_LOG.md`](process/SESSION_LOG.md) | Log append-only | Handoffs cronológicos; correcciones se agregan, no se reescriben |

## ADRs, Plantillas y Esquema

| Documento | Clasificación | Vigencia/uso |
| --- | --- | --- |
| [`process/adr/README.md`](process/adr/README.md) | Normativo de ADR | Formato y listado actual |
| [`process/adr/0001-management-and-motion-boundaries.md`](process/adr/0001-management-and-motion-boundaries.md) | ADR aceptado, objetivo | Gestión y movimiento separados |
| [`process/adr/0002-canonical-json-robot-profile.md`](process/adr/0002-canonical-json-robot-profile.md) | ADR aceptado, objetivo | Perfil JSON canónico |
| [`process/adr/0003-simultaneous-command-source-arbitration.md`](process/adr/0003-simultaneous-command-source-arbitration.md) | ADR aceptado, objetivo | `RC > LAN > Bluetooth`, TTL y epochs |
| [`process/adr/0004-temporary-maintenance-lan-bench-actuation.md`](process/adr/0004-temporary-maintenance-lan-bench-actuation.md) | Excepción temporal aceptada | Registra el bypass build 19 y sus criterios de eliminación |
| [`process/templates/WORK_ITEM.md`](process/templates/WORK_ITEM.md) | Plantilla vigente | Crear/actualizar work items |
| [`process/templates/SESSION_ENTRY.md`](process/templates/SESSION_ENTRY.md) | Plantilla vigente | Agregar handoffs al log |
| [`schemas/README.md`](schemas/README.md) | Diseño vigente | El schema es objetivo y todavía no se carga en firmware |
| [`examples/README.md`](examples/README.md) | Diseño vigente | Fixtures con `activation_allowed:false` no son perfiles activables |

## Evidencia Fechada

| Documento | Clasificación | Vigencia/uso |
| --- | --- | --- |
| [`process/evidence/SVD48_ID2_CURRENT_STATE_SUMMARY.md`](process/evidence/SVD48_ID2_CURRENT_STATE_SUMMARY.md) | Snapshot 2026-07-20 | Baseline original, con correcciones de labels/unidades |
| [`process/evidence/svd48_id2_inventory_2026-07-20.md`](process/evidence/svd48_id2_inventory_2026-07-20.md) | Captura generada | Raw read-only; interpretar con contrato actual |
| [`process/evidence/KK16_SVD48_CONFIGURATION_2026-07-20.md`](process/evidence/KK16_SVD48_CONFIGURATION_2026-07-20.md) | Intermedio/supersedido | Configuración y calibraciones luego restauradas |
| [`process/evidence/SVD48_KK16_MOTOR_IDENTIFICATION_2026-07-20.md`](process/evidence/SVD48_KK16_MOTOR_IDENTIFICATION_2026-07-20.md) | Evidencia fechada | Identificación eléctrica; no convierte resultados en specs aprobadas |
| [`process/evidence/svd48_id2_inventory_2026-07-21_pre_calibration.md`](process/evidence/svd48_id2_inventory_2026-07-21_pre_calibration.md) | Captura generada intermedia | Conserva labels antiguos; banner corrige MOS/bus/speed |
| [`process/evidence/SVD48_BENCH_DIAGNOSIS_2026-07-21.md`](process/evidence/SVD48_BENCH_DIAGNOSIS_2026-07-21.md) | Evidencia fechada | Hall/PID/speed/gear y fallo `5/1`; termina con restauración referenciada |
| [`process/evidence/SVD48_RESTORE_TO_INITIAL_2026-07-21.md`](process/evidence/SVD48_RESTORE_TO_INITIAL_2026-07-21.md) | Evidencia vigente de restauración | Fuente del último baseline restaurado; power-cycle pendiente |

Los CSV/JSON bajo `process/evidence/` son datos de captura, no documentación
normativa. No se “corrigen” valores medidos; solo se corrigen nombres/unidades de
columnas cuando puede conservarse exactamente la palabra cruda original.

## Regla de Mantenimiento

Todo cambio que modifique comandos, allowlists, unidades, prioridades, target,
particiones, NVS, seguridad, OTA o registros SVD48 debe actualizar en el mismo
commit:

- el contrato/skill aplicable;
- `process/07_CURRENT_STATUS_AND_RELEASE_CHECKLIST.md`;
- la matriz/plan con los IDs afectados;
- `SESSION_LOG.md` con tests y limitaciones;
- este índice si cambia la clasificación o supersesión de un documento.
