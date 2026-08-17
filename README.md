# BotFarms ESP32-S3 motion firmware

ESP-IDF firmware for an ESP32-S3 connected to Fulling SVD48 motor drives over
RS485. It provides serial diagnostics, low-priority Wi-Fi maintenance, OTA and
an active build-time robot profile with profile-driven buses, devices, channels
and typed actuator endpoints.

> **Status: bench firmware, not a production motion controller.** Build 20 has
> unresolved safety gaps. Keep wheels off the ground and an independent power
> cut-off available whenever actuation is possible. See [Safety](docs/SAFETY.md).

## Current behavior

- Target: `esp32s3`, ESP-IDF 5.4.1, 16 MB flash with dual OTA slots.
- RS485: UART2, TX GPIO17, RX GPIO16, 115200 baud; `current_robot`
  configures addresses 1 and 2, while the SVD48 bench and `rafa` profiles use
  address 1.
- RC input: PPM on GPIO14; the signal must be limited to 3.3 V.
- The `current_robot` profile maps four logical traction motors through two
  dual-channel SVD48 controllers. The `bench_single_svd48_motor` profile maps
  only logical index `0` to M1 of one controller and has no motion geometry.
- The `rafa` profile maps one SVD48 at address 1 to the neutral endpoint names
  `rafa_traction_m1` and `rafa_traction_m2`, configures PPM on GPIO14 and has no
  motion geometry until channel side/sign are physically qualified.
- A separate build-selected `bench_single_steering_as5600` development profile
  composes one motor-mode PWM output, one AS5600 position sensor and one local
  steering controller. It has no traction geometry and is not a qualified robot
  profile; its PWM output is not a conventional position-servo command.
- On a supported normal startup, Wi-Fi reconnect, manifest checks, OTA announcements
  and maintenance LAN run as low-priority services. The main loop itself only sleeps.
- Board, bus, device and endpoint configuration is selected from an immutable C
  profile in `components/robot_profile`; runtime JSON/YAML loading is not implemented.
- One shared `rs485_transport` owns and serializes the physical UART bus. Each
  configured controller is one `svd48_device` with explicit M1 and M2 channels;
  a shared polling service schedules all configured devices.
- `SET_SPEED`, `STOP n`, `STOP ALL`, boot stop and safety stop use an application
  port backed by the serialized `actuation_coordinator` and the direct SVD48
  channel endpoint adapter.
- The full serial gateway can enumerate logical endpoint IDs, capabilities and
  criticality, command/stop velocity or position through that application port, and
  read typed observations. Velocity observations are controller-derived; the steering
  profile's position observation is supplied by its separate AS5600 endpoint.
  Explicit position reference is a separately confirmed maintenance operation, never
  automatic homing. These commands are not exposed by the LAN-safe or diagnostic-only
  policies.
- `VERSION` reports the build Git SHA/dirty state and `PROFILE_STATUS` reports the
  board profile. A host-only manifest runner uses those fields for bounded HIL
  orchestration; it is not a motion authority or a physical-test result.
- A schema-valid profile that cannot be composed enters a restricted serial
  diagnostic mode without constructing or enabling actuator outputs, except that a
  pending-verification OTA image follows rollback handling instead.

Start with the [documentation index](docs/README.md). The implemented runtime and
the difference between active, transitional and dormant modules are documented in
[Architecture](docs/ARCHITECTURE.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `main/` | Firmware composition root and build identity |
| `components/` | ESP-IDF drivers, services and pure domain modules |
| `tests/host/` | Native host tests for hardware-independent logic |
| `tests/hil/` | Host-run physical-test manifests, safety rules and fake-only runner tests |
| `tools/` | Serial, LAN and OTA command-line tools |
| `docs/` | Indexed current contracts, target design, migration records and history |
| `partitions_ota_16mb.csv` | 16 MB OTA partition layout |
| `sdkconfig.defaults` | Versioned project defaults |

## Read first

1. [Documentation index](docs/README.md)
2. [Architecture](docs/ARCHITECTURE.md)
3. [Safety](docs/SAFETY.md)
4. [Command API](docs/API.md)
5. [SVD48 integration](docs/SVD48.md) when changing RS485 behavior
6. [OTA](docs/OTA.md) when building or deploying releases
7. [Field-ready roadmap](docs/FIELD_READY_ITERATION_ROADMAP.md) for pre-field work
8. [Platform roadmap](docs/ROADMAP.md) for longer-horizon architectural features

Agent-specific rules are in [AGENTS.md](AGENTS.md).

## Build and test

Use ESP-IDF 5.4.1. A matching devcontainer is included. For a local ESP-IDF
installation:

```bash
source /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py set-target esp32s3
idf.py build
```

Run the hardware-independent tests before building firmware:

```bash
tools/run_host_tests.sh
BOTFARMS_HOST_TEST_SANITIZERS=ON tools/run_host_tests.sh
python3 tools/test_svd48_protocol.py
python3 tools/test_dependency_contracts.py
python3 tools/test_application_compatibility.py
python3 tools/test_firmware_identity.py
python3 tools/test_as5600_linearity.py
python3 -m unittest tests.hil.test_hil_runner
```

GitHub Actions is configured to repeat the host suite, ASan/UBSan suite and clean
ESP-IDF 5.4.1 builds for every versioned profile fragment under `ci/`, including the
isolated steering bench and Rafa profiles. The workflow never flashes or contacts
robot hardware.

Build Rafa reproducibly without changing a shared `sdkconfig`:

```bash
idf.py -D SDKCONFIG=/tmp/sdkconfig-rafa \
  -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults;$PWD/ci/sdkconfig.rafa.defaults" \
  -B /tmp/build-rafa set-target esp32s3
idf.py -D SDKCONFIG=/tmp/sdkconfig-rafa \
  -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults;$PWD/ci/sdkconfig.rafa.defaults" \
  -B /tmp/build-rafa build
```

`build/`, `sdkconfig`, release binaries, local tokens and editor state are
generated or private and therefore ignored by Git.

## First USB provisioning

Confirm the serial port before flashing. A new board needs the bootloader,
partition table and application, so use `flash`, not only `app-flash`:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

In another terminal, use the serial CLI for diagnostics:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 raw PLATFORM_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw PROFILE_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw COMPOSITION_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw SAFETY_STATUS
```

Provision Wi-Fi and separate OTA/maintenance tokens only over a trusted serial
connection. Commands and response formats are listed in [Command API](docs/API.md).
Do not put Wi-Fi credentials in this repository.

Rafa's one-time USB and OTA handoff is documented in the
[Rafa bootstrap runbook](docs/RAFA_BOOTSTRAP.md).

## Maintenance over LAN

Copy `.env.example` to an untracked `.env` and set the same maintenance token
that was provisioned into NVS. The tools also accept an environment variable or
`--token`; they never require the OTA token for maintenance commands.

```bash
python3 tools/esp_lanctl.py discover --broadcast 192.168.1.255
python3 tools/esp_lanctl.py status --host 192.168.1.185
python3 tools/esp_lanctl.py command --host 192.168.1.185 VERSION
python3 tools/esp_lanctl.py watch --host 192.168.1.185 --motor 0 --period-ms 200
```

UDP maintenance listens on port `32321`. Discovery is authenticated and is the
preferred way to handle dynamic IP addresses. This channel currently exposes
some actuation and confirmed register-write commands; it is therefore a trusted
bench interface, not a production remote-control boundary. The source of truth
for its allowlist is `components/serial_gateway/serial_gateway_policy.c`.

## OTA

OTA uses an independent token and UDP port `32320`. Automatic work is limited to
manifest checks; automatic installation is disabled. An explicit update is
accepted only when the current safety check reports that OTA is allowed. Follow
the complete release and recovery procedure in [OTA](docs/OTA.md).

## Known blockers

- A failed boot-time `STOP ALL` logs a warning and startup continues.
- Per-observation freshness and partial polling are represented by the SVD48
  driver, but offline, stale or degraded required endpoints are not yet promoted
  into the active safety policy.
- Maintenance LAN has no command lease, authority arbitration or deadman.
- The coordinator uses a mutex, not a priority-aware owner task; safety stop can
  wait up to 500 ms behind an in-progress driver operation.
- `ENABLE`, `MOVE_VEL`, fault clearing, OTA preparation, motor identification and
  maintenance register/configuration writes still bypass the coordinator.
- The executable factory registry supports SVD48 plus the narrow development
  motor-mode-PWM/AS5600/steering-controller chain; it is not a general runtime
  factory for arbitrary hardware.
- The compatibility `svd48_handle_t` view is limited to four channel bindings.
- SVD48 given and observed speed registers are treated as signed raw RPM without
  artificial scaling. That unconfirmed value already feeds the legacy 5-RPM
  OTA/maintenance readiness gate and `PLATFORM_STATUS`; it is not qualified safety
  evidence. A controlled future physical test must confirm the interpretation.
- The development steering path separates the PWM actuator from an AS5600
  observation endpoint, but no L2–L5 steering session has run. The provisional LUT
  and controller estimate do not prove mechanical reference or physical angle.
- Clean ESP-IDF 5.4.1 builds report 1 byte free in the dedicated 16 KiB IRAM
  category, but the ESP32-S3 linker continues into shared D/IRAM. CI enforces a
  192 KiB effective-headroom floor for every supported build profile; see the
  [field-ready roadmap](docs/FIELD_READY_ITERATION_ROADMAP.md). Runtime heap, stack
  and timing qualification remain open.
- `robot_state`, `command_authority`, `robot_kinematics` and `control_lan` are
  compiled foundations but are not wired into the active runtime.
- Firmware authenticity relies on a manifest SHA-256 checksum, not signed images
  with a protected trust root.

These items are release gates, not documentation footnotes. Pre-field ordering is
maintained in the [field-ready roadmap](docs/FIELD_READY_ITERATION_ROADMAP.md); the
[platform roadmap](docs/ROADMAP.md) owns the longer-horizon sequence.
