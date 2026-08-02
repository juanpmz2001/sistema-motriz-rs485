# Safety contract

## Classification

Build 19 is bench firmware. It is not approved for a robot on the floor, around
people or with mechanically loaded actuators. Software stop commands are not a
replacement for an independent emergency power path.

## Implemented behavior

- Startup creates the SVD48 and robot facade, then issues a best-effort `STOP ALL`.
- A priority-9 safety task runs every 20 ms.
- After a valid RC frame has been seen, invalid RC for at least 150 ms activates
  RC-loss handling.
- A nonzero error code from online, fresh SVD48 telemetry activates motor-fault
  handling.
- While either condition remains active, `STOP ALL` is repeated every 500 ms.
- OTA update checks `robot_control_is_safe_for_ota()` and prepares a stop before
  changing the boot partition.
- Wi-Fi and maintenance failures do not block local firmware startup.

`SAFETY_STATUS` reports observations and stop attempts; it is not a certification
that all hazards are controlled.

## Required invariants

Future code must preserve these rules:

1. There is one actuator-output owner after initialization.
2. Every motion source has an explicit authority, sequence and expiration time.
3. Expired, malformed, replayed or unauthorized commands cannot sustain motion.
4. Stop is idempotent, bounded in time and has a hardware-level fallback strategy.
5. Configuration is fully validated before any output can be enabled.
6. A required unhealthy capability inhibits only the operating modes that depend
   on it, unless policy marks it globally critical.
7. A device omitted from the active development profile is not treated as failed.
8. Maintenance and OTA cannot silently take control from RC or autonomous control.
9. Network, storage, logging and JSON work never run in the safety/control task.
10. Telemetry claims distinguish commanded, observed and inferred values.

## Known gaps

The following are blockers for a production baseline:

| Gap | Current behavior | Required result |
| --- | --- | --- |
| Boot stop failure | Warning; startup continues | Inhibit actuation and enter explicit fault |
| Offline/stale drive | Ignored by motor-fault detection | Profile-aware degraded/fault policy |
| Initial RC absence | RC loss starts only after first valid frame | Explicit startup/arming policy |
| Command ownership | Serial/LAN can call robot control directly | Coordinator with source arbitration |
| Command lifetime | Maintenance speed has no TTL/deadman | Lease expiry forces stop |
| LAN trust | Shared token and plaintext UDP | Replay protection, rotation and network threat model |
| Servo feedback | PWM command only | Report command only or add independent feedback |
| Watchdog evidence | Configured, not timing-qualified | Measured worst-case timing and fault injection |
| OTA authenticity | SHA-256 integrity only | Signed firmware and protected verification key |
| Memory headroom | Clean build: 16,383/16,384 IRAM bytes used | Restore measured engineering margin |
| Speed units | Raw 0.1 RPM labeled RPM | Correct conversion and tests |

The one-byte IRAM margin was reproduced with ESP-IDF 5.4.1 after repository
cleanup. It must be improved, then remeasured on every release build.

## Profile-aware peripheral policy

Every configured device or endpoint needs an explicit criticality:

- `required`: failure prevents modes that depend on it.
- `optional`: failure reports degraded health but does not globally inhibit motion.
- `development`: permitted to be absent or incomplete for isolated driver work;
  only explicitly enabled outputs can operate.

An unconfigured peripheral does not exist from the runtime's perspective. This is
how a servo-only bench profile can run without inventing failures for traction
drives that are physically absent. A development profile must still enforce hard
electrical limits, command expiry and a stop path for the connected actuator.

## Hardware test preconditions

Before any command that can move or alter persistent drive configuration:

- Confirm the exact ESP32-S3 and serial/network endpoint.
- Confirm firmware project, target and build with `VERSION`/`PLATFORM_STATUS`.
- Keep the mechanism unloaded and restrained; lift traction wheels.
- Provide a person-operated power disconnect independent of the ESP32.
- Verify voltage levels, grounding, RS485 direction hardware and drive IDs.
- Start with read-only diagnostics and capture the original configuration outside
  the repository before writes.
- Set conservative current, velocity and travel limits in the physical drive.
- Stop immediately on unexpected direction, sound, current, telemetry or latency.

Agents must not initiate actuation merely to prove that a build succeeded.

## Production release gates

A candidate is not a production baseline until all of these have executable
evidence:

- Boot, brownout and reset always produce bounded safe output behavior.
- RC loss, source timeout, stale telemetry, drive fault and bus loss are injected.
- Arbitration and state transitions are covered by host tests and hardware tests.
- Worst-case control/safety latency is measured during Wi-Fi reconnect, LAN load,
  OTA check and heavy logging.
- OTA success, interrupted download, invalid manifest, invalid signature, rollback
  and power loss are tested.
- NVS corruption and invalid profile cannot enable outputs.
- Memory, task stack, watchdog and long-duration stability margins are recorded.
- A wiring-specific commissioning checklist and emergency procedure exist outside
  the generic firmware repository.
