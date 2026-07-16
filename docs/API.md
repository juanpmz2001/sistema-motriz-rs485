# Robot Serial API

This is the PC-to-ESP32 contract for the SVD48 robot framework. Commands are ASCII lines terminated by `\n`. Responses are ASCII lines beginning with `OK`, `ERR`, or `DATA`.

Read `docs/skills/SVD48B50A_SKILL.md` before changing any RS485 register behavior.

## Transport

- Default transport: USB serial console exposed by the ESP32-S3.
- Baud rate: `115200` when using a UART bridge.
- Commands are case-insensitive.
- Motor indices are zero-based: `MOTOR_0`..`MOTOR_3`.
- Default mapping: drive ID 1 M1/M2 -> motors 0/1, drive ID 2 M1/M2 -> motors 2/3.

## Commands

```text
PING
```

Returns `OK PONG`.

```text
VERSION
```

Returns firmware identity and boot context without printing secrets.

Example:

```text
DATA VERSION PROJECT:sistema-motriz-rs485 TARGET:esp32s3 VERSION:1.0.0 BUILD_NUMBER:5 IDF:v5.4.1 PARTITION:ota_0 OTA_STATE:VALID PENDING_VERIFY:0 ROLLBACK_POSSIBLE:1
```

```text
PLATFORM_STATUS
```

Returns the current read-only platform boundary status for supervised prototype tests. This command does not change motor state and does not implement heartbeat, E-stop, command leasing or Jetson arbitration.

Example:

```text
DATA PLATFORM STATE:SAFE_IDLE AUTHORITY:SERIAL_ASCII PROTOCOL:ASCII_V1 HEARTBEAT:UNSUPPORTED ESTOP:UNSUPPORTED LAST_SEQ:3 LAST_AGE_MS:1204 MOTION_ACTIVE:0 SAFE_FOR_OTA:1 SAFE_REASON:SAFE ONLINE:4 STALE:0 RUNNING:0 FAULTED:0 TRACE:0 STREAM:0
```

Fields:

- `STATE`: `SAFE_IDLE`, `MOTION_ACTIVE`, or `FAULT`.
- `AUTHORITY`: current command authority. The current firmware only supports `SERIAL_ASCII`.
- `PROTOCOL`: current PC/Jetson gateway protocol. The current firmware uses `ASCII_V1`.
- `HEARTBEAT` and `ESTOP`: currently `UNSUPPORTED`.
- `LAST_SEQ`: sequence number for the last accepted movement or stop command.
- `LAST_AGE_MS`: elapsed milliseconds since that accepted command. It is `0` before the first tracked command.
- `MOTION_ACTIVE`: `1` when the last accepted command or online/non-stale telemetry indicates motion.
- `SAFE_FOR_OTA` and `SAFE_REASON`: result from the same safety gate used by `OTA_UPDATE`.
- `ONLINE`, `STALE`, `RUNNING`, and `FAULTED`: aggregate counts across the four logical motors.
- `TRACE` and `STREAM`: current serial diagnostics and telemetry stream state.

```text
SAFETY_STATUS
```

Returns the high-priority safety supervisor state. This task does not perform Wi-Fi, HTTP, JSON, OTA or NVS work.

Example:

```text
DATA SAFETY TASK:RUNNING RC_AVAILABLE:1 RC_SEEN:1 RC_VALID:1 RC_LOSS:0 RC_LAST_AGE_MS:12 MOTOR_FAULT:0 STOP_REQUESTS:0 LAST_STOP_REASON:NONE LAST_STOP_ERR:0x0 LOOPS:5210
```

The safety task requests `STOP ALL` if it has seen valid RC/i-BUS signal and then detects RC loss beyond the configured timeout, or if online/non-stale motor telemetry reports a fault.

```text
CONFIG_STATUS
CONFIG_CLEAR
```

`CONFIG_STATUS` prints the persistent Wi-Fi and OTA configuration currently loaded from NVS. It never prints the Wi-Fi password or OTA announce token values; it only reports `<set>` or `<empty>`.

`CONFIG_CLEAR` erases the config namespace and restores runtime defaults.

Default values:

- `WIFI_SSID:<empty>`
- `WIFI_PASSWORD:<empty>`
- `OTA_HOST:192.168.10.10`
- `OTA_PORT:8080`
- `OTA_MANIFEST:/api/firmware/latest`
- `OTA_AUTO_CHECK:0`
- `OTA_AUTO_INTERVAL_MS:600000`
- `OTA_AUTO_UPDATE:0`
- `OTA_ANNOUNCE_TOKEN:<empty>`

```text
WIFI_SET "ssid" "password"
WIFI_CLEAR
WIFI_STATUS
WIFI_CONNECT
WIFI_DISCONNECT
```

Stores or clears Wi-Fi credentials in NVS. Quote SSIDs or passwords that contain spaces. `WIFI_SET` responds with a redacted password state and never echoes the password.

`WIFI_STATUS` reports station state without secrets:

```text
DATA WIFI STATUS:UNCONFIGURED SSID:<empty> IP:<none> RSSI:0 RETRIES:0/3 DISCONNECT_REASON:0 LAST_ERR:0x0 AUTOCONNECT:RUNNING PAUSED:0 RETRY_DELAY_MS:5000
DATA WIFI STATUS:CONNECTED SSID:BotfarmsNet IP:192.168.10.42 RSSI:-58 RETRIES:0/3 DISCONNECT_REASON:0 LAST_ERR:0x0 AUTOCONNECT:RUNNING PAUSED:0 RETRY_DELAY_MS:5000
```

States are `UNCONFIGURED`, `DISCONNECTED`, `CONNECTING`, `CONNECTED`, and `FAILED`.

`WIFI_CONNECT` starts an asynchronous station connection using credentials from NVS and resumes auto-connect. It does not block robot control. If no SSID is configured it returns `ERR WIFI_NOT_CONFIGURED`.

When credentials are saved, a low-priority `wifi_reconnect` supervisor connects on boot and retries with backoff after failures or disconnects. `WIFI_DISCONNECT` disconnects station mode and pauses auto-connect without deleting saved credentials. Use `WIFI_CLEAR` to erase credentials.

```text
OTA_SET_SERVER host port
OTA_SET_MANIFEST path
OTA_ANNOUNCE_TOKEN_SET token
OTA_ANNOUNCE_TOKEN_CLEAR
OTA_ANNOUNCE_STATUS
OTA_CONFIG
OTA_CHECK
OTA_DOWNLOAD_TEST
OTA_UPDATE
OTA_ROLLBACK_STATUS
OTA_ROLLBACK_TEST NONE|NO_CONFIRM_ONCE|SELF_TEST_FAIL_ONCE
OTA_AUTO_STATUS
OTA_AUTO_FORCE_CHECK
OTA_AUTO_INTERVAL [milliseconds]
OTA_AUTO_CHECK ON|OFF
OTA_AUTO_UPDATE OFF
```

Stores local OTA server config in NVS. `OTA_AUTO_UPDATE ON` is intentionally blocked until manual OTA, rollback, and serial recovery are validated.

`OTA_ANNOUNCE_TOKEN_SET` stores the authenticated UDP announce token in NVS and never echoes the token. `OTA_ANNOUNCE_TOKEN_CLEAR` removes it. `TRACE` output redacts `OTA_ANNOUNCE_TOKEN_SET`.

`OTA_ANNOUNCE_STATUS` reports the low-priority UDP listener on port `32320`:

```text
DATA OTA_ANNOUNCE TASK:RUNNING PORT:32320 SEEN:1 ACCEPTED:1 REJECTED:0 CHECKS:1 DOWNLOAD_TESTS:0 UPDATES:0 LAST_SENDER:192.168.1.107 LAST_ACTION:check DETAIL:UPDATE_AVAILABLE
```

An announce packet must be JSON:

```json
{"type":"botfarms_ota_offer","token":"<token>","port":8080,"manifest":"/api/firmware/latest","action":"check"}
```

Accepted actions are `config`, `check`, `download_test`, and `update`. After token validation, the ESP32 uses the UDP packet source IP as the OTA host and saves `host:port` plus `manifest` to NVS. `download_test` and `update` require `SAFE_FOR_OTA:1`; `update` reboots only after a verified download and boot-partition switch.

`OTA_CHECK` performs a manifest-only HTTP GET using the configured server and path. It validates project, target, build number, `min_supported_build`, URL, filename, size and SHA256, then reports whether the remote build is current or newer. It does not download the firmware binary, write flash, switch partitions or reboot.

`OTA_DOWNLOAD_TEST` repeats the manifest validation, downloads the firmware binary, writes it to the inactive OTA partition returned by ESP-IDF, verifies byte count and SHA256, and finalizes the image with `esp_ota_end`. It requires Wi-Fi connected and the robot to be safe for OTA, does not call `esp_ota_set_boot_partition`, does not reboot, and does not change the active firmware. Manual download/update paths reject a lower remote build as `BUILD_DOWNGRADE` and reject the same remote build as `BUILD_NOT_NEWER`.

`OTA_UPDATE` is the manual real OTA path. It requires Wi-Fi to be connected and the robot to be stopped/safe, downloads and verifies the binary, stops known active/online motors, calls `esp_ota_set_boot_partition` only after `esp_ota_end` succeeds, then reboots with `esp_restart`. Automatic OTA writes remain disabled.

`OTA_AUTO_CHECK ON` enables a low-priority background task that periodically runs the same manifest-only validation as `OTA_CHECK`. It does not download firmware, write flash, switch partitions, call `OTA_UPDATE`, or reboot. The first check runs soon after enabling; later checks use the configured interval, with backoff on failures. `OTA_AUTO_CHECK OFF` disables background checks.

The background task does not print unsolicited `DATA OTA_AUTO_CHECK` lines. It only updates internal state so serial command responses are not interleaved with automatic reports. During background checks it also suppresses routine HTTP-client log tags; use `OTA_AUTO_STATUS` to inspect the last automatic or forced check result.

`OTA_AUTO_FORCE_CHECK` runs one manifest-only diagnostic check immediately, even when `OTA_AUTO_CHECK` is off. It requires Wi-Fi to already be connected, refuses to run while another OTA operation is active, and never downloads, writes flash, switches partitions, or reboots.

Example:

```text
DATA OTA_AUTO_FORCE_CHECK STATUS:UP_TO_DATE CURRENT_BUILD:5 REMOTE_BUILD:5 HTTP_STATUS:200 UPDATE_AVAILABLE:0
ERR OTA_AUTO_FORCE_CHECK WIFI_NOT_CONNECTED
ERR OTA_AUTO_FORCE_CHECK BUSY
```

`OTA_AUTO_INTERVAL` prints the persisted auto-check interval in milliseconds. `OTA_AUTO_INTERVAL <milliseconds>` validates, saves and applies the interval at runtime. The accepted range is `60000..86400000` ms. The default is `600000` ms.

`OTA_AUTO_STATUS` reports the background checker state, last result and next scheduled check:

```text
DATA OTA_AUTO TASK:RUNNING ENABLED:1 CHECKING:0 INTERVAL_MS:600000 BACKOFF_MS:30000 NEXT_DELAY_MS:598800 CHECK_COUNT:1 FAILURE_COUNT:0 LAST_RESULT:UP_TO_DATE LAST_ERROR:0x0 LAST_HTTP_STATUS:200 LAST_CHECK_AGE_MS:1200 LAST_CHECK_TIME_MS:42110 CURRENT_BUILD:5 LAST_REMOTE_BUILD:5 LAST_VERSION:1.0.0 UPDATE_AVAILABLE:0 AUTO_UPDATE_ENABLED:0 DETAIL:OK LAST_URL:http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b5.bin
```

`OTA_AUTO_UPDATE ON` remains blocked and currently returns `ERR AUTO_UPDATE_DISABLED_UNTIL_EXPLICITLY_APPROVED`.

With rollback enabled, a newly booted OTA image starts as `PENDING_VERIFY`. The firmware marks it `VALID` only after NVS, `config_manager`, `svd48`, SVD48 polling, `robot_control`, `robot_safety`, and `serial_gateway` start successfully. Wi-Fi and OTA services are maintenance paths; failure to initialize or associate Wi-Fi does not by itself cause rollback or block robot startup.

`OTA_ROLLBACK_STATUS` reports the running partition, OTA image state, whether rollback is currently possible, and any pending one-shot rollback test flag.

`OTA_ROLLBACK_TEST` is for lab validation only:

- `NONE` or `CLEAR`: clear the test flag.
- `NO_CONFIRM_ONCE`: on the next pending OTA boot, reboot before marking the app valid. This simulates reset or power loss before confirmation.
- `SELF_TEST_FAIL_ONCE`: on the next pending OTA boot, force self-test failure and call ESP-IDF rollback.

The test flag is consumed once and stored in NVS namespace `bot_ota`; it does not contain secrets.

Example:

```text
DATA OTA_CHECK STATUS:UP_TO_DATE PROJECT:sistema-motriz-rs485 TARGET:esp32s3 VERSION:1.0.0 BUILD_NUMBER:5 CURRENT_BUILD:5 MIN_SUPPORTED_BUILD:1 SIZE:988656 SHA256:<64-hex> FILENAME:sistema-motriz-rs485-v1.0.0-b5.bin URL:http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b5.bin
DATA OTA_DOWNLOAD_TEST STATUS:VERIFIED PARTITION:ota_1 BYTES:988656 SHA256:<64-hex>
DATA OTA_UPDATE STATUS:REBOOTING PARTITION:ota_1 BYTES:988656 SHA256:<64-hex>
DATA OTA_ROLLBACK PARTITION:ota_0 OTA_STATE:VALID PENDING_VERIFY:0 ROLLBACK_POSSIBLE:1 STATE_ERR:0x0 TEST_MODE:NONE TEST_ERR:0x0
```

```text
TRACE ON
TRACE OFF
TRACE STATUS
```

Enables or disables detailed RS485 diagnostics on the same USB serial console. `TRACE STATUS` reports the active CRC settings.

Trace lines include:

- `TRACE PC_RX ASCII:"..."`: command received from the PC.
- `TRACE RS485_TX ... HEX:...`: exact frame written by ESP32 UART2 to the RS485 adapter.
- `TRACE RS485_RX ... HEX:...`: raw response bytes or timeout.
- `TRACE RS485_DECODE ...`: decoded register/value/error view.

CRC remains `init=0xFFFF`, `poly=0xA001`, transmitted as high byte then low byte.

For `WIFI_SET`, trace output redacts the password. For `OTA_ANNOUNCE_TOKEN_SET`, trace output redacts the token.

```text
POLL_ONCE
```

Runs one explicit telemetry poll. Use it with `TRACE ON` to inspect the read frames for speed, current, status, temperature, bus voltage, position and errors.

```text
READ_REG drive_id reg [count]
WRITE_REG drive_id reg value
```

Reads or writes raw SVD48 holding registers through the ESP32 RS485 bus. `drive_id`, `reg`, and `value` accept decimal or `0x` hex. Use this for SV-Config-equivalent inspection and changes.

```text
GET_SVD48_CONFIG drive_id [M1|M2|ALL]
```

Reads the motor configuration registers used for PY6514/PYD6514 validation: `0x5018/0x5019` pole pairs, `0x502C/0x502D` sensor type, `0x2201` wheel diameter, `0x2202/0x2203` gear teeth, and Hall status/install registers.

Use the optional channel argument when only one channel is physically configured, for example `GET_SVD48_CONFIG 0x02 M1`.

```text
APPLY_PY6514_CONFIG drive_id [M1|M2|ALL] CONFIRM
```

Writes the current Botfarms Toño hypothesis to one or both SVD48 channels: pole pairs `10`, sensor type `1/HALL`, wheel diameter `330 mm`, motor teeth `1`, wheel teeth `5`. Stop the motors before using it. For the current bench setup with only controller `0x02` M1 active, use `APPLY_PY6514_CONFIG 0x02 M1 CONFIRM`.

```text
GET_SPEED n
```

Returns current RPM for motor `n`.

Example:

```text
GET_SPEED 0
DATA MOTOR_0 RPM:1450 STALE:0 ONLINE:1
```

```text
GET_MOTOR n
```

Returns full telemetry for motor `n`: RPM, current in 0.1 A, status, bus voltage in 0.1 V, motor/MOS temperatures in 0.1 C, encoder position, error code, online flag, stale flag.

The response also includes `STEER_DEG`, which is the last commanded steering angle for that logical wheel. It is not an independent steering sensor reading.

```text
SET_SPEED n rpm
```

Starts motor `n` and writes a signed RPM target.

```text
ENABLE n|ALL
STOP n|ALL
CLEAR_FAULT n|ALL
```

Controls one motor or all motors.

```text
MOVE_VEL vx vy wz
```

Runs four-wheel independent steering kinematics. Units:

- `vx`: forward velocity in m/s.
- `vy`: left velocity in m/s.
- `wz`: yaw velocity in rad/s.

The firmware converts this into four wheel RPM targets and four steering servo angles.

Example:

```text
MOVE_VEL 1.0 0.0 0.5
OK MOVE_VEL VX:1.000 VY:0.000 WZ:0.500 M0:94/5.7 M1:114/4.6 M2:94/-5.7 M3:114/-4.6
```

```text
STREAM ON [period_ms]
STREAM OFF
```

Enables or disables periodic full telemetry output. `period_ms` must be between 50 and 10000.

## Error Shape

Errors are one-line responses:

```text
ERR UNKNOWN_COMMAND
ERR BAD_MOTOR
ERR USAGE GET_SPEED n
ERR MOVE_VEL_FAILED 0x107
```

Firmware errors include the ESP-IDF `esp_err_t` value in hex when useful.

## Safety Defaults

- `STOP ALL` writes zero speed and stop commands to all four logical motors.
- Telemetry is stale when no valid sample arrives within 1000 ms.
- The gateway serializes RS485 through the `svd48` driver; PC commands and telemetry polling do not write to UART concurrently.
- Direct speed commands and `MOVE_VEL` start motors automatically. Use `STOP` to disable motor motion.
