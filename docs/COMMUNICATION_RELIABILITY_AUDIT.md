# Communication reliability audit

**Status:** Phase-0 baseline, 2026-08-30. This is an as-built audit of
firmware `main` at `c8326f1` and Engineering Console `main` at `749844c`.
It is source/test evidence, not physical qualification.

## Contract vocabulary

| Term | Meaning |
| --- | --- |
| **raw attempt** | One protocol transaction or decoded frame, including the literal outcome. |
| **valid sample** | A raw attempt that passed its protocol framing/integrity checks. It may validly report a device fault. |
| **last-known-good (LKG)** | Last valid value plus its original acquisition timestamp. A failure never refreshes either. |
| **freshness** | LKG age measured against a source-owned deadline, independent of the last attempt. |
| **quality** | Recent transport evidence: failures, CRC errors, sequence gaps, streaks. |
| **effective health** | Stable operational state derived from valid fault, LKG age and source-specific quality policy. |
| **fault** | A valid physical/controller/safety fault, never a synonym for timeout or corrupt frame. |

Useful common presentation states are `UNKNOWN`, `HEALTHY`, `SUSPECT`,
`DEGRADED`, `STALE`, `OFFLINE`, and `FAULT`. They do not require one
universal driver implementation or one global threshold.

## Reliability matrix

| Vertical | Transport / expected rate | Integrity and structure today | LKG / freshness / retry today | Health / safety today | Current weakness | Recommended bounded change |
| --- | --- | --- | --- | --- | --- |
| **PPM (Rafa)** | GPIO14 rising-edge ISR. PPM motion and safety tasks run every 20 ms; receiver stale timeout 300 ms, generic RC-loss timeout 150 ms. | No CRC. `ppm_decoder_model` accepts `working_channel_count >= min_frame_channels`, then clamps to `channel_count`; it uses only one global pulse envelope. **Rafa startup currently supplies 10 channels / 4 minimum**, although the observed receiver contract is 8. | Atomic published channels and `last_valid_frame_us` preserve the last accepted frame. Counters exist for invalid pulse, incomplete and overflow, but not rejected frame or valid-frame reason. | `robot_safety_rc_lan_interlock_model` treats one published CH5 frame as an immediate authority transition and increments priority epoch, which revokes LAN. Genuine stale still uses existing safe behavior. | 9/10-pulse interference can become a truncated valid frame and alter CH5/authority. | Make exact expected count profile-owned; reject too-few and extra frames without changing channels, sequence, CH5 or epoch. Add profile-owned channel plausibility, raw counters and confirmed source transitions. Preserve stale deadline and `WAIT_NEUTRAL`. |
| **i-BUS** | UART 115200, 20 ms receive wait; default stale timeout 100 ms. Not selected for Rafa. | Fixed 32-byte frame, `0x20 0x40` header, 16-bit subtractive checksum. Bad header/checksum is rejected before channel update. | Last valid channels/timestamp retained; header/checksum counters exist. No explicit invalid/recovery streak. No frame retry is appropriate. | `signal_valid` is age of last valid frame. A bad checksum does not make receiver immediately offline. | Backend/UI lacks a raw-checksum quality projection and can conflate receiver state with stale/fault. | Retain rejection and LKG. Add raw invalid evidence and source-specific quality/recovery policy; use existing age deadline for stale/loss. |
| **SBUS** | UART configuration supports 100000, even parity, 8N2 and inversion modes. Not selected for Rafa. | **No SBUS parser exists.** The receive task still searches i-BUS 32-byte header/checksum. SBUS 25-byte packing, end byte, lost-frame and failsafe bits are not decoded. | No valid SBUS LKG contract. | No supported SBUS safety behavior can be claimed. | Mode names imply a capability that is not implemented. | Report unsupported or add an independent SBUS parser later; never apply i-BUS/PPM assumptions to SBUS. |
| **Control LAN** | Authenticated UDP `:32322`; browser target is 10 Hz while armed; profile TTL is 300 ms. | Bounded JSON/schema, token, stream identity hash, monotonic sequence and deadman. | Command-authority retains last valid command until TTL. UDP is intentionally unretried. | TTL expiry, explicit STOP and valid RC takeover retain local immediate authority. | Only seen/accepted/rejected and last text are exported. Gaps are accepted but invisible; duplicate/order/schema/auth classes and last-valid age are unavailable. | Add quality counters and last-valid age. Record sequence gaps but do not STOP because of one. Do not weaken TTL, STOP or ordering. |
| **Maintenance LAN (firmware)** | Authenticated UDP `:32321`, one request/response, listener timeout 1 s. | JSON parse/type/token validation; request ID echoed; serial LAN-safe policy. No application CRC. | No request-id deduplication: a duplicated request can execute command side effects again. Aggregate packet counts and last action only. | No persistent host connection is inferred from one missing request; action gates remain below gateway. | Request ID is correlation, not exactly-once. Failure taxonomy is sparse. | Do not auto-retry writes/actions from host until dedupe exists. Export raw request outcome/detail counters. |
| **Maintenance LAN (Console adapter)** | Serialized synchronous UDP transactions; polling target 5 Hz. Discovery sends 3 broadcasts. | Response type/request-id/status checked. | No retries: a socket timeout raises `FirmwareError`. Connection stays selected but poll emits no frame. | No direct motor safety effect; STOP has its explicit separate path. | One safe read timeout aborts the whole telemetry frame and there is no host LKG/quality output. | Retry only classified read-only/idempotent calls with bounded backoff. Preserve every attempt. Never retry side effects without firmware dedupe. |
| **Wi-Fi station** | ESP-IDF STA events plus reconnect supervisor/backoff. | ESP-IDF association/IP events; integrity belongs to upper UDP/application protocols. | State/retry/disconnect reason/last error persist. It represents link state, not a peer response. | Link loss prevents LAN traffic; Control LAN TTL remains motion deadline. | UI can confuse association, discovery visibility and active Maintenance peer liveness. | Keep it link-layer truth; derive peer health from authenticated unicast response age separately. |
| **RS485 transport** | Serialized UART half-duplex; Rafa profile: 115200, 100 ms response timeout, 30 ms poll, 1000 ms stale, 2 retries. | Bus lock and raw results: timeout/busy/I/O/incomplete/cancelled. Payload stays opaque by design. | `bus_transport_stats` has transaction/success/error/byte counters. CRC belongs above. | No motor-health/safety decision here. | Statistics are not visible in SVD48 telemetry; higher layer can turn a single failure into health flapping. | Keep raw transport responsibility; surface taxonomy/counters via device diagnostics, not physical fault at transport. |
| **SVD48 protocol/device** | Modbus-like RS485. Position/speed/current high-rate every poll; status/temp/bus/error every 20 polls. | CRC16, address/function/byte-count/write-echo validation, valid exception parsing; bounded retryable errors. | Per-field values/timestamps retained. Device tracks transactions, success/failure, streak, success/failure times. | CRC-valid nonzero error register is immediately `FAULT`; online is success within stale timeout. | One high-rate failure sets `failed_observations`; health maps it immediately to `DEGRADED`, then one success clears it. Aggregate stale means *any* field stale. | Preserve raw masks and real faults. Introduce effective quality from failure + recovery streak and absolute LKG age. Use explicit high-rate liveness fields, not slow diagnostics. |
| **SVD48 poll service** | Scheduled per physical device; default/profile 30 ms, non-OK backoff 250–1500 ms. | Delegates integrity to device. | Entry failure streak schedules backoff; `PARTIAL` is treated non-OK. | No direct safety decision. | Actual cadence/failure streak is not surfaced to endpoint diagnostics. | Feed actual attempts/cadence into effective quality while retaining age deadline. |
| **I2C / AS5600** | Profile-owned bit-banged I2C, not Rafa traction path. | I2C ACK/transaction result; no payload CRC. STATUS+RAW is contiguous; optional diagnostics separate. | Already preserves raw-angle LKG through a failed poll and records last result, success/failure times and streak. Age makes it stale/offline. | Valid magnet/status evidence affects health immediately. | Rich quality exists only in AS5600 diagnostics, not common Console health projection. | Preserve current LKG behavior; expose raw attempt/age/quality without a false offline on one failure. |
| **Serial gateway** | UART command grammar and serialized command lock. | Command parser/policy and typed routing; no independent CRC at text boundary. | Read-only commands expose cached snapshots. | Routes through application ports; not an actuator-health owner. | SVD48/IBUS public status lacks effective quality, LKG age and raw last attempt. | Extend typed snapshots; do not add arbitrary raw bus access. |
| **Firmware → backend telemetry** | Pull of cached channel data, `IBUS_STATUS`, `SAFETY_STATUS`, `CONTROL_STATUS`; no push stream. | Authenticated response/request-id plus typed ASCII parsing. | Firmware owns acquisition time but SVD48 public response lacks field acquisition timestamps; backend makes host timestamp. | Observational only. | Any subrequest aborts frame. `read_telemetry()` currently asks `SAFETY_STATUS` twice. | Remove duplicate. Preserve grouped LKG and raw poll outcome; expose real field age when firmware has it. |
| **Backend polling/runtime** | `PollingTelemetrySource` at 5 Hz, feeding recorder and hub. | Exceptions are caught so task survives. | Success publishes frame; any exception only sets `last_error` and publishes no frame. No observation-level LKG/streak/recovery model. | It cannot affect firmware safety. | Browser sees missing frames and overloaded field state instead of stable host evidence. | Pure host quality/LKG model; publish stable effective values plus raw failure diagnostic transition/event. Connection liveness gets distinct age/streak policy. |
| **Backend → frontend WS/API** | REST plus `/ws/telemetry`; subscriber queue 8 drops oldest, hub history 300, browser 120. | HTTP/Pydantic schema; browser JSON parse. | Queue drops counted only in hub. | No safety authority. | `TelemetryObservation(valid, stale, health)` overloads raw, freshness and effective health; React re-infers whole-device health. | Compatibly add raw attempt, quality, effective health, LKG age and counters. Frontend only presents backend decision. |
| **Discovery** | Authenticated HELLO via directed and limited UDP broadcasts; 3 attempts. | Response identity/token/request-id checks. | No LKG: it is a scan result. | Must never alter existing authority. | A missing broadcast can look like disconnect. | Present visibility only; active connection health is unicast Maintenance LKG age. |
| **OTA** | Wi-Fi TCP/HTTP announce/check/download; Rafa normal changes OTA-only. | HTTP/status/manifest/image verification in OTA components. | Manager retains check status/backoff/last result. | OTA rollback/release safety independent of telemetry quality. | It is separate from live peer liveness. | Expose only diagnostic last result/age as needed; no OTA follows from this audit. |

## Cross-cutting findings and ownership

1. Control LAN TTL, explicit STOP, valid controller faults and physical safety
   inputs must remain immediate. Communication hysteresis never extends them.
2. Integrity remains protocol-owned: PPM in `ppm_decoder`, checksum in
   `ibus_receiver`, CRC/semantics in `svd48_device`, and schema/auth in LAN.
3. A small pure temporal-quality primitive may be shared, but drivers retain
   their own integrity rules and profiles retain their own thresholds.
4. `robot_safety`, `command_authority` and `motion_application` retain source
   authority and actuation semantics. React never becomes a second authority.
5. Field cadence is independent: slow SVD48 diagnostics must not invalidate
   fresh velocity/current/position liveness.
6. The canonical implementation must retain raw failures and derived state
   transitions for recorder/diagnostics; hysteresis stabilizes presentation, not
   evidence.

## Implemented reliability contract

The baseline findings above resulted in the following implemented, source-owned
policies. These lines supersede the "recommended change" column for the current
implementation.

- Rafa's `ppm_motion.expected_frame_channels` is `8`; the decoder publishes only an
  exact frame, retains LKG on any rejected frame, and reports `REJECTED` counters.
  PPM channel plausibility remains profile-owned; the current reviewed 750–2250 µs
  envelope is not silently narrowed without physical evidence.
- RC/LAN authority candidates are counted only on a new accepted frame sequence.
  Three candidates commit a CH5 transition; a reverse frame cancels it. Missing valid
  frames still take the existing stale/loss path immediately once its age deadline is
  crossed.
- i-BUS retains checksum rejection and now records an invalid-frame streak. SBUS is
  explicitly rejected as unsupported because UART configuration alone is not an SBUS
  parser.
- `communication_quality_model` owns only SVD48 primary-observation temporal quality.
  Its policy is configured by the SVD48 composition from real poll/stale timings:
  one failure is transient, two are suspect, three are degraded, stale/offline remain
  age based, and two primary good polls recover.
- `control_lan` records accepted/rejected packets, sequence gaps, duplicate/out-of-
  order, schema/auth rejects and last valid command age. It accepts a monotonic gap;
  it does not change control TTL, ordering or STOP behavior.
- Console Maintenance reads retry only actual socket timeouts and only for the
  documented idempotent read set. The Console projects cached LKG per signal, records
  raw poll failures and state transitions, and never retries side effects.

## Evidence limits

The audit identifies the current code paths and contracts. It does not establish
Rafa physical resilience. A later controlled/elevated evidence run is required
after implementation, tests and builds are green.
