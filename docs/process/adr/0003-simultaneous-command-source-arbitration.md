# ADR-0003: Simultaneous Command Source Arbitration

- Status: `ACCEPTED`
- Date: 2026-07-19
- Owners/session IDs: parent `019f6e72-3486-7ce1-af40-72d240a5f676`
- Work-item IDs: `SAFE-002`, `SAFE-003A..003D`, `SAFE-004`, `SAFE-006`

## Context

RC, maintenance LAN and Bluetooth may all be connected simultaneously. Letting
each transport call movement APIs creates races, stale-command fallback and a
path for a lower-priority source to reapply motion while another task is stopping
the robot.

The required precedence is always RC above LAN above Bluetooth. Communication
loss must never leave the last speed active indefinitely.

## Decision

- Each source adapter only publishes a timestamped, sequenced command mailbox.
- LAN motion uses a compact `control_lan` ingress separate from the
  `maintenance_lan` management dispatcher.
- One high-priority arbiter selects the command consumed by control.
- Semantic precedence is `RC > LAN > Bluetooth`.
- A fresh valid RC frame owns precedence even at neutral or with dead-man
  released; it produces zero and blocks lower sources.
- LAN preempts Bluetooth.
- Every source has a bounded command TTL. Loss while moving causes emergency stop,
  a latched inhibit and `FAULTED`.
- Source changes use a new authority epoch: stop first, discard commands from the
  prior epoch, then require a command received after the switch.
- There is no automatic fallback to an older lower-priority command.
- `STOP ALL` is universally accepted and bypasses selection only in the direction
  of less motion.
- Network, JSON and Bluetooth parsing do not execute in the control task.
- USB movement must enter the same arbiter or be disabled; it cannot remain a
  direct actuator bypass.

## Alternatives Considered

- Last command wins: rejected because scheduling jitter determines authority.
- Direct fallback when RC disappears: rejected because a stale LAN/BT command can
  restart or continue movement unexpectedly.
- Disable lower sources whenever RC is configured: rejected because simultaneous
  connectivity and controlled takeover are required.
- Use FreeRTOS task priority as authority: rejected because it does not define
  data freshness, preemption or stale-command behavior.

## Consequences

- Source priority and task scheduling are explicitly separate.
- A takeover includes a deliberate stop discontinuity. Smooth blending can be
  evaluated later only after safety evidence.
- RC remains continuously sampled; LAN and Bluetooth continue receiving data but
  cannot bypass the selected epoch.
- The status API must expose all source freshness plus selected source/epoch.

## Verification and Migration

- Add virtual-clock tests for all pairwise and three-source conflicts.
- Inject loss during nonzero motion and prove stop plus no stale fallback.
- Measure elevated RC preemption, source-loss detection, zero-frame and zero-RPM
  latency.
- Remove every direct movement call from serial, LAN, RC and Bluetooth adapters.

## Supersedes / Superseded By

None.
