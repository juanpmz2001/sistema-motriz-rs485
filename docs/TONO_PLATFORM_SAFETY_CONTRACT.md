# Toño Platform Safety Contract

This document captures the current low-level platform contract for the ESP32-S3 firmware. It is intentionally scoped to the supervised prototype state of the project.

## Current Boundary

The ESP32-S3 firmware owns hardware-adjacent behavior:

- SVD48 RS485 reads, writes, telemetry polling and fault visibility.
- Four logical motor commands through `SET_SPEED`, `MOVE_VEL`, `ENABLE`, `STOP` and `CLEAR_FAULT`.
- Legacy independent-steering calculations remain available, but steering PWM is disabled by the current RAFA defaults.
- Wi-Fi configuration and OTA diagnostics/update flow.
- The ASCII serial gateway used by a PC or Jetson-side supervisor in lab conditions.
- Authenticated LAN maintenance for diagnostics/telemetry, `STOP ALL`, and provisional confirmed raw SVD48 configuration writes.

The ESP32-S3 does not currently own:

- A hardware-latched emergency stop input.
- A formal heartbeat/deadman timeout for motion commands.
- Lease arbitration between web UI, teleop, autonomy and scripts.
- ROS2, navigation, fleet management or operator HMI policy.
- A production-grade binary Jetson-to-ESP32 protocol.

## Current Command Authority

Current movement authority is `SERIAL_ASCII`.

- Any client with access to the ESP32 console can send movement and maintenance commands.
- A LAN maintenance client can send diagnostics/telemetry, `STOP ALL`, reads and confirmed SVD48 configuration writes. It still blocks movement, runtime-actuation registers, sensitive ESP configuration mutation and destructive OTA commands with `ERR LAN_COMMAND_BLOCKED`.
- `STOP n|ALL` is always the expected immediate software stop path.
- Raw register commands are elevated-bench maintenance tools. They pre-read and verify readback but do not yet provide an exclusive maintenance state, complete typed ranges, persistence or rollback.
- PPM GPIO14 is observed by diagnostics and RC-loss safety only. It is not current movement authority and never auto-arms this firmware.
- OTA update is blocked unless `robot_control_is_safe_for_ota()` sees no active command and no online/non-stale motor above the safe RPM threshold.

This is acceptable only for supervised prototype testing. The approved target is
not a Jetson-only authority: RC, LAN backend and Bluetooth may all be connected,
but one ESP-owned arbiter enforces `RC > LAN > Bluetooth`, command TTL,
stop-before-switch and no stale fallback. Any future Jetson/backend participates
as the LAN source and cannot bypass that firmware policy.

## MVP Platform States

The firmware now exposes a read-only `PLATFORM_STATUS` command with these MVP states:

| State | Meaning |
|---|---|
| `SAFE_IDLE` | No tracked motion command and no online/non-stale motor above the firmware RPM threshold. |
| `MOTION_ACTIVE` | Last accepted command or live telemetry indicates motion. |
| `FAULT` | At least one motor telemetry sample has a non-zero SVD48 error code. |

Not implemented yet:

- `ESTOP`: requires physical hardware input and latched power/brake behavior.
- `COMMAND_TTL`: required independently for RC, LAN and Bluetooth.
- `SOURCE_AUTHORITY`: requires the simultaneous mailbox/epoch arbiter.
- `ROBOT_PROFILE`: requires the canonical JSON and capabilities validator.

## Low-Difficulty Action Triage

Current highest-priority work:

- Wire the existing pure state model into every actuator output.
- Add command TTL and simultaneous `RC > LAN > Bluetooth` arbitration.
- Add emergency-class stop scheduling and fault latch integration.
- Replace compile-time topology with the validated canonical robot JSON.
- Keep all physical tests elevated with a dedicated operator at power cutoff.

Deferred beyond the local MVP:

- Binary framed protocol with CRC, ACK/NACK, sequence and protocol version.
- Hardware-latched E-stop and power-domain sequencing.
- ROS2 bridge, navigation stack, LiDAR/IMU/encoder fusion and fleet APIs.
- Operator accounts, HMAC/TLS/replay security and Internet-facing deployment.

## Manual Test Checklist

Before live motor tests:

- Confirm the robot is mechanically safe and lifted or restrained for bench tests.
- Confirm the ESP32-S3 serial port and firmware build target are `esp32s3`.
- Run `PLATFORM_STATUS` and confirm `AUTHORITY:SERIAL_ASCII`.
- Run `STOP ALL` before any OTA update.
- Treat `HEARTBEAT:UNSUPPORTED` and `ESTOP:UNSUPPORTED` as hard reminders that this is not product safety.
