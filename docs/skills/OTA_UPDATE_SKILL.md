---
name: botfarms-ota-operations
description: Use before operating, debugging, documenting, or changing OTA, Wi-Fi maintenance, firmware releases, rollback, OTA LAN announce, ESP32-S3 recovery, or commands such as WIFI_STATUS, OTA_CONFIG, OTA_CHECK, OTA_DOWNLOAD_TEST, OTA_UPDATE, OTA_ROLLBACK_STATUS, OTA_AUTO_STATUS, and OTA_ANNOUNCE_STATUS in this repository.
---

# Botfarms OTA Operations Skill

## Operating Rules

- Keep `FW_TARGET` as `esp32s3`.
- Keep rollback enabled and keep `partitions_ota_16mb.csv` unless the user explicitly asks for a storage redesign.
- Treat USB serial as the recovery path. Do not rely on OTA when the ESP is not reachable.
- Never expose Wi-Fi passwords or OTA tokens in docs, logs, commit messages, PRs, or final answers.
- Never commit `.env`, `build/`, `ota_release/`, `sdkconfig`, generated `.bin` files, or generated `.pyc` files.
- Never hand-write manifest `size` or `sha256`; use `tools/ota_prepare_release.py`.
- Never use `localhost`, `127.0.0.1`, or `0.0.0.0` as the OTA host. The ESP must fetch from a LAN-reachable host.
- Never reuse `FW_BUILD_NUMBER` with different firmware bytes. Real OTA writes require a strictly newer build.
- Keep `OTA_AUTO_UPDATE ON` blocked. Automatic OTA may check manifests only; writes and reboot must remain explicit.
- Keep Wi-Fi/OTA lower priority than RC, SVD48 polling, serial gateway, robot safety, and motion control.
- Do not make Wi-Fi/OTA failure a robot startup blocker or rollback trigger.
- Do not run multiple `robotctl.py` serial commands in parallel. The serial gateway is a single stream and parallel readers corrupt responses.

## Read First

Read the smallest set needed for the task:

- Firmware identity: `main/app_version.h`.
- Boot flow and task startup: `main/main.c`.
- OTA manifest/download/rollback: `components/ota_manager/ota_manager.c`.
- LAN OTA announce listener: `components/ota_announce/ota_announce.c`.
- Wi-Fi connect/reconnect: `components/wifi_manager/wifi_manager.c`.
- Persistent config and token storage: `components/config_manager/config_manager.c`.
- Safety gate for OTA: `components/robot_control/robot_control.c` and `components/robot_safety/robot_safety.c`.
- Serial command contract: `components/serial_gateway/serial_gateway.c` and `docs/API.md`.
- Operator tutorial: `ota_documentation_for_dummies.md`.
- Release scripts: `tools/ota_prepare_release.py` and `tools/ota_announce.py`.
- Historical design context: `docs/OTA_IMPLEMENTATION_PLAN.md`.

## Architecture Summary

- `wifi_manager` stores station credentials in NVS, auto-connects on boot, and reconnects with low-priority backoff.
- `ota_manager` validates manifests, downloads binaries, verifies size/SHA256, writes inactive OTA slots, sets boot partitions, and reports rollback state.
- `ota_announce` listens on UDP `32320`, validates a stored token, uses the packet source IP as the temporary OTA server, and runs `config`, `check`, `download_test`, or `update`.
- `robot_safety` is high priority and does not do Wi-Fi, HTTP, JSON, OTA, or NVS.
- `OTA_DOWNLOAD_TEST` and `OTA_UPDATE` require Wi-Fi connected and `SAFE_FOR_OTA:1`.
- `OTA_CHECK` is manifest-only; it does not write flash.
- Same-build writes are rejected as `BUILD_NOT_NEWER`; lower builds are rejected as `BUILD_DOWNGRADE`.

## USB Recovery And Factory Setup

Use this when the ESP is new, unreachable, boot-looping, or after a bad OTA:

```bash
ls -l /dev/serial/by-id /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

Then query serial one command at a time:

```bash
python3 tools/robotctl.py --port <PORT> raw VERSION
python3 tools/robotctl.py --port <PORT> platform-status
python3 tools/robotctl.py --port <PORT> raw SAFETY_STATUS
python3 tools/robotctl.py --port <PORT> raw WIFI_STATUS
python3 tools/robotctl.py --port <PORT> raw OTA_CONFIG
python3 tools/robotctl.py --port <PORT> raw OTA_ANNOUNCE_STATUS
```

Expected healthy baseline after recovery:

- `TARGET:esp32s3`
- `OTA_STATE:VALID`
- `PENDING_VERIFY:0`
- `SAFE_FOR_OTA:1` before writes/reboots
- Wi-Fi either `CONNECTED` or reconnecting without blocking robot startup
- `OTA_ANNOUNCE TASK:RUNNING` once OTA announce is initialized

If credentials are missing or wrong, provision over serial:

```bash
python3 tools/robotctl.py --port <PORT> raw WIFI_SET "<ssid>" "<password>"
python3 tools/robotctl.py --port <PORT> raw WIFI_CONNECT
```

If the announce token is missing, set it once over serial. Do not print the real token:

```bash
python3 tools/robotctl.py --port <PORT> raw OTA_ANNOUNCE_TOKEN_SET "<token>"
```

## Real OTA Release Flow

1. Read `main/app_version.h`.
2. Increment `FW_BUILD_NUMBER` for every real OTA update.
3. Build:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py build
```

4. Find the developer machine LAN IP reachable by the ESP:

```bash
ip route get 8.8.8.8 | awk '{print $7; exit}'
```

5. Generate the release:

```bash
python3 tools/ota_prepare_release.py --host <LAN_IP> --port 8080
```

`ota_prepare_release.py` must be allowed to fail if the binary is stale or if the same build number would overwrite different bytes. Fix by rebuilding or incrementing `FW_BUILD_NUMBER`; do not bypass this guard.

6. Serve `ota_release`:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

7. Validate over serial if USB is available:

```bash
python3 tools/robotctl.py --port <PORT> raw OTA_SET_SERVER <LAN_IP> 8080
python3 tools/robotctl.py --port <PORT> raw OTA_SET_MANIFEST /api/firmware/latest
python3 tools/robotctl.py --port <PORT> raw OTA_CHECK
python3 tools/robotctl.py --port <PORT> --timeout 60 raw OTA_DOWNLOAD_TEST
python3 tools/robotctl.py --port <PORT> raw STOP ALL
python3 tools/robotctl.py --port <PORT> platform-status
python3 tools/robotctl.py --port <PORT> --timeout 90 raw OTA_UPDATE
```

Proceed with `OTA_UPDATE` only when `PLATFORM_STATUS` reports `SAFE_FOR_OTA:1 SAFE_REASON:SAFE`.

8. Verify after reboot:

```bash
python3 tools/robotctl.py --port <PORT> raw VERSION
python3 tools/robotctl.py --port <PORT> raw OTA_ROLLBACK_STATUS
python3 tools/robotctl.py --port <PORT> platform-status
python3 tools/robotctl.py --port <PORT> raw WIFI_STATUS
```

Expected final state:

- New `BUILD_NUMBER`
- New partition if OTA switched slots
- `OTA_STATE:VALID`
- `PENDING_VERIFY:0`
- Wi-Fi reconnected

## OTA Without USB

Use this only after Wi-Fi credentials and an OTA announce token are already stored on the ESP.

Serve the release:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

Announce from the developer laptop. Prefer target IP when known; broadcast may work but is less deterministic:

```bash
BOTFARMS_OTA_TOKEN=<redacted> python3 tools/ota_announce.py --target <ESP_IP> --server-port 8080 --manifest /api/firmware/latest --action check
BOTFARMS_OTA_TOKEN=<redacted> python3 tools/ota_announce.py --target <ESP_IP> --server-port 8080 --manifest /api/firmware/latest --action download_test --timeout 90
BOTFARMS_OTA_TOKEN=<redacted> python3 tools/ota_announce.py --target <ESP_IP> --server-port 8080 --manifest /api/firmware/latest --action update --timeout 90
```

Do not paste the real token into logs or final answers. If `.env` contains `BOTFARMS_OTA_TOKEN`, load it without printing it.

After `update`, wait for the ESP to return:

```bash
ping -c 3 <ESP_IP>
```

If it does not return, scan the LAN and then use USB recovery:

```bash
nmap -sn <LAN_CIDR>
ip neigh show
```

## Troubleshooting Map

- `BAD_TOKEN`: local `BOTFARMS_OTA_TOKEN` does not match the token stored by `OTA_ANNOUNCE_TOKEN_SET`.
- `TOKEN_NOT_CONFIGURED`: the ESP has no announce token; set it over serial.
- `BAD_TYPE`, `JSON_PARSE`, `BAD_ACTION`, `BAD_PORT`, `BAD_MANIFEST`: inspect `tools/ota_announce.py` payload and `components/ota_announce/ota_announce.c`.
- `WIFI_NOT_CONNECTED`: check `WIFI_STATUS`, saved SSID, RSSI, and router/DHCP. Wi-Fi failure must not block robot control.
- `BAD_SERVER_CONFIG`: check `OTA_CONFIG`, host, port, and manifest path.
- `HTTP_CONNECT`, `HTTP_OPEN`, `HTTP_STATUS`, endpoint timeout, or connection reset: confirm the HTTP server is running, the laptop IP is LAN-reachable, firewall is not blocking port `8080`, and the manifest URL is not loopback.
- `BAD_URL`: manifest URL is malformed or uses forbidden loopback/unsuitable host.
- `PROJECT_MISMATCH` or `TARGET_MISMATCH`: check manifest against `FW_PROJECT` and `FW_TARGET`.
- `BUILD_DOWNGRADE`: remote build is lower than running build. Increment from the current running build.
- `BUILD_NOT_NEWER`: remote build equals running build. Increment `FW_BUILD_NUMBER`, rebuild, regenerate release.
- `CURRENT_BUILD_UNSUPPORTED`: running firmware is below manifest `min_supported_build`; use USB recovery or a compatible staged update.
- `IMAGE_TOO_LARGE`: binary does not fit the inactive OTA partition; inspect partition table and binary size.
- `SIZE_MISMATCH` or `SHA256_MISMATCH`: release binary and manifest disagree; regenerate with `tools/ota_prepare_release.py`.
- `ROBOT_NOT_SAFE`: run `STOP ALL`, inspect `PLATFORM_STATUS`, RC state, motion state, and motor faults. Do not bypass the safety gate.
- `OTA_BUSY`: another OTA operation is active; wait and retry.
- `PENDING_VERIFY:1` after reboot: inspect `OTA_ROLLBACK_STATUS`; boot self-test should mark valid. If startup fails while pending, rollback may trigger.
- Corrupt serial output or random `SerialException`: stop parallel serial readers and run one `robotctl.py` command at a time.
- ESP disappears after OTA: check ping, ARP, DHCP/LAN scan. If not found quickly, reconnect USB, query `VERSION`/rollback status, and flash latest known-good firmware if needed.

## Validation Before Reporting Success

For docs/scripts-only changes:

```bash
python3 -m py_compile tools/ota_prepare_release.py tools/ota_announce.py tools/robotctl.py
python3 tools/ota_prepare_release.py --host 192.168.1.100 --port 8080 --dry-run
git diff --check
```

For firmware changes:

```bash
python3 tools/test_svd48_protocol.py
. /home/jp/esp/esp-idf-v5.4.1/export.sh && idf.py build
. /home/jp/esp/esp-idf-v5.4.1/export.sh && idf.py size
```

For hardware OTA validation, capture evidence:

- `VERSION` before and after
- `PLATFORM_STATUS`
- `WIFI_STATUS`
- `OTA_CONFIG`
- `OTA_ANNOUNCE_STATUS`
- `OTA_CHECK`
- `OTA_DOWNLOAD_TEST`
- `OTA_UPDATE`
- `OTA_ROLLBACK_STATUS` after reboot

When reporting results, summarize statuses and build numbers, but do not quote credentials or tokens.
