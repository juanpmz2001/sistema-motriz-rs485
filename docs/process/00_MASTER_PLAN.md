# Multi-Robot Configuration Master Plan

Date: 2026-07-17

Program status: `IN_PROGRESS`; first firmware hardening slice has `E1/E2` evidence, hardware gates remain open.

Priority update 2026-07-19: the robot is a canonical self-contained JSON, not a
fixed four-motor struct. First deliver runtime safety and simultaneous source
arbitration `RC > LAN > Bluetooth`; then JSON profiles, typed SVD48 configuration
and firmware/backend contracts. Frontend work starts only after those backend
contracts are stable. Normative details are in
`02_ROBOT_PROFILES_KINEMATICS_SAFETY.md`,
`05_SAFE_CONFIGURATION_WRITE_PLAN.md` and
`06_SIMULATED_QA_FAULT_INJECTION_PLAN.md`.

Current implementation slice (2026-07-19, firmware build 12):

- LAN compatibility responses now map captured `ERR` lines to `status:"err"` (`TRANS-001`, hardware/web verification pending).
- Serial overlength input drains through the delimiter before accepting another command (`TRANS-007`; explicit response completion is still pending).
- Motor targets are prepared before `START`; partial prepare/start failures request a fail-stop (`SAFE-005/009`; elevated HIL evidence pending).
- A pure `robot_state` model now defines all six operational states, active inhibits, fault latches, transition actions and separate motion/configuration/OTA authorization policies. Runtime service integration and movement gates remain pending (`SAFE-001/002`).
- The host QA project now uses CMake/CTest with strict C11 warnings, a fake monotonic clock and bounded event sink. State, protocol, framing and reference-vector tests pass normally and with ASan/UBSan (`TEST-001/007`).
- SVD48 exceptions preserve matching high-bit function/code/time, and the internal `0x10` path now builds, transacts and validates echoed start/count without blind write retries. It remains unreachable from serial/LAN and has no maintenance/change-set semantics (`SVD-001/002/003`).

## Outcome

One ESP32-S3 firmware and one web application must support multiple robot variants while preserving safe control. A developer must be able to inspect robot identity, geometry, actuator mapping, live SVD48 configuration, expected-versus-actual drift, and verified parameter changes over USB or token-protected local LAN without using raw hexadecimal registers.

The target flow is:

```text
Browser UI
  -> typed REST/WebSocket contract
  -> web command broker and profile store
  -> SerialTransport or local token LAN transport
  -> firmware management service
  -> robot profile / safety / SVD48 parameter services
  -> locked and prioritized RS485 scheduler
  -> one or more SVD48 drives
```

Motion control remains a separate high-priority path:

```text
RC + LAN + Bluetooth adapters (simultaneously active)
  -> timestamped source mailboxes
  -> deterministic arbiter RC > LAN > Bluetooth
  -> authority epoch + TTL + safety state machine
  -> selected kinematic strategy
  -> validated complete actuator target
  -> actuator coordinator
  -> SVD48 + steering outputs
```

Configuration and OTA may observe safety state, but they must never bypass or own the motion path.

## Non-Negotiable Gates

These gates block the surface named by each row. `GATE-WEB` is required before a
frontend release, not before the firmware/backend CLI vertical:

| Gate | Requirement | Minimum evidence |
| --- | --- | --- |
| `GATE-SAFE` | Latched motion inhibit is enforced below every movement API; configured source availability/loss, controller faults, stale state, maintenance, and OTA have explicit transitions | `E5` |
| `GATE-STOP` | Worst-case stop latency is measured with polling, bus timeouts, and one controller absent | `E5` |
| `GATE-MVP-LAN` | Local LAN operations require the provisioned maintenance token, bounded payloads, request IDs and mutation dedupe; production identity/TLS/HMAC is tracked separately | `E2` plus `E3` |
| `GATE-CONTRACT` | A structured result cannot claim success when the underlying command failed; request/command IDs correlate end to end | `E2` plus `E3` |
| `GATE-PROFILE` | Canonical JSON active/staged/known-good slots are versioned, schema/semantically validated, hash-protected and recoverable after interrupted persistence | `E3` |
| `GATE-SVD-READ` | Parameter types, widths, order, ranges, and confidence are cataloged; uncertain fields remain read-only | `E4` |
| `GATE-SVD-WRITE` | Every enabled write has stopped-state interlock, old-value capture, readback, audit, and documented power-loss behavior | `E4` |
| `GATE-BACKEND` | Serial/LAN transports and profile/SVD routes preserve typed IDs/results and pass fault-injection tests | `E2` plus `E3/E4` |
| `GATE-WEB` | After backend completion, UI distinguishes desired/live/stale/unsupported values and cannot enable actions beyond negotiated capabilities | `E2`, frontend phase only |
| `GATE-BENCH` | All mandatory tests in `04_OFF_GROUND_TEST_MATRIX.md` pass before a floor test | `E5` |

## Workstreams

### WS0 - Containment and baseline

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `OPS-001` | Freeze a reproducible baseline: firmware/web commits, toolchain, current profiles, controller firmware IDs, and redacted configuration | `NOT_STARTED` | none | yes |
| `OPS-002` | Inventory physical robot variants, motors, drives, sensors, servos, batteries, wiring, and flash sizes | `NOT_STARTED` | none | yes |
| `OPS-003` | Enforce restrictive permissions and secret-handling checks for local `.env`, keys, NVS provisioning tools, and deployment mounts | `IN_PROGRESS` | none | yes |
| `AUTH-001` | Restrict production web deployment: no privileged container, explicit device/network policy | `DEFERRED` | before Internet/client deployment | no for local prototype |
| `AUTH-002` | Add operator identity, roles, CSRF/origin protection and security rate limits | `DEFERRED` | before multi-user/Internet deployment | no for local prototype |
| `TRANS-001` | Fix command result semantics so firmware `ERR` becomes structured failure through LAN/HTTP/UI | `IN_PROGRESS` | none | yes |

Exit: local prototype failures are represented truthfully and mutations are
bounded/deduplicated. Production trust controls remain explicit deferred work.

### WS1 - Safety and command authority foundation

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `SAFE-001` | Define states `BOOTING`, `DISARMED`, `ARMED`, `FAULTED`, `MAINTENANCE`, `OTA`; define legal transitions | `IN_PROGRESS` (pure model/tests complete; runtime service pending) | `OPS-001` | yes |
| `SAFE-002` | Enforce latched inhibit in `robot_control`/actuator coordinator, not only in periodic safety task | `NOT_STARTED` | `SAFE-001` | yes |
| `SAFE-003` | Add simultaneous RC/LAN/Bluetooth mailboxes, strict `RC > LAN > BT` arbitration, authority epochs, TTL and stop-before-switch | `NOT_STARTED` | `SAFE-001` | yes |
| `SAFE-004` | Enforce per-source `availability_policy`; distinguish optional absence, invalid/stale data, selected-source loss and explicit disarm | `NOT_STARTED` | `SAFE-003`, `PROF-001` | yes |
| `SAFE-005` | Make complete actuator updates fail-safe: precompute, validate, commit, and stop on partial failure | `IN_PROGRESS` | `SAFE-002` | yes |
| `SAFE-006` | Add emergency-class bus scheduling or bounded preemption for stop commands | `NOT_STARTED` | `SAFE-002` | yes |
| `SAFE-007` | Add deterministic task shutdown/acknowledgement to avoid deinit races | `NOT_STARTED` | none | yes |
| `SAFE-008` | Define physical E-stop input and electrical power-cut architecture | `BLOCKED` | hardware decision | yes before floor |
| `SAFE-009` | Eliminate residual-setpoint startup: write/validate the intended safe target before `START`; prohibit generic enable from reusing an unknown prior target | `IN_PROGRESS` | `SAFE-002`, `SAFE-005` | yes |

Exit: no configuration work can cause a movement API to bypass an inhibit, and stop latency has a measured bound.

### WS2 - Versioned robot profiles

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `PROF-001` | Define canonical JSON schema: board/I/O, variable controllers/channels/actuators, geometry, kinematics, sources, safety and embedded desired SVD48 configuration | `IN_PROGRESS` (schema/design/2WD draft fixture added; implementation pending) | `OPS-002` | yes |
| `PROF-002` | Implement pure schema, semantic, capability and hard-ceiling validation | `NOT_STARTED` | `PROF-001`, `SAFE-001` | yes |
| `PROF-003` | Persist exact JSON in A/B active/staged/known-good slots with hash and small NVS commit metadata | `NOT_STARTED` | `PROF-002` | yes |
| `PROF-004` | Migrate current literals into a factory JSON and prove target equivalence before deleting them | `NOT_STARTED` | `PROF-003` | yes |
| `PROF-005` | Add get/validate/stage/activate/rollback/status APIs; activation only stopped and safe | `NOT_STARTED` | `PROF-003`, `SAFE-002` | yes |
| `PROF-006` | Add schema migration, corrupt-profile and interrupted-commit recovery tests | `NOT_STARTED` | `PROF-003` | yes |
| `PROF-007` | Define and advertise measured resource capabilities without assigning topological meaning to a fixed motor count | `NOT_STARTED` | `PROF-001`, timing/memory budget | yes |
| `PROF-008` | Include UART, RS485, RC, servo GPIO/LEDC and recovery settings in the same JSON; activate resource changes reboot-only | `NOT_STARTED` | `PROF-002`, `PROF-003`, `SAFE-001` | yes |
| `PROF-009` | Add chunked/hash-verified full-JSON upload over USB and LAN | `NOT_STARTED` | `PROF-005`, `TRANS-003/004/008` | yes |

Exit: each known robot is representable, but only the profile matching current hardware is activated during initial validation.

### WS3 - Kinematic strategies and actuator mapping

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `KIN-001` | Define pure strategy interface over variable actuator IDs/poses and complete target arrays | `NOT_STARTED` | `PROF-001` | yes |
| `KIN-002` | Implement generic differential strategy with 1..N motors per left/right group | `NOT_STARTED` | `KIN-001` | yes |
| `KIN-003` | Prove one-drive/two-motor, two-drive/four-motor and four-drive/four-motor fixtures use the same differential strategy | `NOT_STARTED` | `KIN-002` | yes |
| `KIN-004` | Implement Ackermann ICR geometry for single-linkage and per-wheel steering | `NOT_STARTED` | `KIN-001` | yes |
| `KIN-005` | Implement independent-steer/crab modules plus per-servo calibration/limits | `NOT_STARTED` | `PROF-002`, `KIN-001` | yes |
| `KIN-006` | Add acceleration/jerk limiting and steering hysteresis | `DEFERRED` | baseline deterministic motion | revisit before performance tuning |

Exit: host golden tests cover all robot variants, then wheel-off/elevated tests validate sign, mapping, and saturation.

### WS4 - SVD48 protocol and typed read path

Detailed ownership is in `01_SVD48_REGISTER_COVERAGE.md`.

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `SVD-001` | Preserve exception function/code and distinguish device exception from timeout/bad response | `IN_PROGRESS` | none | yes |
| `SVD-002` | Add response parser tests, including `0x90`, standard high-bit exception forms, CRC, truncation, and wrong slave/function | `DONE` | `SVD-001` | yes |
| `SVD-003` | Implement `0x10` write-multiple request/response support with golden vectors | `IN_PROGRESS` (host/build complete; restrained controller pending) | `SVD-002` | yes |
| `SVD-004` | Implement typed codecs for `u16`, `i16`, `u32`, `i32`, and verified float word order | `BLOCKED` for float | SV-Config capture | yes |
| `SVD-005` | Build parameter catalog with access, range, unit, scope, persistence, safety class, and confidence | `NOT_STARTED` | audit complete | yes |
| `SVD-006` | Add typed single/group reads and controller inventory/capabilities | `NOT_STARTED` | `SVD-004`, `SVD-005` | yes |
| `SVD-007` | Add expected-versus-live drift fingerprint without automatic boot writes | `NOT_STARTED` | `SVD-006`, `PROF-001` | yes |
| `SVD-008` | Deprecate direct raw maintenance access from normal builds/policies | `NOT_STARTED` | typed diagnostics available | yes |

Exit: browser and CLI can inspect all verified fields read-only over USB and LAN.

### WS5 - Typed transport and management plane

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `TRANS-002` | Extract typed command/management service from `serial_gateway`; transports own framing only | `NOT_STARTED` | `TRANS-001` | yes |
| `TRANS-003` | Define protocol version, boot ID, capabilities, request/command ID, deadline, result code, and schema evolution rules | `NOT_STARTED` | `TRANS-002` | yes |
| `TRANS-004` | Bound UDP requests/responses below configured safe datagram size; add pagination/chunks or move large operations to reliable transport | `NOT_STARTED` | `TRANS-003` | yes |
| `TRANS-005` | Preserve correlation across browser -> HTTP/WS -> backend -> USB/LAN -> firmware -> result | `NOT_STARTED` | `TRANS-003` | yes |
| `TRANS-006` | Implement a backend-owned telemetry scheduler and typed samples; browsers subscribe instead of independently polling | `NOT_STARTED` | `TRANS-005` | yes |
| `TRANS-007` | Make serial framing recover safely from overlong/malformed input by draining to a delimiter; add explicit completion framing | `IN_PROGRESS` | `TRANS-002` | yes |
| `TRANS-008` | Add idempotency/deduplication for retried requests and jobs; same ID with different body is an error | `NOT_STARTED` | `TRANS-003` | yes |
| `TRANS-009` | Add compact `control_lan` ingress on a separate port; validate stream/sequence/TTL and publish only the LAN mailbox | `NOT_STARTED` | `SAFE-003`, `TRANS-003` | yes |
| `AUTH-003` | Replace local bearer token with identity/HMAC/nonce/timestamp/replay security | `DEFERRED` | before hostile/shared network or production | no for local prototype |
| `AUTH-004` | Evaluate authenticated TCP/TLS for large configuration payloads | `DEFERRED` | measured UDP/chunking constraints | future architecture decision |
| `AUTH-005` | Add signed firmware trust policy and evaluate Secure Boot, flash encryption, and NVS encryption against recovery/manufacturing needs | `NOT_STARTED` | threat model and key-provisioning procedure | yes before production deployment |

Exit: serial and LAN produce equivalent domain results and capabilities; ASCII lines remain diagnostics only.

### WS6 - Safe SVD48 write sessions

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `SVD-020` | Add exclusive firmware maintenance operation with stopped-state guard, expiry and visible job state; no operator/session ownership in local MVP | `NOT_STARTED` | `SAFE-002`, `TRANS-003` | yes |
| `SVD-021` | Add change-set validation, compiled hard ceilings, old-value capture, ordered writes, readback, and per-field results | `NOT_STARTED` | `SVD-005`, `SVD-020` | yes |
| `SVD-022` | Implement controller flash-save workflow and document non-atomic power-loss behavior | `BLOCKED` | hardware experiment | yes |
| `SVD-023` | Enable first verified low-risk fields: pole pairs, RPM/current limits, direction, sensor type, ramps | `NOT_STARTED` | `SVD-021`, hardware evidence | yes |
| `SVD-024` | Board address/baud migration with recovery channel | `DEFERRED` | capture and recovery design | no |
| `SVD-025` | Enable bounded speed PID reads/writes after float word-order and persistence evidence | `NOT_STARTED` | `SVD-004/021`, hardware capture | yes |
| `SVD-026` | Hall/encoder calibration workflows | `DEFERRED` | hazardous state-machine design | later MVP increment |
| `SVD-027` | Throttle/brake/PPM/CAN/card-reader features | `DEFERRED` | actual robot requirement | no |
| `SVD-028` | Add compile-time-disabled engineering raw-write mode with maintenance gate, denylist, old value, readback and audit | `NOT_STARTED` | `SVD-021`, `GATE-SAFE`, hardware procedure | no |

Exit: a deliberately small verified parameter set can be changed and recovered on restrained hardware.

### WS7 - Web backend, then frontend

The detailed consumer plan lives in the web repository `docs/process/00_WEB_IMPLEMENTATION_PLAN.md`.

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `WEB-001` | Introduce typed backend `SerialTransport`/`LanTransport` and connection state | `NOT_STARTED` | `TRANS-001/003` | yes |
| `WEB-002` | Add backend JSON schema validation and stable API/error envelopes | `NOT_STARTED` | `PROF-001`, `TRANS-003` | yes |
| `WEB-003` | Add backend profile/capability/source/controller/read/change-set routes and job correlation | `NOT_STARTED` | `SVD-006/021`, `PROF-005/009` | yes |
| `WEB-004` | Add backend unit/API/transport/fault-injection tests for USB and LAN | `NOT_STARTED` | `WEB-001..003` | yes |
| `WEB-101` | Design and implement profile/controller Configuration frontend from stable backend capabilities | `NOT_STARTED` | `WEB-001..004` complete | after backend |
| `WEB-102` | Add guarded change-set review/readback frontend | `NOT_STARTED` | `WEB-101`, `SVD-021/025` | after backend |
| `WEB-103` | Add PID telemetry/tuning frontend | `DEFERRED` | `SVD-025`, backend evidence | later increment |

Exit: the UI cannot invent register semantics and cannot offer unsupported/unsafe operations.

### WS8 - Release and robot-elevated validation

| ID | Work item | Status | Dependencies | MVP |
| --- | --- | --- | --- | --- |
| `TEST-001` | Automate firmware protocol/profile/kinematic/safety unit tests | `IN_PROGRESS` | corresponding units | yes |
| `TEST-002` | Automate Node backend unit/API/serial/LAN/control transport tests | `NOT_STARTED` | `WEB-001..004` | yes |
| `TEST-003` | Build mock SVD48/UDP fault-injection harness | `NOT_STARTED` | `SVD-002`, `TRANS-003` | yes |
| `TEST-004` | Execute all USB and LAN-only tests with ESP and no motor bus | `NOT_STARTED` | transport implementation | yes |
| `TEST-005` | Execute SVD48 tests with motor power isolated or wheels removed | `NOT_STARTED` | typed read/write slices | yes |
| `TEST-006` | Execute complete elevated-robot matrix with physical cutoff operator | `NOT_STARTED` | all MVP gates | yes |
| `TEST-007` | Automate deterministic state/authority/change-set tests with virtual clock, fake SVD48/bus and communication fault injection | `IN_PROGRESS` (harness/state tests complete; authority/bus/change-set pending) | `SAFE-001`, `SVD-003`, test harness | yes |
| `TEST-008` | Add frontend Playwright/accessibility tests only after backend B0-B5 | `DEFERRED` | `WEB-101/102` | frontend phase |
| `OPS-010` | Sign firmware/artifacts and define compatibility/rollback matrix | `NOT_STARTED` | transport/release baseline | yes before production |

Exit: `GATE-BENCH` passes and a separate approved floor-test plan is created. Floor movement is not authorized by this plan alone.

## Recommended Delivery Slices

Keep pull requests independently reviewable while prioritizing a usable configuration vertical:

1. Finish runtime state service, gate every output, latch faults and prioritize
   emergency stop: `SAFE-001/002/005/006/009`, `TEST-001/003/007`.
2. Implement simultaneous source mailboxes/arbiter and compact LAN control ingress:
   `SAFE-003/004`, `TRANS-009`.
3. Implement canonical JSON validation/capabilities/runtime snapshot and generic
   differential 2WD: `PROF-001/002/004/007`, `KIN-001/002`.
4. Finish `0x10`, typed codecs, parameter catalog, inventory and compare:
   `SVD-003..007`.
5. Add JSON A/B persistence/upload plus typed management/change-set jobs with no
   public writes: `PROF-003/005/006/008/009`, `SVD-020/021`,
   `TRANS-002..005/007/008`.
6. Enable hardware-verified allowlisted fields and speed PID over USB/LAN:
   `SVD-023/025`.
7. Add remaining generic topology fixtures and Ackermann/independent-steer
   strategies: `KIN-003..005`.
8. Implement and test the backend completely, preserving serial/LAN equivalence:
   `WEB-001..004`.
9. Only then build the frontend from negotiated capabilities: `WEB-101/102`.
10. Expand the allowlist, complete controller persistence and elevated validation:
    `SVD-022/028`, `TEST-004..006`.

Do not combine a safety-state refactor, a protocol rewrite, a writable UI, and a controller parameter change in one release.

## Definition of MVP

The MVP includes:

- all current robot variants represented by canonical self-contained JSON profiles;
- variable controller/channel/actuator mappings within advertised capabilities;
- differential, Ackermann linkage/per-wheel and independent-steer/crab strategies;
- simultaneous `RC > LAN > Bluetooth` authority with TTL and safe handoff;
- read-only inventory of all verified SVD48 fields;
- expected-versus-live drift reporting;
- a small allowlist of verified controller writes in an exclusive maintenance job;
- equivalent USB/LAN behavior with structured results;
- local-LAN token, bounded/idempotent management operations and truthful errors;
- automated tests plus the complete off-ground matrix.

The MVP does not include operator accounts, TLS/HMAC, PID auto-tuning, automatic
configuration writes at boot, arbitrary raw LAN writes, every manual register,
direct browser-to-ESP communication or autonomous floor operation.
