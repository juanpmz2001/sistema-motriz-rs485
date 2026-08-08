# Safety contract

## Classification

The `bench-baseline-v1` Iteration 4 baseline and current Iteration A integration
remain bench firmware. They are not approved for a robot on the floor, around people
or with mechanically loaded actuators. Software stop commands are not a replacement
for an independent emergency power path.

## Implemented startup and stop behavior

- Startup selects and validates an immutable C profile, preflights executable
  factories, constructs one serialized bus transport, one device per configured SVD48
  controller and direct M1/M2 endpoint adapters.
- Preflight rejects an empty/nonconstructible endpoint set and rejects every
  velocity-capable SVD48 endpoint that lacks `STOPPABLE`, before any output is
  initialized.
- A schema-valid but composition-unsupported profile does not construct actuator
  outputs. It starts only a restricted serial diagnostic gateway; motion, enable,
  fault clear, register writes, OTA actions and streaming are blocked.
- A pending-verification OTA image does not use that fallback after composition
  failure; rollback handling takes precedence.
- Diagnostic-only `STOP ALL` reports that outputs were not initialized; it never
  claims a physical stop that could not be attempted.
- Normal startup issues a best-effort global stop through the application port and
  coordinator before starting polling and external services. Failure is logged and
  startup currently continues.
- Reported endpoint unavailability inhibits speed/observation but does not suppress
  a stop attempt for an existing stoppable endpoint.
- A priority-9 safety task runs every 20 ms. After a valid RC frame has been seen,
  invalid RC for at least 150 ms activates RC-loss handling.
- A nonzero error code from online, fresh legacy-projected SVD48 telemetry activates
  motor-fault handling.
- While RC loss or a reported motor fault remains active, the safety task requests a
  serialized global stop every 500 ms.
- OTA checks `robot_control_is_safe_for_ota()` and uses the legacy preparation path
  before changing the boot partition. That predicate skips offline/stale telemetry and
  otherwise blocks on an unconfirmed raw observed-speed magnitude above 5 RPM.
- Wi-Fi and maintenance failures do not block a supported local robot startup.
- A polling-task stop timeout preserves its devices, locks and UART instead of
  destroying dependencies that the task may still access; restart remains inhibited
  until completion is collected.

`SAFETY_STATUS` reports observations and stop attempts; it is not certification that
all hazards are controlled.

## Current actuation ownership

| Path | Current physical write path | Status |
| --- | --- | --- |
| `SET_SPEED` | application port → coordinator → direct SVD48 channel adapter | Migrated |
| `STOP n`, `STOP ALL` | application port → coordinator → direct adapter | Migrated |
| `SET_ENDPOINT_SPEED`, `STOP_ENDPOINT` | application port → coordinator → direct adapter | Migrated; serial only |
| Boot and safety stop | application port → coordinator → direct adapter | Migrated |
| `ENABLE` | gateway → `robot_control` compatibility facade | Bypass |
| `CLEAR_FAULT` | gateway → `robot_control` compatibility facade | Bypass |
| `MOVE_VEL` | gateway → legacy kinematics/facade | Bypass; unsupported by bench profile |
| OTA preparation | OTA service → `robot_control` | Bypass |
| `SVD48_IDENTIFY ... START|STOP` | gateway → `robot_control`/legacy SVD48 view | Bypass; can cause physical identification motion |
| Maintenance register/config writes | gateway → `robot_control`/legacy SVD48 view | Bypass |

The coordinator mutex prevents interleaving only for migrated calls. It has a 500 ms
acquire timeout and driver operations execute while it is held, so safety stop has no
priority over an in-progress writer. The target remains one priority-aware actuation
owner for every hardware-changing path.

## Observation and health semantics

The Iteration 4 SVD48 driver keeps validity, timestamp and freshness per observation.
A successful current, temperature or position read cannot make a failed speed sample
fresh.

- **Complete poll cycle:** every observation scheduled for that cycle succeeded. A
  fast cycle requires position, speed and current; a slow cycle requires those plus
  status, both temperatures, bus voltage and error code.
- **Partial poll:** the controller responded, but at least one required observation
  failed. It is not counted as complete success and receives polling backoff. It
  contributes to `DEGRADED` only after the higher-precedence `OFFLINE`, fresh `FAULT`
  and `STALE` checks described below.
- **Healthy:** every configured SVD48 observation is valid and fresh, the latest cycle
  is complete and the controller reports no fault.
- **Degraded:** communication remains available and prior values are still fresh, but
  an observation has a failure bit or the latest cycle is partial/failed.
- **Stale observation:** its own age exceeds the configured freshness threshold,
  independently of later success for another field.
- **Offline:** no successful device transaction remains within the configured
  communication freshness interval.
- **Fault:** a valid, fresh error-code observation is nonzero; a successful unrelated
  bus operation does not erase it.

Channel health applies these states in order: `OFFLINE`, fresh `FAULT`, `STALE`,
`DEGRADED`, then `HEALTHY`. Offline always wins; a fresh nonzero error yields `FAULT`
even if another field is stale, while a stale error observation no longer yields
`FAULT`. A totally failed poll can be degraded during the window in which prior
observations and the last successful transaction remain fresh.

These driver facts are diagnostic foundations. The active `robot_safety` task still
reads the legacy projection and ignores offline/stale telemetry rather than applying
required/optional/development policy. Driver health therefore must not be described as
an active motion inhibit yet.

Given-speed registers `0x5304/0x5305` and observed-speed registers
`0x5410/0x5411` are treated as signed raw RPM per the manufacturer register table,
without artificial scaling. The observed value already feeds the legacy 5-RPM
OTA/maintenance readiness predicate and `PLATFORM_STATUS` motion indication despite
lacking physical confirmation. Those checks are not qualified safety evidence; a
future controlled physical test must confirm the interpretation and failure policy.

The typed velocity-observation boundary preserves validity, sample timestamp,
controller-feedback source, online, speed-specific stale and health fields. It lets
an L4 host test avoid concrete driver knowledge, but it does not make the feedback
independent physical evidence and it is not yet an active motion inhibit.

## Required invariants

Future code must preserve these rules:

1. There is one actuator-output owner after initialization.
2. Every motion source has explicit authority, sequence and expiration time.
3. Expired, malformed, replayed or unauthorized commands cannot sustain motion.
4. Stop is idempotent, bounded in time and has a hardware-level fallback strategy.
5. Configuration is fully validated before any output can be enabled.
6. Unsupported composition leaves outputs unconstructed and diagnostics read-only.
7. A required unhealthy capability inhibits only modes that depend on it unless
   reviewed policy marks it globally critical.
8. A device omitted from the active profile is not treated as failed.
9. Maintenance and OTA cannot silently take control from RC or autonomous control.
10. Network, storage, logging and JSON work never run in the safety/control task.
11. Telemetry distinguishes commanded, observed and inferred values.

## Known gaps

The following block a production baseline:

| Gap | Current behavior | Required result |
| --- | --- | --- |
| Boot stop failure | Warning; normal startup continues | Inhibit actuation and enter explicit fault |
| Offline/stale drive | Represented in driver, ignored by active safety | Profile-aware degraded/fault policy |
| Initial RC absence | RC loss starts only after first valid frame | Explicit startup/arming policy |
| Command ownership | Speed/stop use coordinator; other writers bypass it | Priority-aware single owner and arbitration |
| Stop latency | May wait 500 ms for coordinator mutex plus driver timeout | Measured deadline with stop precedence |
| Command lifetime | Maintenance speed has no TTL/deadman | Lease expiry forces stop |
| LAN trust | Shared token and plaintext UDP | Replay protection, rotation and threat model |
| Servo feedback | PWM command only | Report command only or add independent feedback |
| Watchdog evidence | Configured, not timing-qualified | Worst-case timing and fault injection |
| OTA authenticity | SHA-256 integrity only | Signed firmware and protected verification key |
| Memory/resource qualification | Static linker-map gate passes at 232,272 B effective shared D/IRAM margin; runtime margins are unqualified | Preserve the 192 KiB CI floor; qualify runtime heap/stacks before field use |
| Speed interpretation | Unconfirmed raw RPM already feeds legacy 5-RPM readiness/status checks | Controlled off-ground unit validation and reviewed fail-safe policy |

The one-byte `IRAM` remainder reported by `idf.py size` is an alignment gap within
the ESP32-S3's dedicated 16 KiB IRAM category, not the remaining linker capacity.
The audited map has 232,272 bytes of effective shared D/IRAM headroom in
both profiles, and CI fails below 192 KiB. This closes the static link-capacity
interpretation gap; it does not qualify runtime heap, task stack high-water marks,
watchdog timing or long-running polling stability.

## Profile-aware peripheral policy

Every configured device or endpoint has explicit criticality:

- `required`: failure prevents modes that depend on it.
- `optional`: failure reports degraded health without a global inhibit by default.
- `development`: may be absent/incomplete for isolated driver work; only explicitly
  configured outputs may operate.

An unconfigured peripheral does not exist from the runtime perspective. The
single-motor bench profile therefore does not invent a second failed controller or
M2 endpoint. Development profiles must still enforce electrical limits, command
expiry and a stop path for every connected actuator.

## Hardware test preconditions

Before any command that can move or alter persistent drive configuration:

- Confirm the exact ESP32-S3 and serial/network endpoint.
- Confirm firmware project, target, build and active profile with read-only status.
- Keep the mechanism unloaded and restrained; lift traction wheels.
- Provide a person-operated power disconnect independent of the ESP32.
- Verify voltage levels, grounding, RS485 direction hardware and drive IDs.
- Start with read-only diagnostics and capture original configuration outside the
  repository before writes.
- Set conservative current, velocity and travel limits in the physical drive.
- Stop immediately on unexpected direction, sound, current, telemetry or latency.

Agents must not initiate actuation merely to prove that a build succeeded. The future
RPM interpretation check requires separate explicit authorization and a reviewed test
setup; it is not part of software closeout.

## Host HIL safety boundary

The Iteration A runner validates the exact full Git SHA, dirty state, board, profile,
logical endpoint, criticality, RPM bounds and declared capabilities before a motion
manifest can run. It records the operator-supplied firmware artifact SHA-256/reference
without claiming on-target image attestation. It also requires a named hardware/PCB
identity and explicit operator confirmations for authorization, unloaded mechanics
and a working physical cut-off. The included L4/E2 manifest applies only to the
one-endpoint bench profile and bounded ±5 RPM requests.

Before its steps the runner requests `STOP ALL` and checks application-level
composition, safe-idle platform, no active RC-loss/motor-fault condition and a fresh
stopped-observation gate. After every
attempt that may have issued motion it requests `STOP ALL` again and requires a
fresh, online, healthy stopped observation before `PASS` can remain possible. These
are orchestration controls, not firmware authority, TTL/deadman, stop precedence or
an emergency stop.

Automatic cleanup is best effort on normal completion, exceptions, timeouts,
`SIGINT`, `SIGTERM` and `SIGHUP`. `SIGKILL`, host power loss, cable loss, target
failure or a blocked transport can defeat it. A person-operated physical power
cut-off therefore remains mandatory. No physical gate is passed merely because the
runner, its fake tests or the firmware build succeeds.

## Production release gates

A candidate is not a production baseline until all of these have executable evidence:

- Boot, brownout and reset always produce bounded safe output behavior.
- RC loss, source timeout, stale/partial telemetry, drive fault and bus loss are
  injected for required and optional profile hardware.
- Arbitration and state transitions are covered by host and hardware tests.
- Worst-case control/safety latency is measured during Wi-Fi reconnect, LAN load, OTA
  check and heavy logging.
- OTA success, interrupted download, invalid manifest/signature, rollback and power
  loss are tested.
- NVS corruption and invalid/unsupported profiles cannot enable outputs.
- Memory, stack, watchdog and long-duration stability margins are recorded for each
  supported build profile.
- SVD48 speed units and stop behavior are confirmed on the exact controller firmware.
- A wiring-specific commissioning checklist and emergency procedure exist outside
  this generic firmware repository.
