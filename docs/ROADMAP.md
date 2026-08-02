# Roadmap to a development baseline

## Goal

Create a safe, profile-driven embedded platform from which teams can branch to add
robots, controllers and actuators without editing a monolithic `main`. The ESP32
owns deterministic I/O, command expiry and local safety. A later Linux ROS 2 layer
owns planning and exposes the controller through `ros2_control`.

Do not begin by adding ROS dependencies to this firmware. First establish a stable
embedded contract that ROS can consume.

## Current baseline

Available now:

- Working ESP32-S3 build, dual-slot OTA, Wi-Fi reconnect and authenticated LAN
  maintenance on a trusted development network.
- SVD48 RS485 driver, serial diagnostics, PPM input and a reactive safety task.
- Host-tested pure models for PPM, kinematics, robot state and command authority.
- A build-selected C profile for boards, buses, devices and typed endpoints, with
  bounded host validation and a transitional SVD48 composition backend.
- Application ports and a mutex-backed coordinator for speed and stop operations.

Not available now:

- A supported JSON/YAML schema, generated/runtime profile pipeline, general driver
  factory or profile-aware health.
- A complete state machine integrated with all outputs.
- A priority-aware single-owner coordinator with source arbitration, command leases
  and deadman.
- Product-qualified timing, security, fault handling or ROS transport.

## Ordered implementation slices

### 1. Freeze and test current contracts

- Correct SVD48 speed scaling and add unit tests.
- Turn boot-stop failure and stale/offline required drives into explicit health.
- Add characterization tests for current serial responses and LAN policy.
- Recover IRAM and stack margin before adding runtime features.

Exit: current hardware behavior has repeatable host tests and measured firmware
resource margins.

### 2. Profile contract and compiler

- Complete the existing C-profile invariants and distinguish profiles accepted by
  the schema from profiles executable by the available composition factories.
- Redesign schema around buses, devices, typed endpoints, capabilities and
  required/optional/development policy.
- Support fixtures for SVD48 differential drive, servo-only development, mixed
  steering/traction and intentionally absent hardware.
- Build a host-side profile compiler/validator that emits a bounded embedded
  representation; reject duplicate pins, IDs, endpoints and incompatible units.
- Version the schema and define migration rules.

Exit: valid fixtures compile deterministically and invalid fixtures fail with
specific errors. Firmware continues using a build-selected immutable profile.

### 3. Composition root and driver ports

- Finish reducing `main` to boot sequencing, profile selection, construction and
  task start.
- Extend the existing velocity, position and stop capabilities with feedback and
  fault ports as supported drivers require them.
- Replace the transitional `robot_control` adapter with direct SVD48 and PWM
  adapters without changing their proven wire behavior.
- Add a static driver registry selected by validated profile type names.

Exit: the current robot runs through interfaces with no behavior regression; a
servo-only profile can initialize without SVD48 hardware.

### 4. State, health and safe lifecycle

- Integrate `robot_state` into explicit `BOOT`, `SAFE_IDLE`, `ARMED`, `ACTIVE`,
  `FAULT` and `MAINTENANCE` transitions.
- Build a capability health graph from the profile.
- Define startup, arming, degraded mode, fault reset and maintenance guards.
- Make all entry-to-safe states issue a bounded stop through the coordinator.

Exit: absent optional hardware and failed required hardware have distinct tested
effects, and no output can activate before a valid state transition.

### 5. Single actuator coordinator

- Replace the bounded mutex wait with a priority-aware owner/mailbox where stop has
  explicit precedence over motion requests.
- Integrate authority mailboxes for RC, serial engineering, LAN and future ROS.
- Require sequence, timestamp/TTL and source policy on every motion command.
- Make the coordinator the only runtime writer to actuator endpoints.
- Expire commands to stop and test replay, source handover and simultaneous input.
- Restrict maintenance LAN to diagnostics/stop until this path is complete.

Exit: no transport directly calls a motor/servo driver, and fault injection proves
bounded stop after every source timeout.

### 6. Embedded transport contract for ROS

- Define a versioned binary or bounded structured protocol for commands, state,
  telemetry, time and capabilities; do not expose raw serial text as the ROS API.
- Separate management/maintenance from real-time control traffic.
- Add protocol compatibility, sequence, timeout and reconnect tests.
- Implement a Linux client library before implementing ROS bindings.

Exit: a Linux process can discover capabilities, exchange commands and recover
from disconnect while firmware authority and safety remain intact.

### 7. ROS 2 integration

- Implement a `ros2_control::SystemInterface` on Linux using the client library.
- Map embedded endpoints to joints defined in URDF/xacro.
- Generate or cross-validate `ros2_control` mappings from the robot profile and
  URDF; reject mismatched names, units and limits.
- Publish diagnostics and lifecycle state without giving ROS a safety bypass.

Exit: ROS controllers can operate a simulator and off-ground robot through the
same versioned contract, including disconnect and emergency-stop tests.

### 8. Product qualification

- Close every release gate in `SAFETY.md`.
- Add signed OTA, key management and network threat controls.
- Run hardware-in-loop fault injection, long-duration tests and resource/timing
  qualification on each supported board profile.
- Define manufacturing provisioning, recovery and per-robot identity.

Exit: a tagged, reproducible baseline can be branched for features without
reopening foundational safety and configuration questions.

## Branching rule

Feature branches should begin only after their prerequisite slice has a tested
contract. Driver work may proceed in parallel behind capability interfaces;
state-machine, authority and transport changes should remain serialized because
they share the safety boundary.

Keep this repository limited to current contracts and executable plans. Store raw
captures, vendor PDFs, one-off experiments and session logs in external artifacts,
then bring back only validated tests, decisions and concise operating guidance.
