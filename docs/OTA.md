# OTA runbook

## Design

The ESP32 uses the custom 16 MB partition table in
`partitions_ota_16mb.csv`: NVS, OTA metadata, PHY, coredump, two 6 MB application
slots and a FAT storage partition. ESP-IDF rollback support is enabled.

OTA has three separate parts:

- `ota_manager`: fetches a JSON manifest over HTTP, validates project, target,
  build constraints, size and SHA-256, then writes the inactive application slot.
- `ota_announce`: authenticated UDP listener on port `32320`; the sender IP becomes
  the temporary HTTP server, so developer laptops can have dynamic addresses.
- `tools/ota_prepare_release.py` and `tools/ota_announce.py`: create/serve a release
  and notify one ESP or a LAN broadcast.

Wi-Fi reconnect and periodic manifest checks run at low priority. Automatic update
and reboot are disabled. A network failure does not prevent local robot startup.

## Trust boundary

The OTA token authorizes a LAN announcement; it is distinct from the maintenance
token. HTTP transport and the UDP offer are plaintext. SHA-256 detects corruption
or mismatch but does not prove who produced the firmware. This is suitable only
for a trusted development LAN. Production requires signed images, secure boot,
flash encryption decisions, protected keys and a defined token-rotation path.

Never commit or print a real token. Tools resolve `BOTFARMS_OTA_TOKEN` from an
explicit `--token`, the process environment or the untracked repository `.env`.

## One-time provisioning over USB

A factory board must first receive the complete flash layout:

```bash
source /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Provision Wi-Fi, server defaults and the OTA announcement token through the serial
gateway. Replace placeholders locally; do not paste credentials into documentation
or shell history shared with others.

```text
WIFI_SET "ssid" "password"
OTA_SET_SERVER 192.168.1.10 8080
OTA_SET_MANIFEST /api/firmware/latest
OTA_ANNOUNCE_TOKEN_SET <token>
WIFI_CONNECT
WIFI_STATUS
OTA_CONFIG
OTA_ANNOUNCE_STATUS
OTA_ROLLBACK_STATUS
```

Use `robotctl.py ... raw <command>` or a serial terminal. Reboot once and confirm
that Wi-Fi reconnects without USB before relying on OTA.

## Prepare a release

1. Increment `FW_BUILD_NUMBER` in `main/app_version.h`. Do not reuse a build number
   for different firmware.
2. Run host tests and a clean ESP-IDF build.
3. Determine the laptop's LAN address reachable by the ESP; never use `localhost`,
   `127.0.0.1` or `0.0.0.0` in the manifest URL.
4. Generate the ignored release directory.

```bash
tools/run_host_tests.sh
BOTFARMS_HOST_TEST_SANITIZERS=ON tools/run_host_tests.sh
idf.py build
python3 tools/ota_prepare_release.py --host 192.168.1.10 --port 8080
```

The script refuses a stale binary, a non-`esp32s3` target and conflicting content
for an existing build number. It writes:

```text
ota_release/api/firmware/latest
ota_release/firmware/sistema-motriz-rs485-v<version>-b<build>.bin
```

Serve it from the repository root in a dedicated terminal:

```bash
python3 -m http.server 8080 --bind 0.0.0.0 --directory ota_release
```

Check the host firewall and confirm that another LAN device can fetch the manifest.

## Check and install without USB

Start with a non-destructive check directed at the ESP IP or subnet broadcast:

```bash
python3 tools/ota_announce.py \
  --target 192.168.1.255 \
  --server-port 8080 \
  --manifest /api/firmware/latest \
  --action check
```

Then test the full download and hash without selecting the partition:

```bash
python3 tools/ota_announce.py --target 192.168.1.185 --action download_test
```

Before installation, verify the robot is physically safe, stopped and cannot move.
An update request downloads to the inactive slot, checks the digest, requests a
safe stop, changes the boot partition and reboots:

```bash
python3 tools/ota_announce.py --target 192.168.1.185 --action update
```

The offer's source IP becomes the HTTP host; `--server-port` and `--manifest`
complete the temporary configuration. The server must remain running until the
device responds and reboots.

### AP-only experimental exception

The branch-only Rafa SoftAP web-joystick image intentionally does not join a
station or run automatic OTA checks. Its AP address is `192.168.4.1`. For an
explicit, operator-authorized update or reversal, a maintenance laptop may join
that AP temporarily, serve the approved artifact from its DHCP-assigned
`192.168.4.x` address, and send the existing authenticated OTA announcement to
`192.168.4.1`. That laptop is an OTA maintenance participant, not a browser-control
authority. Do not assume the normal upstream-LAN broadcast address works in this
mode, and do not use this exception to add a router, STA or APSTA path. If the AP
maintenance path is unavailable, use USB recovery.

## Post-update verification

The new image initially boots pending verification. Startup initializes critical
subsystems, starts serial/safety and then marks the app valid. A failure during
that sequence requests rollback. After the device returns:

```bash
python3 tools/esp_lanctl.py version --host 192.168.1.185
python3 tools/esp_lanctl.py platform-status --host 192.168.1.185
python3 tools/esp_lanctl.py safety-status --host 192.168.1.185
python3 tools/esp_lanctl.py ota-status --host 192.168.1.185
```

Confirm the expected build number, partition, Wi-Fi state and safety state. A UDP
success response only confirms that the request was accepted; it is not final
evidence that the new image survived reboot.

## Failure diagnosis

| Symptom | Check |
| --- | --- |
| No UDP response | Same subnet, broadcast address, firewall, port 32320, token provisioned |
| `AUTH_REQUIRED`/auth error | OTA token exists in NVS and local token matches; do not display it |
| Manifest connection fails | HTTP server bound to `0.0.0.0`, sender IP reachable, port open |
| Manifest rejected | Project, target, increasing build, minimum build, URL, size and SHA-256 |
| Update rejected as unsafe | Stop physical robot and inspect `SAFETY_STATUS`/motor telemetry |
| New image rolls back | Read boot logs and startup-stage failure; do not disable rollback |
| Device does not return | Restore USB, inspect bootloader/partition logs and flash known-good full image |

Rollback fault-injection commands exist for engineering validation. Do not leave a
rollback test mode armed on a normal device; check `OTA_ROLLBACK_STATUS` afterward.
