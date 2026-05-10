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
DATA VERSION PROJECT:sistema-motriz-rs485 TARGET:esp32s3 VERSION:1.0.0 BUILD_NUMBER:2 IDF:v5.4.1 PARTITION:ota_0
```

```text
CONFIG_STATUS
CONFIG_CLEAR
```

`CONFIG_STATUS` prints the persistent Wi-Fi and OTA configuration currently loaded from NVS. It never prints the Wi-Fi password value; it only reports `WIFI_PASSWORD:<set>` or `WIFI_PASSWORD:<empty>`.

`CONFIG_CLEAR` erases the config namespace and restores runtime defaults.

Default values:

- `WIFI_SSID:<empty>`
- `WIFI_PASSWORD:<empty>`
- `OTA_HOST:192.168.10.10`
- `OTA_PORT:8080`
- `OTA_MANIFEST:/api/firmware/latest`
- `OTA_AUTO_CHECK:0`
- `OTA_AUTO_UPDATE:0`

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
DATA WIFI STATUS:UNCONFIGURED SSID:<empty> IP:<none> RSSI:0 RETRIES:0/3 DISCONNECT_REASON:0 LAST_ERR:0x0
DATA WIFI STATUS:CONNECTED SSID:BotfarmsNet IP:192.168.10.42 RSSI:-58 RETRIES:0/3 DISCONNECT_REASON:0 LAST_ERR:0x0
```

States are `UNCONFIGURED`, `DISCONNECTED`, `CONNECTING`, `CONNECTED`, and `FAILED`.

`WIFI_CONNECT` starts an asynchronous station connection using credentials from NVS. It does not block robot control. If no SSID is configured it returns `ERR WIFI_NOT_CONFIGURED`.

`WIFI_DISCONNECT` disconnects station mode without deleting saved credentials. Use `WIFI_CLEAR` to erase credentials.

```text
OTA_SET_SERVER host port
OTA_SET_MANIFEST path
OTA_CONFIG
OTA_CHECK
OTA_DOWNLOAD_TEST
OTA_UPDATE
OTA_AUTO_CHECK ON|OFF
OTA_AUTO_UPDATE OFF
```

Stores local OTA server config in NVS. `OTA_AUTO_UPDATE ON` is intentionally blocked until manual OTA, rollback, and serial recovery are validated.

`OTA_CHECK` performs a manifest-only HTTP GET using the configured server and path. It validates project, target, build number, `min_supported_build`, URL, filename, size and SHA256, then reports whether the remote build is current or newer. It does not download the firmware binary, write flash, switch partitions or reboot.

`OTA_DOWNLOAD_TEST` repeats the manifest validation, downloads the firmware binary, writes it to the inactive OTA partition returned by ESP-IDF, verifies byte count and SHA256, and finalizes the image with `esp_ota_end`. It does not call `esp_ota_set_boot_partition`, does not reboot, and does not change the active firmware.

`OTA_UPDATE` is the manual real OTA path. It requires Wi-Fi to be connected and the robot to be stopped/safe, downloads and verifies the binary, stops known active/online motors, calls `esp_ota_set_boot_partition` only after `esp_ota_end` succeeds, then reboots with `esp_restart`. Automatic OTA remains disabled.

Example:

```text
DATA OTA_CHECK STATUS:UP_TO_DATE PROJECT:sistema-motriz-rs485 TARGET:esp32s3 VERSION:1.0.0 BUILD_NUMBER:2 CURRENT_BUILD:2 MIN_SUPPORTED_BUILD:1 SIZE:972032 SHA256:<64-hex> FILENAME:sistema-motriz-rs485-v1.0.0-b2.bin URL:http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b2.bin
DATA OTA_DOWNLOAD_TEST STATUS:VERIFIED PARTITION:ota_1 BYTES:972032 SHA256:<64-hex>
DATA OTA_UPDATE STATUS:REBOOTING PARTITION:ota_1 BYTES:972032 SHA256:<64-hex>
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

For `WIFI_SET`, trace output redacts the password.

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
