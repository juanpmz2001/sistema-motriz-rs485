# ADR-0004: Temporary Maintenance-LAN Bench Actuation

- Status: `ACCEPTED` as a temporary engineering exception; prohibited for floor/product use
- Date: 2026-07-25
- Owners/session IDs: parent `019f6e72-3486-7ce1-af40-72d240a5f676`
- Work-item IDs: `SAFE-011`, `TRANS-009`, `TEST-SAFE-026`

## Context

The build-19 KK16/SVD48 diagnosis needed individual low-speed commands while the
ESP was powered by the assembled platform and reachable only over Wi-Fi. The
target `control_lan` path already parses sequenced commands with TTL, but it is
intentionally not started because runtime state, authority adapters and a single
actuator coordinator are not integrated.

The implemented expedient added `SET_SPEED n rpm` and `STOP n|ALL` to the
authenticated `maintenance_lan` allowlist. The gateway rejects requests beyond
`+/-15 RPM`, and `robot_control` clamps to the same ceiling.

## Decision

- Preserve the build-19 behavior only as an explicitly documented elevated-bench
  engineering exception while its captured evidence remains reproducible.
- Require the provisioned maintenance token, wheels clear of the floor, an
  operator at physical power cutoff, one motor at a time, conservative current
  limits, the tool's `--confirm-elevated` acknowledgement and an explicit final
  stop.
- Do not describe authentication or the RPM ceiling as a motion-safety system.
- Do not use this path for floor, unattended, loaded, customer or production
  operation.
- Remove `SET_SPEED` from `LAN_SAFE`, or route it through the common
  state/authority/TTL actuator coordinator, before any floor/product release.

## Safety Deficit

This exception has no command TTL, dead-man, source mailbox, authority epoch,
latched operational-state gate or automatic stop on UDP/client/Wi-Fi loss. A
successful nonzero command may remain active until another command/stop arrives
or an independent fault/RC-loss condition triggers the best-effort safety task.
The current safety task is not a substitute for a command lease.

Raw `WRITE_REG(S)` still denies known actuation registers; this ADR does not
authorize broadening that raw-write surface or enabling `MOVE_VEL`, `ENABLE`,
`CLEAR_FAULT` or destructive OTA through maintenance LAN.

## Alternatives Considered

- Start `control_lan` immediately: rejected because its parser does not yet own a
  runtime mailbox/coordinator and starting it would imply safeguards that are not
  present.
- Require USB for every bench command: operationally unavailable during the
  assembled-platform session, but remains the preferred recovery/provision path.
- Treat laptop-side `finally: STOP` as the dead-man: rejected because process,
  laptop, AP or packet loss can prevent that cleanup from reaching the ESP.

## Consequences

- Build 19 is suitable only for supervised engineering diagnosis, not a product
  release candidate.
- Backend/UI code must not silently expose this exception as normal remote
  control.
- `SAFE-011` and `TEST-SAFE-026` are critical blockers. Documentation and policy
  tests must fail review if they again claim all LAN movement is blocked while
  this exception exists.

## Verification and Migration

1. Add host policy coverage for the temporary allowlist and the final removed or
   gated behavior.
2. Integrate `robot_state`, `command_authority`, source TTL and one actuator
   coordinator; enforce the permit immediately before I/O.
3. Start `control_lan` only after the common mailbox path and validated robot
   profile exist.
4. Prove source loss, dead-man release, Wi-Fi loss and stop latency on the
   elevated robot under `04_OFF_GROUND_TEST_MATRIX.md`.
5. Remove the direct maintenance dispatcher path and update API/docs/backend
   capability negotiation in the same commit.

## Supersedes / Superseded By

This ADR does not supersede ADR-0001 or ADR-0003. It records a temporary deviation
from both. It must be superseded by the ADR/commit that completes `SAFE-011`.
