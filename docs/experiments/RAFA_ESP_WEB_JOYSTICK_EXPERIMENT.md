# Rafa ESP Web Joystick Experiment

## Isolation and base

- Branch: `experiment/rafa-esp-web-joystick`
- Worktree: `sistema-motriz-rs485-rafa-web-joystick`
- Base commit: `9adb0a9e4b437150a837b1fe004c04e5c7689bd6` (B43)

The B43 commit is the known Rafa build lineage with the current qualified
composition, safety path and elevated-bench diagnostic context.  This branch
does not alter B43, `main`, or `feature/control-rejection-diagnostics`; it
introduces a separately selected experimental build only.

## Scope and ownership

The experimental path is:

```
Safari UI -> ESP-IDF HTTP/WebSocket adapter -> WEB_DIRECT semantic event
          -> motion_application -> actuation_application -> endpoints
          -> SVD48 driver -> RS485
```

`components/web_direct_control` will own the WebSocket session, bounded
command parsing, deadzone, 300 ms lease, UI asset delivery and presentation of
cached telemetry.  It will only submit typed ARM, DISARM, STOP and COMMAND
events to `motion_application_service_publish()`.  It owns no motor registers,
wheel mixing, endpoint limits, PID values or RS485 transactions.

`motion_application`, `robot_kinematics`, `actuation_application`, endpoint
adapters, `svd48` and `robot_safety` retain their existing ownership.  The web
adapter reads telemetry only through the existing cached SVD48 workspace port;
a browser refresh never polls Modbus directly.

The experiment adds a distinct `WEB_DIRECT` authority source.  Its build-only
authority projection disables PPM motion, the RC/LAN interlock, generic
RC-loss control stopping, and the UDP Control LAN server.  PPM acquisition can
remain observational.  This is not a change to the normal Rafa policy.

## Files expected to change

- `CMakeLists.txt`, `main/CMakeLists.txt`, `main/main.c`: compose and start
  the new adapter only for the experimental profile; do not change production
  paths.
- `components/robot_profile/Kconfig` and
  `components/robot_profile/robot_runtime_authority_policy.*`: declare the
  build-selected experimental profile identity and source participation.
- `components/command_authority/*` and `components/robot_capabilities` /
  `components/motion_application`: add the explicit `WEB_DIRECT` semantic
  source and status name.
- `components/web_direct_control/*` (new): HTTP `GET /`, WebSocket
  `/control`, session/TTL model, embedded HTML/CSS/JavaScript and cached
  telemetry response.
- `tests/host/web_direct_control_test.c` and the host-test manifest: pure
  session, deadzone and lease coverage.

## Deliberately not reused

- BotFarms Engineering Console, FastAPI, React and its browser code.
- `control_lan` UDP protocol/port 32322 and its stream protocol.
- `ppm_motion_source`, PPM decoder inputs and RC as motion sources.
- Maintenance LAN commands, serial gateway raw commands, SVD48 workspace
  writes, OTA, parameter persistence and arbitrary Modbus.

## Initial operating contract

- One ephemeral WebSocket control session; a second client is observational
  until the first one disarms or disconnects.
- Deadzone: `0.10`; within it the intent is zero and deadman is false.
- Valid control updates renew a 300 ms ESP-owned lease.  Release, disconnect,
  stale lease, DISARM and software STOP withdraw movement through the typed
  application path.
- ARM sends an ARM event only: it never commands non-zero velocity.  A centred
  joystick commands zero velocity while armed.
- Web telemetry is a 5 Hz view of cached M1/M2 RPM and current.  Peak speed
  and current reset at ARM.  Torque is `N/D` unless a reviewed, motor-specific
  telemetry or torque constant is found; no estimate will be invented.

## Non-goals

No OTA, flash, physical actuation test, SVD48 parameter/PID change, SAVE,
Wi-Fi provisioning/AP mode, raw register endpoint, browser-hosted dependency,
or merge to another branch is part of this experiment.
