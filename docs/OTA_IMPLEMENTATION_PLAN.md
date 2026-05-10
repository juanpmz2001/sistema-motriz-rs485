# OTA Implementation Plan

Date: 2026-05-09  
Firmware repo: `/mnt/windows/Users/juanp/OneDriveShouldDie/Documents/BotFarms/sistema-motriz-rs485`  
Web/backend repo: `/home/jp/Documents/botfarms/web_controll_esp_svd48`

## 1. Executive Summary

The goal is to add a local pull-based OTA update system to the ESP32-S3 robot firmware. The ESP32-S3 will connect to a local Wi-Fi network, periodically query a firmware manifest served by a local backend running on the development computer, decide whether a newer build exists, download the `.bin`, verify `size` and `sha256`, write it to an inactive OTA partition, switch boot partition, stop the robot safely, reboot, and validate the new application using ESP-IDF rollback support.

The proposed architecture is:

- ESP32-S3 firmware keeps the existing RS485 robot control stack.
- New firmware components are added incrementally:
  - `config_manager`: persistent NVS settings.
  - `wifi_manager`: Wi-Fi station first, SoftAP provisioning later.
  - `ota_manager`: manifest pull, download, verification, OTA write, rollback validation.
- Existing `serial_gateway` remains available for all diagnostics and manual control.
- Local backend in `web_controll_esp_svd48` serves static UI, firmware binaries, health checks, and JSON manifests.
- Initial development uses HTTP on the LAN. The design must keep a clean path to HTTPS and signed firmware later.

First implementation recommendation:

1. Restore/confirm the ESP-IDF toolchain and measure real flash size with `flash_id`.
2. Add firmware version/build diagnostics.
3. Migrate to OTA partitions only after proving that the final OTA slot size is safe.
4. Add NVS and Wi-Fi after partition migration.
5. Add manifest check before any OTA write.
6. Add real OTA only as a manual serial command first.
7. Add automatic low-priority manifest checks last. Keep automatic OTA writes disabled until manual `OTA_UPDATE`, rollback, and serial recovery are proven end to end.

Do not implement SoftAP provisioning, HTTPS, firmware signing, secure boot, flash encryption, or automatic OTA writes in the first code iteration. Those are important, but they should come after the base OTA path is measurable and recoverable.

## 2. Real Repository Diagnosis

### 2.1 Files Inspected

Firmware:

- `CMakeLists.txt`
- `sdkconfig`
- `sdkconfig.defaults`
- `main/main.c`
- `main/CMakeLists.txt`
- `components/svd48/CMakeLists.txt`
- `components/svd48/svd48.c`
- `components/robot_control/CMakeLists.txt`
- `components/robot_control/robot_control.c`
- `components/serial_gateway/CMakeLists.txt`
- `components/serial_gateway/serial_gateway.c`
- `build/project_description.json`
- `build/flasher_args.json`
- `build/partition_table/partition-table.bin`
- `build/sistema-motriz-rs485.bin`
- `docs/API.md`
- `docs/skills/SVD48B50A_SKILL.md`

Web/backend:

- `/home/jp/Documents/botfarms/web_controll_esp_svd48/package.json`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/README.md`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/index.html`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/src/app.js`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/src/styles.css`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/src/theme.css`
- `/home/jp/Documents/botfarms/web_controll_esp_svd48/docs/esp-trace-contract.md`

### 2.2 Firmware Configuration Detected

Target detected from `sdkconfig` and `build/project_description.json`:

```text
CONFIG_IDF_TARGET="esp32s3"
target: esp32s3
```

ESP-IDF version detected from build artifact:

```text
git_revision: v5.4.1
idf_path: /tmp/esp-idf-v5.4.1
```

Important limitation found during the first audit:

```text
/tmp/esp-idf-v5.4.1/export.sh: No such file or directory
python3 -m esptool: No module named esptool
```

The previous build artifacts show ESP-IDF v5.4.1, but the shell initially no longer had the temporary IDF installation or `esptool` module available. Iteration 0 restored a persistent ESP-IDF v5.4.1 installation at `/home/jp/esp/esp-idf-v5.4.1`.

Current partition config from `sdkconfig`:

```text
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp_large.csv"
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
# CONFIG_APP_ROLLBACK_ENABLE is not set
# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set
```

Current partition table decoded from `build/partition_table/partition-table.bin`:

| Name | Type | Subtype | Offset | Size |
|---|---|---|---:|---:|
| `nvs` | data | nvs | `0x9000` | `0x6000` |
| `phy_init` | data | phy | `0xf000` | `0x1000` |
| `factory` | app | factory | `0x10000` | `0x177000` |

Current flash settings from `build/flasher_args.json`:

```json
{
  "flash_mode": "dio",
  "flash_size": "2MB",
  "flash_freq": "80m"
}
```

Iteration 0 `flash_id` detected physical flash size `16MB`. This means the current firmware configuration still artificially limits flashing/partition generation to `2MB`. Do not change it until the OTA partition migration iteration is explicitly approved, but the next partition design should target `16MB` and update `CONFIG_ESPTOOLPY_FLASHSIZE` accordingly.

Current binary sizes from build artifacts:

| Artifact | Size |
|---|---:|
| `build/sistema-motriz-rs485.bin` | `261632` bytes |
| `build/bootloader/bootloader.bin` | `21024` bytes |
| `build/partition_table/partition-table.bin` | `3072` bytes |

Current app partition margin:

```text
factory partition: 0x177000 = 1536000 bytes
current app: 261632 bytes
current margin in factory partition: 1274368 bytes
```

Active project components from root `CMakeLists.txt`:

```cmake
set(COMPONENTS main svd48 robot_control serial_gateway)
```

This means these local components are present but not active in the current build:

- `bluetooth_controller`
- `motor_controller`
- `ppm_decoder`

Relevant current runtime flow from `main/main.c`:

1. Log startup.
2. Configure SVD48 UART2 RS485 bus.
3. Initialize `svd48`.
4. Initialize `robot_control`.
5. Start SVD48 telemetry polling.
6. Initialize and start `serial_gateway`.
7. Stay alive in a low-frequency main loop.

Current RS485 settings:

```text
UART2
TX pin 17
RX pin 16
baud 115200
drive IDs {1, 2}
response_timeout_ms 100
retries 2
telemetry_period_ms 30
stale_timeout_ms 1000
```

Current serial gateway is the only external command interface. This is valuable and should remain available through all OTA work.

### 2.3 Flash Size Status

Configured flash size is `2MB`.

Real physical flash size detected during Iteration 0 is `16MB`:

```text
Manufacturer: c8
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

This is a configuration mismatch, not a hardware limitation. The firmware should remain unchanged during Iteration 0, but Iteration 2 must update `CONFIG_ESPTOOLPY_FLASHSIZE` and choose a `16MB` OTA partition table.

Detected serial device:

```text
/dev/ttyACM0
/dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00 -> ../../ttyACM0
```

This is the ESP32-S3 currently connected for Iteration 0. The user mentioned it may move to another USB port, so every flash/test iteration should first re-scan `/dev/ttyACM*`, `/dev/ttyUSB*`, and `/dev/serial/by-id`.

### 2.4 Firmware Size and Memory Risks

Current firmware is small today, but OTA growth risk is real because Wi-Fi, HTTP, JSON, NVS, rollback and diagnostics will add flash and RAM pressure.

The physical module has `16MB` flash, but the project is still configured as `CONFIG_ESPTOOLPY_FLASHSIZE="2MB"`. The next partition iteration must correct that mismatch before selecting and flashing the OTA layout.

With the current single-app layout:

```text
current app: 261632 bytes
factory app slot: 0x177000 = 1536000 bytes
current app uses: 17.0% of the current factory slot
current margin: 1274368 bytes
```

That margin is acceptable for the current firmware, but adding Wi-Fi, TCP/IP, HTTP client, JSON parsing, mbedTLS/SHA256, NVS, OTA logic and diagnostics can push the binary closer to 700-900 KB. The OTA path for this board should use the detected 16MB flash, not a constrained 2MB layout.

Risks:

- Wi-Fi and HTTP dependencies increase both flash and RAM usage.
- TLS later will add a larger flash/RAM cost than HTTP.
- JSON parsing should be constrained and defensive.
- OTA task must not allocate large buffers permanently.
- OTA task must not run at a priority that competes with RS485 or robot control.
- A failed endpoint or slow Wi-Fi must not block telemetry polling or serial commands.
- IRAM is currently `16383 / 16384` bytes. Every iteration must run `idf.py size` and explicitly check IRAM, not only flash size.

### 2.5 Web Repo Diagnosis

Current web repo is static.

Evidence:

```json
{
  "scripts": {
    "dev": "python3 -m http.server 5173"
  }
}
```

The UI currently uses Web Serial from the browser. It has no backend API, no firmware directory, no manifest generation script, and no binary serving endpoint.

Detected current LAN IP on this computer:

```text
wlo1: 192.168.10.10/24
```

For ESP32 access, the backend must listen on `0.0.0.0`, and manifest URLs must use `http://192.168.10.10:<port>/...` or another hostname resolvable by the ESP32. They must not use `localhost`, because `localhost` from the ESP32 means the ESP32 itself.

No active listener on `5173` or `8080` was confirmed by the audit command. This can change, so verify before starting a backend.

### 2.6 Backend Recommendation

Recommended backend for this repo: Node.js + Express.

Why:

- The web repo already has `package.json` and `"type": "module"`.
- The frontend is JavaScript.
- Express can serve static UI and API endpoints in one process.
- Manifest generation can be a Node script using built-in `crypto`.
- No Python virtualenv is needed for normal use.

Alternative: Python/FastAPI.

FastAPI is good if the local tooling is Python-oriented, but it adds a Python dependency stack and a separate mental model from the frontend. For this project, Express is the lower-friction choice.

Recommended server behavior:

- Listen on `0.0.0.0:8080` by default.
- Serve the existing static UI.
- Serve firmware binaries from `firmware/`.
- Generate or load `firmware/manifest.json`.
- Refuse to generate `localhost` URLs unless explicitly overridden for browser-only testing.

## 3. Evidence: Commands Executed and Results

### 3.1 Firmware Audit Commands

```bash
pwd
rg --files -g '!*build*'
ls -la
ls -la components main docs
rg -n "CONFIG_IDF_TARGET|CONFIG_ESPTOOLPY_FLASHSIZE|CONFIG_PARTITION_TABLE|CONFIG_APP_ROLLBACK|CONFIG_BOOTLOADER_APP_ROLLBACK|CONFIG_ESP_HTTPS_OTA|CONFIG_NVS|CONFIG_ESP_WIFI|CONFIG_HTTP|PROJECT_VER|version|build_number" sdkconfig sdkconfig.defaults CMakeLists.txt main components -g '!build'
find build -maxdepth 3 \( -name '*.bin' -o -name 'partition-table.bin' -o -name 'project_description.json' -o -name 'flasher_args.json' \) -printf '%p %s bytes\n'
```

Relevant results:

```text
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y
CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp_large.csv"
# CONFIG_APP_ROLLBACK_ENABLE is not set
# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set
build/sistema-motriz-rs485.bin 261632 bytes
```

### 3.2 Partition Table Decode

The binary partition table was decoded locally using a small Python parser for the ESP-IDF partition entry format.

Result:

```text
name,type,subtype,offset,size,flags
nvs,data,nvs,0x9000,0x6000,0
phy_init,data,phy,0xf000,0x1000,0
factory,app,factory,0x10000,0x177000,0
```

### 3.3 Toolchain Availability Check

Commands:

```bash
. /tmp/esp-idf-v5.4.1/export.sh
idf.py --version
python3 -m esptool --help
```

Relevant results:

```text
/tmp/esp-idf-v5.4.1/export.sh: No such file or directory
/usr/bin/python3: No module named esptool
```

Interpretation:

- The previous build was created with ESP-IDF v5.4.1.
- The shell initially could not rebuild or run `flash_id` until ESP-IDF/esptool was restored.
- Iteration 0 fixed this by installing ESP-IDF v5.4.1 under `/home/jp/esp/esp-idf-v5.4.1`.

### 3.4 USB Port Detection

Commands:

```bash
ls -la /dev/ttyACM* /dev/ttyUSB*
ls -l /dev/serial/by-id /dev/serial/by-path
```

Relevant results:

```text
/dev/ttyACM0
/dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00 -> ../../ttyACM0
```

Iteration 0 verified this is the ESP32-S3 and detected physical flash size `16MB`.

### 3.5 Web Repo Audit Commands

Commands:

```bash
cd /home/jp/Documents/botfarms/web_controll_esp_svd48
ls -la
rg --files
cat package.json
rg -n "serial|navigator\.serial|fetch|http|localhost|5173|Web Serial|manifest|firmware|server" .
```

Relevant results:

```text
"dev": "python3 -m http.server 5173"
navigator.serial usage exists in src/app.js
no backend API exists
no firmware manifest exists
```

### 3.6 Iteration 0 Execution Results

Iteration 0 was executed after the plan corrections requested on 2026-05-09.

Protection baseline:

```text
git init
git commit -m "Baseline before OTA implementation"
baseline commit: 1b6612b
```

No functional firmware source or partition changes were made after this baseline. The only tracked changes after the baseline are documentation updates to this plan.

Toolchain restore:

```text
ESP-IDF path: /home/jp/esp/esp-idf-v5.4.1
idf.py --version: ESP-IDF v5.4.1
esptool: esptool.py v4.11.0
Python env: /home/jp/.espressif/python_env/idf5.4_py3.12_env
```

Serial port detection:

```text
/dev/ttyACM0
/dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00 -> ../../ttyACM0
```

Note: Chrome had `/dev/ttyACM0` open through Web Serial. The port was released before running `flash_id`.

`flash_id` result:

```text
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Crystal is 40MHz
Manufacturer: c8
Device: 4018
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse to 3.3V
```

Build command:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py -B build_iter0_audit build
```

Result:

```text
Build successful.
sistema-motriz-rs485.bin binary size 0x3fe00 bytes.
Smallest app partition is 0x177000 bytes.
0x137200 bytes (83%) free.
```

Artifact sizes from the audit build:

| Artifact | Size |
|---|---:|
| `build_iter0_audit/sistema-motriz-rs485.bin` | `261632` bytes |
| `build_iter0_audit/bootloader/bootloader.bin` | `21024` bytes |
| `build_iter0_audit/partition_table/partition-table.bin` | `3072` bytes |

`idf.py size` result:

```text
Total image size: 261508 bytes (.bin may be padded larger)
Flash Code: 138242 bytes
Flash Data: 49548 bytes
DIRAM used: 59507 bytes / 341760 bytes, 17.41%
IRAM used: 16383 bytes / 16384 bytes, 99.99%
RTC FAST used: 52 bytes / 8192 bytes, 0.63%
```

Important risk from `idf.py size`: IRAM is almost fully allocated in the current config. Future Wi-Fi/OTA additions may not use IRAM heavily, but every iteration should keep checking IRAM and not only flash size.

`idf.py partition-table` result:

```text
# ESP-IDF Partition Table
# Name, Type, SubType, Offset, Size, Flags
nvs,data,nvs,0x9000,24K,
phy_init,data,phy,0xf000,4K,
factory,app,factory,0x10000,1500K,
```

Current mismatch to resolve in Iteration 2:

```text
Physical flash: 16MB
Configured flash: 2MB
Current partition mode: single-app large
Required future state: OTA table sized for 16MB, with CONFIG_ESPTOOLPY_FLASHSIZE updated to 16MB
```

### 3.7 Iteration 2 Execution Results

Iteration 2 was executed and approved on 2026-05-09.

Stable commit:

```text
cd512fc Migrate firmware to 16MB OTA partitions
```

Hardware and build configuration:

```text
Physical flash: 16MB
CONFIG_ESPTOOLPY_FLASHSIZE: 16MB
Partition table: partitions_ota_16mb.csv
Rollback: disabled
```

Boot validation:

```text
SPI Flash Size : 16MB
ota_0 OTA app 00030000 00600000
ota_1 OTA app 00630000 00600000
No factory image, trying OTA 0
Loaded app from partition at offset 0x30000
App version: cd512fc
```

Runtime validation:

```text
VERSION reports PARTITION:ota_0
Serial gateway: OK
PING: OK
GET_MOTOR 2: OK
GET_SVD48_CONFIG 0x02 M1: OK
STOP 2: OK
12 seconds monitor idle: no task_wdt
```

Memory result:

```text
Total image size: 261852 bytes
Flash Code: 138362 bytes
Flash Data: 49772 bytes
DIRAM: 59507 / 341760 bytes, 17.41%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Important risk: IRAM remains almost fully allocated at `16383 / 16384` bytes. Every subsequent iteration must run `idf.py size` and explicitly report IRAM.

### 3.8 Iteration 3 Execution Results

Iteration 3 was executed on 2026-05-09.

Stable commit:

```text
c626d3f Add NVS config manager diagnostics
```

Implementation summary:

```text
Added component: components/config_manager
Persistent namespace: bot_config
NVS initialized during app boot before robot subsystems
Wi-Fi connection: not implemented
OTA client/update: not implemented
Rollback: still disabled
Partition table: unchanged from Iteration 2
```

Stored defaults:

```text
wifi_ssid: <empty>
wifi_password: <empty>
ota_server_host: 192.168.10.10
ota_server_port: 8080
ota_manifest_path: /api/firmware/latest
ota_auto_check_enabled: false
ota_auto_update_enabled: false
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0x46ba0 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x5b9460 bytes, 95%
Total image size: 289584 bytes
Flash Code: 159354 bytes
Flash Data: 54388 bytes
DIRAM: 61687 / 341760 bytes, 18.05%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Flash and boot validation:

```text
Flashed on /dev/ttyACM0
SPI Flash Size: 16MB
Loaded app from partition at offset 0x30000
VERSION reports PARTITION:ota_0
```

Serial validation:

```text
VERSION: OK
VERSION reports PARTITION:ota_0
PING: OK
CONFIG_STATUS defaults: OK
OTA_CONFIG defaults: OK
CONFIG_STATUS, OTA_CONFIG, WIFI_SET, WIFI_CLEAR, CONFIG_CLEAR and OTA flags: OK
OTA_AUTO_UPDATE ON: blocked with ERR AUTO_UPDATE_DISABLED_UNTIL_MANUAL_OTA_VALIDATED
WIFI_SET: persisted SSID/password and printed PASSWORD:<set>, never the password value
TRACE + WIFI_SET: password redacted as <redacted>
OTA_SET_SERVER: OK
OTA_SET_MANIFEST: OK
OTA_AUTO_CHECK ON: OK
OTA_AUTO_UPDATE OFF: OK
Reboot persistence: OK
WIFI_CLEAR: cleared only Wi-Fi credentials
CONFIG_CLEAR: restored all defaults
GET_MOTOR 2: OK, controller online
STOP 2: OK
12 seconds monitor idle: no task_wdt
```

Important risk: IRAM is still at `16383 / 16384` bytes. The next iteration must continue reporting IRAM explicitly before flashing.

### 3.9 Iteration 4 Execution Results

Iteration 4 was implemented on 2026-05-09.

Implementation summary:

```text
Added component: components/wifi_manager
Mode: Wi-Fi station only
SoftAP/provisioning: not implemented
OTA_CHECK/OTA_UPDATE/backend: not implemented
Rollback: still disabled
Partition table: unchanged from Iteration 2
Auto-connect at boot: disabled
Manual connect command: WIFI_CONNECT
```

Design validation:

```text
wifi_manager uses esp_netif, esp_event and esp_wifi
Wi-Fi stack initializes without starting a connection
Robot startup continues even if Wi-Fi manager init fails
Connection is asynchronous from the serial command path
Connection timeout: 15000 ms
Retries: 3
States: UNCONFIGURED, DISCONNECTED, CONNECTING, CONNECTED, FAILED
Status reports SSID and IP only; password is never printed
config_manager lock waits are finite instead of portMAX_DELAY
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0xcaf50 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x5350b0 bytes, 87%
Total image size: 831188 bytes
Flash Code: 582670 bytes
Flash Data: 135456 bytes
DIRAM: 112187 / 341760 bytes, 32.83%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Flash and boot validation:

```text
Flashed on /dev/ttyACM0
SPI Flash Size: 16MB
Loaded app from partition at offset 0x30000
Wi-Fi station manager ready
SVD48 bus ready after Wi-Fi manager init
Serial gateway ready
```

Serial and hardware validation:

```text
VERSION: OK, PARTITION:ota_0
PING: OK
CONFIG_STATUS: OK
OTA_CONFIG: OK
WIFI_CLEAR + reboot: OK
WIFI_STATUS without credentials: STATUS:UNCONFIGURED
WIFI_CONNECT without credentials: ERR WIFI_NOT_CONFIGURED
WIFI_SET with invalid password: password printed only as PASSWORD:<set>
TRACE + WIFI_SET: password redacted as <redacted>
WIFI_CONNECT with invalid password: OK WIFI_CONNECT STARTED, then STATUS:FAILED
Invalid password retry result: RETRIES:3/3, IP:<none>
WIFI_DISCONNECT: OK
GET_MOTOR 2: OK, controller online
STOP 2: OK
12 seconds monitor idle: no task_wdt
```

Additional physical validation completed before Iteration 6:

```text
Firmware commit: ed53a8f Support quoted Wi-Fi credentials
WIFI_SET now accepts quoted SSID/password arguments, e.g. WIFI_SET "LAB SSID" "..."
.env is ignored by Git to protect local credentials
FW_BUILD_NUMBER bumped to 2 because a new firmware binary was flashed
ESP system event task stack increased to 4096 bytes
Serial gateway command task stack increased to 12288 bytes during Iteration 6 validation
SSID with space validated using a local test network name containing a space
Two password candidates were read from local .env without printing either password
Candidate 2 connected successfully
WIFI_STATUS: STATUS:CONNECTED, IP:192.168.1.166
PING: OK
GET_MOTOR 2: OK, controller online
STOP 2: OK
12 seconds monitor idle: no task_wdt
```

Important risk: IRAM remains at `16383 / 16384` bytes. Wi-Fi increased flash and DIRAM substantially, but did not increase the reported IRAM usage.

Note: the PC's active Wi-Fi LAN IP during this validation was `192.168.1.107`, not `192.168.10.10`. OTA tests therefore used `OTA_SET_SERVER 192.168.1.107 8080` because the ESP32 was on `192.168.1.0/24`.

## 4. Technical Decisions

### 4.1 OTA Pull Instead of Push

Alternatives:

- Push firmware from PC to ESP32.
- Pull firmware from ESP32 from a local server.

Decision:

- Use pull OTA.

Reason:

- The ESP32 can own update timing and refuse updates when the robot is moving.
- The local server can be a simple static/API server.
- The ESP32 can retry with backoff without user intervention.
- This maps cleanly to future cloud or HTTPS endpoints.

Trade-offs:

- Requires Wi-Fi configuration on the ESP32.
- Requires the ESP32 to parse a manifest and download reliably.
- Requires local server to bind to LAN and firewall to allow inbound access.

Validation:

- `OTA_CHECK` must detect versions without writing flash.
- `OTA_UPDATE` must refuse unsafe robot state.
- Pull task must be disabled until manual update is reliable.

### 4.2 HTTP Local First, HTTPS Later

Alternatives:

- Start with HTTPS and certificates.
- Start with HTTP on isolated LAN.

Decision:

- Start with HTTP local for development.

Reason:

- Faster bring-up and easier packet/debug inspection.
- Avoids TLS certificate provisioning during early firmware work.
- Hash verification still catches corruption and accidental wrong binaries.

Trade-offs:

- HTTP does not authenticate the server.
- SHA256 in a manifest served over HTTP does not prevent malicious manifest replacement.

Future hardening:

- HTTPS with pinned certificate or CA bundle.
- Signed firmware manifest.
- Signed firmware image.
- Secure boot and flash encryption after OTA flow is stable.

Validation:

- Every manifest must include `sha256` and `size`.
- ESP32 must reject missing hash, wrong hash, wrong size, malformed URL, and downgrade attempts.

### 4.3 JSON Manifest

Alternatives:

- Raw text file.
- JSON manifest.
- Binary manifest.

Decision:

- Use JSON manifest.

Reason:

- Easy to generate from backend.
- Easy to inspect with `curl`.
- Flexible enough to add metadata later.

Trade-offs:

- Requires JSON parser on ESP32.
- Must enforce small maximum response size.

Validation:

- `OTA_CHECK` must reject invalid JSON.
- Manifest fetch must cap max bytes, for example 4096 or 8192 bytes.

### 4.4 Include `size` and `sha256`

Alternatives:

- Trust HTTP content length only.
- Trust ESP-IDF image validation only.
- Verify explicit manifest metadata.

Decision:

- Verify both `size` and `sha256` before switching boot partition.

Reason:

- Size catches truncation and wrong file class quickly.
- SHA256 catches corruption or wrong binary.
- ESP-IDF image validation remains an additional safety layer.

Trade-offs:

- Must hash while streaming or after writing.
- Adds minor CPU time during OTA, but acceptable in a low-priority update path.

Validation:

- Wrong `sha256` test must leave current firmware unchanged.
- Wrong `size` test must leave current firmware unchanged.

### 4.5 Partition Strategy

Alternatives:

- Keep single app partition.
- Use built-in two OTA table.
- Use custom OTA partition table.

Decision:

- Use custom OTA partition table after real flash size is measured.

Reason:

- Current project uses `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`.
- OTA requires at least `ota_0`, `ota_1`, and `otadata`.
- Custom table allows explicit slot size and room for NVS/coredump.

Trade-offs:

- Partition migration requires full flash and can erase current NVS.
- If slot too small, future builds fail or OTA becomes impossible.
- 2 MB flash is viable but tight.

Validation:

- Run `idf.py partition-table`.
- Decode partition table.
- Run `idf.py size`.
- Confirm app size is below slot size with safety margin.

### 4.6 NVS Configuration

Alternatives:

- Hardcode Wi-Fi/server settings.
- Use compile-time Kconfig.
- Store runtime config in NVS.

Decision:

- Store runtime config in NVS.

Reason:

- No Wi-Fi secrets in source code.
- Configuration survives reboot.
- Serial gateway and SoftAP provisioning can update settings.

Trade-offs:

- Password is stored on flash unless NVS encryption is enabled later.
- Must avoid printing secrets in logs.

Validation:

- Set config over serial.
- Reboot.
- Read config with password redacted.
- Confirm no secrets appear in logs or Git.

### 4.7 Wi-Fi Station First, SoftAP Later

Alternatives:

- Implement station and SoftAP together.
- Implement station first with serial config.
- Implement SoftAP first.

Decision:

- Implement station first with serial configuration. Add SoftAP provisioning later.

Reason:

- Station mode is required for OTA.
- Serial config is simpler and already available.
- SoftAP adds HTTP server/provisioning complexity and should not block OTA base path.

Trade-offs:

- First bring-up requires serial access.
- Nontechnical provisioning comes later.

Validation:

- Wi-Fi connects with valid NVS credentials.
- Wi-Fi fails cleanly with invalid credentials.
- Robot remains controllable when Wi-Fi fails.

### 4.8 OTA Task Priority

Alternatives:

- Run OTA in main task.
- Run OTA in high-priority task.
- Run OTA in low-priority task.

Decision:

- Run OTA in a dedicated low-priority FreeRTOS task.

Reason:

- OTA is non-real-time maintenance work.
- RS485, robot control and serial gateway must stay responsive.

Trade-offs:

- OTA may take longer.
- Must coordinate shared robot state safely.

Validation:

- Telemetry and serial commands continue while endpoint is down.
- OTA task uses backoff and does not spin.
- No UART/RS485 contention is introduced.

### 4.9 Conditions to Allow or Block OTA

OTA must be blocked unless all required safety conditions are true.

Minimum conditions:

- Wi-Fi connected.
- Manifest is valid.
- Candidate build is newer and supported.
- Robot is not moving.
- No motor command is currently active.
- No critical RS485 command is in progress.
- `robot_control_stop_all()` can be executed before reboot.
- Telemetry says RPM is zero or below a threshold, if telemetry is reliable.

For current bench setup, because only controller `0x02/M1` may be connected, the safety check must be configurable and should not assume all four motors are online.

Validation:

- Try `OTA_UPDATE` while a motor command is active; it must refuse.
- Try `OTA_UPDATE` while stopped; it may proceed.

### 4.10 Rollback and Self-Test

Alternatives:

- No rollback.
- Mark app valid immediately on boot.
- Mark app valid after self-test.

Decision:

- Enable ESP-IDF rollback and mark app valid only after self-test.
- Detect `ESP_OTA_IMG_PENDING_VERIFY` early during boot, but do not mark the new app valid until the critical subsystems have initialized successfully.

Self-test should include:

- NVS initializes.
- `config_manager` initializes.
- Firmware version metadata is readable.
- `svd48` initializes.
- `robot_control` initializes.
- `serial_gateway` starts.
- `wifi_manager` initializes without blocking robot startup.
- `ota_manager` initializes.

Do not require remote server availability in the first self-test, because an offline PC or router should not cause false rollback. A later optional policy can require successful Wi-Fi association if the update specifically claims to modify Wi-Fi/OTA.

Validation:

- Firmware that marks valid stays active.
- Firmware that intentionally does not mark valid rolls back after reboot.
- Recovery procedure is documented.

### 4.11 Backend Local

Alternatives:

- Keep Python static server and add separate API.
- Replace with Node/Express backend.
- Use FastAPI.

Decision:

- Add Node/Express backend to the web repo.

Reason:

- Single local server can serve static UI and OTA API.
- Existing frontend project already uses JavaScript.
- Manifest generation is simple with Node `crypto`.

Trade-offs:

- Adds npm dependencies.
- Must document firewall and LAN IP behavior.

Validation:

- `curl http://127.0.0.1:8080/api/health` works.
- `curl http://192.168.10.10:8080/api/health` works from another device or same LAN.
- Manifest uses LAN IP, not localhost.
- SHA256 matches binary.

## 5. Proposed File Structure

### 5.1 Firmware Repo

```text
sistema-motriz-rs485/
  partitions_ota_16mb.csv
  partitions_ota_2mb.csv
  partitions_ota_4mb.csv
  partitions_ota_8mb.csv
  sdkconfig.defaults
  main/
    main.c
    app_version.h
  components/
    config_manager/
      CMakeLists.txt
      include/config_manager.h
      config_manager.c
    wifi_manager/
      CMakeLists.txt
      include/wifi_manager.h
      wifi_manager.c
    ota_manager/
      CMakeLists.txt
      include/ota_manager.h
      ota_manager.c
    serial_gateway/
      serial_gateway.c
      include/serial_gateway.h
  docs/
    OTA_IMPLEMENTATION_PLAN.md
    OTA_OPERATION.md
```

### 5.2 Web/Backend Repo

```text
web_controll_esp_svd48/
  package.json
  server.js
  firmware/
    .gitkeep
    manifest.json
    sistema-motriz-rs485-v1.0.3.bin
  scripts/
    build_firmware_manifest.js
    copy_firmware_release.js
  docs/
    ota-backend.md
  index.html
  src/
  assets/
```

Firmware binaries and generated manifests should generally not be committed unless explicitly wanted. Recommended `.gitignore` additions:

```gitignore
firmware/*.bin
firmware/manifest.json
.env
.env.local
```

## 6. Partition Table Proposals

### 6.1 Mandatory First Step

Iteration 0 already confirmed physical flash size is `16MB`. The main implementation path is therefore:

- Add `partitions_ota_16mb.csv`.
- Update `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB` in the partition migration iteration.
- Keep `2MB`, `4MB`, and `8MB` tables only as references for other boards, not as the path for this ESP32-S3.

For future boards, still detect physical flash before choosing the table. Do not choose or flash an OTA partition table until `flash_id` confirms physical flash size:

```bash
idf.py set-target esp32s3
idf.py build
python -m esptool --chip esp32s3 -p /dev/ttyACM0 flash_id
```

If `/dev/ttyACM0` is wrong, use:

```bash
ls -l /dev/serial/by-id
ls -la /dev/ttyACM* /dev/ttyUSB*
```

Because this board is physically `16MB` while the current firmware config is `2MB`, Iteration 2 must update `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB` before selecting the OTA table and rebuilding. The configured flash size, the partition table, and the physical flash size must agree.

Slot margin criterion for this phase:

- The final OTA-enabled binary must use no more than 70-75% of a single OTA slot.
- If the build exceeds 75% of the selected slot, stop and either reduce scope, choose a larger flash layout, or postpone heavier features such as HTTPS/SoftAP.

### 6.2 Main Path: Detected 16 MB Flash

Recommended `partitions_ota_16mb.csv`:

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
coredump,   data, coredump,0x12000,  0x10000,
ota_0,      app,  ota_0,   0x30000,  0x600000,
ota_1,      app,  ota_1,   0x630000, 0x600000,
storage,    data, fat,     0xc30000, 0x3d0000,
```

Slot size:

```text
0x600000 = 6291456 bytes
current app: 261632 bytes
current app uses about 4.2% of one slot
75% slot limit: 4718592 bytes
70% slot limit: 4404019 bytes
```

Recommendation:

- Use this as the primary OTA partition table for this robot.
- Keep the large slot margin while Wi-Fi, HTTP, JSON, OTA and future HTTPS/signature features are added.
- Update `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB` in the same iteration that introduces this table.

### 6.3 Reference Only: 2 MB Flash

2 MB is viable but tight.

Candidate `partitions_ota_2mb.csv`:

```csv
# Name,   Type, SubType, Offset,   Size,    Flags
nvs,      data, nvs,     0x9000,   0x4000,
otadata,  data, ota,     0xd000,   0x2000,
phy_init, data, phy,     0xf000,   0x1000,
ota_0,    app,  ota_0,   0x10000,  0xF0000,
ota_1,    app,  ota_1,   0x100000, 0xF0000,
```

Slot size:

```text
0xF0000 = 983040 bytes
current app: 261632 bytes
current margin: 721408 bytes
75% slot limit: 737280 bytes
70% slot limit: 688128 bytes
```

Risks:

- Wi-Fi + HTTP + JSON + OTA may fit, but future HTTPS and richer diagnostics may pressure the slot.
- NVS is reduced from `0x6000` to `0x4000`.
- There is little room for coredump or local filesystem.

Recommendation if flash is truly 2 MB:

- Implement minimal OTA first.
- Track binary size on every iteration.
- Stop before real OTA if the final binary exceeds the 70-75% slot limit.
- Keep SoftAP/HTTPS for later only if size allows.

### 6.4 Reference Only: 4 MB Flash

Recommended `partitions_ota_4mb.csv`:

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
coredump,   data, coredump,0x12000,  0x10000,
ota_0,      app,  ota_0,   0x20000,  0x1E0000,
ota_1,      app,  ota_1,   0x200000, 0x1E0000,
```

Slot size:

```text
0x1E0000 = 1966080 bytes
current app uses: 13.3%
75% slot limit: 1474560 bytes
```

Recommendation:

- Prefer this over 2 MB if physical flash is at least 4 MB.
- Provides much better margin for HTTPS later.

### 6.5 Reference Only: 8 MB Flash

Recommended `partitions_ota_8mb.csv`:

```csv
# Name,     Type, SubType, Offset,   Size,     Flags
nvs,        data, nvs,     0x9000,   0x6000,
otadata,    data, ota,     0xf000,   0x2000,
phy_init,   data, phy,     0x11000,  0x1000,
coredump,   data, coredump,0x12000,  0x10000,
ota_0,      app,  ota_0,   0x20000,  0x300000,
ota_1,      app,  ota_1,   0x320000, 0x300000,
storage,    data, fat,     0x620000, 0x1E0000,
```

Slot size:

```text
0x300000 = 3145728 bytes
current app uses: 8.3%
```

Recommendation:

- Best balance for development if the module has 8 MB.
- Leaves room for logs, crash dumps, and future local storage.

## 7. Local API Endpoints

Base URL for development, using the detected LAN IP:

```text
http://192.168.10.10:8080
```

Do not use `localhost` in any URL stored on the ESP32.

### 7.1 `GET /api/health`

Response:

```json
{
  "ok": true,
  "service": "botfarms-ota-server",
  "time": "2026-05-09T19:00:00.000Z"
}
```

### 7.2 `GET /api/firmware/latest`

Response:

```json
{
  "project": "sistema-motriz-rs485",
  "target": "esp32s3",
  "version": "1.0.3",
  "build_number": 3,
  "url": "http://192.168.10.10:8080/firmware/sistema-motriz-rs485-v1.0.3.bin",
  "filename": "sistema-motriz-rs485-v1.0.3.bin",
  "size": 123456,
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "min_supported_build": 1,
  "notes": "Local development OTA build"
}
```

### 7.3 `GET /firmware/:filename`

Behavior:

- Serves binary firmware files from `firmware/`.
- Must reject path traversal.
- Should set `Content-Type: application/octet-stream`.
- Should set `Content-Length`.

### 7.4 Optional `POST /api/devices/checkin`

Request:

```json
{
  "device_id": "tono-esp32s3-001",
  "project": "sistema-motriz-rs485",
  "version": "1.0.2",
  "build_number": 2,
  "target": "esp32s3",
  "ip": "192.168.10.42",
  "uptime_ms": 123456,
  "ota_state": "valid",
  "robot_safe": true
}
```

Response:

```json
{
  "ok": true
}
```

## 8. Manifest JSON Contract

Exact required format:

```json
{
  "project": "sistema-motriz-rs485",
  "target": "esp32s3",
  "version": "1.0.3",
  "build_number": 3,
  "url": "http://<LAN_IP>:8080/firmware/sistema-motriz-rs485-v1.0.3.bin",
  "filename": "sistema-motriz-rs485-v1.0.3.bin",
  "size": 123456,
  "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "min_supported_build": 1,
  "notes": "..."
}
```

ESP32 validation rules:

- `project` must equal `sistema-motriz-rs485`.
- `target` must equal `esp32s3`.
- `remote.build_number` must be greater than `CURRENT_BUILD_NUMBER` for update.
- `CURRENT_BUILD_NUMBER` must be greater than or equal to `remote.min_supported_build`.
- It is not sufficient to check `remote.build_number >= remote.min_supported_build`; the compatibility gate applies to the currently running firmware.
- `url` must be HTTP initially, HTTPS later.
- `url` must not contain `localhost`.
- `size` must be positive and less than inactive OTA partition size.
- `sha256` must be exactly 64 lowercase/uppercase hex chars.
- Manifest body should be limited to a small size, for example 8192 bytes.

## 9. ESP32 Pseudocode

```c
void app_main(void)
{
    init_logging();
    print_version();

    nvs_flash_init();
    bool pending_verify = ota_manager_detect_pending_verify_early();

    config_manager_init();
    svd48_init();
    robot_control_init();
    svd48_start_polling();
    serial_gateway_start();

    wifi_manager_init(config_manager_get_wifi_config());
    ota_manager_init(config_manager_get_ota_config(), robot_control_handle);

    if (pending_verify) {
        bool ok = nvs_ok() &&
                  config_manager_ok() &&
                  svd48_ok() &&
                  robot_control_ok() &&
                  serial_gateway_ok() &&
                  wifi_manager_initialized() &&
                  ota_manager_ok();
        if (!ok) {
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        // Do not require the OTA server or local PC to be online here.
        esp_ota_mark_app_valid_cancel_rollback();
    }

    wifi_manager_start_station();

    if (!wifi_manager_connected_after_retries()) {
        // Early iterations: stay offline and keep robot/serial working.
        // Later iteration: start SoftAP provisioning.
        wifi_manager_maybe_start_softap_provisioning();
    }

    ota_manager_start_low_priority_task();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

OTA task:

```c
void ota_task(void *arg)
{
    uint32_t delay_ms = INITIAL_CHECK_INTERVAL_MS;

    while (true) {
        if (!wifi_manager_is_connected()) {
            sleep_with_backoff();
            continue;
        }

        if (!ota_policy_auto_check_enabled()) {
            sleep_normal_interval();
            continue;
        }

        result = ota_manager_check_manifest();
        if (result == UPDATE_AVAILABLE) {
            if (robot_control_is_safe_for_ota()) {
                // Automatic update should remain disabled until manual OTA is proven.
                if (ota_policy_auto_update_enabled()) {
                    ota_manager_perform_update();
                }
            }
        }

        sleep_with_backoff_or_normal_interval();
    }
}
```

Manual OTA update:

```c
esp_err_t ota_manager_update_now(void)
{
    if (!wifi_manager_is_connected()) return ESP_ERR_INVALID_STATE;
    if (!robot_control_is_safe_for_ota()) return ESP_ERR_INVALID_STATE;

    manifest = fetch_manifest();
    validate_manifest_shape(manifest);
    compare_build_number(manifest.build_number, CURRENT_BUILD_NUMBER);
    ensure_inactive_partition_large_enough(manifest.size);

    partition = esp_ota_get_next_update_partition(NULL);
    esp_ota_begin(partition, manifest.size, &handle);

    while (download_chunk(&chunk)) {
        sha256_update(chunk);
        bytes_written += chunk.len;
        esp_ota_write(handle, chunk.data, chunk.len);
    }

    if (bytes_written != manifest.size) abort_without_switch();
    if (sha256_final != manifest.sha256) abort_without_switch();
    esp_ota_end(handle);

    robot_control_stop_all();
    wait_until_robot_safe_or_timeout();

    esp_ota_set_boot_partition(partition);
    esp_restart();
}
```

## 10. Proposed Serial Commands

Version and system:

```text
VERSION
PARTITIONS
REBOOT
SYSTEM_STATUS
```

Wi-Fi:

```text
WIFI_STATUS
WIFI_SET <ssid> <password>
WIFI_CLEAR
WIFI_CONNECT
WIFI_DISCONNECT
```

Security rule:

- `WIFI_SET` may accept password input, but logs must redact it.
- `WIFI_STATUS` must never print the password.

OTA server config:

```text
OTA_SET_SERVER <host> <port>
OTA_SET_MANIFEST <path>
OTA_CONFIG
OTA_CLEAR_CONFIG
```

OTA status/update:

```text
OTA_STATUS
OTA_CHECK
OTA_UPDATE
OTA_ABORT
OTA_CONFIRM
```

Notes:

- `OTA_UPDATE` should be manual-only until Iteration 10.
- `OTA_CONFIRM` may be useful for lab testing rollback, but production should mark valid automatically only after self-test.

## 11. Exact Terminal Commands

### 11.1 Restore ESP-IDF Environment

Example if ESP-IDF v5.4.1 is installed persistently:

```bash
export IDF_TARGET=esp32s3
. ~/esp/esp-idf-v5.4.1/export.sh
idf.py --version
```

If no IDF is installed, install or clone ESP-IDF v5.4.1 first.

### 11.2 Firmware Build/Size/Partition

```bash
cd /mnt/windows/Users/juanp/OneDriveShouldDie/Documents/BotFarms/sistema-motriz-rs485
idf.py set-target esp32s3
idf.py build
idf.py size
idf.py partition-table
```

### 11.3 Flash Detection

```bash
ls -l /dev/serial/by-id
ls -la /dev/ttyACM* /dev/ttyUSB*
python -m esptool --chip esp32s3 -p /dev/ttyACM0 flash_id
```

### 11.4 Flash and Monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### 11.5 Start Local Backend

Recommended future command:

```bash
cd /home/jp/Documents/botfarms/web_controll_esp_svd48
npm install
HOST=0.0.0.0 PORT=8080 PUBLIC_HOST=192.168.10.10 npm run dev
```

### 11.6 Generate Manifest

Proposed:

```bash
cd /home/jp/Documents/botfarms/web_controll_esp_svd48
node scripts/copy_firmware_release.js \
  --bin /mnt/windows/Users/juanp/OneDriveShouldDie/Documents/BotFarms/sistema-motriz-rs485/build/sistema-motriz-rs485.bin \
  --version 1.0.3 \
  --build-number 3 \
  --public-base-url http://192.168.10.10:8080
```

### 11.7 Test Endpoints

```bash
curl -s http://127.0.0.1:8080/api/health | jq
curl -s http://192.168.10.10:8080/api/health | jq
curl -s http://192.168.10.10:8080/api/firmware/latest | jq
curl -O http://192.168.10.10:8080/firmware/sistema-motriz-rs485-v1.0.3.bin
sha256sum firmware/sistema-motriz-rs485-v1.0.3.bin
```

### 11.8 Serial Diagnostics

```bash
python3 -m serial.tools.miniterm /dev/ttyACM0 115200
```

Commands to send:

```text
VERSION
PARTITIONS
WIFI_STATUS
OTA_CONFIG
OTA_CHECK
OTA_STATUS
```

## 12. Implementation Iterations

### Iteration 0: Initial Audit and Measurements

Objective:

- Verify the real starting point before any functional code changes.
- Create a verifiable baseline before any functional code or partition changes.

Scope:

- Target, IDF version, binary size, flash size, current partitions, current build, current web repo, and baseline/backup protection.

Files likely touched:

- `docs/OTA_IMPLEMENTATION_PLAN.md`.
- `.git/` if a Git baseline can be initialized.
- Or an external backup directory/archive if Git initialization is not viable.

Subtasks:

- Verify whether this directory is a valid Git repository.
- If Git can be initialized safely, run `git init`, stage the current project state, and create a baseline commit before functional changes.
- If Git initialization is not viable, create a complete copy/archive of the project outside the working tree and verify it.
- Do not modify partitions or functional firmware code without a verifiable Git baseline or backup.
- Restore ESP-IDF v5.4.1.
- Run `idf.py build`.
- Run `idf.py size`.
- Run `idf.py partition-table`.
- Run `flash_id`.
- Confirm serial port.
- Confirm web repo static state.

Commands:

```bash
git rev-parse --show-toplevel
git init
git add .
git commit -m "Baseline before OTA implementation"

# If Git baseline is not viable:
rsync -a --delete /path/to/sistema-motriz-rs485/ /path/to/backup/sistema-motriz-rs485-baseline/

idf.py set-target esp32s3
idf.py build
idf.py size
idf.py partition-table
python -m esptool --chip esp32s3 -p /dev/ttyACM0 flash_id
```

Acceptance criteria:

- A baseline commit exists, or a full backup copy/archive exists and was verified.
- Build succeeds.
- Binary size documented.
- Flash size documented.
- Partition table documented.
- Risks documented.

Mandatory tests:

- Existing serial gateway still responds after a normal flash.
- `PING` works.
- No functional code changed.
- No partition or source-code changes are made before baseline/backup protection exists.

Risks:

- Toolchain missing.
- Wrong USB port.
- Flash size configured differently from real hardware.
- Baseline Git repository accidentally includes build artifacts if `.gitignore` is incomplete.

Rollback/recovery:

- Use the baseline commit or verified backup to recover the starting point.
- No functional code changes except documentation during this iteration.
- If build environment is broken, restore IDF before proceeding.

Expected end state:

- Approval-quality measurements exist.

### Iteration 1: Firmware Versioning and Basic Diagnostics

Objective:

- Add explicit firmware version/build metadata and basic serial diagnostics.

Scope:

- No OTA yet.
- Add `VERSION`.
- Add minimal `SYSTEM_STATUS` or `PARTITIONS` only if low risk.

Files likely touched:

- `main/app_version.h`
- `main/main.c`
- `components/serial_gateway/serial_gateway.c`
- `components/serial_gateway/include/serial_gateway.h`
- `docs/API.md`

Subtasks:

- Define `FW_PROJECT`, `FW_VERSION`, `FW_BUILD_NUMBER`, `FW_TARGET`.
- Print version at boot.
- Implement `VERSION` command.
- Optionally expose running partition label.

Commands:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- `VERSION` prints project, target, version, build number, IDF version and app partition.
- No secrets printed.
- Existing robot commands still work.

Mandatory tests:

- `PING`
- `VERSION`
- `GET_MOTOR 2` or the relevant current bench motor.

Risks:

- Minimal; mostly string metadata.

Rollback/recovery:

- Revert version command patch if it breaks build.

Expected end state:

- Firmware can identify itself over serial and logs.

### Iteration 2: Safe Migration to OTA Partitions

Objective:

- Replace single-app layout with OTA-capable layout.

Scope:

- Add the primary `partitions_ota_16mb.csv` table because Iteration 0 confirmed physical flash is `16MB`.
- Update `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB`.
- Configure custom partition table.
- Keep 2MB/4MB/8MB tables only as references for other boards.
- Do not enable rollback in this iteration. Rollback belongs to Iteration 9 after the basic OTA partition boot path is proven.

Files likely touched:

- `partitions_ota_16mb.csv`
- Optional reference files: `partitions_ota_2mb.csv`, `partitions_ota_4mb.csv`, `partitions_ota_8mb.csv`
- `sdkconfig.defaults`
- `sdkconfig`
- `docs/OTA_IMPLEMENTATION_PLAN.md`

Subtasks:

- Use the already measured `16MB` flash result as the main path.
- Create/select `partitions_ota_16mb.csv`.
- Update `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB`.
- Build.
- Confirm binary fits slots.
- Flash full image.
- Confirm firmware boots exactly like before.

Commands:

```bash
idf.py menuconfig
idf.py build
idf.py partition-table
idf.py size
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- Partition table shows `ota_0`, `ota_1`, `otadata`.
- App binary fits slot with margin.
- Firmware boots.
- Serial gateway works.
- RS485 control still works.

Mandatory tests:

- `PING`
- `VERSION`
- `GET_SVD48_CONFIG 0x02 M1`
- `STOP 2` or current safe stop command for bench setup.

Risks:

- Full flash may erase NVS.
- Wrong partition table can prevent boot.
- Config/partition mismatch can prevent boot if `CONFIG_ESPTOOLPY_FLASHSIZE`, partition CSV and physical flash do not agree.

Rollback/recovery:

- Keep previous build artifacts.
- Reflash previous single-app firmware if OTA table fails.
- Use serial bootloader mode if app does not boot.

Expected end state:

- Same robot behavior, but OTA partition layout exists.

### Iteration 3: Config Manager with NVS

Objective:

- Add persistent runtime configuration.

Scope:

- Store Wi-Fi and OTA server config.
- No Wi-Fi connection required yet.

Files likely touched:

- `components/config_manager/CMakeLists.txt`
- `components/config_manager/include/config_manager.h`
- `components/config_manager/config_manager.c`
- `components/serial_gateway/serial_gateway.c`
- `main/main.c`
- `docs/API.md`

Stored values:

- `wifi_ssid`, default empty
- `wifi_password`, default empty; never printed in logs or serial responses
- `ota_server_host`, default `192.168.10.10`
- `ota_server_port`, default `8080`
- `ota_manifest_path`, default `/api/firmware/latest`
- `ota_auto_check_enabled`, default `false`
- `ota_auto_update_enabled`, default `false`

Subtasks:

- Initialize NVS.
- Add typed getters/setters.
- Add default config.
- Add serial commands to inspect and set config with password redaction.
- Block `OTA_AUTO_UPDATE ON` until manual OTA, rollback and recovery are validated.

Commands:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- Config can be set.
- Config persists after reboot.
- Password is never printed in cleartext.
- Clearing config works.

Mandatory tests:

- `CONFIG_STATUS`
- `OTA_CONFIG`
- `OTA_SET_SERVER 192.168.10.10 8080`
- `OTA_SET_MANIFEST /api/firmware/latest`
- `WIFI_SET <ssid> <password>`
- reboot
- `CONFIG_STATUS`
- `OTA_CONFIG`
- `WIFI_CLEAR`
- `CONFIG_CLEAR`

Risks:

- NVS namespace can fill or become incompatible if the schema changes without a clear migration/reset path.
- Accidental secret logging.

Rollback/recovery:

- Add `CONFIG_CLEAR` or `NVS_CLEAR` command.
- Full erase if NVS schema is corrupted during development.

Expected end state:

- Runtime config is persistent and safe to inspect.

### Iteration 4: Wi-Fi Station Manager

Objective:

- Connect ESP32-S3 as Wi-Fi station using NVS config.

Scope:

- Station mode only.
- SoftAP later.
- Robot must work if Wi-Fi fails.

Files likely touched:

- `components/wifi_manager/CMakeLists.txt`
- `components/wifi_manager/include/wifi_manager.h`
- `components/wifi_manager/wifi_manager.c`
- `components/config_manager/*`
- `components/serial_gateway/serial_gateway.c`
- `main/main.c`
- `components/serial_gateway/include/serial_gateway.h`
- `docs/API.md`
- `docs/OTA_IMPLEMENTATION_PLAN.md`

Subtasks:

- Add `esp_netif`, `esp_event`, `esp_wifi`, `nvs_flash`.
- Implement manual station connect with finite retry/timeout.
- Add state snapshot.
- Add `WIFI_STATUS`, `WIFI_CONNECT`, `WIFI_DISCONNECT`.
- Do not auto-connect at boot in this iteration.
- Do not start SoftAP, OTA client, OTA task or rollback.

Commands:

```bash
idf.py build
idf.py size
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- Valid credentials connect.
- Invalid credentials fail cleanly.
- Firmware boots with no credentials.
- RS485 telemetry continues when Wi-Fi is down.

Mandatory tests:

- no Wi-Fi config
- wrong password
- correct password
- `VERSION`, `PING`, `CONFIG_STATUS`, `OTA_CONFIG`
- `GET_MOTOR 2`, `STOP 2`
- 12 seconds monitor idle without `task_wdt`

Risks:

- Wi-Fi task memory usage.
- Blocking connect path.
- Serial logs exposing credentials.

Rollback/recovery:

- Disable Wi-Fi autoconnect via serial config.
- Reflash previous firmware if boot loop occurs.

Expected end state:

- ESP32 has optional Wi-Fi station connectivity.

### Iteration 5: Local OTA Backend

Objective:

- Add local API server to web repo.

Scope:

- Serve static UI and OTA endpoints.
- Generate manifest.

Files likely touched in web repo:

- `package.json`
- `server.js`
- `scripts/build_firmware_manifest.js`
- `scripts/copy_firmware_release.js`
- `firmware/.gitkeep`
- `.gitignore`
- `docs/ota-backend.md`

Subtasks:

- Add Express.
- Serve `index.html` and assets.
- Add `/api/health`.
- Add `/api/firmware/latest`.
- Add `/firmware/:filename`.
- Add manifest generation script.

Commands:

```bash
cd /home/jp/Documents/botfarms/web_controll_esp_svd48
npm install
HOST=0.0.0.0 PORT=8080 PUBLIC_HOST=192.168.10.10 npm run dev
curl -s http://127.0.0.1:8080/api/health
curl -s http://192.168.10.10:8080/api/firmware/latest
```

Acceptance criteria:

- Server listens on `0.0.0.0`.
- Manifest URL uses LAN IP.
- Firmware file downloads.
- SHA256 matches file.

Mandatory tests:

- Health endpoint.
- Manifest endpoint.
- Binary download.
- Path traversal rejection.
- Missing binary response.

Risks:

- Firewall blocks port.
- Wrong LAN IP in manifest.
- Generated manifest not synced with binary.

Rollback/recovery:

- Keep existing static server script as `dev:static`.

Expected end state:

- PC can serve OTA artifacts locally.

### Iteration 6: OTA Client Without Firmware Write

Objective:

- ESP32 fetches and parses manifest but does not write OTA.

Scope:

- Add HTTP client and JSON parser.
- Compare build numbers.
- Add `OTA_CHECK`.

Files likely touched:

- `components/ota_manager/CMakeLists.txt`
- `components/ota_manager/include/ota_manager.h`
- `components/ota_manager/ota_manager.c`
- `components/serial_gateway/serial_gateway.c`
- `main/main.c`
- `sdkconfig.defaults`

Subtasks:

- Build manifest URL from NVS config.
- Fetch manifest with timeout.
- Limit manifest size.
- Parse JSON.
- Validate required fields.
- Compare version/build.

Commands:

```bash
idf.py build
idf.py size
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- `OTA_CHECK` reports up-to-date.
- `OTA_CHECK` reports update available.
- Endpoint down does not affect robot.
- Invalid JSON is rejected.

Mandatory tests:

- version equal
- version greater
- invalid JSON
- missing URL
- wrong target
- endpoint unreachable

Risks:

- HTTP calls blocking too long.
- Heap fragmentation from JSON parsing.

Rollback/recovery:

- Disable OTA manager start with a config flag.

Expected end state:

- ESP32 can reason about available updates without writing flash.

Iteration 6 was implemented and validated on 2026-05-09.

Implementation summary:

```text
Added component: components/ota_manager
Added command: OTA_CHECK
HTTP client: esp_http_client
JSON parser: cJSON via ESP-IDF json component
Firmware download: not implemented
OTA partition writes: not implemented
Boot partition switch: not implemented
Rollback: still disabled
Automatic OTA task: still disabled
```

Validation rules implemented:

```text
Manifest URL is built from NVS: OTA_HOST + OTA_PORT + OTA_MANIFEST
HTTP scheme is local development HTTP only
localhost, 127.0.0.1, 0.0.0.0 and ::1 are rejected
Required fields: project, target, version, build_number, min_supported_build, url, filename, size, sha256
project must match FW_PROJECT
target must match FW_TARGET
filename must be a .bin basename
sha256 must be exactly 64 hex characters
size must be non-zero
CURRENT_BUILD_NUMBER must be >= remote.min_supported_build
remote.build_number > CURRENT_BUILD_NUMBER reports UPDATE_AVAILABLE
remote.build_number <= CURRENT_BUILD_NUMBER reports UP_TO_DATE
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0xed500 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x512b00 bytes, 85%
Total image size: 971920 bytes
Flash Code: 698598 bytes
Flash Data: 160212 bytes
DIRAM: 113835 / 341760 bytes, 33.31%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Physical validation:

```text
Flashed on /dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00
VERSION: BUILD_NUMBER:2, PARTITION:ota_0
WIFI_STATUS: CONNECTED, IP:192.168.1.166
OTA_CONFIG: HOST:192.168.1.107 PORT:8080 MANIFEST:/api/firmware/latest
OTA_CHECK equal build: STATUS:UP_TO_DATE, BUILD_NUMBER:2, CURRENT_BUILD:2
OTA_CHECK remote build greater: STATUS:UPDATE_AVAILABLE, BUILD_NUMBER:3, CURRENT_BUILD:2
OTA_CHECK localhost manifest URL: ERR OTA_CHECK_FAILED 0x108 DETAIL:BAD_URL
OTA_CHECK invalid manifest JSON through backend: ERR OTA_CHECK_FAILED 0x108 DETAIL:HTTP_STATUS
OTA_CHECK endpoint down: ERR OTA_CHECK_FAILED 0x7002 DETAIL:HTTP_PERFORM
PING after OTA checks: OK
GET_MOTOR 2 after OTA checks: OK, controller online
STOP 2 after OTA checks: OK
12 seconds monitor idle: no task_wdt
```

Important caveat:

```text
During this test the actual PC LAN IP was 192.168.1.107. The previously suggested 192.168.10.10 is not reachable from the ESP32 while the ESP32 is connected to the tested 192.168.1.0/24 LAN unless the network provides routing.
```

### Iteration 7: Download and Verify in Inactive OTA Slot Without Boot Switch

Objective:

- Download candidate binary, optionally write it to the inactive OTA partition, and verify size/hash without changing the active firmware.

Scope:

- Stream file over HTTP.
- Calculate SHA256.
- It may write to the inactive OTA partition using `esp_ota_begin` / `esp_ota_write` / `esp_ota_end` to test the real write path.
- It must not call `esp_ota_set_boot_partition`.
- It must not reboot.
- The active firmware must remain unchanged.

Files likely touched:

- `components/ota_manager/ota_manager.c`
- `components/ota_manager/include/ota_manager.h`

Subtasks:

- Validate inactive partition size.
- Stream download in chunks.
- Track bytes.
- Compute SHA256.
- Abort on mismatch.

Commands:

```bash
idf.py build
idf.py size
```

Acceptance criteria:

- Correct binary verifies.
- Wrong SHA fails.
- Wrong size fails.
- Missing binary fails.
- No reboot.
- No boot partition switch.
- Running firmware and `VERSION` remain unchanged after the test.

Mandatory tests:

- good binary
- bad hash
- bad size
- localhost URL
- 404 binary
- interrupted server
- invalid manifest

Risks:

- Wear on inactive OTA partition if repeated often.
- Chunk buffer too large.

Rollback/recovery:

- Since no boot switch, current firmware remains active.
- If the inactive OTA slot contains a test image, the next real OTA write can overwrite it.

Expected end state:

- Download pipeline is proven before boot changes.

Iteration 7 was implemented and validated on 2026-05-09.

Implementation summary:

```text
Added command: OTA_DOWNLOAD_TEST
Download transport: esp_http_client over local HTTP
Write target: esp_ota_get_next_update_partition(NULL)
OTA write path: esp_ota_begin / esp_ota_write / esp_ota_end
SHA256 implementation: mbedtls_sha256 while streaming
Chunk size: 4096 bytes
Boot partition switch: not implemented
esp_ota_set_boot_partition: not called
Reboot: not performed by OTA_DOWNLOAD_TEST
Rollback: still disabled
Automatic OTA task: still disabled
```

Validation rules implemented:

```text
OTA_DOWNLOAD_TEST reuses OTA_CHECK manifest validation.
project must match FW_PROJECT.
target must match FW_TARGET.
CURRENT_BUILD_NUMBER must be >= remote.min_supported_build.
URL must not point to localhost/127.0.0.1/0.0.0.0/::1.
filename must be a .bin basename.
size must be non-zero.
size must be smaller than the inactive OTA partition size.
sha256 must be exactly 64 hex characters.
bytes_written must equal manifest.size.
calculated sha256 must equal manifest.sha256.
esp_ota_abort is called on validation/download/write/hash failure after OTA begin.
esp_ota_end is called only after a complete download and matching SHA256.
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0xefd40 bytes / 982336 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x5102c0 bytes, 84%
Total image size: 982216 bytes
Flash Code: 705358 bytes
Flash Data: 163572 bytes
DIRAM: 114019 / 341760 bytes, 33.36%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Backend manifest used for validation:

```text
URL: http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b2.bin
size: 982336
sha256: 7855f931c0a0c1330e71033ed9b5d46e485a7e61eafc350c9c05666f3d26087a
build_number: 2
min_supported_build: 1
```

Physical validation:

```text
Flashed on /dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00
VERSION before test: BUILD_NUMBER:2, PARTITION:ota_0
WIFI_STATUS: CONNECTED, IP:192.168.1.166
OTA_CHECK: STATUS:UP_TO_DATE, BUILD_NUMBER:2, CURRENT_BUILD:2
OTA_DOWNLOAD_TEST valid: STATUS:VERIFIED, PARTITION:ota_1, BYTES:982336, SHA256:7855f931c0a0c1330e71033ed9b5d46e485a7e61eafc350c9c05666f3d26087a
VERSION after valid download: PARTITION:ota_0
PING after valid download: OK
GET_MOTOR 2 after valid download: OK, controller online
STOP 2 after valid download: OK
```

Error validation in one continuous serial session after Wi-Fi reconnect:

```text
bad sha256: ERR OTA_DOWNLOAD_TEST_FAILED 0x108 DETAIL:SHA256_MISMATCH PARTITION:ota_1 BYTES:982336
VERSION after bad sha256: PARTITION:ota_0
bad size: ERR OTA_DOWNLOAD_TEST_FAILED 0x104 DETAIL:CONTENT_LENGTH PARTITION:ota_1 BYTES:0
VERSION after bad size: PARTITION:ota_0
localhost URL: ERR OTA_DOWNLOAD_TEST_FAILED 0x108 DETAIL:BAD_URL PARTITION:NONE BYTES:0
VERSION after localhost URL: PARTITION:ota_0
404 binary: ERR OTA_DOWNLOAD_TEST_FAILED 0x108 DETAIL:HTTP_STATUS PARTITION:ota_1 BYTES:0
VERSION after 404 binary: PARTITION:ota_0
endpoint down: ERR OTA_DOWNLOAD_TEST_FAILED 0x7002 DETAIL:HTTP_PERFORM PARTITION:NONE BYTES:0
VERSION after endpoint down: PARTITION:ota_0
invalid manifest missing target: ERR OTA_DOWNLOAD_TEST_FAILED 0x108 DETAIL:target PARTITION:NONE BYTES:0
VERSION after invalid manifest: PARTITION:ota_0
OTA_CHECK after restoring manifest/server: STATUS:UP_TO_DATE
PING after failures: OK
GET_MOTOR 2 after failures: OK, controller online
STOP 2 after failures: OK
12 seconds serial idle after failures: no task_wdt
```

Important caveat:

```text
Opening /dev/ttyACM0 from pyserial toggles the adapter lines and can reset the ESP32-S3. The reliable error matrix above was run in a single continuous serial session after the initial open/reset, so the individual OTA_DOWNLOAD_TEST commands did not cause reboots. VERSION remained on ota_0 after every valid and invalid test.
```

### Iteration 8: Manual Real OTA

Objective:

- Implement actual OTA update by manual serial command.

Scope:

- `OTA_UPDATE` only.
- No automatic updates yet.

Files likely touched:

- `components/ota_manager/*`
- `components/robot_control/include/robot_control.h`
- `components/robot_control/robot_control.c`
- `components/serial_gateway/serial_gateway.c`

Subtasks:

- Add `robot_control_is_safe_for_ota()`.
- Add `robot_control_prepare_for_ota()` that performs STOP ALL and waits.
- Write OTA partition.
- Validate size/hash.
- End OTA.
- Set boot partition.
- Reboot.

Commands:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- Manual update works.
- New `VERSION` appears after reboot.
- Unsafe robot state blocks update.
- Hash/size errors do not switch boot partition.

Mandatory tests:

- OTA success.
- version unchanged if failed.
- robot moving blocks OTA.
- endpoint down fails safely.

Risks:

- Boot into bad firmware if rollback is not ready.
- Power loss during OTA.

Rollback/recovery:

- Keep serial flashing recovery path.
- Do not enable automatic OTA writes yet.
- Enable rollback before field use.

Expected end state:

- Manual OTA works in controlled lab conditions.

Iteration 8 was implemented and validated on 2026-05-09.

Toolchain validation before functional changes:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py --version
python -m esptool --help
python3 - <<'PY'
import serial
print("pyserial OK", serial.__version__)
PY
PATH=/home/jp/.local/node-local/bin:$PATH node --version
PATH=/home/jp/.local/node-local/bin:$PATH npm --version
. /home/jp/esp/esp-idf-v5.4.1/export.sh && idf.py build && idf.py size
```

Observed toolchain result:

```text
ESP-IDF: v5.4.1 from /home/jp/esp/esp-idf-v5.4.1/export.sh
esptool: available through the ESP-IDF Python environment
pyserial: available as Python package serial 3.5
node: v22.22.2 from /home/jp/.local/node-local/bin
npm: 10.9.7 from /home/jp/.local/node-local/bin
No sudo installation was required.
```

Implementation summary:

```text
Added command: OTA_UPDATE
OTA_UPDATE requires Wi-Fi state CONNECTED.
OTA_UPDATE checks robot_control_is_safe_for_ota before downloading.
OTA_UPDATE reuses ota_manager_download_to_inactive, the same validated download/write/hash path used by OTA_DOWNLOAD_TEST.
OTA_UPDATE calls robot_control_prepare_for_ota after esp_ota_end succeeds and before esp_ota_set_boot_partition.
OTA_UPDATE calls esp_ota_set_boot_partition only after manifest/download/write/sha256/esp_ota_end and robot preparation succeed.
OTA_UPDATE reboots with esp_restart after printing STATUS:REBOOTING.
Rollback remains disabled.
Automatic OTA remains disabled.
SoftAP, HTTPS and firmware signing were not implemented.
```

Safety behavior:

```text
robot_control tracks manual SET_SPEED commands in last_command.
STOP n clears the tracked command for that motor.
STOP ALL clears all tracked command state.
robot_control_is_safe_for_ota blocks non-zero motion commands and online telemetry with actual RPM above 5 RPM.
Controller status alone is not treated as unsafe when actual RPM is zero, because the SVD48 can briefly report running/enabled after STOP while RPM is already zero.
robot_control_prepare_for_ota stops motors that are online or have a tracked non-zero command.
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0xf0860 bytes / 985184 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x50f7a0 bytes, 84%
Total image size: 985060 bytes
Flash Code: 707338 bytes
Flash Data: 164436 bytes
DIRAM: 114019 / 341760 bytes, 33.36%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Backend manifest used for validation:

```text
URL: http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b2.bin
size: 985184
sha256: e1a46d0845805c7cc6e2876ce5b4cfc12c3ea0e652d2ad07ec8eb414179ac735
build_number: 2
min_supported_build: 1
```

Physical validation:

```text
Flashed on /dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00
VERSION before error tests: BUILD_NUMBER:2, PARTITION:ota_0
WIFI_STATUS: CONNECTED, IP:192.168.1.166
OTA_CHECK: STATUS:UP_TO_DATE, BUILD_NUMBER:2, CURRENT_BUILD:2
bad sha256: ERR OTA_UPDATE_FAILED 0x108 DETAIL:SHA256_MISMATCH PARTITION:ota_1 BYTES:985184
VERSION after bad sha256: PARTITION:ota_0
bad size: ERR OTA_UPDATE_FAILED 0x104 DETAIL:CONTENT_LENGTH PARTITION:ota_1 BYTES:0
VERSION after bad size: PARTITION:ota_0
localhost URL: ERR OTA_UPDATE_FAILED 0x108 DETAIL:BAD_URL PARTITION:NONE BYTES:0
VERSION after localhost URL: PARTITION:ota_0
404 binary: ERR OTA_UPDATE_FAILED 0x108 DETAIL:HTTP_STATUS PARTITION:ota_1 BYTES:0
VERSION after 404 binary: PARTITION:ota_0
endpoint down: ERR OTA_UPDATE_FAILED 0x7002 DETAIL:HTTP_PERFORM PARTITION:NONE BYTES:0
VERSION after endpoint down: PARTITION:ota_0
robot commanded with SET_SPEED 2 10: ERR OTA_UPDATE_BLOCKED ROBOT_NOT_SAFE REASON:MOTOR_COMMAND_ACTIVE
STOP 2 after unsafe test: OK
GET_MOTOR 2 after STOP: RPM:0, STATUS:0, ONLINE:1, STALE:0
OTA_UPDATE success: DATA OTA_UPDATE STATUS:REBOOTING PARTITION:ota_1 BYTES:985184 SHA256:e1a46d0845805c7cc6e2876ce5b4cfc12c3ea0e652d2ad07ec8eb414179ac735
Boot after OTA_UPDATE: Loaded app from partition at offset 0x630000
VERSION after OTA_UPDATE: PARTITION:ota_1
PING after OTA_UPDATE: OK
GET_MOTOR 2 after OTA_UPDATE: OK, controller online
STOP 2 after OTA_UPDATE: OK
12 seconds serial idle after OTA_UPDATE: no task_wdt
```

Important caveat:

```text
Iteration 8 intentionally allows manual same-build OTA for slot validation in the lab. Future automatic OTA should require remote.build_number > CURRENT_BUILD_NUMBER before writing or rebooting.
```

### Iteration 9: Rollback and Post-Boot Validation

Objective:

- Enable ESP-IDF rollback and mark app valid only after self-test.

Scope:

- Boot-state handling.
- Self-test.
- rollback simulation.

Files likely touched:

- `sdkconfig.defaults`
- `components/ota_manager/*`
- `main/main.c`
- `components/serial_gateway/serial_gateway.c`
- `docs/API.md`
- `docs/OTA_IMPLEMENTATION_PLAN.md`

Subtasks:

- Enable rollback Kconfig.
- Detect `ESP_OTA_IMG_PENDING_VERIFY`.
- Run self-test.
- Mark valid or invalid.
- Add test flag to intentionally skip confirmation.

Commands:

```bash
idf.py build
idf.py size
idf.py -p /dev/serial/by-id/usb-1a86_USB_Single_Serial_5A4B026509-if00 flash
```

Acceptance criteria:

- Valid app confirms.
- Invalid app rolls back.
- Rollback behavior documented.

Mandatory tests:

- normal OTA confirms
- forced no-confirm rollback
- power cycle before confirmation

Risks:

- Self-test too strict causing false rollback.
- Self-test too weak allowing broken OTA.

Rollback/recovery:

- Serial reflash.
- Bootloader rollback should return to previous app.

Expected end state:

- Device is protected against broken OTA app.

Iteration 9 was implemented and validated on 2026-05-09.

Pre-implementation guardrails:

```text
Firmware repo .gitignore ignores .env and sdkconfig.
Web/backend repo .gitignore ignores firmware/*.bin and firmware/manifest.json.
No .env file or Wi-Fi password is tracked by Git in either repository.
Automatic OTA remains disabled: OTA_AUTO_UPDATE ON is still rejected.
SoftAP provisioning, HTTPS, firmware signing, secure boot and flash encryption remain out of scope.
Manual same-build OTA remains allowed only for lab slot/rollback validation. Future automatic OTA must require remote.build_number > CURRENT_BUILD_NUMBER.
```

Implementation summary:

```text
Enabled CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE in sdkconfig.defaults.
Updated local ignored sdkconfig for the physical build/flash used in this iteration.
Incremented FW_BUILD_NUMBER to 3.
Added ota_manager_get_boot_state(), ota_manager_mark_app_valid(), ota_manager_mark_app_invalid_and_rollback().
Added one-shot lab rollback test mode in NVS namespace bot_ota with key rb_test.
Added VERSION fields: OTA_STATE, PENDING_VERIFY, ROLLBACK_POSSIBLE.
Added serial commands: OTA_ROLLBACK_STATUS and OTA_ROLLBACK_TEST NONE|NO_CONFIRM_ONCE|SELF_TEST_FAIL_ONCE.
main/app_main detects PENDING_VERIFY early, initializes critical subsystems, then marks valid only after serial_gateway starts.
Self-test does not require Wi-Fi connection success or OTA backend availability.
```

Critical self-test sequence:

```text
1. Read running partition OTA state.
2. Initialize NVS.
3. Initialize config_manager.
4. Initialize wifi_manager.
5. Initialize ota_manager.
6. Initialize SVD48 UART/RS485 driver.
7. Initialize robot_control.
8. Start SVD48 telemetry polling.
9. Initialize and start serial_gateway.
10. If the app was PENDING_VERIFY, consume any one-shot rollback test mode.
11. If no forced test mode is active, call esp_ota_mark_app_valid_cancel_rollback().
12. If a critical init step fails while PENDING_VERIFY, call esp_ota_mark_app_invalid_rollback_and_reboot().
```

Build and size result:

```text
sistema-motriz-rs485.bin binary size: 0xf15f0 bytes / 988656 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x50ea10 bytes, 84%
Total image size: 988536 bytes
Flash Code: 709582 bytes
Flash Data: 165668 bytes
DIRAM: 114019 / 341760 bytes, 33.36%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

Rollback config evidence:

```text
build/config/sdkconfig.h: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1
build/config/sdkconfig.h: CONFIG_APP_ROLLBACK_ENABLE CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
Bootloader binary size: 0x5280 bytes, 36% free before partition table.
```

Backend manifest used:

```text
URL: http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b3.bin
build_number: 3
size: 988656
sha256: 3a21dcb7c2309c4ee1d391b03683aad246ec8f16cd9d36bf022fd835d80a1e65
min_supported_build: 1
```

Physical validation:

```text
Initial serial flash boot:
VERSION: BUILD_NUMBER:3, PARTITION:ota_0, OTA_STATE:VALID, PENDING_VERIFY:0, ROLLBACK_POSSIBLE:0
WIFI_STATUS after WIFI_CONNECT: CONNECTED, IP:192.168.1.166
OTA_CHECK: UP_TO_DATE, BUILD_NUMBER:3, CURRENT_BUILD:3
GET_MOTOR 2: ONLINE:1, STALE:0
STOP 2: OK
12 seconds idle: no task_wdt

Successful OTA with rollback enabled:
OTA_UPDATE: STATUS:REBOOTING, PARTITION:ota_1, BYTES:988656, SHA256:3a21dcb7...
Boot: ota_1 loaded as PENDING_VERIFY with rollback_possible:1
After self-test: Pending OTA app marked valid after subsystem self-test
VERSION: PARTITION:ota_1, OTA_STATE:VALID, PENDING_VERIFY:0, ROLLBACK_POSSIBLE:1
PING, GET_MOTOR 2 and STOP 2: OK
12 seconds idle: no task_wdt

No-confirm rollback simulation:
OTA_ROLLBACK_TEST NO_CONFIRM_ONCE: OK
OTA_UPDATE to ota_0: STATUS:REBOOTING
Boot: ota_0 loaded as PENDING_VERIFY
main: Rollback test mode NO_CONFIRM_ONCE consumed; rebooting before app validation
Next boot: ota_1 loaded, OTA_STATE:VALID
VERSION/PING/GET_MOTOR 2/STOP 2: OK

Self-test failure rollback simulation:
OTA_ROLLBACK_TEST SELF_TEST_FAIL_ONCE: OK
OTA_UPDATE to ota_0: STATUS:REBOOTING
Boot: ota_0 loaded as PENDING_VERIFY
main: Pending OTA app failed self-test stage:forced_self_test_failure
esp_ota_ops: Rollback to previously worked partition. Restart.
Next boot: ota_1 loaded, OTA_STATE:VALID
VERSION/PING/GET_MOTOR 2/STOP 2: OK

Final recovery state:
OTA_ROLLBACK_TEST NONE: OK
Final OTA_UPDATE to ota_0: STATUS:REBOOTING
Boot: ota_0 loaded as PENDING_VERIFY
After self-test: Pending OTA app marked valid after subsystem self-test
VERSION: PARTITION:ota_0, OTA_STATE:VALID, PENDING_VERIFY:0, ROLLBACK_POSSIBLE:1
OTA_ROLLBACK_STATUS: TEST_MODE:NONE
PING, GET_MOTOR 2 and STOP 2: OK
12 seconds idle: no task_wdt
```

Power-cycle note:

```text
The no-confirm test intentionally rebooted before calling esp_ota_mark_app_valid_cancel_rollback().
This exercises the same ESP-IDF bootloader path used by reset, WDT or power loss while an app is PENDING_VERIFY.
A physical unplug/replug was not automated from this terminal because no controllable power switch is connected.
```

### Iteration 9.5: IRAM and Memory Audit

Iteration 9.5 was opened after Iteration 9 because the firmware remains functionally valid but memory risk is now the main blocker before adding automatic OTA logic.

Current audited memory state after Iteration 9:

```text
Baseline commit: 213223d Enable OTA rollback validation
Total image size: 988536 bytes
Flash Code: 709582 bytes
Flash Data: 165668 bytes
DIRAM: 114019 / 341760 bytes, 33.36%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

The active Botfarms application components (`main`, `svd48`, `robot_control`, `serial_gateway`, `config_manager`, `wifi_manager`, and `ota_manager`) do not directly contribute to `.iram0.text` in the audit output. The reported IRAM pressure is dominated by ESP-IDF FreeRTOS, Wi-Fi, flash/HAL, heap, PHY and support code.

Phase 9.5-A is documentation-only and records the evidence in `docs/OTA_MEMORY_AUDIT.md`.

Phase 9.5-B is not approved for execution yet. It should be run as separate experiments, one change per commit, in this order:

1. Try compiler optimization for size.
2. Try placing heap functions in flash.
3. Try placing selected non-ISR FreeRTOS functions in flash.
4. If needed, try disabling Wi-Fi RX IRAM optimization.
5. Only if still critical, try disabling broader Wi-Fi IRAM optimization.

Phase 9.5-B/9.5-C were reviewed. B4 was selected and consolidated by disabling `CONFIG_ESP_WIFI_RX_IRAM_OPT` while keeping `CONFIG_ESP_WIFI_IRAM_OPT` enabled. Iteration 10 may proceed only as normal non-ISR FreeRTOS/application code.

### Iteration 10: Automatic Pull OTA Check Task

Objective:

- Periodically check for updates from a low-priority task. Automatic OTA writes remain disabled unless explicitly approved after manual OTA, rollback, and serial recovery are proven.

Scope:

- Low-priority FreeRTOS task.
- Backoff/retry.
- Auto-check may be enabled through existing NVS config.
- Auto-update/write/reboot remains disabled.
- The task may run manifest validation only; it must not download firmware, write flash, switch partitions, call `OTA_UPDATE`, or reboot.

Files likely touched:

- `components/ota_manager/*`
- `components/config_manager/*`
- `components/serial_gateway/serial_gateway.c`

Subtasks:

- Add `ota_task`.
- Use a conservative default interval, currently 10 minutes.
- Run the first check soon after `OTA_AUTO_CHECK ON`.
- Add backoff for endpoint/network failures.
- Add `OTA_AUTO_STATUS`.
- Keep `OTA_AUTO_UPDATE ON` blocked.

Commands:

```bash
idf.py build
idf.py size
```

Acceptance criteria:

- Periodic checks happen.
- Endpoint failures do not affect robot.
- No download/write/reboot happens automatically in this iteration.
- Telemetry remains responsive.

Mandatory tests:

- auto-check disabled: no automatic manifest request
- auto-check enabled: manifest request happens automatically
- endpoint up: reports `UP_TO_DATE` or `UPDATE_AVAILABLE`
- endpoint down: records failure and backs off
- Wi-Fi disconnected: skips without reconnecting aggressively
- manual `OTA_DOWNLOAD_TEST` and `OTA_UPDATE` still work
- rollback success, `NO_CONFIRM_ONCE`, and `SELF_TEST_FAIL_ONCE` still work
- RS485 `GET_MOTOR 2` and `STOP 2` still work
- 12 seconds idle without `task_wdt`

Risks:

- Network task competes with robot tasks.
- Automatic writes accidentally enabled too early. This iteration keeps `OTA_AUTO_UPDATE ON` rejected.

Rollback/recovery:

- Serial command to disable auto OTA.
- NVS clear command.

Expected end state:

- Safe automatic OTA checks exist. Automatic OTA writes remain disabled by default.

### Iteration 10.5: OTA Auto-Check Observability and Operation

Objective:

- Make automatic manifest polling easier to operate and diagnose without expanding into automatic updates.

Scope:

- Keep the auto-check task as normal FreeRTOS/application code only.
- Do not add `IRAM_ATTR`, ISR handlers, `ESP_INTR_FLAG_IRAM`, `ppm_decoder`, or low-level memory/cache/FreeRTOS flag changes.
- Do not download firmware automatically.
- Do not write flash automatically.
- Do not call `OTA_UPDATE` automatically.
- Do not switch boot partitions or reboot automatically.
- Keep `OTA_AUTO_UPDATE ON` blocked.

Implementation plan:

- Stop emitting unsolicited `DATA OTA_AUTO_CHECK` lines from the background task. The task updates internal state only so serial command responses cannot be interleaved with automatic reports.
- Suppress routine HTTP-client log tags only while the background task is performing its manifest request. Manual `OTA_CHECK`, `OTA_DOWNLOAD_TEST`, and `OTA_UPDATE` keep their existing response behavior.
- Extend persisted config with `ota_auto_check_interval_ms`, default `600000`, minimum `60000`, maximum `86400000`.
- Add `OTA_AUTO_INTERVAL [milliseconds]` to read/update the interval. Runtime changes apply without reboot and persist in NVS.
- Add `OTA_AUTO_FORCE_CHECK` to run one manifest-only check immediately, even if automatic polling is disabled. It refuses to run without Wi-Fi or while another OTA operation is active.
- Expand `OTA_AUTO_STATUS` with task state, enable state, checking state, interval, next delay, backoff, check/failure counts, last result/error/http status, last check age/time, current build, remote build, update availability, last URL and auto-update state.

Validation commands:

```bash
idf.py build
idf.py size
```

Mandatory tests:

- `VERSION` reports build 5 and the current OTA partition.
- `PING`.
- `WIFI_CONNECT` and `WIFI_STATUS CONNECTED`.
- Manual `OTA_CHECK`.
- `OTA_AUTO_STATUS` with `OTA_AUTO_CHECK OFF`.
- Confirm no automatic manifest requests occur while disabled.
- `OTA_AUTO_FORCE_CHECK` with Wi-Fi connected reports `UP_TO_DATE` or `UPDATE_AVAILABLE`.
- `OTA_AUTO_INTERVAL` reads the current value.
- `OTA_AUTO_INTERVAL <valid>` saves, applies at runtime and persists after reboot.
- `OTA_AUTO_INTERVAL <invalid>` is rejected.
- `OTA_AUTO_CHECK ON` runs periodic manifest checks at the configured interval.
- Endpoint-down state records failure/backoff without blocking serial or causing watchdog resets.
- Wi-Fi disconnected state reports `WIFI_NOT_CONNECTED` without reconnecting aggressively.
- `OTA_AUTO_UPDATE ON` remains blocked.
- Manual `OTA_DOWNLOAD_TEST` and `OTA_UPDATE` still work.
- Rollback valid path, `NO_CONFIRM_ONCE`, and `SELF_TEST_FAIL_ONCE` still work.
- `GET_MOTOR 2` and `STOP 2` still work.
- With auto-check enabled, manual `PING`, `OTA_AUTO_STATUS`, and `GET_MOTOR 2` responses do not get corrupted or mixed with unsolicited auto-check `DATA` lines or background HTTP error logs.
- 12 seconds idle without `task_wdt`.

Acceptance criteria:

- Auto-check is observable through `OTA_AUTO_STATUS`.
- Forced check works and updates status.
- Interval configuration works and persists.
- No automatic download, flash write, boot switch or reboot exists.
- OTA manual and rollback behavior remain valid.
- RS485 and serial gateway behavior remain valid.
- Memory reports stay within expected bounds for `IRAM`, `DIRAM .text`, full `.iram0.text`, `Flash Code`, `Flash Data`, and total image size.

### Iteration 11: SoftAP Provisioning Fallback

Objective:

- Provide Wi-Fi/server configuration without serial.

Scope:

- SoftAP if no config or repeated station failure.
- Minimal provisioning HTTP page/API.

Files likely touched:

- `components/wifi_manager/*`
- `components/config_manager/*`
- optional `components/provisioning_server/*`

Subtasks:

- Start SoftAP with secure default password or generated password.
- Serve config endpoint/page.
- Save config to NVS.
- Reconnect station or reboot.

Commands:

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Acceptance criteria:

- Opens SoftAP when no Wi-Fi.
- Configures SSID/password/server.
- Saves config.
- Connects afterward.
- SoftAP is not open without password.

Mandatory tests:

- no config
- wrong config
- successful provisioning
- reboot persistence

Risks:

- Security of provisioning AP.
- More flash/RAM usage.
- HTTP server complexity on ESP32.

Rollback/recovery:

- Serial config still works.
- Disable SoftAP flag.

Expected end state:

- Robot can be provisioned without source changes or serial-only workflow.

### Iteration 12: Final Documentation and Hardening

Objective:

- Make the OTA system maintainable and auditable.

Scope:

- Documentation, operation, recovery, future security.

Files likely touched:

- `README.md`
- `docs/API.md`
- `docs/OTA_OPERATION.md`
- `docs/OTA_IMPLEMENTATION_PLAN.md`
- web repo `README.md`
- web repo `docs/ota-backend.md`

Subtasks:

- Document commands.
- Document endpoints.
- Document manifest.
- Document recovery.
- Document security limitations.
- Document future HTTPS/signature plan.

Acceptance criteria:

- Another engineer can reproduce OTA setup.
- All serial commands are documented.
- All endpoints are documented.
- Failure recovery is clear.

Mandatory tests:

- Follow docs from clean checkout.
- Build firmware.
- Start backend.
- Run OTA check.
- Run OTA update.

Risks:

- Docs drift from code.

Rollback/recovery:

- N/A.

Expected end state:

- OTA implementation is operationally usable.

## 13. Concrete Test Plan

### 13.1 Firmware/Network Tests

| Test | Expected Result |
|---|---|
| No Wi-Fi config | Firmware boots, robot works, Wi-Fi status says unconfigured |
| Correct Wi-Fi config | ESP32 connects and reports IP |
| Wrong Wi-Fi password | Connect fails with backoff, robot still works |
| Endpoint down | `OTA_CHECK` fails cleanly, no reboot |
| Manifest invalid JSON | Rejected, no reboot |
| Manifest missing required field | Rejected, no reboot |
| Manifest wrong project | Rejected, no reboot |
| Manifest wrong target | Rejected, no reboot |
| Manifest same build | Reports up-to-date |
| Manifest lower build | Rejects downgrade |
| Manifest higher build | Reports update available |
| Binary missing | Rejected, no boot switch |
| SHA256 incorrect | Rejected, no boot switch |
| Size incorrect | Rejected, no boot switch |
| OTA success | Reboots into new version |
| New app not confirmed | Rolls back |
| Power loss during download | Existing firmware remains bootable |
| Robot moving | OTA refused |
| STOP ALL fails | OTA refused |

### 13.2 Backend Tests

| Test | Expected Result |
|---|---|
| `GET /api/health` | 200 JSON `{ ok: true }` |
| `GET /api/firmware/latest` | 200 valid manifest |
| `GET /firmware/<bin>` | 200 binary with content length |
| Path traversal | 400 or 404 |
| Missing binary | 404 |
| Manifest URL generation | Uses LAN IP, not localhost |
| SHA generation | Matches `sha256sum` |

### 13.3 Robot Safety Tests

| Test | Expected Result |
|---|---|
| `OTA_UPDATE` after `STOP ALL` | Allowed if other checks pass |
| `OTA_UPDATE` during nonzero `MOVE_VEL` | Refused |
| `OTA_UPDATE` while speed target nonzero | Refused |
| `OTA_UPDATE` with stale telemetry | Ignored unless a tracked command is active; bench setup may have only one online motor |
| `OTA_UPDATE` after safe stop timeout | Refused |

## 14. Additional Data Needed From User

Required before implementation:

- Confirm actual ESP32 serial port, or allow automatic detection from `/dev/serial/by-id`.
- Restore/install ESP-IDF v5.4.1 and esptool in a persistent path.
- Confirm whether `/dev/ttyACM0` is the ESP32-S3.
- Provide Wi-Fi SSID/password for test via environment variable or local ignored file, not in Git.
- Confirm LAN IP to expose the server. Current tested IP is `192.168.1.107` on the local lab LAN.
- Confirm whether the server runs from Linux, WSL, Windows, or macOS.
- Confirm firewall allows inbound TCP on chosen port, recommended `8080`.
- Confirm whether Node/Express is acceptable for backend implementation.
- Confirm whether the current bench should continue assuming only controller `0x02/M1` is connected.
- Confirm whether automatic OTA writes should remain disabled until manual `OTA_UPDATE`, rollback, and serial recovery are proven.

Optional but useful:

- Preferred firmware version scheme.
- Device ID naming convention for Toño.
- Whether NVS encryption, secure boot, or flash encryption is planned later.
- Whether OTA should require operator confirmation from serial/web UI.

## 15. Approval Checklist

### 15.1 Before Modifying Firmware Code

- [ ] Git baseline commit exists, or a full verified backup exists outside the working tree.
- [ ] ESP-IDF v5.4.1 available in current shell.
- [ ] `idf.py --version` works.
- [ ] `idf.py build` works from clean state.
- [ ] `idf.py size` output captured.
- [ ] `flash_id` confirms real flash size.
- [ ] `CONFIG_ESPTOOLPY_FLASHSIZE` matches physical flash if physical flash is larger than 2 MB.
- [ ] Correct serial port identified.
- [ ] Partition table decision approved.
- [ ] Backend choice approved.
- [ ] Wi-Fi credential handling approved.

### 15.2 Before Flashing Partition Migration

- [ ] Previous firmware binary saved.
- [ ] Current serial recovery method verified.
- [ ] OTA partition table decoded and inspected.
- [ ] App binary fits slot with margin.
- [ ] App binary uses no more than 70-75% of the selected OTA slot.
- [ ] User accepts that NVS may be erased or invalidated.
- [ ] Robot is physically safe and stopped.

### 15.3 Before Testing Real OTA

- [ ] Manual `VERSION` works.
- [ ] Wi-Fi station works.
- [ ] Backend health endpoint works on LAN IP.
- [ ] Manifest uses LAN IP, not localhost.
- [ ] SHA256 verified with local tool.
- [ ] `OTA_CHECK` works without writing flash.
- [ ] `OTA_DOWNLOAD_TEST` verifies the binary in the inactive OTA slot without changing `VERSION` partition.
- [ ] Robot safety gate works.
- [ ] Rollback plan exists.
- [ ] Serial reflash recovery path verified.
- [ ] Automatic OTA writes are disabled.

## 16. Recommended Next Decision

Current completed baseline:

1. Iteration 0 restored the ESP-IDF toolchain, measured the board and documented the original firmware state.
2. Iteration 1 added firmware version metadata and the `VERSION` serial command.
3. Iteration 2 migrated the board to `partitions_ota_16mb.csv`, updated `CONFIG_ESPTOOLPY_FLASHSIZE` to `16MB`, and confirmed boot from `ota_0`.
4. Iteration 3 added NVS-backed configuration diagnostics.
5. Iteration 4 added manual Wi-Fi station mode and confirmed real Wi-Fi connectivity.
6. Iteration 5 added the local backend/UI server for manifest and firmware binary serving.
7. Iteration 6 added `OTA_CHECK`, which fetches and validates the manifest without downloading or writing firmware.
8. Iteration 7 added `OTA_DOWNLOAD_TEST`, which downloads, writes to the inactive OTA slot, verifies size/SHA256, and does not switch boot partitions or reboot.
9. Iteration 8 added manual `OTA_UPDATE`, including Wi-Fi connected check, robot safety gate, STOP preparation, boot partition switch, and controlled reboot.
10. Iteration 9 enabled ESP-IDF rollback, added post-boot self-test before marking OTA images valid, and validated success, no-confirm rollback and forced self-test rollback.
11. Phase 9.5-B tested compiler/heap/FreeRTOS/Wi-Fi IRAM configuration experiments. None recovered the reported first 16 KB `IRAM` bucket, and all experiments were reverted.
12. Phase 9.5-C identified that `IRAM 16383 / 16384` is the fixed `0x40374000-0x40378000` bucket before `_diram_i_start`. Normal C logic grows `Flash Code`, while `IRAM_ATTR` grows executable internal RAM and may appear as `DIRAM .text` once the first bucket is saturated.
13. Follow-up verification rebuilt B2-B5 using `DIRAM .text` / full `.iram0.text` as the correct metric. B3 and B4 were physically validated. B4 is the recommended first optimization because it recovers about 9.6 KB executable internal RAM without moving FreeRTOS task functions to flash.
14. B4 was selected for permanent consolidation: keep `CONFIG_ESP_WIFI_IRAM_OPT=y` and disable `CONFIG_ESP_WIFI_RX_IRAM_OPT`.

Implement next:

1. Iteration 10 may add automatic manifest polling only if it is implemented as normal non-ISR FreeRTOS/application logic.
2. Iteration 10 must keep OTA writes/reboots disabled by default. It may automate `OTA_CHECK`; it must not automate `OTA_UPDATE`.
3. Keep SoftAP disabled until explicit approval.

Leave for later phases:

- Automatic OTA writes/reboots.
- SoftAP provisioning.
- HTTPS, firmware signing, secure boot and flash encryption.

Do not implement yet:

- Automatic OTA writes/reboots.
- SoftAP provisioning.
- HTTPS.
- Firmware signing.
- Secure boot.
- Flash encryption.

Main risks before the next code iteration:

- Physical flash and configured flash are now both `16MB`; future iterations must not regress `CONFIG_ESPTOOLPY_FLASHSIZE`, the selected CSV, or slot sizing.
- IRAM is currently `16383 / 16384` bytes because the fixed first 16 KB bucket is full. Future iterations must monitor `IRAM`, `DIRAM .text`, total `DIRAM`, full `.iram0.text` from the map file, `Flash Code`, and total image size.
- New `IRAM_ATTR`, ISR handlers, `ESP_INTR_FLAG_IRAM`, IRAM-safe driver options, low-level SPI flash/cache/MMU changes, heap/FreeRTOS placement changes, and enabling `ppm_decoder` are blocked unless separately audited.
- Wrong USB port could cause failed diagnostics.
- Partition migration is already complete, but any future partition edit remains high risk and requires full build, partition-table and boot validation.
- OTA must be blocked unless robot safety state is known.
