# OTA Documentation For Dummies

This guide explains how to update the ESP32-S3 firmware in this repository without breaking the OTA setup.

## The Short Version

There are two ways to put a new program on the ESP32-S3:

1. **Serial flash over USB**: developer/recovery path. Use this when the ESP32 is connected to your computer.
2. **OTA update over Wi-Fi**: deployment path. The ESP32 downloads a hash-verified firmware binary from a local HTTP server, writes the inactive OTA slot, switches boot partition, reboots, then validates itself.

For normal development, serial flashing is simpler. For validating the real deployed flow, use OTA.

## What This Firmware Actually Supports

The current firmware supports:

- Target: `esp32s3`.
- Flash size: 16 MB.
- Partition table: `partitions_ota_16mb.csv`.
- OTA slots:
  - `ota_0` at `0x30000`, size `0x600000`.
  - `ota_1` at `0x630000`, size `0x600000`.
- Rollback: enabled with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- Manifest transport: plain HTTP.
- OTA manifest path default: `/api/firmware/latest`.
- OTA server default: `192.168.10.10:8080`.
- Wi-Fi behavior: saved credentials auto-connect on boot and reconnect with low-priority backoff.
- Automatic checks: manifest-only.
- Automatic writes/updates: disabled; `OTA_AUTO_UPDATE ON` is intentionally rejected.
- LAN OTA announce: authenticated UDP on port `32320`; the ESP32 uses the sender IP as the temporary OTA server host.

The OTA manager validates:

- `project` matches `FW_PROJECT`.
- `target` matches `FW_TARGET`.
- `build_number`, `min_supported_build`, `size`, `filename`, `url`, and `sha256` are valid.
- The URL is HTTP and is not localhost/127.0.0.1/0.0.0.0.
- The downloaded byte count and SHA256 match the manifest.

## Important Files

- `main/app_version.h`: firmware identity and `FW_BUILD_NUMBER`.
- `partitions_ota_16mb.csv`: OTA partition layout.
- `sdkconfig` and `sdkconfig.defaults`: target, flash, rollback, partition config.
- `components/ota_manager/ota_manager.c`: manifest validation, download, SHA256, boot partition switch, rollback helpers.
- `components/ota_announce/ota_announce.c`: authenticated UDP announce listener for no-USB OTA server discovery.
- `components/wifi_manager/wifi_manager.c`: station connection plus low-priority auto-connect/reconnect supervisor.
- `components/robot_safety/robot_safety.c`: high-priority RC/motor-fault safety supervisor.
- `components/serial_gateway/serial_gateway.c`: serial commands such as `OTA_CHECK`, `OTA_DOWNLOAD_TEST`, `OTA_UPDATE`.
- `tools/ota_prepare_release.py`: generates the OTA release directory and manifest.
- `tools/ota_announce.py`: announces a release server to ESP32 devices on the LAN.
- `tools/robotctl.py`: sends serial commands to the ESP32.

## Direct USB Flash For Developers

Use this when the ESP32-S3 is connected to your development machine.

1. Source ESP-IDF:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
```

2. Confirm the target:

```bash
idf.py set-target esp32s3
```

3. Build:

```bash
idf.py build
```

4. Find the port:

```bash
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*
```

5. Flash:

```bash
idf.py -p /dev/ttyACM0 flash
```

6. Verify after reboot:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Serial flash is not OTA. It is the fastest developer path and the recovery path if OTA breaks.

## OTA Update Sequence

Use this when you want to validate the deployed Wi-Fi update flow.

### 1. Increment The Build Number

Edit `main/app_version.h`.

For a real update, increase:

```c
#define FW_BUILD_NUMBER 8
```

Keep these stable unless you intentionally changed product identity:

```c
#define FW_PROJECT "sistema-motriz-rs485"
#define FW_TARGET "esp32s3"
```

The firmware rejects mismatched project or target. Downgrades and same-build releases are rejected by the download/update path. This prevents accidentally installing an older binary that reused the same build number.

### 2. Build The Firmware

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py build
```

Expected binary:

```text
build/sistema-motriz-rs485.bin
```

### 3. Find Your Computer's LAN IP

Use an IP that the ESP32 can reach over Wi-Fi. Do not use `localhost`, `127.0.0.1`, or `0.0.0.0` in the manifest.

On Linux, this usually works:

```bash
ip route get 8.8.8.8 | awk '{print $7; exit}'
```

Example result:

```text
192.168.1.107
```

### 4. Generate The OTA Release

Replace the host with your LAN IP:

```bash
python3 tools/ota_prepare_release.py --host 192.168.1.107 --port 8080
```

This creates:

```text
ota_release/
  api/firmware/latest
  firmware/sistema-motriz-rs485-v1.0.0-b6.bin
```

The manifest includes exact size and SHA256 for the binary.

You can inspect without writing files:

```bash
python3 tools/ota_prepare_release.py --host 192.168.1.107 --port 8080 --dry-run
```

### 5. Serve The Release

Keep this terminal open:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

From another terminal, verify your PC can serve the manifest:

```bash
curl http://192.168.1.107:8080/api/firmware/latest
```

### 6. Configure The ESP32 Serial Gateway

Find the ESP32 port:

```bash
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*
```

Set Wi-Fi only if it is not already configured:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw WIFI_SET "YourSSID" "YourPassword"
```

Then configure and connect:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw WIFI_CONNECT
python3 tools/robotctl.py --port /dev/ttyACM0 raw WIFI_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_SET_SERVER 192.168.1.107 8080
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_SET_MANIFEST /api/firmware/latest
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ANNOUNCE_TOKEN_SET "choose-a-long-dev-token"
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_CONFIG
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ANNOUNCE_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 raw SAFETY_STATUS
```

`WIFI_SET` stores credentials in NVS. The firmware redacts passwords in status/trace output, but your shell history may still contain the command.
After credentials are stored, the ESP32 connects automatically after boot and retries with backoff if the router or laptop disappears. `WIFI_DISCONNECT` pauses auto-connect until `WIFI_CONNECT` or `WIFI_SET` is used again.

### Optional: Announce A Release Without USB

Use this after the ESP32 already has Wi-Fi credentials and an OTA announce token in NVS.

Keep the HTTP server open:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

From another terminal, send an authenticated UDP announce:

```bash
BOTFARMS_OTA_TOKEN="choose-a-long-dev-token" \
  python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action check
```

For a real update:

```bash
BOTFARMS_OTA_TOKEN="choose-a-long-dev-token" \
  python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action update
```

The ESP32 listens on UDP `32320`. It ignores packets with no token or a wrong token, writes the announced server/manifest to NVS only after authentication, and uses the packet source IP as the OTA server host. `update` is still an explicit action and still refuses to run unless the robot is safe for OTA.

### 7. Check The Manifest

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_CHECK
```

Expected shape:

```text
DATA OTA_CHECK STATUS:UPDATE_AVAILABLE PROJECT:sistema-motriz-rs485 TARGET:esp32s3 VERSION:1.0.0 BUILD_NUMBER:6 CURRENT_BUILD:5 ...
```

`OTA_CHECK` does not download or write flash.

### 8. Download-Test The Image

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 --timeout 30 raw OTA_DOWNLOAD_TEST
```

Expected shape:

```text
DATA OTA_DOWNLOAD_TEST STATUS:VERIFIED PARTITION:ota_1 BYTES:994208 SHA256:<64-hex>
```

`OTA_DOWNLOAD_TEST` writes and verifies the inactive OTA partition, but it does not switch boot partition and does not reboot.
It now requires Wi-Fi connected and uses the same safety gate as `OTA_UPDATE`; if the robot is not `SAFE_IDLE`, it returns `ERR OTA_DOWNLOAD_TEST_BLOCKED ROBOT_NOT_SAFE`.
It also requires the manifest build to be strictly newer than the running firmware; stale same-build releases fail with `BUILD_NOT_NEWER`.

### 9. Make The Robot Safe

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw STOP ALL
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Continue only if the status is safe for OTA:

```text
SAFE_FOR_OTA:1 SAFE_REASON:SAFE
```

`OTA_UPDATE` will refuse to run if Wi-Fi is not connected or the robot is not safe.

### 10. Run The Real OTA Update

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 --timeout 60 raw OTA_UPDATE
```

Expected before reboot:

```text
DATA OTA_UPDATE STATUS:REBOOTING PARTITION:ota_1 BYTES:994208 SHA256:<64-hex>
```

The ESP32 reboots automatically.

### 11. Verify After Reboot

Wait a few seconds, then run:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ROLLBACK_STATUS
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Expected:

```text
DATA VERSION ... BUILD_NUMBER:6 PARTITION:ota_1 OTA_STATE:VALID PENDING_VERIFY:0 ...
DATA OTA_ROLLBACK ... OTA_STATE:VALID PENDING_VERIFY:0 ...
```

On first boot after OTA, ESP-IDF may mark the image as `PENDING_VERIFY`. This firmware marks it `VALID` only after NVS, config, SVD48 polling, robot control, high-priority `robot_safety`, and serial gateway start successfully. Wi-Fi and OTA are maintenance services; failure to associate Wi-Fi or initialize the OTA service does not by itself trigger rollback or block the robot.

## Rollback Tests

Rollback test commands are for lab validation only:

```bash
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ROLLBACK_TEST NO_CONFIRM_ONCE
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ROLLBACK_TEST SELF_TEST_FAIL_ONCE
python3 tools/robotctl.py --port /dev/ttyACM0 raw OTA_ROLLBACK_TEST NONE
```

Use these only when you are intentionally validating rollback behavior and have serial recovery available.

## Common Failures

`ERR OTA_CHECK_FAILED ... DETAIL:BAD_SERVER_CONFIG`

- Host is empty, includes `://`, is localhost, or manifest path is invalid.

`ERR OTA_CHECK_FAILED ... DETAIL:BAD_URL`

- Manifest `url` is invalid or points to localhost/127.0.0.1/0.0.0.0.

`ERR OTA_CHECK_FAILED ... DETAIL:PROJECT_MISMATCH`

- Manifest project does not match `FW_PROJECT`.

`ERR OTA_CHECK_FAILED ... DETAIL:TARGET_MISMATCH`

- Manifest target does not match `FW_TARGET`.

`ERR OTA_DOWNLOAD_TEST_FAILED ... DETAIL:SHA256_MISMATCH`

- Binary changed after generating the manifest, or the server is serving a different file.

`ERR OTA_DOWNLOAD_TEST_BLOCKED ROBOT_NOT_SAFE`

- Stop the robot and verify `PLATFORM_STATUS` reports `SAFE_FOR_OTA:1`.

`ERR OTA_DOWNLOAD_TEST_BLOCKED WIFI_NOT_CONNECTED`

- Wait for auto-connect or run `WIFI_CONNECT`, then verify `WIFI_STATUS`.

`ERR OTA_UPDATE_BLOCKED WIFI_NOT_CONNECTED`

- Run `WIFI_CONNECT`, wait, and verify `WIFI_STATUS`.

`ERR OTA_UPDATE_BLOCKED ROBOT_NOT_SAFE`

- Run `STOP ALL`, check `PLATFORM_STATUS`, and confirm motors are not running.

`ERR OTA_UPDATE_SET_BOOT_FAILED`

- The inactive partition label could not be selected. Stop and use serial flash recovery if needed.

UDP announce returns `TOKEN_NOT_CONFIGURED`

- Set a token once over serial: `OTA_ANNOUNCE_TOKEN_SET "choose-a-long-dev-token"`.

UDP announce returns `BAD_TOKEN`

- The script token does not match the token stored on the ESP32.

## Rules For Agents And Developers

- Do not change `FW_TARGET` away from `esp32s3`.
- Do not remove rollback support.
- Do not replace `partitions_ota_16mb.csv` unless you are deliberately redesigning OTA storage.
- Do not hand-write SHA256 or size in the manifest; use `tools/ota_prepare_release.py`.
- Do not use localhost in OTA URLs.
- Do not enable automatic OTA writes. They are intentionally blocked.
- Do not share or print OTA announce tokens in logs or docs.
- Keep Wi-Fi/OTA work lower priority than RC, SVD48 polling and safety.
- Always keep USB serial recovery available when validating a new OTA flow.
