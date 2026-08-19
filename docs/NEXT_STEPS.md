# Firmware handoff state

This is a compact status handoff, not authorization for a hardware test or a new
feature. Recheck the current code, tests and task scope before acting.

## Current baseline

- Firmware build identity is version `1.0.0`, build `27`; the runtime topology is a
  build-selected immutable C profile.
- Supported compositions are `current_robot`, `bench_single_svd48_motor`, `rafa`,
  and the unqualified `bench_single_steering_as5600` development slice.
- `SET_SPEED` and stop paths use the application port/coordinator, but several
  hardware-changing legacy paths still bypass it. For profiles with validated
  differential geometry, NEXT-3 now activates `control_lan → command_authority →
  motion_application/robot_kinematics → traction endpoints`. `robot_state` remains
  inactive.
- Rafa has one SVD48 at RS485 address `2`; M1/M2 physical wheel identity and
  direction remain unqualified. Deploy Rafa firmware changes by OTA only.
- The NEXT-1 maintenance contract now exposes typed N-controller SVD48 inventory,
  cached physical-channel telemetry and bounded bench commands. It bypasses neither
  the application coordinator nor the four-binding legacy limit, and it is not the
  continuous control transport. Build 27 physical Rafa inventory, telemetry,
  parameter reads and a restored volatile wheel-diameter write passed. Typed Bench
  Control remains unaccepted after a pre-actuation HTTP 400 on the first M1 HOLD;
  no speed command was attempted.
- NEXT-3 adds the dedicated UDP `32322` session protocol and Console `/control` UI.
  Host/model tests cover ARM/DISARM, stream/sequence replay, deadman release, exact
  TTL expiry, retired streams and STOP semantics. The current `rafa` profile correctly
  reports control unavailable because its physical side/sign mapping is still open.
  Build 27 OTA and the unavailable/ARM gate passed; no Rafa loss-path motion test was
  attempted or accepted.

## Gates that remain open

1. Treat every Rafa motion or parameter-write investigation as a separately
   authorized elevated bench session under [Safety](SAFETY.md) and the testing
   contract. Resolve physical channel identity/direction and the M2 feedback anomaly
   with independent observation; controller feedback alone is insufficient.
2. Keep Maintenance LAN out of continuous motion and do not add PPM authority. Before
   enabling NEXT-3 on Rafa, qualify M1/M2 side and positive direction, encode that
   mapping in its immutable profile, deploy by OTA, then measure key-release, browser
   close, backend loss and LAN-loss expiry with wheels elevated and an independent
   physical cut-off. A software/model pass is not that evidence.
3. Keep the AS5600 steering slice development-only until its explicit reference,
   calibration, sensor and closed-loop physical gates have evidence.
4. Generalize only a contract required by a real profile or console workflow. In
   particular, a greater-than-four legacy channel migration is deferred until a real
   profile requires it.

For a concrete change, use the route list in [the documentation index](README.md)
rather than reviving an archived plan wholesale.
