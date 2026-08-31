# Communication safety matrix

**Status:** Phase-0 policy baseline, 2026-08-30. The reaction class distinguishes
communication quality from real safety facts.

| Condition | Reaction class | Required behavior | Never do |
| --- | --- | --- | --- |
| Independent physical safety input / E-stop when configured | **IMMEDIATE** | Preserve current safety path. | Debounce through communications quality. |
| CRC-valid SVD48 controller error register nonzero | **IMMEDIATE** | Report `FAULT` and retain configured motor-fault safety behavior. | Treat it as a retryable transport error. |
| SVD48 exception response | **IMMEDIATE** command result; **QUALITY_ONLY** liveness | Return exact exception; retain raw evidence. | Claim offline from the exception alone. |
| One SVD48 timeout/CRC/incomplete/busy observation | **QUALITY_ONLY** | Reject new sample, increment evidence, preserve LKG/time. | Replace value with zero, create physical fault, flap endpoint. |
| Persistent SVD48 failures | **CONFIRMED_N_SAMPLES** and **AGE_TIMEOUT** | Advance configured quality state; stale/offline remains absolute LKG age. | Let count prevent stale forever. |
| Required motion observation age exceeded | **AGE_TIMEOUT** | Mark only that required field/channel liveness stale/offline. | Let slow temp/bus cadence define velocity liveness. |
| Exact, plausible PPM frame | **PRESENTATION_ONLY** normally | Update PPM LKG/sequence and perhaps authority candidate. | Let malformed history become fresh. |
| PPM count/pulse rejection | **QUALITY_ONLY** | Count it; do not update channels, CH5, sequence or epoch. | Truncate/accept it or generate motion. |
| Fresh CH5 authority transition | **CONFIRMED_N_VALID_SAMPLES** | Profile-owned confirmation; cancel reverse candidate. | Change `WAIT_NEUTRAL`, CH5 semantics or stale deadline. |
| No valid PPM through deadline | **AGE_TIMEOUT** / existing safety action | Preserve current stale/loss/stop behavior. | Add extra missed-frame grace after deadline. |
| One i-BUS bad header/checksum | **QUALITY_ONLY** | Reject frame and retain LKG. | Immediately declare receiver offline. |
| i-BUS valid-frame age expired | **AGE_TIMEOUT** | Existing receiver/safety path. | Refresh time on bad bytes. |
| SBUS selected without parser | **IMMEDIATE configuration rejection** | Report unsupported. | Pretend UART settings validate SBUS. |
| One missing Control LAN UDP packet | **QUALITY_ONLY** | Retain valid command until TTL; record gap if observable. | STOP from a single missing sequence. |
| Control LAN duplicate/out-of-order | **IMMEDIATE protocol rejection** | Reject/count; do not extend lease. | Accept/replay or stop a fresh session just for duplicate. |
| Control LAN TTL or deadman deadline | **IMMEDIATE** | Preserve deterministic expiry/STOP. | Add retry/debounce grace. |
| Explicit Control LAN STOP/DISARM | **IMMEDIATE** | Preserve priority and retirement. | Wait for confirmation samples. |
| Committed RC priority epoch | **IMMEDIATE after confirmation** | Stop/revoke LAN once. | Increment epoch on rejected/candidate frame. |
| One Maintenance read timeout | **QUALITY_ONLY** | Preserve host LKG and raw failure; bounded safe-read retry allowed. | Disconnect robot or retry side effect. |
| Persistent Maintenance failure / last-success age | **AGE_TIMEOUT** + **CONFIRMED_N_SAMPLES** | Present host peer degraded/stale/offline; Control TTL independent. | Infer RS485/motor fault from it. |
| Maintenance action timeout without dedup | **IMMEDIATE result ambiguity** | Retain request/action evidence; read state or ask operator. | Auto-retry. |
| Wi-Fi disconnect event | **IMMEDIATE link state** | Show link loss; live control expires on unchanged TTL. | Treat missed discovery as same fact. |
| Discovery misses robot | **PRESENTATION_ONLY** | Show absent from this scan. | Change active connection/authority. |
| AS5600 one primary I2C failure | **QUALITY_ONLY** until age | Preserve sample/time and expose attempt. | Substitute zero or call magnet fault. |
| Valid AS5600 magnet/status fault | **IMMEDIATE** for configured sensor policy | Preserve existing device policy. | Debounce as noise. |
| WebSocket queue drop/browser loss | **PRESENTATION_ONLY** | Expose host stream quality/reconnect. | Claim hardware fault. |

## Invariants

1. **Control LAN TTL does not change.** No valid command by the current deadline
   still causes the existing STOP behavior.
2. **`WAIT_NEUTRAL` does not change.** RC source re-entry still requires its
   existing neutral handshake.
3. **HOLD 0 does not change.** Reliability telemetry does not alter the reviewed
   `target=0 + START` SVD48 behavior.
4. A valid controller/physical fault is never delayed by hysteresis.
5. An invalid observation never overwrites LKG or its original timestamp.
6. Thresholds are source-owned and time-bounded; no global magic “three errors
   before stopping” rule exists.
