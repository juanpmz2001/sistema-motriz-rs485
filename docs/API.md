# Command and LAN API

## Serial framing

The serial gateway accepts one ASCII command per newline at 115200 baud. Commands
are case-insensitive; arguments are space-separated, with quoted SSID/password
support where needed. Responses use stable line prefixes:

- `OK ...`: command completed.
- `ERR ...`: command rejected or failed.
- `DATA ...`: requested state or telemetry.
- `TRACE ...`: optional diagnostic tracing.

Status and inventory records use space-separated `KEY:VALUE` fields. Consumers must
parse by key and ignore unknown additional fields; appending a field does not change
the `ASCII_V1` protocol. Required fields and their semantics remain versioned by this
document, and fields are not silently renamed or removed.

Use `HELP` on the device for the exact command grammar compiled into that build.
`tools/robotctl.py` is a thin serial client and accepts arbitrary commands through
its `raw` subcommand.

## Command groups

| Group | Commands |
| --- | --- |
| Identity/health | `PING`, `VERSION`, `PLATFORM_STATUS`, `PROFILE_STATUS`, `COMPOSITION_STATUS`, `SAFETY_STATUS`, `HELP` |
| Stored config | `CONFIG_STATUS`, `CONFIG_CLEAR` |
| Wi-Fi | `WIFI_SET`, `WIFI_CLEAR`, `WIFI_STATUS`, `WIFI_CONNECT`, `WIFI_DISCONNECT` |
| Maintenance auth | `MAINT_LAN_STATUS`, `MAINT_TOKEN_SET`, `MAINT_TOKEN_CLEAR` |
| OTA config | `OTA_CONFIG`, `OTA_SET_SERVER`, `OTA_SET_MANIFEST` |
| OTA announce | `OTA_ANNOUNCE_TOKEN_SET`, `OTA_ANNOUNCE_TOKEN_CLEAR`, `OTA_ANNOUNCE_STATUS` |
| OTA actions | `OTA_CHECK`, `OTA_DOWNLOAD_TEST`, `OTA_UPDATE` |
| OTA policy/test | `OTA_ROLLBACK_STATUS`, `OTA_ROLLBACK_TEST`, `OTA_AUTO_STATUS`, `OTA_AUTO_FORCE_CHECK`, `OTA_AUTO_INTERVAL`, `OTA_AUTO_CHECK`, `OTA_AUTO_UPDATE` |
| RC diagnostics | `IBUS_MODE`, `IBUS_STATUS`, `IBUS_CHANNELS`, `IBUS_RAW`, `IBUS_PIN`, `PPM_CAPTURE` |
| Bus diagnostics | `TRACE`, `POLL_ONCE`, `READ_REG`, `GET_SPEED`, `GET_MOTOR` |
| Endpoint discovery/observation | `ENDPOINTS`, `GET_ENDPOINT_OBSERVATION` |
| Drive configuration | `WRITE_REG`, `WRITE_REGS`, `SAVE_SVD48_CONFIG`, `SET_SVD48_GEAR_RATIO`, `SVD48_IDENTIFY_STATUS`, `SVD48_IDENTIFY`, `GET_SVD48_CONFIG`, `APPLY_PY6514_CONFIG` |
| Actuation | `SET_SPEED`, `ENABLE`, `STOP`, `CLEAR_FAULT`, `MOVE_VEL`, `SET_ENDPOINT_SPEED`, `STOP_ENDPOINT` |
| Telemetry | `STREAM` |

Commands that write drive registers require the literal `CONFIRM` where shown by
`HELP`. This is an operator acknowledgement, not authorization or transaction
rollback. Raw writes, identify, enable, speed and motion commands are engineering
bench operations subject to [the safety contract](SAFETY.md).

Automatic OTA installation cannot be enabled: `OTA_AUTO_UPDATE` only accepts
`OFF`. Automatic manifest checks can be enabled independently.

## Useful read-only examples

```text
VERSION
PLATFORM_STATUS
PROFILE_STATUS
COMPOSITION_STATUS
ENDPOINTS
GET_ENDPOINT_OBSERVATION 1
SAFETY_STATUS
WIFI_STATUS
OTA_CONFIG
OTA_AUTO_STATUS
IBUS_STATUS
GET_MOTOR 0
READ_REG 1 0x5018 1
```

Logical motor numbers depend on the selected build profile. `current_robot` exposes
`0..3`; `bench_single_svd48_motor` exposes only `0`, and rejects `1`. The mapping is
documented in [SVD48](SVD48.md).
Telemetry suffixes include `DA` for 0.1 A, `DV` for 0.1 V and `DC` for 0.1 C.
The `RPM` field contains the signed raw value of SVD48 motor-speed register
`0x5410/0x5411`, interpreted as RPM according to the manufacturer register table;
the firmware applies no artificial factor-of-ten scaling. This unconfirmed value
already feeds the legacy 5-RPM `RUNNING`/`MOTION_ACTIVE` indication and
`robot_control_is_safe_for_ota()` gate used by OTA and several maintenance commands.
Those checks are not qualified safety assurance. A future controlled physical test
must confirm the interpretation before that dependency is accepted or expanded.

`MOVE_VEL` requires application geometry and four traction endpoints. It is supported
by `current_robot` only; the single-motor profile returns the existing unsupported/
failure response rather than inventing missing wheels.

Build, profile and composition identity use stable one-line shapes in both normal
and diagnostic-only startup:

```text
DATA VERSION PROJECT:<project> TARGET:<target> VERSION:<version> BUILD_NUMBER:<n> IDF:<version> PARTITION:<label> OTA_STATE:<state> PENDING_VERIFY:<0|1> ROLLBACK_POSSIBLE:<0|1> GIT_SHA:<40-hex|UNKNOWN> GIT_DIRTY:<0|1>
DATA PROFILE NAME:<name> SCHEMA_VALID:<0|1> COMPOSITION_SUPPORTED:<0|1> BOARD:<board-id|UNKNOWN>
DATA COMPOSITION MODE:<ACTIVE|DIAGNOSTIC_ONLY> RUNTIME_READY:<0|1> CODE:<code> STAGE:<stage> DRIVER:<n> BUS:<n> DEVICE:<n> ENDPOINT:<n> ERROR:0x<hex> REQUIRED_STORAGE:<n> AVAILABLE_STORAGE:<n> OUTPUTS_INITIALIZED:<0|1>
```

`GIT_SHA` is the full source revision and `GIT_DIRTY` records whether the checkout
had tracked or untracked changes when the firmware identity header was generated.
CMake refreshes that header on every build, including incremental builds; neither
field uses a wall-clock timestamp. Builds outside a Git checkout, including
reproducible release jobs, can set the CMake cache inputs
`BOTFARMS_FW_GIT_SHA_OVERRIDE` and `BOTFARMS_FW_GIT_DIRTY_OVERRIDE`; the SHA override
accepts exactly 40 hexadecimal characters or `UNKNOWN`, and the dirty override
accepts `0` or `1`. Outside a verifiable checkout both overrides are required. When
dirty is not overridden, Git `HEAD` must be readable, any SHA override must match it,
and `git status` must succeed. `UNKNOWN` is never reported with `GIT_DIRTY:0`; an
unverifiable clean claim fails the build instead of failing open.

In the current API, `CODE`/`STAGE` report `OK`/`NONE` after successful normal startup,
or identify a preflight failure in restricted diagnostic startup; the numeric
identities are zero when no specific object applies. Construction or service-start
failure does not leave a gateway running and is available only in boot logs. The
capacity fields report bytes for executable runtime storage, or item counts for
endpoint/legacy-binding capacity checks, according to `CODE`/`STAGE`; they make
static-capacity failures diagnosable without exposing mutable configuration.

## Generic endpoint boundary for HIL

The endpoint commands address immutable endpoint IDs from the selected build profile,
not legacy zero-based motor indices. They are available on the full serial gateway
only; the maintenance-LAN policy and restricted diagnostic startup reject them. The
legacy `GET_SPEED`, `GET_MOTOR`, `SET_SPEED` and `STOP` commands remain unchanged.

`ENDPOINTS` returns a count followed by one line per endpoint in registry order:

```text
DATA ENDPOINTS COUNT:<n>
DATA ENDPOINT ID:<id> NAME:<name> CRITICALITY:<REQUIRED|OPTIONAL|DEVELOPMENT> AVAILABLE:<0|1> CAPABILITIES:0x<mask> VELOCITY_RPM:<0|1> VELOCITY_OBSERVATION:<0|1> STOPPABLE:<0|1> MIN_RPM:<rpm> MAX_RPM:<rpm>
```

`CRITICALITY` lets a test distinguish required, optional and development-only
hardware without embedding profile implementation details. The named `VELOCITY_RPM`,
`VELOCITY_OBSERVATION` and `STOPPABLE` fields let it choose actuation and observation
independently without knowing a concrete driver. `MIN_RPM`/`MAX_RPM` apply when
`VELOCITY_RPM:1`.
Actuation uses the same coordinator and stop serialization as migrated legacy paths:

```text
SET_ENDPOINT_SPEED <id> <rpm>
OK SET_ENDPOINT_SPEED ID:<id> RPM_TARGET:<rpm>

STOP_ENDPOINT <id>
OK STOP_ENDPOINT ID:<id>
```

Unknown or unsupported endpoints are rejected before either request. Speed requests
also reject `AVAILABLE:0` and out-of-range RPM. A configured stoppable endpoint still
receives a best-effort stop attempt when `AVAILABLE:0`; loss of reported readiness
must not suppress the fail-safe path. Driver/coordinator failures return an `ERR`
line with a normalized `RESULT` token.

`GET_ENDPOINT_OBSERVATION <id>` currently requests one explicitly typed velocity
observation; it is not a universal sensor interface:

```text
DATA ENDPOINT_OBSERVATION ID:<id> TYPE:VELOCITY_RPM VALID:<0|1> RPM:<rpm> TIMESTAMP_MS:<n> SOURCE:<DEVICE_FEEDBACK|UNKNOWN> ONLINE:<0|1> STALE:<0|1> HEALTH:<UNKNOWN|HEALTHY|DEGRADED|OFFLINE|FAULT|STALE> HEALTH_AVAILABLE:<0|1>
```

`VALID` says whether a device-feedback RPM sample exists, and `TIMESTAMP_MS` is that
sample's monotonic update time (`0` before the first valid sample). `STALE` applies to
the velocity sample itself. `RPM` must not be treated as measured data when
`VALID:0`. `HEALTH_AVAILABLE:0` pairs with `HEALTH:UNKNOWN`. These fields intentionally
do not expose a concrete controller type, bus address or register number.

## Restricted diagnostic startup

If profile schema validation succeeds but executable composition is unsupported, the
firmware does not construct buses, devices, endpoints or actuator outputs. It starts
the serial gateway in `diagnostic_only` mode with this allowlist:

- `PING`, `VERSION`, `HELP` and `PLATFORM_STATUS`;
- `CONFIG_STATUS`, `WIFI_STATUS`, `PROFILE_STATUS` and `COMPOSITION_STATUS`; and
- exactly `STOP ALL` as a fail-safe-compatible request.

Because no output endpoint exists, `STOP ALL` reports
`ERR STOP_UNAVAILABLE OUTPUTS_NOT_INITIALIZED`; it does not claim that a physical stop
was written. Every other command, including motion, enable, fault clear, register
access/writes, OTA actions, receiver diagnostics and streaming, returns
`ERR DIAGNOSTIC_MODE_COMMAND_BLOCKED <command>`.

The diagnostic gateway exposes the immutable preflight failure code/stage and relevant
driver, bus, device or endpoint identity through `PROFILE_STATUS` and
`COMPOSITION_STATUS`. Maintenance LAN, OTA announce, safety, RC acquisition and
background streaming do not start in this mode.

An OTA image still pending verification does not enter this fallback after a
composition failure; normal startup-failure rollback handling takes precedence so an
unsupported image is not marked valid merely because diagnostics started.

In this mode `PLATFORM_STATUS` has the exact shape:

```text
DATA PLATFORM STATE:DIAGNOSTIC_ONLY AUTHORITY:SERIAL_ASCII PROTOCOL:ASCII_V1 OUTPUTS_INITIALIZED:0 MOTION_ACTIVE:0 SAFE_FOR_OTA:0 SAFE_REASON:COMPOSITION_UNAVAILABLE TRACE:0 STREAM:0
```

## Maintenance LAN

`maintenance_lan` wraps selected ASCII commands in authenticated UDP JSON on port
`32321`. It starts without a token but rejects requests until `MAINT_TOKEN_SET`
has provisioned one in NVS. Tokens are never returned by status commands.

Request:

```json
{
  "type": "botfarms_maintenance_request",
  "request_id": "a-client-unique-id",
  "token": "secret",
  "action": "command",
  "command": "VERSION"
}
```

Response:

```json
{
  "type": "botfarms_maintenance_response",
  "request_id": "a-client-unique-id",
  "status": "ok",
  "detail": "OK",
  "lines": ["DATA VERSION ..."]
}
```

Actions are `hello`, `status` and `command`. Authentication is required for all
three. The implementation takes the sender IP from UDP and does not need a fixed
developer-computer IP.

The exact LAN allowlist is code-owned in
`components/serial_gateway/serial_gateway_policy.c`. The current build permits:

- No-argument status/diagnostic commands listed in that file.
- `GET_SPEED`, `GET_MOTOR`, `READ_REG`, `GET_SVD48_CONFIG` and `POLL_ONCE`.
- `STOP <motor|ALL>` and `SET_SPEED <motor> <rpm>`.
- Confirmed register writes, save, gear-ratio and identify operations.

Everything else returns `ERR LAN_COMMAND_BLOCKED <command>`. This allowlist is
broader than the intended production boundary: speed and persistent writes over
LAN have no command lease, deadman or replay protection. Use LAN as a trusted
bench maintenance interface only.

The independent client resolves its token in this order: `--token`,
`BOTFARMS_MAINT_TOKEN`, then the untracked repository `.env`.

```bash
python3 tools/esp_lanctl.py discover --broadcast 192.168.1.255
python3 tools/esp_lanctl.py status --host 192.168.1.185
python3 tools/esp_lanctl.py command --host 192.168.1.185 GET_MOTOR 0
python3 tools/esp_lanctl.py stop-all --host 192.168.1.185
```

`watch` polls from the computer and optionally writes CSV; the ESP does not push
LAN telemetry in this version.

## Control LAN

`control_lan` defines a separate sequenced protocol on default port `32322`, but the
current build does not initialize or start it. It is not an available device API and
must not be confused with maintenance LAN.
