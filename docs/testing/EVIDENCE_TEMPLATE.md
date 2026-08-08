# Physical test evidence template

> **Template version:** 1.0
> **Testing contract:** [Testing Architecture Guide version
> 1.0](../TESTING_ARCHITECTURE_GUIDE.md).

Copy this template into the external evidence system for one bounded session. Do not
commit filled records, raw captures, credentials or device secrets to this repository.
Use `UNAVAILABLE` or `NOT_APPLICABLE` instead of guessing a value.

## Record identity

- Record ID:
- Test/specification ID and version:
- Test/specification SHA-256:
- Date/time and timezone:
- Operator and safety observer:
- Declared level (L0–L7):
- Required evidence class (E0–E4):
- Runbook gate:

## Source and build identity

- Repository commit SHA:
- Worktree dirty (`yes`/`no`):
- Firmware artifact URI and SHA-256:
- Firmware-reported `VERSION` response:
- Firmware-reported Git SHA/dirty state, if available:
- ESP-IDF version:
- Expected build profile:
- Reported `PROFILE_STATUS`:
- Reported `COMPOSITION_STATUS`:
- Reported board profile, if available:
- Identity/provenance match decision (`PASS`/`BLOCK`):

If firmware does not report its Git SHA, document how the flashed artifact was built
and tied to the repository SHA. Unknown or mismatched provenance blocks motion.

## Hardware identity

- Exact serial port/network endpoint:
- PCB and assembly/BOM revision:
- Fixture or robot ID:
- Power supply and current limit:
- Actuator/controller IDs and firmware:
- Sensor/reference instrument IDs and calibration revision:
- Connected endpoints/capabilities:
- Intentionally omitted hardware:

## Authorization and physical preconditions

- Explicit hardware-task authorization reference:
- Setup confirmed safe by:
- Physical power cut-off present/tested:
- Unloaded/restrained condition:
- Motion envelope clear:
- Electrical levels, grounding and termination checked:
- Conservative device limits confirmed:
- Maximum command and duration:
- Abort thresholds/triggers:
- Preconditions decision (`PASS`/`BLOCK`):

## Read-only baseline

Record exact responses or artifact references for:

- `PING` / `VERSION`:
- `PLATFORM_STATUS`:
- `PROFILE_STATUS` / `COMPOSITION_STATUS`:
- `SAFETY_STATUS`:
- Endpoint health, fault, freshness and observation baseline:
- Voltage/current/temperature baseline:
- Communication statistics:
- Unexpected conditions and disposition:

## Initial stop

- `STOP ALL` attempted at:
- Response/acknowledgement:
- Fresh stopped observation and threshold:
- Initial-stop gate (`PASS`/`BLOCK`):

## Bounded execution

| Timestamp | Step/intent | Public boundary and target capability | Command/bound | Response | Observation/evidence | Decision |
| --- | --- | --- | --- | --- | --- | --- |
|  |  |  |  |  |  |  |

- Motion may have started at:
- Timeout/interruption/exception, if any:
- Artifact URI(s) and SHA-256:

## Acceptance evidence

- Exact claim being evaluated:
- Observation endpoint/source:
- Independent from actuator (`yes`/`no`):
- Unit and sign convention:
- Timestamp/freshness bound:
- Sample count and interval:
- Tolerance/settling/stopped thresholds:
- Measured summary:
- Acceptance decision and reason:

## Mandatory cleanup and final state

- Cleanup trigger (`success`/`failure`/`timeout`/`interrupt`/`exception`):
- Final `STOP ALL` attempted at:
- Response/acknowledgement:
- Observed motion decreased/stopped:
- Time to stopped threshold:
- Final endpoint health/fault/freshness:
- Final `PLATFORM_STATUS` / `SAFETY_STATUS`:
- Physical cut-off used (`yes`/`no`, reason):
- Cleanup gate (`PASS`/`FAIL`):

A failed or unverifiable cleanup gate requires `ABORTED_FOR_SAFETY` and forbids
`PASS`. Use `INCONCLUSIVE` only when cleanup and physical containment are verified
safe but the evidence cannot decide the declared claim. Preserve the failure or
missing-evidence record in either case.

## Result and verification boundary

- Result (`PASS`/`FAIL`/`INCONCLUSIVE`/`ABORTED_FOR_SAFETY`):
- Evidence class actually achieved:
- Exit conditions satisfied:
- What was verified:
- What was not verified:
- Deviations from the specification/runbook:
- Follow-up issue/decision links:
- Operator sign-off:
- Reviewer sign-off:
