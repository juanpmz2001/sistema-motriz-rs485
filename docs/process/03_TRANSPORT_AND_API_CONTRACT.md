# Transport and API Contract Plan

Date: 2026-07-17

Status: current behavior audited; v1 result/framing hardening is in progress with `E1/E2` evidence, normative v2 contract is not implemented.

## Ownership

- Firmware domain services own command semantics, safety checks, profiles, SVD48 schema, and results.
- USB serial owns line/chunk framing only.
- Maintenance LAN owns datagram framing, local token validation, mutation dedupe,
  bounded queues and discovery only. Production authentication/replay protection
  is a later extension.
- Web backend owns transport lifecycle, request correlation, telemetry scheduling,
  result persistence and browser API. Operator authentication is deferred for the
  local MVP.
- Frontend renders typed state and requests capabilities; it never parses registers or constructs raw SVD48 writes.

## Current End-to-End Paths

USB:

```text
browser -> HTTP /api/robot/command -> backend write queue
-> serial ASCII line -> serial_gateway parser/dispatcher
-> printed OK/DATA/ERR lines -> serial parser -> WebSocket serial_line
-> browser global line matcher
```

LAN:

```text
browser -> HTTP /api/robot/command -> backend UDP request UUID
-> maintenance_lan token check -> serial_gateway_execute_command(LAN_SAFE)
-> captured ASCII lines -> UDP JSON response -> backend serial_line broadcasts
-> browser global line matcher
```

## Confirmed Contract Gaps

| ID | Gap | Consequence |
| --- | --- | --- |
| `TRANS-GAP-001` | V1 LAN now maps captured `ERR ...` to an error envelope, but the dispatcher still returns `ESP_OK` and meaning is inferred from text | Compatibility path is truthful for current `ERR` lines; typed result migration remains required |
| `TRANS-GAP-002` | Request ID ends at UDP backend boundary | Browser cannot reliably associate traces/results with its request |
| `TRANS-GAP-003` | One global frontend active transaction | Concurrent commands/clients or unsolicited telemetry can close the wrong request |
| `TRANS-GAP-004` | UDP receive buffer is 768 bytes; response capture can exceed 9 KB | Truncation, IP fragmentation, loss, and incomplete JSON risk |
| `TRANS-GAP-005` | No explicit protocol version/capability negotiation | Web/firmware compatibility is accidental |
| `TRANS-GAP-006` | Static token is sent in plaintext and compared with `strcmp` | Capture/replay and timing concerns; no client/session identity |
| `TRANS-GAP-007` | Backend generic command endpoint has no independent LAN command allowlist | Firmware is the only effective LAN policy for most blocked commands |
| `TRANS-GAP-008` | Web API/WS lacks operator auth/origin/CSRF checks | Any reachable browser/client can invoke exposed endpoints |
| `TRANS-GAP-009` | LAN connection status remains true after later communication failures | UI can display a stale connection state |
| `TRANS-GAP-010` | Each browser owns telemetry polling | Multiple browsers compete for command bandwidth |
| `TRANS-GAP-011` | Serial path and baud are caller-controlled; container is privileged with broad `/dev` | Excess device access and unsafe deployment boundary |
| `TRANS-GAP-012` | Audit is volatile lines/counters only | No durable actor/request/result/profile/controller history |
| `TRANS-GAP-013` | Overlong serial input now drains through CR/LF; explicit response completion framing is still absent | Suffix execution is covered by host tests, but clients can still misattribute multi-line/unsolicited output |
| `TRANS-GAP-014` | LAN retries reuse an ID but firmware has no dedupe cache; OTA announce retries lack equivalent request identity | A retried mutating operation can execute more than once |
| `TRANS-GAP-015` | Compatibility tools consume the first matching line without a reliable response terminator | Multi-line/unsolicited output can be attributed incompletely |
| `TRANS-GAP-016` | OTA announce can update NVS before action validation and builds some JSON manually | Invalid/retried input can mutate state; unescaped output can be malformed |
| `TRANS-GAP-017` | Build 19 permits direct `SET_SPEED` through `maintenance_lan` without TTL/dead-man/authority arbitration | A successful speed can remain active after LAN/client loss; this blocks floor/product use until `SAFE-011` removes or gates the bypass |

## Structured Management Result

Internal firmware operations must return a typed result, not print their meaning:

```c
typedef enum {
    MGMT_OK,
    MGMT_ACCEPTED,
    MGMT_INVALID_ARGUMENT,
    MGMT_UNSUPPORTED,
    MGMT_UNAUTHORIZED,
    MGMT_FORBIDDEN_STATE,
    MGMT_BUSY,
    MGMT_TIMEOUT,
    MGMT_TRANSPORT_ERROR,
    MGMT_DEVICE_EXCEPTION,
    MGMT_READBACK_MISMATCH,
    MGMT_INTERNAL_ERROR,
} management_result_code_t;
```

Result fields:

- `protocol_version`, `request_id`, `command_id`, `boot_id`.
- `accepted`, `completed`, `result_code`, safe human `detail`.
- typed `data` or paginated item payload.
- structured field errors where applicable.
- start/end monotonic timestamps and optional device exception code.
- diagnostic `lines` only during compatibility migration.

Serial gateway text output becomes an adapter over this result. Maintenance LAN calls the same management operation directly. There must be no output-capture inference in the final design.

## Protocol Version and Capabilities

Every connection starts with capabilities:

```json
{
  "protocol": {"major": 2, "minor": 0},
  "firmware": {"project": "...", "build": 10, "boot_id": "..."},
  "limits": {
    "max_request_bytes": 768,
    "max_response_bytes": 1200,
    "max_page_items": 16
  },
  "features": {
    "robot_profiles": true,
    "svd48_typed_read": true,
    "svd48_change_sets": false,
    "maintenance_sessions": false,
    "maintenance_jobs": true
  },
  "policies": {
    "lan_safe_commands": ["diagnostics", "telemetry", "stop_all"],
    "configuration_write": "unsupported"
  }
}
```

Compatibility rules:

- Unknown major: reject with `UNSUPPORTED_PROTOCOL`.
- Same major/newer minor: ignore unknown optional fields but rely on advertised capabilities.
- Required missing field: reject explicitly; never default a missing action to another action.
- Capabilities govern UI controls; version numbers alone do not.
- Maintain v1 compatibility only for a named deprecation window and read/stop functions.

## LAN Request Envelope v2 for the Local MVP

```json
{
  "type": "botfarms_management_request",
  "protocol_version": "2.0",
  "request_id": "uuid",
  "boot_id": "optional-last-seen",
  "deadline_ms": 1500,
  "action": "svd48.parameters.read",
  "payload": {},
  "token": "provisioned-maintenance-token"
}
```

Local MVP requirements:

- reuse the already provisioned maintenance LAN token;
- never print or return the token;
- bound datagrams, actions, deadlines and work queues;
- deduplicate mutating requests by `{boot_id, request_id, request_digest}`;
- reject reuse of an ID with a different payload;
- execute configuration only as an exclusive firmware maintenance job;
- keep all safety/range/readback checks in firmware.

Operator identity, HMAC, nonce/timestamp, replay-security and TLS are production
extensions, not blockers for the local prototype. They become mandatory before a
hostile/shared network, Internet exposure, customer delivery or untrusted
multi-operator use.

UDP is suitable for discovery, compact status, compact reads, heartbeat, and idempotent stop. It is not suitable for unbounded schemas or profile documents in one datagram.

## Payload Sizing and Pagination

MVP rule: keep application UDP datagrams at or below `1200` bytes unless network MTU evidence justifies another value.

Large operations use one of:

- paginated reads with `page_token` and fixed item limit;
- chunk IDs with index/count, per-chunk hash, total hash, expiry, and explicit finalization;
- a future authenticated reliable TCP/TLS transport.

Requirements:

- detect truncated input (`MSG_TRUNC` or equivalent) and return/count `REQUEST_TOO_LARGE` when possible;
- declare exact maximum request/response sizes in capabilities;
- never create a 9 KB stack/heap response merely because line capture permits it;
- reject missing/duplicate/out-of-order chunks;
- stage uploads separately and validate complete payload before profile activation;
- retries must be idempotent by request ID/change-set ID.
- repeated `{boot_id, request_id, request_digest}` returns the cached result; reuse of an ID with a different digest is rejected.

## Serial Framing

Retain simple ASCII for human diagnostics and small commands. Add a framed, chunked representation for typed payloads rather than increasing command-line length.

Possible compatibility form:

```text
REQ <request_id> CAPABILITIES
RES <request_id> OK <json-fragment>
BEGIN <request_id> <content-type> <bytes> <sha256>
CHUNK <request_id> <index> <base64-data>
END <request_id>
```

The final syntax should be selected only after golden parser tests define limits, CR/LF behavior, malformed chunks, timeout cleanup, and resynchronization. Binary CBOR framing may be preferable later, but is not required for the first read-only page.

For the existing ASCII parser, overflow or invalid framing must enter a drain state until the next complete delimiter. It must never reinterpret a suffix from the rejected line as a fresh command. Compatibility clients need an explicit response completion marker or correlated structured result instead of stopping at the first plausible line.

## Command Policy

Capabilities are domain actions, not raw command strings:

| Capability | Local token LAN | Maintenance operation | USB engineering |
| --- | --- | --- | --- |
| Diagnostics/status | allow | allow | allow |
| Typed telemetry/read-only SVD48 | allow | allow | allow |
| `STOP ALL` | allow, idempotent, highest priority | allow | allow |
| Robot profile upload/validate | allow staging | allow | allow |
| Robot profile activate | explicit guarded job | explicit guarded job | explicit guarded job |
| SVD48 change-set validate | deny or dry-run only | allow | allow |
| SVD48 change-set apply/save | deny | allowlisted fields and confirmation | allowlisted fields and confirmation |
| Motion | deny in maintenance protocol | deny | separate authority path only |
| Raw register write | deny | deny | diagnostic build only |
| Wi-Fi/key/OTA trust mutation | deny | separate provisioning policy | explicit provisioning only |

Backend policy must mirror firmware for early rejection and good UX, but firmware remains authoritative.

### Provisional Bench Deviations (Build 19)

To unblock supervised elevated-bench diagnosis, authenticated maintenance LAN currently permits
`READ_REG`, `WRITE_REG ... CONFIRM`, and `WRITE_REGS ... CONFIRM`. Firmware still
blocks runtime-actuation addresses, checks its stopped heuristic, captures old
words and verifies readback. The web backend mirrors known ranges for pole pairs,
current, direction, sensor type and speed dead zone.

Build 19 additionally permits `STOP n|ALL` and direct `SET_SPEED n rpm`, with an
absolute `+/-15 RPM` firmware/gateway ceiling. This is a more serious temporary
deviation: the ASCII maintenance path has no TTL, dead-man, authority epoch,
latched state gate or automatic stop when the client/network disappears. It is
restricted to a physically supervised elevated bench and tracked by ADR-0004 and
`SAFE-011`.

This is a documented temporary deviation from the target table above. It has no
job ID/deduplication, exclusive `MAINTENANCE` owner, complete catalog, save or
rollback. `OUTCOME:UNKNOWN`/`ACKED_UNVERIFIED` is terminal for that request: the
client must read back and must not retry blindly. `MOVE_VEL`, `ENABLE` and other
movement APIs remain denied, but direct `SET_SPEED` is not denied in build 19.

## Separate LAN Motion Ingress

The target architecture keeps `maintenance_lan` as a management plane with
motion blocked in its ASCII command policy. Operational LAN movement uses
`control_lan` on a separate
configured port (draft default `32322`). It accepts only a compact fixed action:

```json
{
  "type": "botfarms_control_command",
  "protocol_version": "1.0",
  "request_id": "request-uuid",
  "stream_id": "random-per-backend-control-stream",
  "sequence": 42,
  "token": "local-maintenance-token",
  "action": "command",
  "command": {"vx_mps": 0.1, "vy_mps": 0.0, "wz_radps": 0.2, "deadman": true}
}
```

Rules:

- receive timestamp on the ESP defines freshness; laptop wall time is not trusted;
- accepted actions are only `arm`, `command`, `disarm` and `stop`;
- sequence must be an exact JSON integer and increase within a stream;
  duplicate/regressive packets are rejected;
- a new stream must first send `arm`, starts with no authority and triggers the
  normal stop/new-epoch path; a retired stream cannot be resumed in the same boot;
- finite/range validation happens before publishing the LAN mailbox;
- socket code never calls kinematics, `robot_control` or SVD48;
- backend refreshes commands while control is active; firmware TTL is the
  authoritative stop on browser/backend/Wi-Fi loss;
- RC can preempt at any time and LAN cannot bypass `RC > LAN > Bluetooth`.

Management read/write reliability and motion freshness therefore have separate
queues, sizing, metrics and failure domains while sharing the local token only as
an MVP provisioning convenience.

## Maintenance Operation Contract

Job fields:

- `job_id`, `request_id`, request digest, originating transport, creation and expiry.
- safety state before entry, current inhibit reason, controller/telemetry health.
- allowed capabilities and fields.
- staged profile/change-set IDs.

Open sequence:

1. Validate USB framing or the local LAN maintenance token.
2. Deduplicate the request and request maintenance transition.
3. Latch movement inhibit and revoke active command lease.
4. Send `STOP ALL` and verify commanded/measured zero with fresh telemetry.
5. Verify required controllers online and fault policy.
6. Return an exclusive firmware-owned job ID.

Expiry/client disconnect:

- stop remains asserted;
- staged writes remain staged, not auto-committed;
- robot remains disarmed;
- job result records reason;
- reconnect queries by job/request ID and inspects fresh state.

The MVP has no operator-owned session or heartbeat. Exclusivity is an internal
consistency and safety mechanism. A later production protocol may add identity
and a user session without changing the change-set semantics.

## Backend/Browser Contract

HTTP response envelope:

```json
{
  "requestId": "uuid",
  "ok": false,
  "code": "SVD48_READBACK_MISMATCH",
  "message": "The controller did not retain the requested value.",
  "fieldErrors": [],
  "data": null
}
```

Recommended status mapping:

- `400`: malformed request.
- `401`: missing/invalid local device token, when the backend exposes that result.
- `403`: firmware policy/state denied.
- `404`: profile/controller/resource absent.
- `409`: state/revision/job/request-ID conflict.
- `422`: semantically invalid field/change set.
- `429`: rate limit/busy queue.
- `502`: downstream malformed/device transport failure.
- `504`: downstream deadline exceeded.

WebSocket event envelope:

```json
{
  "protocolVersion": "1.0",
  "eventId": "uuid",
  "requestId": "optional-originating-request",
  "commandId": "optional-firmware-command",
  "bootId": "firmware-boot-id",
  "timestamp": "ISO-8601",
  "type": "telemetry.sample",
  "payload": {}
}
```

Do not replay arbitrary historical serial lines to every newly connected browser.
Typed current state and a redacted result-history query replace implicit line
history.

## Telemetry Ownership

Backend owns one scheduler per connected ESP:

- chooses a supported sample rate within firmware/bus limits;
- coalesces subscriptions from browsers;
- emits typed samples with device sequence/time, receipt time, freshness, and parse status;
- prevents tuning/configuration reads from starving stop or safety polling;
- records dropped/coalesced sample metrics;
- stops/restarts deterministically across transport state changes.

Frontend never converts missing numeric fields to zero. It displays `unknown`, `stale`, or `invalid` distinctly.

## Automated Tests

Shared fixtures should be consumed by C/Python/Node where practical:

- version/capability negotiation and unsupported major/minor.
- valid, malformed, missing, unknown, and wrong-type fields.
- 0/boundary/oversized request and response payloads.
- truncation, drop, duplicate, reorder, stale reply, spoofed source, and retry.
- local token missing/wrong/redacted and mutating request dedupe/ID conflict.
- production-only suite later: HMAC wrong key/MAC/payload, nonce replay, expired timestamp and rotation overlap.
- structured success/failure parity across serial and LAN.
- command ID correlation through HTTP, transport, firmware, WebSocket, and audit.
- two clients issuing reads, disconnects, and maintenance jobs concurrently.
- telemetry scheduler plus long configuration read plus emergency stop.
- job exclusivity/expiry/reconnect and staged change behavior.
- serial `N-1/N/N+1` limits, overflow followed by a dangerous-looking suffix, delimiter resynchronization, and argument overflow.
- duplicate identical request, duplicate ID with changed body, retry after lost response, reboot-cleared dedupe state, and long-job idempotency.
- invalid OTA action and malformed/escaped detail fields must not mutate NVS or produce invalid JSON.

## Migration Sequence

1. Finish current result truthfulness (`TRANS-001`): firmware compatibility mapping is implemented; verify ESP/LAN/backend/UI and then replace text inference with typed results.
2. Introduce internal typed management result and adapter tests (`TRANS-002`).
3. Add capabilities/version/IDs and v1 compatibility (`TRANS-003/005`).
4. Bound/chunk profile payloads and add request idempotency (`TRANS-004/008`).
5. Fix serial drain/completion framing (`TRANS-007`).
6. Add maintenance jobs/change sets only after the safety foundation.
7. Implement and test the backend transports/routes before frontend work.
8. Move telemetry scheduling to backend (`TRANS-006`).
9. Validate OTA actions before persistence and use structured JSON serialization.
10. Deprecate line inference and broad raw endpoints after typed consumers are proven.
11. Before production exposure, add containment/operator auth (`AUTH-001/002`) and HMAC/TLS/replay protection (`AUTH-003/004`).
