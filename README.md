# BotFarms ESP32-S3 motion firmware

ESP-IDF firmware for an ESP32-S3 connected to Fulling SVD48 motor drives over
RS485. It provides serial diagnostics, low-priority Wi-Fi maintenance, OTA and
host-tested building blocks for a future profile-driven robot architecture.

> **Status: bench firmware, not a production motion controller.** Build 19 has
> unresolved safety gaps. Keep wheels off the ground and an independent power
> cut-off available whenever actuation is possible. See [Safety](docs/SAFETY.md).

## Current behavior

- Target: `esp32s3`, ESP-IDF 5.4.1, 16 MB flash with dual OTA slots.
- RS485: UART2, TX GPIO17, RX GPIO16, 115200 baud, drives 1 and 2.
- RC input: PPM on GPIO14; the signal must be limited to 3.3 V.
- Four logical traction motors are mapped through two dual-channel SVD48 drives.
- Steering servo support exists but is disabled in the active configuration.
- Wi-Fi reconnect, manifest checks, OTA announcements and maintenance LAN run as
  low-priority services. The main loop itself only sleeps.
- Hardware configuration is currently compiled into `main/main.c`; runtime JSON
  robot profiles have not been implemented.

The intended architecture and the difference between active and dormant modules
are documented in [Architecture](docs/ARCHITECTURE.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `main/` | Firmware composition root and build identity |
| `components/` | ESP-IDF drivers, services and pure domain modules |
| `tests/host/` | Native host tests for hardware-independent logic |
| `tools/` | Serial, LAN and OTA command-line tools |
| `docs/` | Current technical contracts only |
| `partitions_ota_16mb.csv` | 16 MB OTA partition layout |
| `sdkconfig.defaults` | Versioned project defaults |

## Read first

1. [Architecture](docs/ARCHITECTURE.md)
2. [Safety](docs/SAFETY.md)
3. [Command API](docs/API.md)
4. [SVD48 integration](docs/SVD48.md) when changing RS485 behavior
5. [OTA](docs/OTA.md) when building or deploying releases
6. [Roadmap](docs/ROADMAP.md) before adding architectural features

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
python3 tools/robotctl.py --port /dev/ttyACM0 raw SAFETY_STATUS
```

Provision Wi-Fi and separate OTA/maintenance tokens only over a trusted serial
connection. Commands and response formats are listed in [Command API](docs/API.md).
Do not put Wi-Fi credentials in this repository.

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
- Offline or stale motor telemetry is not yet promoted to a safety fault.
- Maintenance LAN has no command lease, authority arbitration or deadman.
- Reported SVD48 speed still labels a raw 0.1 RPM register value as RPM.
- Servo output has no position feedback and cannot prove physical position.
- Clean ESP-IDF 5.4.1 build leaves only 1 byte of reported IRAM headroom.
- `robot_state`, `command_authority`, `robot_kinematics` and `control_lan` are
  compiled foundations but are not wired into the active runtime.
- Firmware authenticity relies on a manifest SHA-256 checksum, not signed images
  with a protected trust root.

These items are release gates, not documentation footnotes. Their implementation
order is maintained in [Roadmap](docs/ROADMAP.md).
