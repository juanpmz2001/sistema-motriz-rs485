# Safety contract

## Classification

The `bench-baseline-v1` Iteration 4 baseline, current Iteration A integration and
the single-axis AS5600 steering development slice remain bench firmware. They are
not approved for a robot on the floor, around people or with mechanically loaded
actuators. Software stop commands are not a replacement for an independent emergency
power path.

## Implemented startup and stop behavior

- Startup selects and validates an immutable C profile, preflights executable
  factories, constructs only the profile's transports/devices and creates typed
  endpoint adapters. The supported paths include SVD48 channels and the isolated
  motor-mode-PWM/AS5600/steering-controller bench chain.
- Preflight rejects an empty/nonconstructible endpoint set and rejects every
  velocity- or position-capable endpoint that lacks `STOPPABLE`, before any output is initialized.
  The steering-axis profile validator separately requires its `POSITION` endpoint to
  include `STOPPABLE` and explicit reference capability.
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
- The `bench_single_steering_as5600` profile is a development-only bench profile.
  Its motor-mode PWM output is owned by the steering controller, not by a direct
  gateway/PWM command. The controller initializes neutral and **UNHOMED**; no normal
  position command may infer a mechanical zero from an AS5600 phase.
- In that profile, a missing fresh steering sample becomes stale at 120 ms and the
  controller commands neutral; at 400 ms it latches a sensor-timeout fault and keeps
  neutral. A hard sensor-health fault also neutralizes and latches. These are
  controller-level behaviors, not a measured physical stop guarantee.
- Each control-rate AS5600 sample is one contiguous three-byte `STATUS+RAW` I2C
  transaction. The optional three-byte AGC/magnitude diagnostic is attempted once
  after the first primary sample. At 5 kHz, profile validation budgets the recovery
  path plus CPU/GPIO margin within a 25 ms transaction deadline and budgets that
  initial pair plus the 40 ms sensor cadence and 10 ms service cadence before the
  120 ms stale-neutral window. This is a source-level scheduling budget only; task
  latency and physical neutral/stop response still need hardware evidence.
- The development profile's sole degraded-feedback exception is the known AS5600
  `ML` warning with `MD` present and a successful primary poll. `MH`, missing `MD`,
  a partial diagnostic result or an invalid primary read are not covered by that
  exception and inhibit/latch the local controller. A one-shot diagnostic failure
  therefore leaves this steering slice in a latched NO-GO state; there is no automatic
  retry or fault-clear path.
- Motor-mode PWM attaches the LEDC channel with neutral duty already configured.
  On teardown it requests neutral, stops LEDC and resets the GPIO routing. This
  prevents a retained generated waveform after a construction/deinit path, but the
  electrical transition and detached-level behavior still require L3 hardware
  characterization; they are not claimed as a proven physical stop.
- Every accepted steering position request uses the profile's 650 ms TTL. Expiry,
  explicit endpoint/global stop, output failure and move timeout clear the target and
  request neutral. The normal stop path is still best effort and remains subordinate
  to the known coordinator/physical-power limitations below.
- A profile with validated differential endpoint geometry starts the continuous LAN
  path `control_lan → command_authority → motion_application → traction endpoints`.
  ARM creates a new nonzero stream, COMMAND requires an increasing sequence and
  explicit deadman, and the client cannot select its TTL. An asserted deadman with
  zero velocity applies zero endpoint targets (HOLD 0 for the current SVD48 adapter).
  The current profile uses
  300 ms; profile validation constrains this contract to 50–500 ms. A released deadman
  causes zero/global stop, and an expired source is
  retired before a stop is issued.
  A retired stream cannot resume without a new ARM/stream.
- STOP and DISARM evict older pending motion intent and STOP plans are consumed before
  APPLY plans. This is semantic priority, not physical preemption: an already-running
  coordinator/driver transaction completes before the service can execute the next
  stop. The 300 ms value and physical stop latency remain workshop-qualification
  gates. Rafa now carries the operator-qualified M1/M2 side and direction mapping;
  enabling the software path does not close its elevated expiry/stop or floor-motion
  gates.
- Establishing the logical steering reference is a separate, explicitly confirmed
  maintenance operation. It stops first and maps a freshly observed, physically
  verified pose into the configured coordinate system; it never drives, auto-homes or
  proves that the selected pose is mechanically correct. It also cannot clear or
  re-arm a latched steering fault.
- A priority-9 safety task runs every 20 ms. After a valid RC frame has been seen,
  invalid RC for at least 150 ms activates RC-loss observation. For profiles without
  an RC/LAN interlock it remains a global stop condition.
- Rafa has a profile-owned RC/LAN interlock on receiver CH5: an accepted CH5≤1500us
  begins a candidate and three consecutive accepted frames commit PPM priority,
  revoke any active LAN stream through `control_lan → motion_application`, and block
  LAN ARM/COMMAND. A candidate never changes authority or increments the revocation
  epoch. CH5=2000us is the reviewed receiver failsafe and allows a fresh LAN ARM
  after the same confirmation. Rafa accepts exactly eight PPM pulses; a malformed,
  short or extra frame cannot modify CH5, its valid sequence, or authority. Rafa's
  PPM source reaches traction
  only through `ppm_motion_source → motion_application → command_authority →
  robot_kinematics`; it never calls an SVD48 driver. A PPM takeover first stops and
  retires the prior stream, then requires a new CH2/CH4-neutral frame before RC ARM.
  PPM loss, CH5 failsafe, or an external STOP requires that neutral handshake again.
  Loss after PPM priority is surfaced as `PPM_LOST`; an old LAN or RC stream cannot
  resume automatically.
- A nonzero error code from online, fresh legacy-projected SVD48 telemetry activates
  motor-fault handling.
- `SVD48_HALL_CALIBRATE` is a controller-owned maintenance procedure, not a generic
  configuration write. Firmware rejects it while continuous control is `ARMED` or
  `ACTIVE`, unless the platform is `SAFE_IDLE`, the safety task is running without a
  motor fault, and the selected bound channel is available, `HEALTHY` and reports
  `STOPPED`; `HOLD 0` is intentionally rejected because it leaves the controller
  enabled. It does not send `SAVE_SVD48_CONFIG` or retry a one-shot trigger after
  ambiguous transport. An ACK or status read is not evidence that calibration
  completed mechanically.
- While a legacy RC-loss stop or a reported motor fault remains active, the safety
  task requests a serialized global stop every 500 ms. Rafa does not turn an absent
  or failsafe RC signal into a stop of a live LAN lease; the source-aware interlock
  above is the applicable safeguard.
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
| `SET_ENDPOINT_POSITION` | application port → coordinator → steering endpoint adapter → controller → motor-mode PWM | Migrated only for the development steering profile; controller TTL applies; not physically qualified |
| `SET_ENDPOINT_POSITION_REFERENCE` | application port → coordinator → stop endpoint → explicit steering reference | Maintenance-only full-serial path; confirmation required; never auto-homes or drives |
| `STOP n`, `STOP ALL` | application port → coordinator → direct SVD48 or steering endpoint adapter | Migrated for constructed stoppable endpoints |
| `SET_ENDPOINT_SPEED`, `STOP_ENDPOINT` | application port → coordinator → direct SVD48 or steering adapter | Migrated; serial only |
| `SVD48_BENCH_SET_SPEED`, `SVD48_BENCH_HOLD`, `SVD48_BENCH_DISABLE`, `SVD48_BENCH_STOP` | device/channel inventory lookup → application port → coordinator → direct SVD48 adapter | Migrated bench-only maintenance path; no lease/deadman, not `/control` |
| `/control` LAN intent | `control_lan` → `motion_application` (`command_authority` + `robot_kinematics`) → application port → coordinator → traction endpoints | Active only for validated differential profiles; fixed 300 ms current-profile TTL, software-tested, not physically qualified |
| Rafa PPM intent | `ibus_receiver` → `ppm_motion_source` → `motion_application` (`command_authority` + `robot_kinematics`) → application port → coordinator → traction endpoints | CH5 source priority, neutral-before-arm, profile TTL; software-tested, not physically qualified |
| Boot and safety stop | application port → coordinator → constructed stoppable adapters | Migrated; physical effectiveness remains unqualified |
| `ENABLE` | gateway → `robot_control` compatibility facade | Bypass |
| `CLEAR_FAULT` | gateway → `robot_control` compatibility facade | Bypass |
| `MOVE_VEL` | gateway → legacy kinematics/facade | Bypass; unsupported by bench profile |
| OTA preparation | OTA service → `robot_control` | Bypass |
| `SVD48_IDENTIFY ... START|STOP` | gateway → `robot_control`/legacy SVD48 view | Bypass; can cause physical identification motion |
| Maintenance register/config writes | gateway → `robot_control`/legacy SVD48 view | Bypass |

The gateway now enforces a narrow cross-path interlock: continuous-control `ARMED` or
`ACTIVE` rejects SVD48 bench set-speed/hold and SVD48 register/configuration writes or
save. Channel disable/stop and global `STOP ALL` stay available. This closes concurrent
session preparation/use through those Maintenance-LAN operations; it does not turn
Maintenance LAN into a leased control path or migrate the remaining bypasses.

The coordinator mutex prevents interleaving only for migrated calls. It has a 500 ms
acquire timeout and driver operations execute while it is held, so safety stop has no
priority over an in-progress writer. The target remains one priority-aware actuation
owner for every hardware-changing path.

The Engineering Console requires the operator to type exactly `motor elevado` before
its set-speed or hold requests and repeats current platform/safety/channel checks in
the backend. This does not prove that a wheel is unloaded or that a physical cutoff is
available. It does not add firmware TTL/deadman semantics. Direct use of the firmware
bench commands remains subject to the same explicit hardware-test authorization and
physical setup requirements as other motion commands. Disable, channel stop and
global `STOP ALL` must remain available as best-effort software stop paths.

## Observation and health semantics

The Iteration 4 SVD48 driver keeps validity, timestamp and freshness per observation.
A successful current, temperature or position read cannot make a failed speed sample
fresh.

- **Complete primary poll:** position, speed and current succeeded. Those fields are
  the SVD48 velocity-liveness set. Slow status, both temperatures, bus voltage and
  error code retain their own cadence and freshness.
- **Raw attempt failure:** a timeout, bad CRC, incomplete response or bus-busy result
  rejects only that attempt. It preserves each field's LKG value and acquisition time,
  increments raw evidence, and does not by itself make a physical controller fault.
- **Communication quality:** per-channel primary attempts remain healthy with one
  transient failure, become `SUSPECT` after two failures, and `DEGRADED` after three.
  A degraded/stale/offline link needs two complete primary polls to recover. These
  source-owned count thresholds never extend an age deadline.
- **Healthy velocity communication:** position, speed and current are valid and
  fresh, and no fresh controller error is present. Lower-rate diagnostic fields have
  their own freshness; their expiry is shown in the snapshot but does not by itself
  make a velocity endpoint unavailable.
- **Degraded velocity communication:** communication remains available and prior
  velocity feedback is still fresh, but the configured persistent primary-failure
  threshold has been reached.
- **Stale observation:** its own age exceeds the configured freshness threshold,
  independently of later success for another field.
- **Offline:** no successful device transaction remains within the configured
  communication freshness interval.
- **Fault:** a valid, fresh error-code observation is nonzero; a successful unrelated
  bus operation does not erase it.

Velocity-channel health applies these states in order: `OFFLINE`, fresh `FAULT`,
fast-feedback `STALE`, quality `DEGRADED`/`SUSPECT`, then `HEALTHY`. Offline always
wins; a fresh nonzero error yields `FAULT` even if a lower-rate diagnostic is stale,
while a stale error observation no longer yields `FAULT`. A totally failed poll can
be degraded during the window in which prior observations and the last successful
transaction remain fresh, but the first failed poll remains raw quality evidence
rather than an automatic health flap.

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

The typed position-observation boundary carries the same freshness/health separation
plus source endpoint, acquisition status and explicit `calibrated`/`referenced`
fields. For the AS5600 adapter, `valid: true` requires a fresh, online,
magnet-detected sample, a profile-approved LUT and an explicit reference that maps
the accepted cyclic phase into the actuator's logical coordinates. Without calibration
or reference, raw device diagnostics can still support an L3 investigation, but the
generic logical-position observation remains invalid. Neither field proves that the
operator's mechanical reference or angle accuracy is correct, nor that a physical
closed-loop test passed.

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
| Command lifetime | `/control` has firmware TTL/deadman; maintenance speed still persists | Physically qualify `/control` expiry/stop timing and migrate or isolate every remaining persistent motion path |
| LAN trust | Shared token and plaintext UDP | Replay protection, rotation and threat model |
| Steering development axis | A separate AS5600 observation endpoint, local controller and provisional profile LUT are composed in source, but no physical session has run | L2/L3 sensor and actuator qualification, explicit-reference verification, then bounded L4/L5 evidence on the named fixture |
| AS5600 calibration/reference | Offline 7+7 analysis can reject a bad capture and produce a monotonic candidate LUT; it does not establish zero or absolute wheel angle. The scoped historical candidate still had 7.925° P95 / 15.924° maximum post-correction residual in its historical validation. | Preserve capture/hash/fixture provenance, independently verify reference/angle and direction effects, and review every LUT change before use; do not derive an L4/L5 tolerance from the controller's 3° arrival band. |
| Watchdog evidence | Configured, not timing-qualified | Worst-case timing and fault injection |
| OTA authenticity | SHA-256 integrity only | Signed firmware and protected verification key |
| Memory/resource qualification | Static linker-map gate passes at 232,272 B effective shared D/IRAM margin; runtime margins are unqualified | Preserve the 192 KiB CI floor; qualify runtime heap/stacks before field use |
| Speed interpretation | Unconfirmed raw RPM already feeds legacy 5-RPM readiness/status checks | Controlled off-ground unit validation and reviewed fail-safe policy |

The one-byte `IRAM` remainder reported by `idf.py size` is an alignment gap within
the ESP32-S3's dedicated 16 KiB IRAM category, not the remaining linker capacity.
The audited Iteration B SVD48 maps had 232,272 bytes of effective shared D/IRAM
headroom. CI applies the 192 KiB floor to every versioned build profile. This closes
the static link-capacity interpretation gap; it does not qualify runtime heap, task
stack high-water marks, watchdog timing or long-running polling stability.

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
- If the SVD48 address is unknown, use the bounded `SVD48_PROBE` read-only command
  across Modbus unicast addresses before declaring the connection dead. The probe
  reads only bus-voltage registers, performs no writes and does not authorize motion
  or configuration changes.
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

The executable runner currently has no steering manifest or actuator control path.
The steering-specific preparation and its offline calibration analyzer do not relax
this boundary; use the dedicated [steering bench runbook](testing/STEERING_AS5600_BENCH_RUNBOOK.md)
only after an operator explicitly authorizes the named hardware session.

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
- Steering reference, AS5600 calibration provenance, stale/fault neutral behavior,
  command expiry and physical stop behavior are qualified on every supported steering
  fixture before it can be required for motion.
- A wiring-specific commissioning checklist and emergency procedure exist outside
  this generic firmware repository.
