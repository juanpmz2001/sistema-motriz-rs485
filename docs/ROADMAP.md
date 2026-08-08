# Roadmap from the Iteration 4 bench baseline

## Goal

Create a safe, profile-driven embedded platform from which teams can add robots,
controllers and actuators without editing a monolithic `main`. The ESP32 owns
deterministic I/O, command expiry and local safety. A later Linux ROS 2 layer owns
planning and exposes the controller through `ros2_control`.

Do not begin by adding ROS dependencies to this firmware. First establish a stable,
fully owned and qualified embedded contract that ROS can consume.

## Current baseline

Implemented in Iteration 4:

- Immutable Kconfig-selected C profiles for board, buses, devices, channels,
  endpoints, limits, criticality and optional application geometry.
- Portable `bus_transport`, ESP-IDF `rs485_transport` and one serialized transport per
  physical bus.
- One `svd48_device` per physical controller, explicit M1/M2 channels and shared
  bounded N-device polling.
- Direct SVD48 channel adapters for coordinated `SET_SPEED`, `STOP n`, `STOP ALL`,
  boot stop and safety stop.
- An executable SVD48 factory/preflight with structured diagnostics.
- `current_robot` and executable one-device/one-endpoint
  `bench_single_svd48_motor` profiles.
- Per-observation validity/freshness and complete/partial/failed polling results at the
  driver boundary.
- A restricted serial diagnostic fallback when a schema-valid composition is
  unsupported, without constructing outputs.

Implemented but transitional:

- `robot_control` remains the compatibility facade for telemetry, maintenance,
  kinematics and OTA helpers.
- `svd48_handle_t` projects composed devices into at most four legacy logical indices.
- The coordinator is synchronous and mutex-backed; it is the writer for migrated
  speed/stop paths, not every hardware-changing command.
- Safety consumes legacy motor telemetry and does not yet apply full profile-aware
  degraded/stale/offline policy.
- The executable registry contains SVD48 only; other schema driver descriptors are
  non-executable fixtures.

Not available now:

- Runtime JSON/YAML topology or a supported generated profile pipeline.
- An active operating-state machine and profile-aware health aggregator.
- A priority-aware single actuation owner with source arbitration, sequence, TTL,
  lease and deadman.
- A versioned real-time transport or ROS binding.
- Product-qualified timing, memory margin, security and fault handling.

Verification status, CI results and profile build metrics are recorded only in
[Iteration 4 closeout](ITERATION_4_CLOSEOUT.md); implementation does not imply a
passing release gate.

## Ordered implementation slices

### 1. Close and stabilize Iteration 4 contracts

- Keep fake-backed host tests for transport, device, polling, channel adapter,
  factory/preflight and both profiles.
- Preserve serial compatibility tests and SVD48 golden protocol vectors.
- Build both profiles with the pinned ESP-IDF version and record flash/DRAM/IRAM,
  stacks, warnings and headroom in CI artifacts.
- Correct any health, unit, bus-selection or concurrency defect revealed by those
  tests without broadening the architecture.

Exit: every closeout criterion has executable evidence and the branch is safe for a
reviewed merge. A build is not a hardware safety test.

### 2. Profile-aware health and safe lifecycle

- Feed per-observation SVD48 health into a profile-aware aggregator.
- Integrate `robot_state` with explicit `BOOT`, `SAFE_IDLE`, `ARMED`, `ACTIVE`,
  `FAULT` and `MAINTENANCE` transitions.
- Define how required, optional and development endpoints affect each operating mode.
- Turn boot-stop failure, stale/offline required observations and partial application
  into reviewed inhibits or faults.
- Define startup/arming policy before accepting any new motion source.

Exit: absent optional hardware, omitted profile hardware and failed required hardware
have distinct tested effects, and no output activates before a valid state transition.

### 3. Priority-aware single actuation owner

- Replace the bounded coordinator mutex with one owner/mailbox where stop has explicit
  precedence over normal requests.
- Migrate `ENABLE`, fault clear, `MOVE_VEL`, OTA preparation, identification and
  maintenance actuation/configuration writes away from `robot_control`.
- Integrate command authority for RC, serial engineering, LAN and future clients.
- Require source, sequence, timestamp/TTL and deadman policy on every motion command.
- Expire commands to stop and test replay, handover, contention and driver delay.
- Restrict maintenance LAN to diagnostics/stop until this path is complete.

Exit: no transport or legacy facade directly writes an actuator, and fault injection
proves bounded stop after every source timeout.

### 4. Profile toolchain and additional drivers

- Decide the supported authoring source and build a versioned host validator/generator
  that emits the bounded embedded representation.
- Define migration/signing rules and the small subset, if any, that NVS may override.
- Add a driver only through a complete executable factory and typed capabilities.
- Add fixtures for servo-only, mixed steering/traction and intentionally absent
  hardware after the relevant factories exist.
- Cross-check endpoint names, units, limits, capacity and required safety dependencies.

Exit: supported profiles compile deterministically, unsupported factories fail with
specific diagnostics and no conditional driver wiring spreads through control code.

### 5. Embedded transport contract for Linux and ROS

- Define a versioned bounded protocol for commands, state, telemetry, time and
  capabilities; do not expose raw serial text as the ROS control API.
- Separate management/maintenance from real-time control traffic.
- Add compatibility, sequence, timeout and reconnect tests.
- Implement a Linux client before implementing ROS bindings.
- Add a `ros2_control::SystemInterface` only after the client and firmware authority
  path are stable.

Exit: Linux and then ROS can discover capabilities, exchange commands and recover
from disconnect without bypassing firmware state, authority or stop policy.

### 6. Product qualification

- Close every release gate in [Safety](SAFETY.md).
- Add signed OTA, key management and reviewed network threat controls.
- Run hardware-in-loop fault injection, worst-case latency, long-duration stability
  and resource/stack/watchdog qualification for every supported board profile.
- Define manufacturing provisioning, recovery, commissioning and per-robot identity.

Exit: a tagged reproducible baseline is suitable for its declared physical operating
envelope. Until then, all firmware remains bench-only.

## Branching rule

Feature branches begin only after their prerequisite slice has a tested contract.
Driver work may proceed in parallel behind capability interfaces; state, authority and
actuation-owner changes remain serialized because they share the safety boundary.

Keep this repository limited to current contracts, reviewed target design and concise
migration evidence. Store raw captures, vendor PDFs, one-off experiments and session
logs in external artifacts, then bring back only reproducible tests and decisions.
