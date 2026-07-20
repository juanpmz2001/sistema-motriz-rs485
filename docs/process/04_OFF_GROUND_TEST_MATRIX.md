# Off-Ground Robot Test Matrix

Date: 2026-07-17

Overall status: `IN_PROGRESS`; host/build subset has passed, no hardware row has passed in this implementation session.

This matrix must pass before creating a floor-test plan. It does not authorize floor operation.

## Safety Setup

All `E4/E5` tests require:

- robot mechanically supported at rated mass with every driven/steered wheel clear of the floor and supports outside wheel paths;
- no person, cable, tool, loose clothing, or fragile object in wheel/steering reach;
- one operator dedicated to a physical power cutoff, not interacting with software;
- accessible physical E-stop or battery disconnect tested before powered motion;
- current-limited supply where compatible, correctly fused battery path, and fire-safe battery procedure;
- known wheel/motor maximum RPM/current reduced to the minimum useful test limits;
- valid controller/profile backup and recovery procedure;
- test owner, observer, firmware build, web commit, active profile revision, controller IDs/versions, battery voltage, and timestamp recorded;
- automatic floor contact impossible even if suspension moves;
- no Hall/encoder calibration unless the specific hazardous calibration procedure is approved.

Stop immediately on unexpected motion, wrong wheel/servo mapping, inability to stop, support movement, overheating, smoke/odor, abnormal current, repeated controller reset, or unclassified fault.

## Evidence Record Required Per Test

- Test ID and status: `PASS`, `FAIL`, `BLOCKED`, or `SKIPPED` with reason.
- Operator/session ID and date.
- Firmware build/commit and active partition/boot ID.
- Web commit/container image if used.
- Profile ID/revision/CRC and expected SVD48 profile revision.
- Transport (`USB`, `LAN`, or both), ESP IP when relevant, controller IDs/channels.
- Preconditions and deviations.
- Exact commands/API actions without secrets.
- Structured results and selected redacted logs/captures.
- Measured values with units and measuring method.
- Recovery/cleanup result.

## A. Static and Power-Off Inspection

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-STATIC-001` | Robot variant identity | Match chassis label/serial to profile inventory | One unambiguous profile ID | photos/record `E4` | `NOT_STARTED` |
| `TEST-STATIC-002` | Motor mapping | Trace every SVD48 M1/M2 cable to physical wheel | Complete drive/channel/corner map | continuity/labels `E4` | `NOT_STARTED` |
| `TEST-STATIC-003` | Servo mapping | Trace every PWM/power/ground to physical steering actuator | GPIO/resource/corner map | continuity/labels `E4` | `NOT_STARTED` |
| `TEST-STATIC-004` | Mechanical dimensions | Measure wheelbase, track, and each effective wheel radius | Values and uncertainty recorded | measurement `E4` | `NOT_STARTED` |
| `TEST-STATIC-005` | Motor metadata | Record part, pole pairs, sensor, gearbox, current/RPM ratings | Datasheet and physical label agree or discrepancy flagged | records `E0/E4` | `NOT_STARTED` |
| `TEST-STATIC-006` | Battery/voltage policy | Record chemistry, cell count, full/nominal/low voltage and BMS limits | Compiled/profile/controller thresholds are compatible | calculation/review `E0` | `NOT_STARTED` |
| `TEST-STATIC-007` | Fusing/wire/controller rating | Compare expected continuous/peak current to components | No under-rated path; unresolved risk blocks power test | review `E0/E4` | `NOT_STARTED` |
| `TEST-STATIC-008` | Physical cutoff | Operate E-stop/disconnect unpowered and verify reach | Dedicated operator can cut power immediately | observation `E4` | `NOT_STARTED` |
| `TEST-STATIC-009` | Supports and clearances | Load supports and move steering/wheels by hand | Stable through full steering/suspension range | observation `E4` | `NOT_STARTED` |
| `TEST-STATIC-010` | RS485 topology | Verify A/B/GND, termination, shielding, drive IDs | Wiring matches documented bus; no duplicate IDs | measurement `E4` | `NOT_STARTED` |

## B. Host and Build Tests

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-HOST-001` | Firmware clean build | Build with supported ESP-IDF/toolchain | Success; sizes within OTA slots | `E2` | `PASS 2026-07-19` |
| `TEST-HOST-002` | SVD48 protocol suite | Run CRC/frame/parser/codec/catalog tests | All golden and malformed cases pass | `E1` | `IN_PROGRESS` |
| `TEST-HOST-003` | Profile validation suite | Run every valid/invalid/migration/corruption fixture | Stable exact results | `E1` | `NOT_STARTED` |
| `TEST-HOST-004` | Kinematic suite | Run all strategy/sign/saturation/NaN fixtures | Targets match independently calculated fixtures | `E1` | `IN_PROGRESS` (differential passes) |
| `TEST-HOST-005` | Safety state suite | Run all legal/illegal transition and lease/fault cases | No movement accepted while inhibited | `E1` | `IN_PROGRESS` |
| `TEST-HOST-006` | Transport shared fixtures | Run serial/LAN/version/auth/correlation/size fixtures | C/Python/Node agree | `E1/E2` | `IN_PROGRESS` |
| `TEST-HOST-007` | Web unit/API suite | Run schemas, auth, policy, profiles, mocked transports | All pass with stable error contract | `E2` | `NOT_STARTED` |
| `TEST-HOST-008` | Web Playwright suite | Run desktop/mobile/accessibility/config flows | No overlap; unsafe controls gated | `E2` | `NOT_STARTED` |
| `TEST-HOST-009` | Container least privilege | Run with explicit device only and no privileged mode | Serial mode works; other `/dev` access absent | `E2` | `NOT_STARTED` |
| `TEST-HOST-010` | Secret scan/log redaction | Exercise auth failures and inspect output | No token/password/key material present | `E2` | `NOT_STARTED` |
| `TEST-HOST-011` | Serial framing boundaries | Send `N-1/N/N+1`, overlong line plus command-like suffix, bad delimiters | Rejected input is drained; suffix never executes | `E1/E2` | `IN_PROGRESS` |
| `TEST-HOST-012` | Request idempotency | Retry same ID/body and same ID/different body | Cached result vs explicit ID-reuse error; no duplicate mutation | `E1/E2` | `NOT_STARTED` |
| `TEST-HOST-013` | OTA action validation order | Submit unknown/malformed/retried announce actions | No NVS mutation; valid escaped structured error | `E1/E2` | `NOT_STARTED` |

Incremental evidence from firmware build 11:

- `./tools/run_host_tests.sh`: `PASS` (`E1`) for known `0x03/0x06` frames, bounded `0x10` requests (1, 2, 8, 32, 123 words), CRC, `0x83/0x86/0x90`, truncation, wrong slave/function, serial 159/160-byte boundary and dangerous suffix drain, and `ERR` code extraction.
- `idf.py build` with ESP-IDF 5.4.1: `PASS` (`E2`), target `esp32s3`, binary `0xfe480`, smallest app slot `0x600000`, 83% free.
- `TEST-HOST-002/006/011` remain `IN_PROGRESS` because typed codecs/catalog, cross-language fixtures, malformed argument/completion framing, and ESP transport tests are not complete.

Incremental evidence from firmware build 12 (2026-07-19):

- `./tools/run_host_tests.sh`: `PASS`, 4/4 CTest tests (`E1`) for the host harness/fakes, pure operational states, SVD48/framing contracts and independent Python vectors.
- The same 4/4 tests pass with ASan/UBSan; LeakSanitizer was disabled because the supervised environment restricts `ptrace`.
- State fixtures cover boot-to-disarmed, arm blockers, stale fault latch, explicit ACK, lease-expiry recovery policy, maintenance/write gates and OTA/maintenance exclusion. Runtime movement APIs are not connected yet, so `TEST-HOST-005` remains `IN_PROGRESS`.
- FC `0x10` fixtures validate request limits and ACK slave/function/start/count/length/CRC. The internal driver transaction makes one attempt and remains inaccessible from USB/LAN; restrained-controller evidence is pending.
- ESP-IDF 5.4.1 `idf.py build`: `PASS` (`E2`), target `esp32s3`, build 12 binary `0xfe600`, smallest app slot `0x600000`, 83% free.

Incremental evidence from firmware build 13 (2026-07-19):

- `./tools/run_host_tests.sh`: `PASS`, 6/6 CTest tests (`E1`). Authority has 14 cases and differential kinematics has 12 cases, including regressions found by independent review for dead-man invalidation, retired/full stream history, temporal epoch barriers, numeric underflow and final RPM clamping.
- `BOTFARMS_HOST_TEST_SANITIZERS=ON ./tools/run_host_tests.sh`: `PASS`, 6/6 with ASan/UBSan; LeakSanitizer remains disabled in this supervised environment.
- ESP-IDF 5.4.1 `idf.py build`: `PASS` (`E2`), target `esp32s3`, build 13 binary `0xfe6a0`, smallest app slot `0x600000`, 83% free. `robot_state_service`, `command_authority`, `robot_kinematics` and `control_lan` compiled in the graph.
- No `E3..E5` row was executed. The new state/authority/kinematics/control-LAN path is not started by `main` and does not authorize movement.

## C. ESP Without SVD48/Motors

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-ESP-001` | Factory/provision boot | Flash and boot with expected partition table | Correct target/flash/partition/build reported | `E3` | `NOT_STARTED` |
| `TEST-ESP-002` | Missing controller behavior | Boot with RS485 disconnected | Firmware remains responsive; state is disarmed/unknown, no reboot loop | `E3` | `NOT_STARTED` |
| `TEST-ESP-003` | USB capabilities | Query version/capabilities/profile/safety | Typed coherent result and boot ID | `E3` | `NOT_STARTED` |
| `TEST-ESP-004` | Wi-Fi reconnect | Cycle access point/connectivity | Control tasks remain responsive; bounded backoff and recovery | `E3` | `NOT_STARTED` |
| `TEST-ESP-005` | LAN discovery/token | Discover with valid/invalid/missing local token | Only valid token receives protected data; counters/logs redact secret | `E3` | `NOT_STARTED` |
| `TEST-ESP-006` | Production replay security | Exercise future HMAC/nonce/timestamp policy | Rejected with stable code; no action | `E3` | `DEFERRED` |
| `TEST-ESP-007` | Payload boundaries | Send exact max, max+1, truncated, malformed chunks | Deterministic rejection; service remains healthy | `E3` | `NOT_STARTED` |
| `TEST-ESP-008` | Correlation | Delay/duplicate/reorder/spoof responses in harness | Wrong/stale response never satisfies request | `E3` | `NOT_STARTED` |
| `TEST-ESP-009` | Profile persistence interruption | Interrupt staged profile writes at controlled points | Active/known-good remain recoverable | `E3` | `NOT_STARTED` |
| `TEST-ESP-010` | Corrupt profile boot | Inject bad JSON/schema/hash/version/length fixture | Firmware uses documented fallback and stays inhibited | `E3` | `NOT_STARTED` |
| `TEST-ESP-011` | Maintenance job expiry | Expire/disconnect a job at controlled phases | Job classifies deterministically; robot remains disarmed/faulted; staged data not activated | `E3` | `NOT_STARTED` |
| `TEST-ESP-012` | OTA/config exclusion | Attempt OTA and configuration jobs concurrently | One exclusive operation runs; other gets explicit conflict | `E3` | `NOT_STARTED` |
| `TEST-ESP-013` | Lost-response retry | Drop first LAN response and retry identical request | Operation executes once and cached result returns | `E3` | `NOT_STARTED` |
| `TEST-ESP-014` | LAN result truthfulness | Send blocked, usage-error, missing-drive, and successful commands | UDP `status/detail/lines` agree; backend rejects every `status:err` | `E3` | `NOT_STARTED` |
| `TEST-ESP-015` | USB overlong recovery | Send 160+ bytes ending in a movement-like suffix, delimiter, then `PING` | One `ERR LINE_TOO_LONG`; suffix is not executed; next `PING` succeeds | `E3` | `NOT_STARTED` |
| `TEST-ESP-016` | Operational state boot/reconnect | Boot and cycle USB/Wi-Fi/backend without arm request | Always reaches/remains `DISARMED`; never auto-arms | `E3` | `NOT_STARTED` |
| `TEST-ESP-017` | Maintenance job reconnect | Start/expire/reconnect to a no-write job by ID | Exclusive job result is deterministic; remains disarmed | `E3` | `NOT_STARTED` |
| `TEST-ESP-018` | Staged robot JSON boot | Stage a safe alternate board/I/O fixture and reboot | Resources initialize while inhibited; active/known-good JSON status is explicit | `E3` | `NOT_STARTED` |
| `TEST-ESP-019` | `control_lan` sequence/TTL | Send valid, duplicate, regressive, new-stream and expired command packets | Only valid fresh sequence reaches LAN mailbox; new stream stops/changes epoch | `E3` | `NOT_STARTED` |

## D. SVD48 Read-Only Bus Tests

Motor power should be isolated where controller communication permits; otherwise wheels remain removed/free and movement commands are forbidden.

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-SVD-001` | Controller inventory | Read IDs/product/software/hardware/boot versions | Stable identity per physical drive | `E4` | `NOT_STARTED` |
| `TEST-SVD-002` | Duplicate/wrong ID | Configure harness/mock duplicate or query absent ID | Structured exception/timeout; no bus lockup | `E4` | `NOT_STARTED` |
| `TEST-SVD-003` | Full verified parameter read | Read every catalog `OBSERVED/IMPLEMENTED` field | Values typed, ranged, timestamped, no writes | `E4` | `NOT_STARTED` |
| `TEST-SVD-004` | Manual-only read survey | Read safe manual fields in approved ranges | Value or preserved exception stored with controller version | `E4` | `NOT_STARTED` |
| `TEST-SVD-005` | USB/LAN parity | Read identical groups over both transports | Same typed values/result codes within timestamp tolerance | `E4` | `NOT_STARTED` |
| `TEST-SVD-006` | Float fixture | Compare typed float read with SV-Config capture | Exact word order/value confirmed | `E4` | `NOT_STARTED` |
| `TEST-SVD-007` | Exception fidelity | Issue approved invalid read/write to spare/bench drive | Function and exception code preserved end to end | `E4` | `NOT_STARTED` |
| `TEST-SVD-008` | CRC/noise handling | Inject corrupt/truncated/wrong-slave responses in harness | Retry/timeout metrics and no wrong data acceptance | `E2/E4` | `NOT_STARTED` |
| `TEST-SVD-009` | One drive absent | Disconnect one controller during polling | Other controller remains fresh; absent drive backs off; safety state explicit | `E4/E5` | `NOT_STARTED` |
| `TEST-SVD-010` | Bus saturation | Run maximum approved telemetry/config reads | Stop class remains bounded; no watchdog/reset | `E4/E5` | `NOT_STARTED` |
| `TEST-SVD-011` | Position scale | Rotate known motor/wheel turns manually or at minimal speed | Counts/revolution and sign recorded per channel | `E4/E5` | `NOT_STARTED` |
| `TEST-SVD-012` | Error decode baseline | Read zero/nonzero error/status fields | Raw and decoded representation agree | `E4` | `NOT_STARTED` |

## E. Profile and Drift Tests

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-PROF-001` | Canonical JSON round trip | Stage/read profile and compare canonical hash/revision | ESP returns the exact active JSON identity; runtime snapshot matches it | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-002` | Actual inventory mismatch | Stage a valid JSON whose controller IDs/channels do not match attached hardware | Compatibility check blocks activation; no automatic SVD write | `E3/E4` | `NOT_STARTED` |
| `TEST-PROF-003` | Duplicate mapping | Stage two logical motors on same drive/channel | Field-level rejection | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-004` | GPIO/resource conflict | Stage duplicate/invalid servo resources | Field-level rejection | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-005` | Geometry/radius limits | Stage zero, negative, NaN, Inf, extreme values | Rejected before persistence/activation | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-006` | Active motion activation attempt | Attempt profile activation while armed/commanded/nonzero RPM | Blocked; no output change | `E5` | `NOT_STARTED` |
| `TEST-PROF-007` | Expected/live match | Compare profile against matching drives | `MATCH` with per-field evidence | `E4` | `NOT_STARTED` |
| `TEST-PROF-008` | Noncritical drift | Alter approved bench field then compare | `DRIFT` identifies old/expected/live and severity | `E4` | `NOT_STARTED` |
| `TEST-PROF-009` | Critical drift | Present wrong sensor/pole/topology fingerprint | Movement remains inhibited; no automatic overwrite | `E4/E5` | `NOT_STARTED` |
| `TEST-PROF-010` | Rollback | Activate staged fixture, fail self-check, restore known-good | Explicit rollback and retained audit | `E3/E5` | `NOT_STARTED` |
| `TEST-PROF-011` | Board pin validation | Stage reserved, input-only, duplicate UART/RC/servo and LEDC conflicts | Exact field rejection; active profile unchanged | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-012` | Invalid staged pin boot | Inject resource-init/self-test failure after staging | No movement; next boot recovers known-good/factory with reason | `E3` | `NOT_STARTED` |
| `TEST-PROF-013` | Recovery path preservation | Attempt profile that removes every recovery transport | Validation rejects it before persistence/activation | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-014` | Draft activation | Stage `activation_allowed:false` topology fixture | Schema validation may pass; activation is rejected explicitly | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-015` | Variable controller topology | Validate 1xSVD48/M1+M2, 2xSVD48/M1+M2 and 4xSVD48 fixtures | All use the same schema and mapping rules within capabilities | `E1` | `NOT_STARTED` |
| `TEST-PROF-016` | Capability overflow | Exceed advertised controllers/motors/servos without changing schema | Exact capacity error; no truncation or partial snapshot | `E1/E3` | `NOT_STARTED` |
| `TEST-PROF-017` | Chunked profile upload | Drop, reorder, duplicate and corrupt chunks | No staged commit until complete hash matches; retry is idempotent | `E2/E3` | `NOT_STARTED` |

## F. Guarded SVD48 Write Tests

Use a backed-up controller, minimum-risk verified parameter, and exclusive
firmware maintenance operation. PID begins only after the float word order is
captured; do not begin with bus address/baud, voltage/current extremes or
calibration.

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-WRITE-001` | Unguarded write | Attempt typed/raw write outside the maintenance job path | Rejected; no frame emitted | `E4` | `NOT_STARTED` |
| `TEST-WRITE-002` | Moving write | Request a configuration job with nonzero command or RPM | Job/write blocked and stop policy visible | `E5` | `NOT_STARTED` |
| `TEST-WRITE-003` | Stale/offline/faulted write | Remove freshness/controller or inject fault | Rejected; no write frame | `E4/E5` | `NOT_STARTED` |
| `TEST-WRITE-004` | Range/access/confidence | Write RO, manual-only, suspect, unknown, and out-of-range fields | Each rejected with exact code | `E2/E4` | `NOT_STARTED` |
| `TEST-WRITE-005` | Dry-run change set | Validate one approved field | Old/new/register/type/range/persistence shown, no write | `E4` | `NOT_STARTED` |
| `TEST-WRITE-006` | Successful apply/readback | Confirm approved benign change | Exact frame accepted; fresh readback equals request | `E4` | `NOT_STARTED` |
| `TEST-WRITE-007` | Readback mismatch | Harness/controller returns different value | Failure, old/new/live retained, robot inhibited | `E2/E4` | `NOT_STARTED` |
| `TEST-WRITE-008` | Partial multi-field failure | Fail field N after prior writes | Per-field result, emergency-safe state, rollback change set | `E2/E4` | `NOT_STARTED` |
| `TEST-WRITE-009` | Duplicate request | Retry same request/change-set ID | Idempotent result; no unintended second apply | `E3/E4` | `NOT_STARTED` |
| `TEST-WRITE-010` | Job expiry mid-operation | Expire deadline at controlled phase | Defined completion/classification; remains disarmed or faulted, never armed | `E3/E4` | `NOT_STARTED` |
| `TEST-WRITE-011` | Save and power cycle | Apply approved value, save, power cycle, read | Persistence exactly matches documented policy | `E4` | `NOT_STARTED` |
| `TEST-WRITE-012` | Power interruption | Cut power at approved save stages on spare drive | Recovery outcome recorded; no false success | `E4` | `NOT_STARTED` |
| `TEST-WRITE-013` | Revert known value | Apply captured old value and save/readback | Original value restored and audited | `E4` | `NOT_STARTED` |
| `TEST-WRITE-014` | Lost response after TX | Drop write response while fixture independently applies/does not apply value | No blind retry; `OUTCOME_UNKNOWN` resolved by readback or recovery-required | `E2/E4` | `NOT_STARTED` |
| `TEST-WRITE-015` | Client/backend disconnect during apply | Disconnect after job accepted, then reconnect by ID | ESP-owned job completes/classifies once; no duplicate mutation | `E3/E4` | `NOT_STARTED` |

## G. Elevated Motor and Steering Mapping

Begin at the smallest command that produces observable movement and enforce low profile ceilings.

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-MAP-001` | Individual wheel identity | Command one enabled logical motor at a time | Only expected physical wheel moves | `E5` | `NOT_STARTED` |
| `TEST-MAP-002` | Individual direction | Minimal positive/negative command per wheel | Physical forward convention and telemetry sign match profile | `E5` | `NOT_STARTED` |
| `TEST-MAP-003` | Individual stop | Stop each moving wheel | Command zero/status/RPM converge within bound | `E5` | `NOT_STARTED` |
| `TEST-MAP-004` | Servo identity | Move each servo by small positive/negative angle | Only expected corner moves in expected direction | `E5` | `NOT_STARTED` |
| `TEST-MAP-005` | Servo center/trim | Command neutral and measure wheel angle | Within accepted alignment tolerance | `E5` | `NOT_STARTED` |
| `TEST-MAP-006` | Servo limits | Approach software min/max in steps | Never contacts mechanical stop or exceeds pulse ceiling | `E5` | `NOT_STARTED` |
| `TEST-MAP-007` | Differential 1 motor per side | Apply fixture velocities to one SVD48 M1/M2 | Left/right RPM match formula, radius, gear ratio and signs | `E5` | `NOT_STARTED` |
| `TEST-MAP-008` | Differential N motors per side | Apply same body command to 2-controller and 4-controller fixtures | Every mapped motor receives the side target adjusted by its own radius/ratio/sign | `E1/E5` | `NOT_STARTED` |
| `TEST-MAP-009` | Ackermann linkage/per-wheel | Apply low-speed left/right radius fixtures | Inner/outer geometry and driven-wheel speeds match poses/layout | `E1/E5` | `NOT_STARTED` |
| `TEST-MAP-010` | Independent steer and crab | Straight, lateral, diagonal and yaw fixtures | Module angles/RPM match local vectors without snapping | `E1/E5` | `NOT_STARTED` |
| `TEST-MAP-011` | Unsupported command | Send lateral/pure-rotation command to unsupported model | Rejected; no actuator output changes | `E5` | `NOT_STARTED` |
| `TEST-MAP-012` | Saturation | Request above profile max | Uniform documented scaling; no field exceeds ceiling | `E5` | `NOT_STARTED` |
| `TEST-MAP-013` | Zero transition | Command motion then zero | Stable steering zero policy and bounded motor stop | `E5` | `NOT_STARTED` |
| `TEST-MAP-014` | Reverse optimization boundary | Sweep near steering reversal threshold | Hysteresis prevents rapid sign/angle oscillation | `E5` | `NOT_STARTED` |

## H. RC, Authority, and Stop Safety

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-SAFE-001` | Receiver absent at boot | Boot profiles with RC optional and required | Optional permits lower sources after explicit arm; required remains inhibited with exact reason | `E5` | `NOT_STARTED` |
| `TEST-SAFE-002` | Invalid RC mode/frame | Wrong baud/inversion/protocol or bad checksum | No arm/motion; diagnostic distinguishes invalid | `E5` | `NOT_STARTED` |
| `TEST-SAFE-003` | Dead-man required | Move stick without dead-man | No motion accepted | `E5` | `NOT_STARTED` |
| `TEST-SAFE-004` | Arm/disarm sequence | Execute exact valid sequence | Authority and state transition logged | `E5` | `NOT_STARTED` |
| `TEST-SAFE-005` | RC loss during motion | Turn off/unplug transmitter/receiver safely | Latched inhibit and bounded stop | `E5` | `NOT_STARTED` |
| `TEST-SAFE-006` | Dead-man release during motion | Release switch | Latched/defined stop within bound | `E5` | `NOT_STARTED` |
| `TEST-SAFE-007` | Command lease expiry | Stop valid command heartbeat | Stop and authority release within deadline | `E5` | `NOT_STARTED` |
| `TEST-SAFE-008` | All sources simultaneous | RC, LAN and BT publish nonzero conflicting commands | Only RC epoch reaches actuators; all source freshness remains visible | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-009` | Universal stop | Invoke stop from USB and LAN in every state | Idempotent, highest priority, truthful result | `E5` | `NOT_STARTED` |
| `TEST-SAFE-010` | E-stop | Operate physical E-stop during minimal motion | Torque/power removed according to design; restart remains disarmed | `E5` | `NOT_STARTED` |
| `TEST-SAFE-011` | Controller fault | Inject approved fault or harness error | Fault latched, all motion inhibited, raw/decoded cause visible | `E5` | `NOT_STARTED` |
| `TEST-SAFE-012` | Stale telemetry | Pause polling/responses while commanded | Defined conservative stop/inhibit | `E5` | `NOT_STARTED` |
| `TEST-SAFE-013` | One controller offline | Disconnect one drive during minimal motion if approved | Emergency stop all and fault latch | `E5` | `NOT_STARTED` |
| `TEST-SAFE-014` | Polling bus timeout stop latency | Force worst-case retries then request stop | Latency under specified bound; no starvation | `E5` | `NOT_STARTED` |
| `TEST-SAFE-015` | Partial apply failure | Fail second/third actuator update | Stop all, `PARTIAL_APPLY` fault, no silent continuation | `E5` | `NOT_STARTED` |
| `TEST-SAFE-016` | Re-arm after fault | Clear underlying issue and try direct movement | Requires explicit fault acknowledgment/disarmed arm sequence | `E5` | `NOT_STARTED` |
| `TEST-SAFE-017` | Residual setpoint then enable | Set/capture nonzero target, stop, then invoke enable/start path without a new motion target | No transient movement; safe/new zero target is written before `START` | `E5` | `NOT_STARTED` |
| `TEST-SAFE-018` | First drive absent, second present | Disconnect/loss-inject ID 1 while ID 2 is reachable; request stop and separately test enable failure | ID 2 stop is not delayed beyond bound; enable cannot leave partial active state | `E5` | `NOT_STARTED` |
| `TEST-SAFE-019` | Poll/config interleaving | Run polling and grouped config reads, then request stop | Emergency request preempts/yields within bound; responses remain correlated | `E5` | `NOT_STARTED` |
| `TEST-SAFE-020` | LAN preempts Bluetooth | Move minimally from BT, then send fresh LAN command | STOP first; old BT command invalidated; only post-switch LAN command applies | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-021` | RC preempts LAN/BT | Move minimally from LAN/BT, then produce a valid RC frame | RC takes precedence within bound through stop/new epoch | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-022` | RC neutral blocks lower sources | Keep LAN/BT nonzero while RC is fresh-neutral or dead-man released | Output remains zero; LAN/BT cannot move | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-023` | No stale fallback after RC loss | Keep an older LAN/BT command, then lose active RC while moving | STOP + fault; no lower-source command applies until ACK/re-arm/fresh epoch | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-024` | LAN TTL loss | Stop LAN heartbeat while moving and BT has an older command | STOP + fault within bound; no fallback to BT | `E1/E5` | `NOT_STARTED` |
| `TEST-SAFE-025` | USB bypass removal | Invoke every legacy USB movement command in conflicting states | Each enters the arbiter/gate or is explicitly disabled; none writes directly | `E1/E3/E5` | `NOT_STARTED` |

## I. Backend, LAN, and Deferred Frontend Tests

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-WEB-001` | Production unauthenticated access | Call future production REST/WS without identity | Rejected by production trust layer | `E2/E3` | `DEFERRED` |
| `TEST-WEB-002` | Production role policy | Use future read-only/operator/admin fixtures | Each sees only negotiated actions | `E2` | `DEFERRED` |
| `TEST-WEB-003` | Production CSRF/origin | Cross-origin state-changing request/WS | Rejected | `E2` | `DEFERRED` |
| `TEST-WEB-004` | Generic command defense | Submit blocked motion/raw/config commands in LAN mode | Backend and firmware both reject with matching policy code | `E2/E3` | `NOT_STARTED` |
| `TEST-WEB-005` | Truthful device error | Cause SVD timeout/exception | HTTP/UI show failure, never success with hidden `ERR` | `E2/E4` | `NOT_STARTED` |
| `TEST-WEB-006` | Two browser telemetry | Subscribe from two clients | One backend poll stream; both receive correlated typed samples | `E2/E4` | `NOT_STARTED` |
| `TEST-WEB-007` | Concurrent maintenance jobs | Two clients apply different change sets | One firmware job runs; other gets explicit busy/conflict | `E2/E3` | `NOT_STARTED` |
| `TEST-WEB-008` | Client disconnect during job | Drop backend/network after acceptance, reconnect by job ID | Job completes/classifies once; ESP remains disarmed; no duplicate write | `E2/E4` | `NOT_STARTED` |
| `TEST-WEB-009` | Transport degradation | Drop LAN/USB after connect | State becomes degraded/disconnected with `lastSeenAt` | `E2/E3` | `NOT_STARTED` |
| `TEST-WEB-010` | Profile revision conflict | Edit stale revision from second client | `409`; no lost update | `E2` | `NOT_STARTED` |
| `TEST-WEB-011` | Unknown/stale values | Supply null, malformed, and stale telemetry | UI never displays false zero/healthy value | `E2` | `NOT_STARTED` |
| `TEST-WEB-012` | Frontend responsive/accessibility | Exercise longest labels/errors desktop/mobile/keyboard | No overlap/truncation; focus/status/labels usable | `E2` | `DEFERRED` |
| `TEST-WEB-013` | Result history export | Apply/read/reject operations and export | Redacted request/build/profile/controller/result history | `E2/E4` | `NOT_STARTED` |

## Toño Motherboard Campaign Order

Use this sequence when the ESP32 is attached to the motherboard, SVD48 drives, and servos. Do not skip directly to movement tests.

1. Power off: execute `TEST-STATIC-001..010`; label drive IDs/channels, wheel corners, servo GPIOs, battery limits, supports, and physical cutoff. Resolve every mismatch before motor power.
2. ESP only or controller motor power isolated: flash build 11 or newer; run `VERSION`, `PLATFORM_STATUS`, `SAFETY_STATUS`, `WIFI_STATUS`, `MAINT_LAN_STATUS`, `TEST-ESP-014`, and `TEST-ESP-015`.
3. RS485 read-only: run `TRACE ON`, controller inventory, `POLL_ONCE`, then read every motor ID returned by the active JSON/capabilities (two for the first 2WD profile), and `TRACE OFF`. Complete `TEST-SVD-001..005/009/012`; do not infer zero from stale/unsupported fields.
4. Exception fidelity: on a backed-up bench controller and with movement forbidden, execute only the approved invalid read/write fixture for `TEST-SVD-007`; verify `COMM_ERR`, `EXC_FUNC`, `EXC_CODE`, age, USB/LAN/HTTP agreement, and recovery.
5. Servos with wheels clear: center first, then use small bounded angle steps for `TEST-MAP-004..006`; stop on wrong corner/direction, binding, current anomaly, or mechanical-limit contact.
6. Traction at the minimum useful RPM: complete wheel identity/direction/stop (`TEST-MAP-001..003`) one logical motor at a time, with a dedicated operator at cutoff.
7. Residual and partial failure safety: execute `TEST-SAFE-015/017/018` with an approved fault-injection method. Verify frame order is target-before-`START`, no residual-target motion occurs, and every reachable drive receives stop after a partial failure.
8. Only after the prior rows pass: execute differential fixtures, simultaneous `RC > LAN > BT` preemption/loss tests, bus timeout/stop latency, Wi-Fi coexistence and soak tests. Record numeric latency/current/RPM/temperature evidence; this still does not authorize a floor test.

## J. Wi-Fi and OTA Coexistence

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-OTA-001` | Wi-Fi reconnect under RC/safety load | Cycle AP during telemetry/minimal allowed operation | No control starvation; state/log bounded | `E5` | `NOT_STARTED` |
| `TEST-OTA-002` | OTA check while disarmed | Check manifest | Read-only operation; no motion/state interference | `E3/E5` | `NOT_STARTED` |
| `TEST-OTA-003` | OTA install while armed/moving | Request install | Rejected before download/install | `E5` | `NOT_STARTED` |
| `TEST-OTA-004` | Motion during OTA lease | Attempt arm/motion after OTA begins | Rejected by shared authority state | `E5` | `NOT_STARTED` |
| `TEST-OTA-005` | Wi-Fi loss during download | Drop AP/server | Safe failure, inactive partition not selected, robot remains disarmed | `E3/E5` | `NOT_STARTED` |
| `TEST-OTA-006` | Power loss per OTA phase | Controlled test on bench | Boot/rollback outcome matches documented state | `E3` | `NOT_STARTED` |
| `TEST-OTA-007` | New image profile compatibility | Boot firmware with older/newer profile schema | Migration/fallback; no unsafe activation | `E3/E5` | `NOT_STARTED` |
| `TEST-OTA-008` | Signed artifact/manifest | Tamper binary/manifest/signature | Rejected before boot selection | `E2/E3` | `NOT_STARTED` |

## K. Duration and Resource Tests

| ID | Test | Action | Expected | Evidence | Status |
| --- | --- | --- | --- | --- | --- |
| `TEST-SOAK-001` | 8-hour disarmed telemetry | Run USB/LAN telemetry and reconnect cycles | No leak, watchdog, queue growth, or stale-connected lie | `E3/E4` | `NOT_STARTED` |
| `TEST-SOAK-002` | Elevated low-speed cycles | Repeated short move/stop under approved duty cycle | Stop bound stable; temperatures/current within limits | `E5` | `NOT_STARTED` |
| `TEST-SOAK-003` | Multi-client/UI duration | Two clients with telemetry/config reads | Bounded CPU/memory/queue/backpressure | `E2/E4` | `NOT_STARTED` |
| `TEST-SOAK-004` | Fault/recovery cycles | Repeat selected disconnect/fault/recovery cases | No automatic unsafe re-arm or state corruption | `E5` | `NOT_STARTED` |
| `TEST-SOAK-005` | Reboot/power cycles | Repeat cold/warm boots with valid profile | Deterministic disarmed startup and profile/controller state | `E3/E5` | `NOT_STARTED` |

## Required Measurements Before Floor-Test Planning

- Maximum observed stop-request-to-zero-command-frame latency.
- Maximum observed stop-request-to-zero-measured-RPM latency per robot/load-free wheel.
- RC loss/dead-man/lease expiry detection latency.
- RS485 transaction and emergency queue latency with one drive absent.
- Telemetry age/jitter under web clients, Wi-Fi reconnect, config reads, and OTA checks.
- Wheel RPM/current/temperature at each elevated test point.
- Servo pulse/command angle versus measured wheel angle and mechanical clearance.
- Position counts per motor and wheel revolution.
- Battery bus voltage behavior during acceleration/deceleration and regenerated voltage, within restrained test limits.
- ESP heap/task stack/watchdog metrics during soak.

## Exit Criteria

A floor-test plan may be written only when:

- every mandatory `TEST-STATIC`, `TEST-HOST`, `TEST-ESP`, `TEST-SVD`, `TEST-PROF`, `TEST-MAP`, `TEST-SAFE`, `TEST-WEB`, and `TEST-OTA` MVP row is `PASS` or explicitly waived by named safety owner with rationale;
- no unresolved critical/high finding affects stop, authority, mapping, electrical limits, authentication, or truthful result reporting;
- active/known-good robot and SVD48 profiles are backed up and reproduce after power cycle;
- stop and fault latencies have numeric acceptance thresholds and pass worst-case tests;
- physical E-stop/power cutoff is functional and restart is disarmed;
- rollback for firmware and profiles has been rehearsed;
- a separate controlled-area floor test defines speed/current limits, perimeter, spotters, tether/power cutoff, test sequence, and abort criteria.
