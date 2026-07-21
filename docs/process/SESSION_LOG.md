# Firmware Session Log

Entries are append-only. Corrections should add a dated note rather than rewriting prior evidence.

## 2026-07-17 - Cross-Repository Architecture Audit and Process Setup

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`, base commit `d932792`
- Web repository: branch `main`, base commit `0d55f8b`
- Dirty worktree before work: yes in both repositories; pre-existing maintenance LAN and web transport changes were preserved.
- Hardware state: not used during this documentation audit.
- Claimed work items: planning and evidence collection only.

### Subagents

| Agent | Session ID | Audit scope | Result |
| --- | --- | --- | --- |
| Ptolemy | `019f721f-fa93-7e02-a1d6-0d42c5f3fc12` | SVD48 protocol, manuals, register coverage, and tests | Completed read-only audit |
| Lovelace | `019f721f-fc7b-7831-9c38-08b8dae7a055` | Firmware runtime, profiles, kinematics, RC, safety, OTA, and task lifecycle | Completed read-only audit |
| Archimedes | `019f721f-fedd-7f80-963b-1bad953d9f4f` | Web frontend/backend, serial/LAN transport, security, and test architecture | Completed read-only audit |
| Descartes | `019f7220-01a3-7a10-8188-db3aa84c0b86` | Cross-repository protocol, correlation, authentication, concurrency, and deployment | Completed read-only audit |
| Pasteur | `019f7221-5495-7462-9ab4-a55cb96657da` | Independent full SVD48 manual/register review | Completed; extracted/reviewed all 81 manual pages and contrasted code/docs |
| Huygens | `019f7221-5688-7f62-a8d4-ec056fafe1f1` | Independent maintenance LAN/gateway contract review | Completed read-only audit and build/tool verification |

### Confirmed Findings

- SVD48 support is strong for telemetry and basic speed/current commands, but lacks typed parameter metadata, `0x10` multi-register writes, float codecs, access/range enforcement, readback transactions, persistence workflow, and calibration state machines.
- Current start/enable ordering can reuse a previous nonzero controller setpoint because `START` precedes the new RPM; this is an MVP safety blocker.
- The robot topology, drive IDs, four-motor arrays, servo resources, geometry, and kinematics are compile-time assumptions.
- Safety stop is repeatedly requested but not latched below every movement API. Serial movement can be reissued while the safety task is trying to stop.
- Multi-wheel commands are not application-level transactions; a mid-command failure can leave mixed actuator state.
- RC is parsed diagnostically but does not yet own motion, implement a dead-man, or fail closed when absent at boot.
- Maintenance LAN can report transport-level `status:"ok"` while captured command output contains `ERR`.
- Current UDP payload sizing can exceed a reliable non-fragmented datagram, and request correlation does not survive through HTTP/WebSocket to the browser.
- Serial overflow can resynchronize unsafely and execute a rejected line's suffix; current CLI response completion is also ambiguous.
- LAN/OTA retries are not idempotently deduplicated, and invalid OTA announce input can mutate NVS before full action validation.
- The web backend binds broadly and has no operator authentication/origin/CSRF boundary. Its generic command endpoint relies primarily on firmware policy.
- Existing tests are sparse: protocol request-vector tests exist, but there is no broad ESP-IDF unit/integration/HIL suite and no web automated test suite.

### Changed

- Added the firmware `docs/process/` program plan, SVD48 inventory, profile/safety plan, transport contract, compatibility matrix, ADR, off-ground tests, templates, and session log.
- Linked the process from `README.md` and linked the earlier profile design to the process source of truth.
- Corrected `docs/API.md` and `docs/skills/SVD48B50A_SKILL.md` where they overstated raw-write equivalence, exception handling, retry behavior, 32-bit ordering confidence, current bench topology, pole-pair certainty, and legacy apply safety.
- Restricted local `.env` permissions from `0755` to `0600` without reading or printing secrets.

### Verified

- Parent planning work used source/manual inspection and no hardware (`E0`); subagents additionally supplied the `E1/E2` evidence below.
- Ptolemy reported `python3 tools/test_svd48_protocol.py`: 3 tests passed (`E1`); this parent session will independently retain verification in the final process check.
- Parent session independently ran `PYTHONDONTWRITEBYTECODE=1 python3 tools/test_svd48_protocol.py`: 3 tests passed (`E1`).
- Huygens ran an ESP-IDF 5.4 build successfully and reported a `0xfe070` firmware binary with substantial OTA-slot headroom (`E2`); no hardware was exercised.
- Pasteur extracted both bundled manuals completely (45-page controller manual and 36-page SV-Config manual) and independently ran the 3 protocol request-vector tests successfully (`E0/E1`); no hardware was exercised.
- Local `.env` permissions were found at `0755` and restricted to owner-only `0600` without reading or printing its contents. This partially advances `OPS-003`; deployment/key/NVS checks remain.

### Remaining

- Execute the master plan in dependency order.
- Gather physical robot variant and motor datasheets.
- Perform the read-only controller inventory before enabling any new write path.
- Complete the remaining `OPS-003` secret-handling and deployment permission checks.

### Deferred

- No runtime feature was implemented in this session. The purpose was complete planning and traceability.

### Rollback

- Revert only the new `docs/process/` files, planning links, and documentation-coherence corrections in `docs/API.md` and `docs/skills/SVD48B50A_SKILL.md`. Do not revert pre-existing dirty worktree changes.

## 2026-07-17 - First Firmware Hardening Slice

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`, base commit `d932792`, dirty pre-existing worktree preserved.
- Hardware state: no USB, SVD48, servo, or robot-elevated evidence used in this slice.
- Claimed work items: `TRANS-001`, `TRANS-007`, `SAFE-005`, `SAFE-009`, `SVD-001`, `SVD-002`, `SVD-003`, `TEST-001`.

### Changed

- Added pure serial framing and result helpers. Overlong input now drains to a delimiter, and maintenance LAN maps captured `ERR` output to an error JSON envelope.
- Extracted pure SVD48 protocol helpers, added bounded `0x10` request construction, recognized matching high-bit exceptions, and preserved last exception function/code/time in telemetry and `GET_MOTOR`.
- Changed motor command ordering to prepare RPM before `START`; `ENABLE ALL` prepares zero targets and partial prepare/start failures request a best-effort stop.
- Added the initial host firmware contract test and `tools/run_host_tests.sh`; bumped firmware build from 10 to 11. The test source was later moved to `tests/host/firmware_contracts_test.c` when CTest was introduced.

### Files

- Runtime/protocol: `components/maintenance_lan/maintenance_lan.c`, `components/robot_control/robot_control.c`, `components/serial_gateway/{CMakeLists.txt,serial_gateway.c,serial_gateway_framing.c,serial_gateway_result.c,include/*}`, `components/svd48/{CMakeLists.txt,svd48.c,svd48_protocol.c,include/*}`, `main/app_version.h`.
- Tests: the original contract test (now `tests/host/firmware_contracts_test.c`) and `tools/run_host_tests.sh`.
- Contract/process docs: `README.md`, `docs/API.md`, `docs/skills/SVD48B50A_SKILL.md`, `docs/process/00_MASTER_PLAN.md`, `01_SVD48_REGISTER_COVERAGE.md`, `03_TRANSPORT_AND_API_CONTRACT.md`, `04_OFF_GROUND_TEST_MATRIX.md`, and this log.

### Verified

- `./tools/run_host_tests.sh`: `PASS` (`E1`). Native C contracts passed; existing Python suite reported 3/3 passing.
- ESP-IDF 5.4.1 `idf.py build`: `PASS` (`E2`) for `esp32s3`; binary size `0xfe480`, smallest app partition `0x600000`, 83% free.
- No firmware was flashed and no hardware behavior is claimed.

### Remaining

- Run `TEST-ESP-014/015` over real USB/LAN and confirm the existing Node backend rejects firmware `status:"err"`.
- Run `TEST-SVD-007` against a restrained/backed-up controller to capture real `0x83/0x86` behavior.
- Run `TEST-SAFE-015/017/018` on the elevated Toño platform; measure frame order, residual motion, partial-start fail-stop, and stop latency.
- Complete typed management results, response completion framing, the `0x10` response/transaction path, a latched inhibit, command arbitration, and an emergency-class bus scheduler.

### Known Limitations

- Fail-stop is best effort on the same serialized RS485 bus; it is not atomic and has no measured deadline.
- Updating targets on motors that are already running remains sequential. The planned actuator coordinator and latched safety state are still required.
- Exception fields preserve the latest historical exception; `COMM_ERR` and age must be considered together.
- The `0x10` builder is intentionally not reachable from serial or LAN.

### Rollback

- Revert this slice's protocol/framing helpers and call-site changes together; reverting only the call sites or only the CMake source lists will break the build. Restore build number 10 only if the complete behavior slice is removed.

## 2026-07-19 - Safe Write, Pin Configuration, QA and Human Guide Planning

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`, base commit `d932792`; existing dirty runtime/documentation work preserved.
- Web repository: existing dirty runtime work preserved; only `docs/process/00_WEB_IMPLEMENTATION_PLAN.md`, `01_WEB_TEST_MATRIX.md`, and `SESSION_LOG.md` were updated.
- Hardware state: not used.
- Claimed work: planning/documentation only; no runtime behavior implemented.

### Confirmed

- Current firmware has no exclusive operational state machine or latched movement gate; `robot_safety` periodically requests stop.
- Current raw USB write reaches SVD48 `0x06` without maintenance state, catalog/range, baseline/readback or audit; LAN blocks writes.
- RS485, i-BUS and servo pins remain compile-time values in `main/main.c`.
- Current web app has serial/LAN prototype transports and generic command routes, but no auth/test runner/typed configuration service.
- Typed reads are a direct prerequisite for safe writes because the same descriptor owns decode, validation, encode and readback.

### Changed

- Added `05_SAFE_CONFIGURATION_WRITE_PLAN.md` with the normative state protocol, write/change-set flow, frontend/USB-to-LAN delivery path, engineering raw boundary and staged pin recovery design.
- Added `06_SIMULATED_QA_FAULT_INJECTION_PLAN.md` with pure host architecture, fake clock/SVD48/bus, invariants and detailed state/motion/communication/write/pin fault suites.
- Added `docs/FIRMWARE_LOGIC_HUMAN_FRIENDLY.md` explaining current and target behavior in Spanish pseudocode, including typed reads and stale semantics.
- Reprioritized `00_MASTER_PLAN.md`, added `PROF-008`, `SVD-028`, `TEST-007`, expanded `04_OFF_GROUND_TEST_MATRIX.md` for state/lease/pin/ambiguous-write evidence, indexed the new docs, and corrected the outdated target-before-`START` limitation.
- Aligned the web process plan/test matrix with the USB-first guarded write vertical, secure LAN follow-up, board-profile routes, ambiguous-write jobs and pin activation/rollback tests; no web runtime code changed.

### Verification

- Source and existing firmware/web plans were inspected (`E0`).
- Documentation-only change: no build, host test, ESP, SVD48 or motor test was required or claimed.

### Next Implementation Slice

- Implement QA scaffold/fake clock/event sink and pure `robot_state` transitions/invariants before exposing any new write.
- In parallel complete SVD48 `0x10` response/transaction and typed integer catalog fixtures.
- Do not relax `LAN_SAFE` or add frontend raw writes during that slice.

## 2026-07-19 - State Model, Host QA Harness and Internal FC 0x10

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`, base commit `d932792`; pre-existing dirty worktree preserved.
- Hardware state: no USB, SVD48, servo or robot hardware used; no firmware was flashed.
- Claimed work items: `SAFE-001`, `SVD-003`, `TEST-001`, and partial `TEST-007`.

### Subagents

| Agent | Session ID | Ownership | Result |
| --- | --- | --- | --- |
| Franklin | `019f7b21-ecfe-71b3-b203-26262aa93c9c` | `components/svd48/**` and protocol contract tests | Implemented internal FC `0x10` request/transaction/ACK validation; no USB/LAN exposure |
| Copernicus | `019f7b22-3759-7a00-8243-70b11b4e5ff6` | `tests/**` and `cmake/host_tests/**` infrastructure | Implemented CMake/CTest harness, strict warnings, fake clock and bounded event sink |

### Changed

- Added `components/robot_state/` as a pure C model for `BOOTING`, `DISARMED`, `ARMED`, `FAULTED`, `MAINTENANCE`, and `OTA`, with active inhibits, fault latches, monotonic revision, stop/revoke actions and separate motion/configuration/OTA authorization queries.
- Added deterministic state tests for boot, arm blockers, stale faulting, explicit fault ACK, expired authority recovery, disarm, maintenance write guards and maintenance/OTA exclusion.
- Added the standalone host-test project under `tests/` and `cmake/host_tests/`, including a reusable test runner, monotonic fake clock and bounded event recorder.
- Consolidated host execution through `tools/run_host_tests.sh`; CTest now runs support/fake tests, state tests, firmware protocol/framing contracts and the independent Python SVD48 vectors.
- Completed internal FC `0x10` request/response support through `svd48_write_registers_by_id()`, including bounds/overflow checks, response slave/function/start/count/CRC validation and preserved Modbus exceptions.
- Changed FC `0x10` to one transport attempt. A lost ACK is intentionally left for future readback/outcome classification instead of a blind retry.
- Added `robot_state` to the ESP-IDF component graph, bumped firmware build 11 to 12, and updated README/API/skill/process documentation to distinguish implemented foundations from runtime and hardware evidence.

### Verified

- `./tools/run_host_tests.sh`: `PASS`, 4/4 CTest tests (`E1`).
- CMake host suite with `BOTFARMS_HOST_TEST_SANITIZERS=ON`: `PASS`, 4/4 with ASan/UBSan (`E1`); `ASAN_OPTIONS=detect_leaks=0` was required because the supervised environment restricts `ptrace`.
- ESP-IDF 5.4.1 `idf.py build`: `PASS` (`E2`) for target `esp32s3`; build 12 binary `0xfe600`, smallest app partition `0x600000`, 83% free.
- `robot_state_model.c` was present in the regenerated ESP-IDF component graph and compiled for the target.
- No E3-E6 hardware behavior is claimed.

### Safety Boundary and Remaining Work

- The state model is not yet instantiated by `main` and is not consulted by `robot_control`, `robot_safety`, serial or LAN. It therefore does not yet enforce `INV-001..003` in runtime firmware.
- Existing raw USB `WRITE_REG` remains unguarded by maintenance/catalog/readback. LAN writes remain blocked and must not be enabled by changing an allowlist.
- FC `0x10` has no TX-phase result, readback, `OUTCOME_UNKNOWN`, rollback or audit service yet and remains internal.
- Next critical slice: add a lock/service wrapper, fail-safe initial health snapshot, command authority/lease, gate every movement entry point at I/O, and feed RC/controller faults into the latch. Only then add maintenance sessions and typed writes.
- Physical test pending: execute FC `0x10` only on a backed-up, restrained SVD48 using approved low-risk registers and capture real ACK/exception/readback behavior.

### Rollback

- Remove `robot_state` from `main/CMakeLists.txt` before removing `components/robot_state/`.
- Revert the FC `0x10` driver/protocol/header/test changes as one unit; do not leave a public declaration without its parser/implementation.
- Revert `tests/`, `cmake/host_tests/` and `tools/run_host_tests.sh` together if restoring the previous direct-compiler runner.

## 2026-07-19 - Canonical Robot JSON and Multi-Source Replanning

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`, base commit `d932792`; existing dirty
  firmware/documentation changes preserved.
- Claimed work items: design update for `PROF-001..009`, `KIN-001..005`,
  `SAFE-003A..003D`, `SVD-020/025`, `TRANS-003/004/008`, `WEB-001..004`.
- Hardware state: none used; documentation/schema work only.

### Decisions

- The robot is one versioned, self-contained canonical JSON persisted and
  transported to the ESP. Runtime C structures are derived immutable snapshots.
- Variable controller/channel/actuator lists replace the planned four-motor/four-
  servo topology assumption. Firmware capabilities bound resources without
  changing schema meaning.
- The same profile represents one SVD48 using M1/M2, two SVD48 using four
  channels, four SVD48, Ackermann linkage/per-wheel steering and independent
  steer/crab combinations.
- RC, LAN and Bluetooth are received simultaneously and arbitrated strictly as
  `RC > LAN > Bluetooth`, with TTL, authority epochs, stop-before-switch and no
  stale fallback.
- Local MVP keeps the provisioned LAN token and firmware-owned exclusive jobs;
  accounts, user sessions, HMAC/TLS and replay security are deferred to
  production exposure.
- Firmware and backend are completed/tested before frontend implementation.

### Changed

- Rewrote the robot profile/kinematics plan and process plan around canonical JSON
  and generic composition.
- Added `docs/schemas/robot-profile.schema.json` and a non-activatable topology
  draft for one SVD48 M1/M2, two driven wheels and two casters.
- Added ADR-0002 for canonical JSON and ADR-0003 for simultaneous source
  arbitration; updated ADR-0001 where superseded.
- Reprioritized master, safe-write, transport, QA and elevated-test plans; moved
  production auth out of the local MVP and frontend after backend.
- Kept `maintenance_lan` as the management plane and specified separate compact
  `control_lan` ingress/port for the LAN movement mailbox.
- Updated the sibling web repository process plan/test matrix/index/session log
  to backend slices B0-B5 first, with frontend and production auth deferred.
- Updated human-friendly pseudocode for JSON normalization and RC/LAN/BT
  preemption/loss behavior.

### Verified

- `jq empty` passed for schema and example JSON.
- `python3 -m jsonschema -i <example> <schema>` passed.
- `git diff --check` passed.
- No firmware build/test was rerun because no runtime source changed in this
  planning slice; previous E1/E2 evidence is unchanged.

### Remaining

- Measure dimensions, wheel radii, gear ratio, signs, pinout, battery ceilings and
  SVD48 parameters before setting `activation_allowed:true` on the 2WD profile.
- Implement P0 runtime state/gates and command arbiter before profile activation
  or any public SVD48 write.
- Define measured firmware capacity values from heap/stack/UART/LEDC/RS485 timing.

### Rollback

- Revert this documentation/schema/ADR slice together. Do not retain the old
  fixed-capacity plan while keeping only the generic schema, or vice versa.

## 2026-07-19 - Authority, Differential Kinematics and Inactive Control Ingress

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `main`; `origin/main` remained at `d932792` during
  this slice and no push was performed.
- Hardware state: no USB, ESP32, SVD48, servo or robot hardware used; no firmware
  was flashed and no `E3..E6` evidence is claimed.
- Work items advanced: `SAFE-001/003/005/009`, `KIN-001/002/003`, `SVD-008`,
  `TRANS-009`, `TEST-001/007`.

### Organized Commits

| Commit | Scope |
| --- | --- |
| `e1da543` | Preserve the pre-existing LAN maintenance, state-model and host-test foundation |
| `9903a69` | Preserve the canonical multi-robot JSON/process roadmap |
| `cc59e74` | Attempt controller `STOP` even when writing a zero target fails |
| `32bd261` | Reject raw single/multiple writes touching known SVD48 runtime-actuation registers |
| `ece1785` | Add deterministic command authority, generic differential kinematics and host regressions |
| `83a3fef` | Add the mutex state service and compiled-but-inactive guarded `control_lan` ingress; bump build 13 |

### Subagents

| Agent | Session ID | Ownership/result |
| --- | --- | --- |
| Popper | `019f7dbc-7c7b-73c1-a2ad-e33ea4780364` | Audited movement entry points, bypasses and concurrency |
| Tesla | `019f7dbc-82aa-7c70-a5f3-184610f3c632` | Audited runtime state, safety and OTA interactions |
| Boole | `019f7dbc-7f72-7d23-81d1-c7c202c2335f` | Audited the P0 host/elevated test matrix |
| Ptolemy | `019f7dc3-ae1c-74f2-8d2d-e797e8b127d4` | Implemented the first state-service wrapper |
| Goodall | `019f7dc3-abaa-71c3-88c9-95ba08245f36` | Implemented the pure authority model and tests |
| Cicero | `019f7dc3-b0e8-70b2-a241-c994f147c458` | Implemented the first `control_lan` component |
| Lovelace | `019f7dd7-9f64-72c1-9bae-b5fc1c2c10fb` | Implemented generic differential kinematics and tests |
| Halley | `019f7de2-8739-7441-bacc-81762503740a` | Found authority replay/dead-man and numeric saturation defects; cleared the final fixes |
| Volta | `019f7fa4-7988-7182-ab55-432a655f2667` | Found state permit/callback races and control-ingress lifecycle/protocol defects; fixes were applied before commit |
| Poincare | `019f7fc3-cd72-7382-abc2-78261d9b48e5` | Extra final review was interrupted after exceeding the bounded wait; no review result is claimed |

### Implemented

- Added a pure `RC > LAN > Bluetooth` authority model with bounded commands,
  TTL/dead-man/velocity validation, monotonic sequence, retired-stream defense,
  stop-before-switch, temporal/revision epoch barriers and fail-safe handling of
  rejected reducing commands.
- Added pure differential inverse kinematics for variable left/right motor arrays,
  per-actuator radius/ratio/sign/RPM limits, proportional saturation and numeric
  overflow/final-clamp defenses.
- Added a mutex-protected state service for single-owner inhibit slots, fault
  transitions, direct transition results, snapshots and gate epochs. Unsafe
  check/use permits and external callbacks were removed after review.
- Added `control_lan` UDP `32322` parsing for protocol `1.0`, token, request ID,
  exact sequence, stream, bounded float velocities and dead-man. Stream handover
  emits STOP before ARM; queued bursts share the first dequeue timestamp; start
  and deinit are serialized. It emits typed events only.
- Added containment around raw SVD48 actuation writes and strengthened best-effort
  stop behavior without exposing public multi-register configuration writes.

### Verified

- `./tools/run_host_tests.sh`: `PASS`, 6/6 CTest tests (`E1`). Authority has 14
  cases and differential kinematics has 12 cases.
- `BOTFARMS_HOST_TEST_SANITIZERS=ON ./tools/run_host_tests.sh`: `PASS`, 6/6 with
  ASan/UBSan (`E1`); LeakSanitizer remains disabled by the runner in this
  supervised environment.
- ESP-IDF 5.4.1 `idf.py build`: `PASS` (`E2`), target `esp32s3`, build 13 binary
  `0xfe6a0`, smallest app partition `0x600000`, 83% free.
- The ESP-IDF graph compiled `command_authority`, `robot_kinematics`,
  `robot_state_service` and `control_lan`.

### Safety Boundary and Next Slice

- `main` does not instantiate the state service, authority model, differential
  strategy or `control_lan`; USB movement and current `robot_control` remain on
  the fixed four-motor path. This slice does not satisfy `GATE-SAFE`.
- There is still no single actuator owner. A permit checked before I/O cannot
  close the check/use race, so the next slice must introduce an actuator
  coordinator that owns every SVD48/servo motion write, serializes emergency
  stop, and evaluates the gate at that ownership boundary.
- A validated factory robot JSON/runtime snapshot must replace fixed topology
  before enabling differential output or starting `control_lan`.
- RC and Bluetooth still lack mailbox adapters; serial movement remains a direct
  legacy path. Ackermann, independent steer/crab, typed SVD48 configuration,
  backend integration and all elevated/hardware tests remain open.
- Raw writes to known actuation registers are blocked, but other raw configuration
  writes and `APPLY_PY6514_CONFIG` remain hazardous legacy bench surfaces.

## 2026-07-20 - Provisional SVD48 Register Vertical and PPM GPIO14

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware repository: branch `feature/mvp-svd48-register-editor`.
- Web repository: branch `feature/mvp-svd48-register-editor`.
- Reference inspected: `origin/lucho@5ac0a52`,
  `bootfarms/tono/tecnico/software/Joystic_iphone/Joystic_iphone.ino`.
- Hardware state for this entry: ESP connected by USB, but flash/HIL evidence is
  recorded separately after the documentation commits.

### Subagents

| Agent | Session ID | Scope/result |
| --- | --- | --- |
| Hypatia | `019f829b-c464-7061-9ec4-f5c3482e6a0e` | Audited `origin/lucho`, RAFA wiring/PPM/differential assumptions and current firmware gaps |
| Gauss | `019f829b-c7ec-72a3-9799-8c60a7c075af` | Reviewed backend/editor validation, response correlation, ambiguous outcomes and responsive UI |
| Turing | `019f829b-c5fc-7d83-aa18-09f3ebe7c25a` | Audited documentation drift across firmware and web repositories |

### Organized Commits

| Repository/commit | Scope |
| --- | --- |
| Firmware `86d832f` | Confirmed SVD48 register writes over USB/LAN with pre-read, one write, readback and actuation denylist |
| Firmware `0adf2e6` | Host-tested PPM decoder/facade, default GPIO14 and RAFA non-servo boot baseline; build 14 |
| Web `8722bea` | Bounded register backend routes, serial/LAN correlation, fake-LAN tests and responsive bench editor |

### Implemented

- `READ_REG`, `GET_SVD48_CONFIG`, `WRITE_REG ... CONFIRM` and bounded
  `WRITE_REGS ... CONFIRM` are available under `LAN_SAFE` for supervised
  configuration. Known runtime-actuation registers remain blocked.
- A write requires the stopped heuristic, captures old values, sends one FC
  `0x06`/`0x10` transaction and verifies exact readback. Ambiguous ACK/readback
  outcomes explicitly say not to retry blindly.
- PPM is the default receiver mode on GPIO14 with 10 channels, `3000 us` sync,
  `750..2250 us` pulses and `300 ms` stale timeout. It feeds status and RC-loss
  safety only; it cannot arm or command motors.
- `main` uses the RAFA reference geometry (`1.60 m` wheelbase, `0.70 m` track,
  `0.10 m` radius), disables steering servo outputs and requests a best-effort
  boot `STOP ALL`.
- The draft canonical profile now represents PPM GPIO14, one SVD48 ID 1 using
  M1/M2, two driven wheels/two casters and command signs `-1/-1`. It remains
  non-activatable and is not consumed by runtime.

### Verified Before Hardware

- `./tools/run_host_tests.sh`: 7/7 passed, including PPM and SVD48 protocol
  reference vectors.
- Web `npm test`: 8/8 passed, including fake-LAN read/single/multiple writes,
  wrong-target rejection, ambiguous outcome and actuation blocking.
- ESP-IDF 5.4.1 build for `esp32s3` passed before the documentation alignment;
  a final rebuild is required before flash.
- JSON Schema validation of the PPM RAFA draft passed.
- Desktop and narrow/mobile register-editor layouts were visually inspected with
  no horizontal overflow; automated accessibility/Playwright remains pending.

### Safety Boundary and Remaining Work

- This vertical authorizes elevated-bench configuration only. It does not satisfy
  `GATE-SAFE`, authorize floor movement or make direct serial movement safe.
- The current motion runtime still assumes the legacy fixed motor topology and
  does not consume the generic differential strategy/profile/authority model.
- PPM-to-motion, LAN movement, Bluetooth motion, command TTL/deadman, one actuator
  coordinator, exclusive `MAINTENANCE`, typed catalog/change sets, PID float word
  order and SVD48 persistence remain pending.
- Physical PPM, RS485 read/write/readback, power-cycle persistence and stop latency
  require wheels-elevated HIL evidence.

### Rollback

- Revert firmware `0adf2e6` to remove active PPM GPIO14, then `86d832f` to remove
  provisional LAN writes. Revert web `8722bea` to remove the register API/editor.

## 2026-07-20 - Build 14 USB/LAN Validation Without SVD48

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Firmware app commit embedded by ESP-IDF: `4fa2889`; build number `14`.
- USB port: stable `/dev/serial/by-id/usb-1a86_USB_Single_Serial_5C37205098-if00`.
- Network observed: laptop `192.168.1.107/24`, ESP `192.168.1.185` on the
  configured local Wi-Fi. Credentials/tokens were not printed or logged.

### Physical Evidence

- Final ESP-IDF 5.4.1 build passed; app size `0x100aa0`, 83% of the smallest
  `0x600000` OTA slot free.
- `idf.py ... flash` wrote/hash-verified bootloader, 16 MB partition table,
  `ota_data_initial` and app, then hard-reset successfully.
- USB reported build 14, target `esp32s3`, partition `ota_0`, `OTA_STATE:VALID`,
  `SAFE_IDLE`, zero motion, PPM `MODE:PPM RX_GPIO:14 STATUS:NO_SIGNAL`, Wi-Fi
  connected and maintenance token set.
- The absent SVD48 produced `READ_REG_FAILED ... ERR:0x109` consistently over
  USB, direct LAN CLI, LAN web backend and serial web backend. No write was sent.
- LAN discovery and valid-token status/version passed; a deliberately wrong token
  returned `ERR BAD_TOKEN`.
- Firmware rejected `MOVE_VEL`, malformed/missing-confirm register commands and
  all six actuation registers `0x5300/01`, `0x5304/05`, `0x5308/09`. A following
  `VERSION` succeeded, showing the service remained healthy.
- Web backends over both transports mapped the absent-drive read to HTTP `409`.
  Backend validation rejected `0x5304` with HTTP `400` before transport.

### Not Proven

- The SVD48/controller was not reachable, so no benign physical write/readback,
  FC `0x10`, PID float order, persistence, polling coexistence or motor telemetry
  is claimed.
- No receiver pulse train was attached, so PPM frame decode and loss-after-valid
  stop behavior remain HIL pending.
- No motor/servo/movement command was executed. Floor operation remains forbidden.

## 2026-07-20 - Full Read-Only SVD48 ID 2 Inventory

- Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`
- Hardware: ESP powered from assembled platform, no USB; maintenance LAN at the
  previously discovered local address. Wheels remained elevated.
- Controller: ID 2 with both M1/M2 channels online; ID 1 absent.

### Implemented

- Added `tools/svd48_read_inventory.py`, a read-only catalog scanner that uses
  correlated maintenance-LAN requests, bounded retries, delay between reads and
  preserved raw/unsupported results.
- The tool records unsigned/signed words, hexadecimal values, both u32/float word
  orders and generates machine-readable JSON plus a complete Markdown table.
- Added a human-friendly current-state summary and updated the prior Toño
  observation document without replacing historical evidence.

### Evidence

- 187 documented groups queried; 151 successful, 36 invalid-register/read
  exceptions preserved. No write, `ENABLE` or movement command was sent.
- Identity: ID 2, software `0x0131`, hardware `0x0300`, bootloader `0x0103`,
  product `0x0101`, RS485 115200 and RS485 control source.
- Both motors: 24 pole pairs, Hall, reverse, speed mode, 100 RPM, 30 A,
  acceleration 45 RPM/s, deceleration 40 RPM/s and smoothing 100 ms.
- Speed PID for both channels reads `Kp=0.3`, `Ki=0.1`, `Kd=2.0` using the only
  plausible word order (high word first). Independent SV-Config evidence remains
  required before writing PID.
- Post-scan state remained `SAFE_IDLE`, command/speed targets zero, actual RPM
  zero and motor errors `0x00000000`.

### Findings Requiring Resolution

- SVD48 wheel diameter is 100 mm, while the current RAFA firmware reference
  radius implies 200 mm diameter.
- Maximum current is 30 A and maximum bus protection is 60.0 V; both require
  hardware/battery ceilings before modification or movement.
- Motor temperature `-22.7 C`, Hall status `103`, current `-0.1 A` and current
  position values have uncertain/sentinel semantics and must not be rendered as
  ordinary healthy measurements.
- Unsupported groups: gear teeth, controller-direct PPM, CAN active upload,
  RS232 active upload and suspect M2 Hall calibration current addresses.

## 2026-07-20 - KK16 Configuration And Hall Calibration Attempt

- Configured ID 2 for two KK16 motors using manufacturer data: 10 pole pairs,
  Hall sensors, 400 mm tire diameter, 10 RPM maximum, and 3 RPM/s acceleration
  and deceleration on both channels.
- Added `SAVE_SVD48_CONFIG drive CONFIRM`, restricted to a stopped robot and the
  SVD48 write-only FLASH-save register `0x3100`. Host tests and ESP-IDF build pass;
  build 15 was deployed by OTA and reported valid on `ota_1`.
- The save write was acknowledged. A physical controller power cycle is pending
  to independently verify persistence.
- Corrected the effective M2 Hall calibration-current address to `0x5625`; this
  controller exposes the symmetric pair `0x5624/0x5625`, while manual addresses
  `0x5605/0x5609` reject reads.
- M1 Hall auto-calibration failed at configured currents 3 A, 6 A, and 15 A.
  M1 rotated only while calibrating, stopped afterward, and retained zero raw
  motor errors. M2 was not calibrated.
- Evidence: `docs/process/evidence/KK16_SVD48_CONFIGURATION_2026-07-20.md`.
- After an M1 Hall-contact repair and complete power cycle, persistence was
  confirmed and both raw motor errors were zero. One 15 A auto-calibration retry
  per channel still ended in status `2`; both stopped cleanly with no motor error.
  Angle tables were populated and closely matched. A temporary higher maximum RPM
  calibration test remains pending explicit operator approval.
- The approved temporary 50 RPM test was completed on both channels. Each reached
  approximately 34 RPM and still ended in Hall calibration status `2`. The 10 RPM
  production limit was restored, read back, and saved; low maximum speed is no
  longer considered the primary calibration failure cause.
- The KK16 5:1 ratio is documented but not written to the controller because
  `0x2202/0x2203` are unsupported on SVD48 software `0x0131`. It remains a robot
  profile/kinematics parameter.
- Manual Hall-table editing found that reads use degrees while writes require Q15
  turn units (`degrees * 32768 / 360`). M1 was restored close to its historical
  table through individual FC06 writes. M2 rejects documented individual and
  multiple table writes on software `0x0131`, so its generated table was retained.
  Both calibration statuses remain failed; no movement validation was attempted.
