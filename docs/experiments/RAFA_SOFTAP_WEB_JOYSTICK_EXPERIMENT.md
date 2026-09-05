# Rafa SoftAP web joystick experiment

## Scope

This branch-only experiment is selected by
`CONFIG_BOTFARMS_RAFA_SOFTAP_WEB_JOYSTICK_EXPERIMENTAL=y`. It is intentionally
separate from the B45 station-network web experiment and from the LAN-only
diagnostic. It reports:

```text
PROFILE_STATUS NAME:rafa_softap_web_joystick_experimental
```

The intended control topology is deliberately small:

```text
iPhone
  -> Wi-Fi 2.4 GHz
ESP32 SoftAP (AP only, no STA and no APSTA)
  -> GET / and ws://192.168.4.1/control
  -> WEB_DIRECT semantic events
  -> motion_application -> endpoint registry -> SVD48 adapter -> RS485
```

There is no router, repeater, Engineering Console backend, external gateway or
station association in that control path. The existing `wifi_manager_init()`
station path is not used. The selected branch calls `wifi_manager_init_softap()`
instead, rejects `WIFI_CONNECT`, `WIFI_DISCONNECT` and reconnect-supervisor
requests in AP mode, and does not start the automatic OTA check task.

## AP contract

| Item | Selected value |
| --- | --- |
| SSID | `RAFA-CONTROL` |
| Mode | `WIFI_MODE_AP` only |
| AP address / gateway | `192.168.4.1` |
| Netmask | `255.255.255.0` |
| Addressing for phone | ESP-IDF DHCP server |
| RF | 2.4 GHz, channel 6, HT20 |
| Browser page | `http://192.168.4.1/` |
| Control socket | `ws://192.168.4.1/control` |

Channel 6 and HT20 are conservative 2.4 GHz settings chosen for broad phone
compatibility and to avoid 40 MHz selection complexity. This is not RF
qualification; it must be checked at the workshop.

The WPA2 passphrase is a required local build input, never a source-controlled
value or NVS station credential. Before a physical deployment, create the ignored
file `ci/sdkconfig.rafa-softap-web-joystick.local.defaults` with only the local
Kconfig assignment for `CONFIG_BOTFARMS_RAFA_SOFTAP_PASSPHRASE`. The code rejects
an empty, short or invalid passphrase rather than silently creating an open AP.
Do not paste a real passphrase into an issue, commit, command log or this document.

The page shows AP mode, SSID, ESP address, DHCP state and connected-client count;
it never displays a passphrase. It also shows WebSocket state
`CONNECTED`/`DISCONNECTED`/`STALE`, control state and the age of the last valid
command. The bounded logs are `SOFTAP_STARTED`, SoftAP client join/leave,
WebSocket join/leave, ARM/DISARM, TTL expiry and rejected control events. Normal
heartbeats are not logged.

## Safety and control invariants retained

This experiment does not change the existing web joystick contract:

- typed `ARM`, `DISARM`, software `STOP`, normalized `forward`/`turn`, 0.10
  deadzone and explicit zero/deadman-false release;
- one ephemeral WebSocket owner; a second client cannot take over its session;
- the existing 300 ms firmware-owned lease, motion status, motor-fault gate,
  endpoint-health checks, application mapping, kinematics and SVD48 adapter;
- PPM acquisition remains read-only observation, while PPM motion and the RC/LAN
  interlock do not participate in authority arbitration;
- UDP `control_lan` is not started, and this adds no raw HTTP/Modbus/register
  endpoint.

Closing a WebSocket is observed and logged, but does not invent a second stop
path. A disconnected **unarmed** owner releases its ephemeral session immediately,
so a reconnect can obtain a new session. A disconnected armed/active owner retains
only its existing session and last-valid-command time; it cannot renew the lease and
the existing bounded 300 ms expiry path withdraws motion before the session is
released. This is software behavior only until the elevated iPhone-disconnect test
records physical evidence.

The page enables ARM and joystick intent only after the firmware acknowledges the
WebSocket session. It does not optimistically mark ARM successful. `SESSION_BUSY`
leaves the current owner untouched, sends no command and makes a bounded reconnect
attempt; it never takes ownership or rearms automatically.

## Build, deployment and reversal

Build from this isolated worktree with `sdkconfig.defaults`,
`ci/sdkconfig.rafa-softap-web-joystick.defaults`, and the ignored local passphrase
file. Use an isolated `SDKCONFIG` and build directory; do not alter the normal Rafa
or B45 fragments. This document does not authorize an OTA, ARM, motion or physical
test.

### Later, explicitly authorized OTA (PowerShell)

The integration build for this branch deliberately contains no real WPA2
passphrase and must not be deployed. Only after a reviewer and operator authorize a
deployment, create the ignored local defaults file with this **single** setting in a
local editor (never put a real passphrase in shell history or Git):

```text
CONFIG_BOTFARMS_RAFA_SOFTAP_PASSPHRASE="<local WPA2 passphrase, 8-63 characters>"
```

Before building, allocate and commit a unique, increasing `FW_BUILD_NUMBER` for the
SoftAP image. Do not reuse B45 or infer the next number from this document. Then use
an isolated configuration and build directory:

```powershell
$repo = (Get-Location).Path
$buildDir = Join-Path $env:TEMP "botfarms-rafa-softap-build"
$sdkconfig = Join-Path $env:TEMP "sdkconfig-rafa-softap"
$defaults = @(
  "$repo\sdkconfig.defaults",
  "$repo\ci\sdkconfig.rafa-softap-web-joystick.defaults",
  "$repo\ci\sdkconfig.rafa-softap-web-joystick.local.defaults"
) -join ';'

idf.py -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" -B $buildDir set-target esp32s3
idf.py -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults" -B $buildDir build
Select-String -Path $sdkconfig -Pattern '^CONFIG_BOTFARMS_PROFILE_RAFA=y$','^CONFIG_BOTFARMS_RAFA_SOFTAP_WEB_JOYSTICK_EXPERIMENTAL=y$'
```

The final command must show both selected symbols and must not be replaced with a
normal Rafa, LAN-only, STA-web, or APSTA build. Run the required host tests and the
OTA release preparation workflow from `docs/OTA.md` using the resulting binary and
an operator-verified laptop address reachable from the active network. The OTA
token remains in `BOTFARMS_OTA_TOKEN`; do not put it on the command line or in a
file under this repository.

For a later operator-authorized OTA, a maintenance laptop may temporarily join the
SoftAP and host the approved artifact on its DHCP-assigned `192.168.4.x` address.
The existing authenticated OTA-announcement listener remains available for that
explicit maintenance action, while automatic checks remain disabled. The laptop is
not part of the control path. Confirm SAFE_IDLE, the OTA token and the selected
artifact before following the normal OTA checks with target `192.168.4.1`, for
example:

```powershell
# Terminal 1, after release preparation:
python -m http.server 8080 --bind 0.0.0.0 --directory <approved-release-directory>

# Terminal 2, with the laptop joined to RAFA-CONTROL:
python tools\ota_announce.py --target 192.168.4.1 --server-port 8080 --manifest /api/firmware/latest --action check
python tools\ota_announce.py --target 192.168.4.1 --action download_test
# Only after the operator confirms SAFE_IDLE and authorizes installation:
python tools\ota_announce.py --target 192.168.4.1 --action update
```

For the reviewed B47 candidate, the branch-local
`tools/install_rafa_softap_b47.ps1` packages this exact sequence for a Windows
maintenance laptop already joined to `RAFA-CONTROL`. It verifies the B47 SHA-256,
discovers its assigned `192.168.4.x` address, performs read-only safety/profile
preflight, hosts the manifest locally, then runs `check` and `download_test`.
`-Install` still requires the operator to type `INSTALL B47` before it sends the
rebooting update action. It prompts hidden for missing process-local maintenance and
OTA tokens and restores/removes them on exit; it never writes either token to disk.
It waits boundedly for the temporary HTTP server, verifies its listener and a direct
loopback HTTP response before announcing, and reports the SoftAP self-check only as
diagnostic evidence. The authenticated `check` and `download_test` are the actual
Rafa-to-laptop reachability proof. It never changes the firewall. It is not a general
OTA mechanism and does not authorize motion.

From the repository root, after joining the laptop manually to `RAFA-CONTROL`, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install_rafa_softap_b47.ps1 -Install
```

After reboot, use read-only Maintenance LAN checks at `192.168.4.1` for `VERSION`,
`PROFILE_STATUS`, `WIFI_STATUS` and `SAFETY_STATUS`. The token comes only from its
environment variable; none of these commands should print it.

To return to B45 or normal Rafa, prepare the approved prior artifact, join the
SoftAP only for the explicit authenticated OTA workflow, deploy it with separate
operator authorization, then verify `VERSION`, `PROFILE_STATUS`, safety and its
expected station behavior after reboot. If that local maintenance path is not
available, use the established USB recovery procedure; do not improvise a raw
network update path.

An old B45 binary is not a valid OTA rollback artifact if its build number is lower
than the running SoftAP build. Rebuild the approved B45-equivalent source as a new,
strictly higher build number, prepare its release, and send that approved image to
`192.168.4.1` using the same explicit sequence. Once it reboots into the station
experiment, rediscover its station address rather than assuming the old address is
still valid. USB recovery remains the fallback if the AP maintenance path cannot be
used.

## Required evidence before use

The software tests cover AP profile validation, AP selection, WebSocket ownership,
deadzone and lease-expiry model behavior. A clean ESP-IDF build checks integration.
The following remain physical tests, not build claims:

1. AP starts with static address and DHCP; an iPhone receives an address.
2. `GET /` and WebSocket connect directly with no router/repeater/PC control path.
3. ARM produces no motion, zero/deadzone releases correctly, and a second client
   cannot take control.
4. Closing the browser or losing Wi-Fi stops lease renewal; record the firmware
   expiry and physical wheel response with the normal safe setup.
5. Confirm the running Wi-Fi mode is AP and there is no STA association/reconnect.
