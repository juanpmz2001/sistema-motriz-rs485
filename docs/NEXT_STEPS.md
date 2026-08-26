# Firmware handoff state

This is a compact status handoff, not authorization for a hardware test or a new
feature. Recheck the current code, tests and task scope before acting.

## Current baseline

- Firmware source identity is version `1.0.0`, candidate build `35`; the last
  independently verified installed artifact remains build `34` in
  [Rafa bench state](robots/RAFA_BENCH_STATE.md). The runtime topology is a
  build-selected immutable C profile.
- Supported compositions are `current_robot`, `bench_single_svd48_motor`, `rafa`,
  and the unqualified `bench_single_steering_as5600` development slice.
- `SET_SPEED` and stop paths use the application port/coordinator, but several
  hardware-changing legacy paths still bypass it. For profiles with validated
  differential geometry, NEXT-3 now activates `control_lan → command_authority →
  motion_application/robot_kinematics → traction endpoints`. `robot_state` remains
  inactive.
- Rafa has one SVD48 at RS485 address `2`. Operator qualification maps M1 to the
  right wheel with positive RPM forward and M2 to the left wheel with negative RPM
  forward. OTA-verified build 35 uses 0.20 m radius, 1.52 m center-to-center track,
  direct drive, ±40 RPM endpoint limits, 0.8 m/s max vx and pi/6 rad/s max wz. Its
  startup/status verification is not physical motion acceptance. Deploy Rafa firmware
  changes by OTA only.
- The NEXT-1 maintenance contract now exposes typed N-controller SVD48 inventory,
  cached physical-channel telemetry and bounded bench commands. It bypasses neither
  the application coordinator nor the four-binding legacy limit, and it is not the
  continuous control transport. Build 27 physical Rafa inventory, telemetry,
  parameter reads and a restored volatile wheel-diameter write passed. One traced M1
  HOLD later passed end to end, but M1 HOLD/DISABLE and M2 acceptance remain incomplete
  after intermittent read-only Console preflight HTTP 400 responses. No speed command
  was attempted.
- NEXT-3 adds the dedicated UDP `32322` session protocol and Console `/control` UI.
  Host/model tests cover ARM/DISARM, stream/sequence replay, deadman release, exact
  TTL expiry, retired streams and STOP semantics. Build 35 updates only profile
  geometry/limits and PPM calibration/CH6 scaling; it does not change
  authority, deadman or differential kinematics. The first elevated bounded motion
  smoke remains to be recorded; no Rafa browser/backend/LAN-loss motion test has yet
  been accepted.

## Gates that remain open

1. Treat every Rafa motion or parameter-write investigation as a separately
   authorized elevated bench session under [Safety](SAFETY.md) and the testing
   contract. Resolve the historical M2 feedback anomaly during operator work;
   controller feedback alone is insufficient for a new physical claim.
2. Keep Maintenance LAN out of continuous motion and do not add PPM authority. Perform
   the single bounded elevated Rafa smoke for the encoded mapping, then
   separately measure key-release, browser close, backend loss and LAN-loss expiry
   with wheels elevated and an independent physical cut-off. A software/model pass is
   not that evidence.
3. Keep the AS5600 steering slice development-only until its explicit reference,
   calibration, sensor and closed-loop physical gates have evidence.
4. Generalize only a contract required by a real profile or console workflow. In
   particular, a greater-than-four legacy channel migration is deferred until a real
   profile requires it.

For a concrete change, use the route list in [the documentation index](README.md)
rather than reviving an archived plan wholesale.
