# SVD48 Robot Control and Telemetry Framework

ESP-IDF firmware for an ESP32-S3 connected to UU Motor SVD48V50A/SVD48B50A hub motor controllers over RS485.

The goal is to make the robot controllable from a PC terminal with simple ASCII commands:

```text
GET_SPEED 0
DATA MOTOR_0 RPM:1450 STALE:0 ONLINE:1

MOVE_VEL 1.0 0.0 0.5
OK MOVE_VEL VX:1.000 VY:0.000 WZ:0.500 ...
```

## Start Here

- Controller knowledge base: `docs/skills/SVD48B50A_SKILL.md`
- PC command contract: `docs/API.md`
- Prototype platform safety contract: `docs/TONO_PLATFORM_SAFETY_CONTRACT.md`
- OTA tutorial: `ota_documentation_for_dummies.md`
- OTA agent skill: `docs/skills/OTA_UPDATE_SKILL.md`
- Downloaded controller sources: `docs/controllers/SVD48B50A/`
- Python CLI: `tools/robotctl.py`
- Host protocol tests: `tools/test_svd48_protocol.py`

## Firmware Architecture

- `components/svd48`: serialized RS485 driver for two SVD48 drives / four logical motors. Implements reads, writes, telemetry polling, UU Motor CRC byte order, and stale/fault state.
- `components/robot_control`: maps four logical motors into robot commands. Implements `MOVE_VEL vx vy wz` as independent steering kinematics and controls four PWM steering servos.
- `components/robot_safety`: high-priority safety supervisor. It watches RC/i-BUS signal loss after a valid signal is seen and online motor fault telemetry, then requests `STOP ALL` without doing Wi-Fi, HTTP, JSON, OTA or NVS work.
- `components/ibus_receiver`: low-latency FlySky i-BUS/SBUS receiver input used by the safety supervisor and diagnostics.
- `components/serial_gateway`: ASCII gateway over the ESP-IDF console/USB serial stream. Emits `OK`, `ERR`, and `DATA` responses.
- `components/config_manager`: persists Wi-Fi and OTA settings in NVS.
- `components/wifi_manager`: Wi-Fi station mode used by manual OTA and background OTA checks. A low-priority supervisor auto-connects on boot and reconnects with backoff when credentials are saved.
- `components/ota_manager`: manifest validation, inactive-slot download/verification, manual boot-slot switch, rollback state, and automatic manifest-only checks.
- `components/ota_announce`: low-priority authenticated UDP listener for LAN OTA announcements from developer laptops.
- `main`: initializes NVS, config, Wi-Fi/OTA managers, RS485, telemetry polling, robot control, i-BUS, safety, serial gateway, rollback self-test, then low-priority Wi-Fi/OTA services.

The old Arduino sketches and previous ESP-IDF Bluetooth/PPM components are kept as historical reference, but the active firmware path is the SVD48 framework above.

## Default Hardware Assumptions

- ESP32-S3.
- UART2 RS485 TX/RX: GPIO 17 / GPIO 16.
- Two SVD48 drives on RS485 IDs `1` and `2`.
- Logical motor mapping:
  - `MOTOR_0`: drive 1 M1, front-left.
  - `MOTOR_1`: drive 1 M2, front-right.
  - `MOTOR_2`: drive 2 M1, rear-left.
  - `MOTOR_3`: drive 2 M2, rear-right.
- Steering servo GPIO defaults: 4, 5, 6, 7.

Adjust these defaults in `main/main.c` before flashing if the harness differs.

## PC Usage

```bash
python tools/robotctl.py --port COM6 ping
python tools/robotctl.py --port COM6 platform-status
python tools/robotctl.py --port COM6 get-speed 0
python tools/robotctl.py --port COM6 get-motor 0
python tools/robotctl.py --port COM6 move-vel 1.0 0.0 0.5
python tools/robotctl.py --port COM6 watch
```

Use `raw` for gateway commands not wrapped by a dedicated subcommand, for example:

```bash
python tools/robotctl.py --port COM6 raw OTA_AUTO_STATUS
python tools/robotctl.py --port COM6 raw READ_REG 1 0x5410 2
```

Install the only PC dependency with:

```bash
python -m pip install pyserial
```

## Build

Use an ESP-IDF shell with the target set to ESP32-S3:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```

The current firmware defaults to `esp32s3`, 16 MB flash, `partitions_ota_16mb.csv`, and ESP-IDF rollback support.

## OTA Update Tutorial

Full beginner-oriented OTA notes live in `ota_documentation_for_dummies.md`. Agents should also read `docs/skills/OTA_UPDATE_SKILL.md` before touching OTA code, manifests, rollback, partitions or release steps.

### Direct Developer Flash

Use USB serial flashing for normal development and recovery:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py set-target esp32s3
idf.py build
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*
idf.py -p /dev/ttyACM0 flash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
python3 tools/robotctl.py --port /dev/ttyACM0 raw SAFETY_STATUS
```

### Real OTA Flow

1. Increment `FW_BUILD_NUMBER` in `main/app_version.h` for every real OTA update. Keep `FW_TARGET` as `esp32s3`; OTA download/update rejects the same or a lower build number.

2. Build the firmware:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py build
```

Do not reuse a build number with different firmware bytes. `tools/ota_prepare_release.py` refuses stale binaries and refuses to overwrite an existing release file with different content for the same build number.

3. Find your computer's LAN IP, reachable by the ESP32:

```bash
ip route get 8.8.8.8 | awk '{print $7; exit}'
```

4. Generate the OTA release directory and manifest. Do not hand-write `size` or `sha256`.

```bash
python3 tools/ota_prepare_release.py --host 192.168.1.107 --port 8080
```

5. Serve the release from another terminal:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

6. Configure the ESP32 gateway:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw WIFI_CONNECT
python3 tools/robotctl.py --port /dev/ttyACM0 raw WIFI_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_SET_SERVER 192.168.1.107 8080
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_SET_MANIFEST /api/firmware/latest
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ANNOUNCE_TOKEN_SET "choose-a-long-dev-token"
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_CONFIG
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ANNOUNCE_STATUS
```

Use `WIFI_SET "ssid" "password"` first only if credentials are missing or wrong. Be aware that your shell history may store that password.
After credentials are saved, the ESP32 auto-connects on boot and reconnects with low-priority backoff. `WIFI_DISCONNECT` pauses that auto-connect behavior until `WIFI_CONNECT` or `WIFI_SET` is used again.

7. Validate before rebooting:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_CHECK
python3 tools/robotctl.py --port /dev/ttyACM0 --timeout 30 raw OTA_DOWNLOAD_TEST
```

`OTA_CHECK` validates only the manifest. `OTA_DOWNLOAD_TEST` writes and verifies the inactive OTA slot, but does not switch boot partition and does not reboot.
`OTA_DOWNLOAD_TEST` and `OTA_UPDATE` both require Wi-Fi connected and `SAFE_FOR_OTA:1`.
Both commands also require the manifest build to be strictly newer than the running firmware.

8. Stop the robot and confirm it is safe:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw STOP ALL
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Continue only when `SAFE_FOR_OTA:1 SAFE_REASON:SAFE`.

9. Run the real OTA update:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 --timeout 60 raw OTA_UPDATE
```

The ESP32 downloads the binary again, verifies SHA256, prepares/stops the robot, switches the boot partition and reboots.

10. Verify after reboot:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ROLLBACK_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Expected final state: the new `BUILD_NUMBER`, `OTA_STATE:VALID`, and `PENDING_VERIFY:0`.

### LAN OTA Announce Without USB

After the ESP32 has saved Wi-Fi credentials and `OTA_ANNOUNCE_TOKEN_SET`, another developer on the same network can point it at a local release server without USB:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
BOTFARMS_OTA_TOKEN="choose-a-long-dev-token" python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action check
BOTFARMS_OTA_TOKEN="choose-a-long-dev-token" python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action update
```

The ESP32 listens on UDP port `32320`, validates the token, uses the sender IP as the temporary OTA server host, and ignores unauthenticated packets. `--action update` is still explicit and still refuses to reboot unless the robot is safe for OTA.

Never use `localhost`, `127.0.0.1`, or `0.0.0.0` in the OTA manifest URL. The firmware intentionally rejects those hosts because the ESP32 must fetch the binary from its own network perspective, not from the developer machine's loopback interface.
