---
name: tono-ota-update
description: Use before changing, documenting, testing, or executing OTA updates for this ESP32-S3 SVD48 firmware. Prevents agents from changing the ESP-IDF target, rollback, partition layout, manifest fields, or update sequence incorrectly.
---

# Toño OTA Update Skill

Use this skill whenever the task mentions OTA, firmware update, rollback, release binary, manifest, Wi-Fi update, LAN announce, `OTA_CHECK`, `OTA_DOWNLOAD_TEST`, or `OTA_UPDATE`.

## Non-Negotiable Rules

- Keep `FW_TARGET` as `esp32s3`.
- Keep rollback enabled: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
- Keep the custom 16 MB OTA partition table unless the user explicitly requests a storage redesign.
- Do not hand-write manifest `size` or `sha256`; use `tools/ota_prepare_release.py`.
- Do not use `localhost`, `127.0.0.1`, or `0.0.0.0` as the OTA manifest/binary host.
- Do not enable automatic firmware writes. `OTA_AUTO_UPDATE ON` is intentionally blocked.
- Do not expose Wi-Fi passwords in docs, logs, or final answers.
- Do not expose OTA announce tokens in docs, logs, or final answers.
- Keep Wi-Fi/OTA tasks lower priority than RC, SVD48 polling, serial gateway, and robot safety.
- Do not make Wi-Fi/OTA initialization failure a rollback condition or a robot startup blocker.
- Treat OTA as a supervised prototype process. Keep USB serial recovery available.

## Sources Of Truth

Read these before making OTA changes:

- `main/app_version.h`: `FW_PROJECT`, `FW_TARGET`, `FW_VERSION`, `FW_BUILD_NUMBER`.
- `sdkconfig` and `sdkconfig.defaults`: target, flash size, rollback and partition settings.
- `partitions_ota_16mb.csv`: `ota_0` and `ota_1` app slots.
- `components/ota_manager/ota_manager.c`: manifest validation, download, SHA256, boot partition, rollback helpers.
- `components/ota_announce/ota_announce.c`: authenticated UDP LAN announce listener.
- `components/wifi_manager/wifi_manager.c`: station connect and low-priority auto-reconnect.
- `components/robot_safety/robot_safety.c`: high-priority safety supervisor consulted before writes/reboots.
- `components/serial_gateway/serial_gateway.c`: serial OTA commands and response shapes.
- `tools/ota_announce.py`: developer script for no-USB OTA server discovery.
- `ota_documentation_for_dummies.md`: operator tutorial.
- `docs/API.md`: serial API contract.

## Correct Developer Flash Path

Use direct serial flash for development and recovery:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash
python3 tools/robotctl.py --port /dev/ttyACM0 raw VERSION
python3 tools/robotctl.py --port /dev/ttyACM0 platform-status
```

Always detect the port first:

```bash
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB*
```

## Correct OTA Release Path

1. Increment `FW_BUILD_NUMBER` in `main/app_version.h` for every real update. Do not reuse a build number with different firmware bytes; OTA download/update requires the manifest build to be strictly newer than the running firmware.
2. Build:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py build
```

3. Determine a LAN IP reachable by the ESP32:

```bash
ip route get 8.8.8.8 | awk '{print $7; exit}'
```

4. Generate release files:

```bash
python3 tools/ota_prepare_release.py --host <LAN_IP> --port 8080
```

5. Serve them:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

6. Configure the ESP32:

```bash
python3 tools/robotctl.py --port <PORT> raw WIFI_CONNECT
python3 tools/robotctl.py --port <PORT> raw WIFI_STATUS
python3 tools/robotctl.py --port <PORT> raw OTA_SET_SERVER <LAN_IP> 8080
python3 tools/robotctl.py --port <PORT> raw OTA_SET_MANIFEST /api/firmware/latest
python3 tools/robotctl.py --port <PORT> raw OTA_ANNOUNCE_TOKEN_SET "<token>"
python3 tools/robotctl.py --port <PORT> raw OTA_CONFIG
python3 tools/robotctl.py --port <PORT> raw OTA_ANNOUNCE_STATUS
```

Only run `WIFI_SET "ssid" "password"` if credentials are missing or wrong. Warn that shell history may contain the password.
Once credentials are stored, the low-priority Wi-Fi supervisor auto-connects on boot and reconnects with backoff. `WIFI_DISCONNECT` pauses auto-connect until `WIFI_CONNECT` or `WIFI_SET`.

7. Validate without switching boot partition:

```bash
python3 tools/robotctl.py --port <PORT> raw OTA_CHECK
python3 tools/robotctl.py --port <PORT> --timeout 30 raw OTA_DOWNLOAD_TEST
```

8. Make the robot safe:

```bash
python3 tools/robotctl.py --port <PORT> raw STOP ALL
python3 tools/robotctl.py --port <PORT> platform-status
```

Proceed only when `SAFE_FOR_OTA:1 SAFE_REASON:SAFE`.

9. Run real OTA:

```bash
python3 tools/robotctl.py --port <PORT> --timeout 60 raw OTA_UPDATE
```

10. Verify after reboot:

```bash
python3 tools/robotctl.py --port <PORT> raw VERSION
python3 tools/robotctl.py --port <PORT> raw OTA_ROLLBACK_STATUS
python3 tools/robotctl.py --port <PORT> platform-status
```

Expected final state includes `OTA_STATE:VALID` and `PENDING_VERIFY:0`.

## Correct LAN Announce Path Without USB

Use this only after the ESP32 has Wi-Fi credentials and an OTA announce token stored through serial provisioning.

1. Build and prepare the release normally.
2. Serve it from the developer laptop:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

3. Announce from the same laptop. The ESP32 uses the UDP packet source IP as the OTA host:

```bash
BOTFARMS_OTA_TOKEN="<token>" python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action check
```

4. For a real update:

```bash
BOTFARMS_OTA_TOKEN="<token>" python3 tools/ota_announce.py --server-port 8080 --manifest /api/firmware/latest --action update
```

`--action update` is an explicit command, not automatic update. The firmware still checks Wi-Fi state and `robot_control_is_safe_for_ota()` before downloading, switching boot partition, or rebooting.

## Manifest Contract

The manifest served at `/api/firmware/latest` must be JSON with:

```json
{
  "project": "sistema-motriz-rs485",
  "target": "esp32s3",
  "version": "1.0.0",
  "build_number": 6,
  "min_supported_build": 1,
  "url": "http://<LAN_IP>:8080/firmware/sistema-motriz-rs485-v1.0.0-b6.bin",
  "filename": "sistema-motriz-rs485-v1.0.0-b6.bin",
  "size": 994208,
  "sha256": "<64 lowercase or uppercase hex chars>"
}
```

`tools/ota_prepare_release.py` creates this from the built binary and should be the default path.

## Response Meaning

- `OTA_CHECK`: validates manifest only. It does not write flash.
- `OTA_DOWNLOAD_TEST`: requires Wi-Fi connected and robot safe, downloads and verifies into the inactive partition. It does not reboot or switch boot partition.
- `OTA_UPDATE`: requires Wi-Fi connected and robot safe, downloads/verifies, prepares/stops robot, sets boot partition, then reboots.
- `OTA_ROLLBACK_STATUS`: reports running partition and ESP-IDF rollback state.
- `OTA_AUTO_STATUS`: reports the manifest-only background checker.
- `OTA_ANNOUNCE_STATUS`: reports authenticated UDP announce listener activity.

## Validation Before Final Answer

For documentation/script-only changes:

```bash
python3 -m py_compile tools/ota_prepare_release.py tools/ota_announce.py
python3 tools/ota_prepare_release.py --host 192.168.1.100 --dry-run
```

For firmware changes:

```bash
python3 tools/test_svd48_protocol.py
. /home/jp/esp/esp-idf-v5.4.1/export.sh && idf.py build
```

For hardware-connected OTA validation, also run serial checks through `robotctl.py`.

## Common Failure Mapping

- `BAD_SERVER_CONFIG`: configured host/path is invalid; check `OTA_CONFIG`.
- `BAD_URL`: manifest URL is localhost/invalid.
- `PROJECT_MISMATCH`: manifest `project` does not match `FW_PROJECT`.
- `TARGET_MISMATCH`: manifest `target` does not match `FW_TARGET`.
- `SHA256_MISMATCH`: binary does not match generated manifest.
- `WIFI_NOT_CONNECTED`: connect Wi-Fi and verify `WIFI_STATUS`.
- `ROBOT_NOT_SAFE`: `STOP ALL`, verify `PLATFORM_STATUS`, and do not force update.
- `TOKEN_NOT_CONFIGURED`: set `OTA_ANNOUNCE_TOKEN_SET` once over serial.
- `BAD_TOKEN`: the LAN announce token does not match the ESP32 token.
