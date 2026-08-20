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
| Identity/health | `PING`, `VERSION`, `PLATFORM_STATUS`, `PROFILE_STATUS`, `COMPOSITION_STATUS`, `SAFETY_STATUS`, `CONTROL_STATUS`, `HELP` |
| Stored config | `CONFIG_STATUS`, `CONFIG_CLEAR` |
| Wi-Fi | `WIFI_SET`, `WIFI_CLEAR`, `WIFI_STATUS`, `WIFI_CONNECT`, `WIFI_DISCONNECT` |
| Maintenance auth | `MAINT_LAN_STATUS`, `MAINT_TOKEN_SET`, `MAINT_TOKEN_CLEAR` |
| OTA config | `OTA_CONFIG`, `OTA_SET_SERVER`, `OTA_SET_MANIFEST` |
| OTA announce | `OTA_ANNOUNCE_TOKEN_SET`, `OTA_ANNOUNCE_TOKEN_CLEAR`, `OTA_ANNOUNCE_STATUS` |
| OTA actions | `OTA_CHECK`, `OTA_DOWNLOAD_TEST`, `OTA_UPDATE` |
| OTA policy/test | `OTA_ROLLBACK_STATUS`, `OTA_ROLLBACK_TEST`, `OTA_AUTO_STATUS`, `OTA_AUTO_FORCE_CHECK`, `OTA_AUTO_INTERVAL`, `OTA_AUTO_CHECK`, `OTA_AUTO_UPDATE` |
| RC diagnostics | `IBUS_MODE`, `IBUS_STATUS`, `IBUS_CHANNELS`, `IBUS_RAW`, `IBUS_PIN`, `PPM_CAPTURE` |
| Bus diagnostics | `TRACE`, `POLL_ONCE`, `SVD48_PROBE`, `READ_REG`, `GET_SPEED`, `GET_MOTOR` |
| SVD48 workspace | `SVD48_INVENTORY`, `GET_SVD48_CHANNEL_TELEMETRY`, `SVD48_BENCH_SET_SPEED`, `SVD48_BENCH_HOLD`, `SVD48_BENCH_DISABLE`, `SVD48_BENCH_STOP` |
| AS5600 L2/L3 diagnostics | `GET_AS5600_DIAGNOSTICS device_id` |
| Endpoint discovery/observation | `ENDPOINTS`, `GET_ENDPOINT_OBSERVATION`, `GET_ENDPOINT_POSITION_OBSERVATION` |
| Drive configuration | `WRITE_REG`, `WRITE_REGS`, `SAVE_SVD48_CONFIG`, `SET_SVD48_GEAR_RATIO`, `SVD48_IDENTIFY_STATUS`, `SVD48_IDENTIFY`, `GET_SVD48_CONFIG`, `APPLY_PY6514_CONFIG` |
| Actuation | `SET_SPEED`, `ENABLE`, `STOP`, `CLEAR_FAULT`, `MOVE_VEL`, `SET_ENDPOINT_SPEED`, `SET_ENDPOINT_POSITION`, `SET_ENDPOINT_POSITION_REFERENCE`, `STOP_ENDPOINT` |
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
GET_ENDPOINT_POSITION_OBSERVATION 2
GET_AS5600_DIAGNOSTICS 2
SAFETY_STATUS
CONTROL_STATUS
WIFI_STATUS
OTA_CONFIG
OTA_AUTO_STATUS
IBUS_STATUS
GET_MOTOR 0
SVD48_INVENTORY
GET_SVD48_CHANNEL_TELEMETRY 1 M1
SVD48_PROBE 7
READ_REG 1 0x5018 1
```

`IBUS_STATUS` is a read-only cached receiver snapshot. In PPM mode it includes
`PULSE_MIN_US` and `PULSE_MAX_US`, the active decoder acceptance window, alongside
mode, GPIO, freshness, frame counters and channel values. It does not sample the pin
or wait for a new frame. Use it for bounded host monitoring; do not place the
blocking `PPM_CAPTURE` diagnostic in a live monitoring path.

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

## SVD48 maintenance workspace

`SVD48_INVENTORY` exposes configured physical controllers without the transitional
legacy motor-index view. It returns one header, one record per SVD48 device and two
physical channel records per controller:

```text
DATA SVD48_INVENTORY CONTROLLERS:<n>
DATA SVD48_CONTROLLER DEVICE_ID:<id> BUS_ID:<id> ADDRESS:<1..247> DRIVER:SVD48 AVAILABLE:<0|1> HEALTH:<health> CHANNELS:2
DATA SVD48_CHANNEL DEVICE_ID:<id> CHANNEL:<M1|M2> ENDPOINT_BOUND:<0|1> ENDPOINT_ID:<id|0> ENDPOINT_NAME:<name|NONE> AVAILABLE:<0|1> HEALTH:<health> CAPABILITIES:0x<mask> MIN_RPM:<rpm> MAX_RPM:<rpm>
```

An unbound M1/M2 record is still valid physical inventory; it is not an actuation
target. Consumers select controllers by `DEVICE_ID` and may display `ADDRESS`, but
must not turn the bus address into logical robot identity.

`GET_SVD48_CHANNEL_TELEMETRY <device_id> <M1|M2>` copies the selected channel's
existing cached driver snapshot. It performs no immediate RS485 transaction:

```text
DATA SVD48_CHANNEL_TELEMETRY DEVICE_ID:<id> CHANNEL:<M1|M2> ENDPOINT_BOUND:<0|1> ENDPOINT_ID:<id|0> STATUS:<n> RPM:<rpm> CURRENT_DA:<n> BUS_DV:<n> MOTOR_TEMP_DC:<n> MOS_TEMP_DC:<n> POS:<n> ERROR:0x<hex> ONLINE:<0|1> STALE:<0|1> HEALTH:<health> VALID_MASK:0x<hex> FAILED_MASK:0x<hex> STALE_MASK:0x<hex> COMM_ERR:<n> EXC_FUNC:0x<hex> EXC_CODE:0x<hex> EXC_AGE_MS:<n>
```

The four typed bench operations are:

```text
SVD48_BENCH_SET_SPEED <device_id> <M1|M2> <rpm>
SVD48_BENCH_HOLD <device_id> <M1|M2>
SVD48_BENCH_DISABLE <device_id> <M1|M2>
SVD48_BENCH_STOP <device_id> <M1|M2>
```

Set-speed validates the endpoint's published RPM range and requests that target;
hold requests zero RPM while leaving the channel actively enabled. Both require a
bound, available and `HEALTHY` channel. Disable and stop both request freewheel stop
and remain best-effort paths for a bound endpoint when availability/health is lost.
All four route through `actuation_application` and the coordinator. The direct SVD48
endpoint adapter owns the target-plus-START and target-zero-plus-STOP register
sequences; the transport command handler does not construct register writes.

When continuous control reports `ARMED` or `ACTIVE`, firmware rejects
`SVD48_BENCH_SET_SPEED` and `SVD48_BENCH_HOLD` with
`ERR CONTINUOUS_CONTROL_CONFLICT ...`. `SVD48_BENCH_DISABLE`,
`SVD48_BENCH_STOP` and global `STOP ALL` remain available. The same interlock rejects
`WRITE_REG`, `WRITE_REGS`, `SAVE_SVD48_CONFIG`, `SET_SVD48_GEAR_RATIO` and
`APPLY_PY6514_CONFIG`. `DISARMED` and profiles where continuous control is
`UNAVAILABLE` remain eligible, subject to every existing stopped/safety/write gate.

These are persistent bench maintenance operations with no TTL, deadman, sequence or
authority lease. The Engineering Console separately requires the exact phrase
`motor elevado` before set-speed/hold; that phrase is a host operator guard and is not
part of this wire protocol. Never reuse these commands as `/control`.

`SVD48_PROBE <address>` is a bounded, read-only L2 diagnostic for an unknown
Modbus address. It accepts unicast addresses `1..247` and performs exactly one
function-`0x03` read of the two bus-voltage registers beginning at `0x540C`, with
no write and no driver-level retry. It reports both presence and absence as data:

```text
DATA SVD48_PROBE ADDRESS:<1..247> READ_OK:1 RESULT:OK REG:0x540c M1_BUS_DV:<n> M2_BUS_DV:<n>
DATA SVD48_PROBE ADDRESS:<1..247> READ_OK:0 RESULT:<TIMEOUT|CRC_ERROR|BAD_RESPONSE|UNAVAILABLE|INVALID_ARGUMENT|IO_ERROR> REG:0x540c ERROR:0x<hex>
```

The probe borrows the configured RS485 transport but does not change the cached
health, polling counters or configured address of any `svd48_device`. A timeout at
one address is not evidence that wiring is dead. An address investigation must scan
the required range, record non-timeout anomalies and account for possible bus
contention before reaching a wiring/power conclusion.

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
DATA ENDPOINT ID:<id> NAME:<name> CRITICALITY:<REQUIRED|OPTIONAL|DEVELOPMENT> AVAILABLE:<0|1> CAPABILITIES:0x<mask> VELOCITY_RPM:<0|1> VELOCITY_OBSERVATION:<0|1> STOPPABLE:<0|1> MIN_RPM:<rpm> MAX_RPM:<rpm> POSITION:<0|1> POSITION_REFERENCE:<0|1> POSITION_OBSERVATION:<0|1> MIN_POSITION_DEG:<degrees> MAX_POSITION_DEG:<degrees>
```

`CRITICALITY` lets a test distinguish required, optional and development-only
hardware without embedding profile implementation details. The named `VELOCITY_RPM`,
`VELOCITY_OBSERVATION`, `POSITION`, `POSITION_REFERENCE`,
`POSITION_OBSERVATION` and `STOPPABLE` fields let it choose actuation, maintenance
reference and observation independently without knowing a concrete driver.
`MIN_RPM`/`MAX_RPM` apply when `VELOCITY_RPM:1`; `MIN_POSITION_DEG`/
`MAX_POSITION_DEG` apply when `POSITION:1`. A client must ignore limits for absent
capabilities rather than treating their numeric placeholder values as a usable range.
Actuation uses the same coordinator and stop serialization as migrated legacy paths:

```text
SET_ENDPOINT_SPEED <id> <rpm>
OK SET_ENDPOINT_SPEED ID:<id> RPM_TARGET:<rpm>

STOP_ENDPOINT <id>
OK STOP_ENDPOINT ID:<id>
```

Position requests use the same generic application/coordinator boundary:

```text
SET_ENDPOINT_POSITION <id> <degrees>
OK SET_ENDPOINT_POSITION ID:<id> POSITION_TARGET_DEG:<degrees>
```

The gateway rejects unknown, unavailable or non-position endpoints, non-finite
values and requests outside the published position range. A position request is not
a reference, homing or calibration command: it is interpreted only in the endpoint's
configured physical coordinate system.

An endpoint with both `POSITION_REFERENCE:1` and `STOPPABLE:1` may expose the
separate maintenance operation:

```text
SET_ENDPOINT_POSITION_REFERENCE <id> <degrees> CONFIRM
OK SET_ENDPOINT_POSITION_REFERENCE ID:<id> REFERENCE_DEG:<degrees>
```

It is available only through the full serial gateway, never through the LAN-safe or
restricted-diagnostic policy. The gateway validates the endpoint, availability,
position range and literal `CONFIRM`; the coordinator stops the endpoint before
establishing the reference. This operation maps a fresh, physically verified current
pose into the endpoint's configured coordinates. It must not command motion,
auto-home, seek a mechanical stop, calibrate the sensor or be interpreted as proof
that the physical pose is correct. It also cannot clear, recover or re-arm a latched
steering fault; fault recovery needs its own separately reviewed policy. It requires
a separately reviewed and explicitly authorized maintenance workflow.

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

`GET_ENDPOINT_POSITION_OBSERVATION <id>` requests an explicitly typed position
observation from the endpoint's application port:

```text
DATA ENDPOINT_POSITION_OBSERVATION ID:<id> TYPE:POSITION_DEGREES VALID:<0|1> CALIBRATED:<0|1> REFERENCED:<0|1> DEGREES:<degrees> TIMESTAMP_MS:<n> SOURCE_ENDPOINT_ID:<id> SOURCE:<DEVICE_FEEDBACK|INDEPENDENT_SENSOR|INFERRED|UNKNOWN> ONLINE:<0|1> STALE:<0|1> HEALTH:<UNKNOWN|HEALTHY|DEGRADED|OFFLINE|FAULT|STALE> HEALTH_AVAILABLE:<0|1> STATUS:<OK|INVALID_ARGUMENT|UNAVAILABLE|UNSUPPORTED|OUT_OF_RANGE|IO_ERROR|UNKNOWN>
```

`VALID:1` means a fresh measured value is usable in the associated actuator's logical
position coordinate. For a calibrated cyclic sensor this requires both
`CALIBRATED:1` and `REFERENCED:1`: calibration makes the sensor phase suitably
linear, while reference explicitly maps the current accepted sample into the logical
coordinate system. An AS5600 phase without an approved LUT or without an established
reference must not be presented as valid logical position feedback.
`CALIBRATED:1` and `REFERENCED:1` are provenance/state fields, not proof that the
operator's physical reference or the measured physical angle is correct.
`SOURCE_ENDPOINT_ID` identifies the endpoint that supplied the observation, which can
differ from the actuator endpoint. `STATUS` is the normalized result of acquiring the
snapshot, while `HEALTH` and `STALE` preserve health/freshness semantics. This
response exposes neither controller registers nor sensor bus details, and it is
observation rather than proof that a physical qualification gate passed.

## AS5600 device diagnostics

`GET_AS5600_DIAGNOSTICS <device_id>` is a concrete L2/L3 diagnostic operation for
an AS5600 declared by the active immutable profile. Its argument is a **profile
device ID**, not an endpoint ID. It is available only through the normal full serial
gateway; maintenance LAN and restricted diagnostic startup reject it.

The command copies the most recent cached device snapshot. It does not poll I2C,
retry communication, alter cadence, command PWM, establish a reference, write a
calibration or cause motion. It is therefore useful for powered read-only bring-up,
but its response is not a sensor or steering qualification result.

```text
DATA AS5600_DIAGNOSTICS DEVICE_ID:<id> ADDRESS:0x<7-bit-address> RAW_VALID:<0|1> RAW_ANGLE:<0..4095> STATUS:0x<status> MAGNET_DETECTED:<0|1> MAGNET_TOO_WEAK:<0|1> MAGNET_TOO_STRONG:<0|1> SAMPLE_TIMESTAMP_MS:<n> LAST_POLL_TIMESTAMP_MS:<n> ONLINE:<0|1> STALE:<0|1> HEALTH:<UNKNOWN|HEALTHY|DEGRADED|OFFLINE|STALE> LAST_POLL_RESULT:<result> LAST_ERROR:<result> DIAGNOSTICS_REQUESTED:<0|1> DIAGNOSTICS_ATTEMPTED:<0|1> DIAGNOSTICS_VALID:<0|1> AGC:<n> MAGNITUDE:<n> DIAGNOSTICS_TIMESTAMP_MS:<n> DIAGNOSTICS_RESULT:<result> CALIBRATION_CONFIGURED:<0|1> CALIBRATION_FORMAT:<n>
DATA AS5600_COMMUNICATION DEVICE_ID:<id> POLLS:<n> SUCCESSFUL_SAMPLES:<n> FAILED_POLLS:<n> CONSECUTIVE_FAILURES:<n> LAST_SUCCESS_MS:<n> LAST_FAILURE_MS:<n> LAST_ERROR:<result>
DATA AS5600_CALIBRATION DEVICE_ID:<id> ID:<id|NONE> ID_TRUNCATED:<0|1> ID_SANITIZED:<0|1> HARDWARE:<id|NONE> HARDWARE_TRUNCATED:<0|1> HARDWARE_SANITIZED:<0|1> PROVENANCE:<sha-or-reference|NONE> PROVENANCE_TRUNCATED:<0|1> PROVENANCE_SANITIZED:<0|1>
```

`RAW_ANGLE` is one-turn device phase, not a mechanical steering coordinate. AGC and
`MAGNITUDE` are the optional one-shot diagnostic read; consume them only when
`DIAGNOSTICS_VALID:1` and retain their own timestamp rather than treating them as a
fresh control-rate observation. The command is intentionally not part of the generic
position-observation/capability API used by L4/L5 tests.

`AS5600_COMMUNICATION` reports cached polling counters and timestamps. It does not
perform a retry or a new read; use it to distinguish no samples, recurring transport
errors and the last observed error from a fresh sensor observation.

The calibration metadata is emitted separately from the sensor snapshot so either
wire line remains bounded. Each metadata token is converted to a serial-safe token
and capped at 128 characters; its `*_SANITIZED` or `*_TRUNCATED` flag makes that
transformation explicit. A normal 64-character SHA-256 provenance is retained in
full.

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
- `GET_SPEED`, `GET_MOTOR`, `SVD48_INVENTORY`, typed SVD48 channel telemetry,
  `SVD48_PROBE`, `READ_REG`, `GET_SVD48_CONFIG` and `POLL_ONCE`.
- `STOP <motor|ALL>` and `SET_SPEED <motor> <rpm>`.
- The exact-shape `SVD48_BENCH_*` commands documented above.
- Confirmed register writes, save, gear-ratio and identify operations.

Everything else returns `ERR LAN_COMMAND_BLOCKED <command>`. This allowlist is
broader than the intended production boundary: speed and persistent writes over
LAN have no command lease, deadman or replay protection. Use LAN as a trusted
bench maintenance interface only.

Allowlisting does not bypass the continuous-control interlock described above; that
firmware-side state check occurs when an allowed command is executed.

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

`control_lan` is the authenticated continuous-intent protocol on UDP port `32322`.
It starts only when the selected profile has validated differential geometry. It is
separate from Maintenance LAN: joystick heartbeats never use port `32321` or ASCII
speed commands, and PPM is not a control authority in this version.

Rafa additionally has a profile-owned RC/LAN interlock. It is source selection, not
PPM motion control:

- Before the first valid PPM frame, LAN may create a session.
- Rafa's reviewed receiver failsafe is `CH5=2000us`; valid `CH5>1500us` is
  `RC_FAILSAFE` and permits a **new** LAN ARM.
- Valid `CH5<=1500us` is `PPM_PRIORITY`. `control_lan` revokes its active stream,
  queues STOP through the normal motion service and rejects ARM/COMMAND while that
  condition persists.
- A loss after PPM priority is recorded as `PPM_LOST`. It never resumes the retired
  LAN stream; a new ARM is required after PPM no longer has priority.

This check is polled by `control_lan` independently of incoming joystick packets.
It therefore also revokes an already-active LAN stream when the browser/backend has
gone quiet. STOP and DISARM remain terminal actions regardless of this interlock.

Every request carries the current maintenance token plus a client-generated stream
and monotonically increasing sequence. ARM, DISARM and STOP omit `command`; COMMAND
includes semantic body velocity and deadman:

```json
{
  "type": "botfarms_control_command",
  "protocol_version": "1.0",
  "request_id": "unique-request-id",
  "token": "secret",
  "action": "command",
  "stream_id": "unique-session-id",
  "sequence": 2,
  "command": {
    "vx_mps": 0.05,
    "vy_mps": 0.0,
    "wz_radps": -0.10,
    "deadman": true
  }
}
```

The response shape is:

```json
{
  "type": "botfarms_control_response",
  "protocol_version": "1.0",
  "request_id": "unique-request-id",
  "status": "ok",
  "detail": "QUEUED"
}
```

`QUEUED` means the semantic event passed transport checks and was copied to the
motion-service mailbox; it is not an SVD48 acknowledgement or proof of physical
motion. The motion service independently checks safety and endpoint health. The
client supplies no TTL: the immutable profile owns it (300 ms for `current_robot`,
with profile validation bounded to 50–500 ms). Replayed/non-increasing commands do
not refresh that lease. Deadman false is normalized by the Console to zero intent and
causes the firmware to stop. Exact source expiry retires the stream and requests
STOP; the old stream cannot silently resume.

STOP/DISARM are accepted as fail-safe terminal actions without requiring a matching
active stream. A new ARM for another stream first publishes a source-switch STOP.
Velocity fields must be finite and within the profile limits returned by
`CONTROL_STATUS`; differential v1 accepts no material lateral velocity. The packet
is parsed as decimal JSON but the active control representation is `float`, so
validation quantizes to that representation before comparing the profile bound. A
decimal equal to a published bound such as Rafa's `0.02` m/s is therefore accepted;
values that quantize above the bound remain rejected as `BAD_VELOCITY`.

`CONTROL_STATUS` is a read-only ASCII command exposed through Maintenance LAN for
observation, not intent. It returns one session line followed by the declared number
of endpoint lines:

```text
DATA CONTROL TASK:<RUNNING|STOPPED> STATE:<DISARMED|ARMED|ACTIVE|EXPIRED|FAULT> SOURCE:<NONE|LAN|RC> DEADMAN:<0|1> TTL_MS:<n> LEASE_FRESH:<0|1> LEASE_AGE_MS:<n> LEASE_REMAINING_MS:<n> STREAM_HASH:<hex> SEQUENCE:<n> MAX_VX_MPS:<n> MAX_VY_MPS:<n> MAX_WZ_RADPS:<n> REQUESTED_VX_MPS:<n> REQUESTED_VY_MPS:<n> REQUESTED_WZ_RADPS:<n> ENDPOINTS:<n> DETAIL:<token>
DATA CONTROL_AUTHORITY LAN_ELIGIBLE:<0|1> RC_INTERLOCK:<DISABLED|RC_NO_SIGNAL|RC_FAILSAFE|PPM_PRIORITY|PPM_LOST|RC_CHANNEL_UNAVAILABLE> RC_CH5_US:<n|0> LAN_REVOCATION_EPOCH:<n>
DATA CONTROL_ENDPOINT ID:<id> NAME:<name> TARGET_RPM:<rpm> OBSERVED_VALID:<0|1> OBSERVED_RPM:<rpm> OBSERVATION_MS:<n> ONLINE:<0|1> STALE:<0|1> HEALTH:<health>
```

Profiles without qualified differential geometry return `ERR CONTROL_UNAVAILABLE`.
The current `rafa` profile carries its operator-qualified M1/M2 side/sign mapping and
therefore exposes this control plane. Availability does not by itself qualify physical
direction, expiry latency or floor motion for a particular deployed artifact.

### Rafa PPM source

Rafa additionally owns one local PPM source. It does not use Maintenance LAN and it
does not send controller commands directly: `ibus_receiver → ppm_motion_source →
motion_application → command_authority → robot_kinematics → traction endpoints`.
`CONTROL_STATUS SOURCE:RC` identifies that selected source; `SOURCE:NONE` means no
armed stream and `SOURCE:LAN` identifies the UDP source.

The immutable Rafa mapping is CH2 high = forward (`+vx`), CH4 high = right
(`-wz`, because differential positive yaw is left), and CH5≤1500us = PPM priority.
The receiver failsafe CH5=2000us leaves LAN eligible. The source needs a *new* valid
PPM frame with CH2 and CH4 neutral (1500±30us) after PPM priority begins, after PPM
loss, or after any external STOP before it can ARM. Each following fresh PPM frame
publishes a bounded command with the profile TTL (300 ms); stale/missing PPM stops
and retires the RC stream. The profile uses the reviewed PPM acceptance window
750–2250us as an input bound, not a claim that radio endpoints are calibrated.

`SAFETY_STATUS` appends the same `RC_INTERLOCK`, `RC_CH5_US`, `LAN_ELIGIBLE` and
`LAN_REVOCATION_EPOCH` fields. Consumers must continue to parse by key and ignore
unknown additions, as required by the ASCII v1 framing contract.
