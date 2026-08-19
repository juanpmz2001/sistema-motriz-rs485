# Firmware documentation

Current source code and executable tests take precedence over this documentation.
Physical observations are evidence only for the conditions and artifact they name.

## Read by task

- **Any task:** [Master engineering guide](BOTFARMS_ENGINEERING_MASTER_GUIDE.md)
  for the shared layering and ownership philosophy.
- **Any firmware change:** [Architecture](ARCHITECTURE.md) and
  [Safety](SAFETY.md).
- **Serial, LAN, or a public response:** [Command API](API.md).
- **SVD48, RS485, polling, or drive units:** [SVD48 integration](SVD48.md).
- **OTA, release, or recovery:** [OTA](OTA.md). Rafa changes are deployed by OTA;
  USB is recovery-only.
- **Physical/HIL work:** [Testing guide](TESTING_ARCHITECTURE_GUIDE.md),
  [physical-test runbooks](testing/README.md), [evidence template](testing/EVIDENCE_TEMPLATE.md)
  and [Safety](SAFETY.md).
- **Rafa workshop decisions:** [Rafa bench state](robots/RAFA_BENCH_STATE.md).

## Current contracts

- [Architecture](ARCHITECTURE.md) is the as-built runtime, composition and
  concurrency description.
- [Safety](SAFETY.md) is the safety contract and release-gate source.
- [API](API.md), [SVD48](SVD48.md), and [OTA](OTA.md) own their respective public
  and operational contracts.
- [Testing guide](TESTING_ARCHITECTURE_GUIDE.md) owns test levels, evidence classes
  and the physical-test lifecycle. The shorter [runbooks](testing/README.md) are
  its operational companion.
- [Next steps](NEXT_STEPS.md) records the compact handoff state. It is not a
  substitute for a requested scope, source inspection, or physical evidence.

## Future work plans

- [SVD48 Workspace v2 plan](SVD48_WORKSPACE_V2_PLAN.md) records the implemented
  NEXT-1 generic controller/channel and Bench Control contract plus the still-future
  NEXT-2 parameter/float design. Its status note separates those scopes.
- [Safe Control Plane v1 plan](SAFE_CONTROL_PLANE_V1_PLAN.md) records the implemented
  NEXT-3 software contract and its still-pending Rafa mapping, OTA and elevated
  loss-path evidence. It does not authorize motion or weaken the Maintenance-LAN
  boundary.

## Physical state

The firmware is bench-only. Rafa is **not ready for floor motion**; its current
installed-hardware evidence, unresolved M2 feedback anomaly, and OTA-only workflow
are in [Rafa bench state](robots/RAFA_BENCH_STATE.md). A successful build, command
ACK, or controller feedback does not prove physical motion or safety.

## Archive

[Archive index](archive/README.md) preserves superseded plans, migration records and
iteration closeouts. They provide traceability, but are not active contracts and
should not be read before the documents above unless a historical decision is needed.
