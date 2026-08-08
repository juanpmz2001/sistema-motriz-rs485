# Physical test runbooks

> **Runbook version:** 1.0
> **Status:** Current operational index from `bench-baseline-v1`. The steering
> development preparation below is not a physical qualification result.

This file selects the next physical gate. It does not redefine test levels, evidence
classes or lifecycle rules; those are owned by the
[Testing Architecture Guide](../TESTING_ARCHITECTURE_GUIDE.md). The
[Field-Ready Iteration Roadmap](../FIELD_READY_ITERATION_ROADMAP.md) owns milestone
order, and [Safety](../SAFETY.md) remains the hard constraint.

These runbooks do not authorize a hardware operation. Motion, enable, fault clear,
identify and register/configuration writes require an explicit task request and
confirmation that the named setup is safe. A successful build or read-only check is
not that authorization.

## Universal session gate

Before any command that can change an output, all of the following must be recorded
in a fresh copy of the [evidence template](EVIDENCE_TEMPLATE.md):

- exact repository SHA and dirty state, flashed artifact provenance/hash, firmware
  version and ESP-IDF version; an unknown binary blocks motion;
- exact serial port, board profile, active robot profile, PCB revision and fixture or
  robot identity;
- expected endpoints, capabilities and required/optional hardware, with composition
  runtime-ready;
- accessible person-operated power cut-off, safe supply/current limits, unloaded or
  restrained mechanism and a clear motion envelope;
- read-only `PING`, `VERSION`, `PROFILE_STATUS`, `COMPOSITION_STATUS`,
  `PLATFORM_STATUS` and `SAFETY_STATUS` baseline, plus domain telemetry needed by the
  selected runbook;
- maximum command, maximum duration, acceptance bounds and immediate abort triggers;
  and
- a successful application-level `STOP ALL` before motion.

The current Iteration A serial identity reports the full Git SHA, dirty state, board
and profile. An older binary or any `UNAVAILABLE`/malformed identity field blocks the
included runner; do not infer a SHA or substitute an unreviewed artifact claim. If a
binary cannot be tied to the intended build with reviewable evidence, stop before
actuation.

The runner also requires the operator-supplied SHA-256 of the firmware artifact and
records its external reference. This provides traceability, not on-target
attestation: retain the flash record that ties that artifact to the connected board,
because a clean Git tree does not capture ignored `sdkconfig` or every build input.

The current executable runner is intentionally limited to L4 generic velocity with
E2 controller feedback. L5+ and E3/E4 require different actuation or independent
observation paths; a manifest label alone cannot claim them.

The repository now also contains a prepared motor-mode steering/AS5600 path, but no
steering HIL manifest or physical execution. Its narrow operator procedure is
[AS5600 motor-mode steering bench runbook](STEERING_AS5600_BENCH_RUNBOOK.md); it
supplements this index and the testing guide rather than authorizing motion.

Every motion-capable executor must attempt application-level `STOP ALL` on success,
failure, timeout, interruption and exception, then record its response and verify
that observed motion decreases to the declared stopped threshold. If software stop
cannot be issued or the stopped state cannot be verified, use the physical cut-off
and classify the run `ABORTED_FOR_SAFETY`, never `PASS`. Reserve `INCONCLUSIVE` for
insufficient claim evidence after cleanup and physical containment are verified safe.
A software stop is not an emergency-stop substitute.

Follow the guide's complete
[mandatory lifecycle](../TESTING_ARCHITECTURE_GUIDE.md#5-mandatory-physical-test-lifecycle)
for every run. Store raw captures outside the repository and identify them by URI and
checksum in the evidence record.

## PCB commissioning — L2

Entry gate:

- PCB, assembly/BOM and wiring revisions are known;
- unpowered polarity, continuity, shorts, termination, grounding and cut-off wiring
  have been inspected; and
- actuators are disabled or mechanically incapable of motion during bring-up.

Run the guide's [commissioning Stages A through
E](../TESTING_ARCHITECTURE_GUIDE.md#6-new-pcb-commissioning): current-limited power
bring-up, rail/idle-current/thermal checks, firmware and profile identity, one bus at
a time, then read-only device observations. Stage F is a separate actuation decision
and is not implied by an L2 pass.

Exit gate:

- no unexpected motion, reset, overheating or unexplained current draw occurred;
- supply rails and brownout/reset behavior are recorded;
- build/profile/board provenance and expected bus/device inventory agree;
- read-only communication is repeatable and error/timeout counts are characterized;
  and
- every missing, stale or faulted required device is resolved or the result is not a
  pass.

## Single traction motor — L3 to L5

Entry gate:

- the PCB L2 gate passed;
- the selected development profile declares exactly the connected traction endpoint
  and composition is runtime-ready;
- the motor/wheel is unloaded, direction is safe in both signs and conservative
  controller limits are active; and
- the stop path passed immediately before each motion step.

Progress in separate claims: qualify controller/channel mapping and stop at L3; then
command the endpoint through the application/capability path at L4, beginning with
the smallest useful positive value and duration, stop, and only then repeat for
larger and negative values. Compare fresh controller-derived velocity as E2 and add
an independent encoder as E3 when available. Direct register access may diagnose L3
but invalidates an L4/L5 claim.

Exit gate:

- endpoint mapping, sign and public RPM unit are known;
- commanded bounds, fresh observed response, current and fault state are recorded;
- stop succeeds after every step and observed velocity reaches the declared stopped
  threshold within its bound;
- stale/offline or incomplete feedback cannot be reported as physical success; and
- any current numeric-index compatibility is recorded as transitional and is not
  embedded in a reusable L4 specification.

## Servo / steering actuator — L3 to L5

Entry gate:

- a profile exposes only the intended position actuator and honest limits;
- the linkage is isolated or restrained across its full permitted range; and
- the evidence plan states whether an independent position observation exists.

At L3, qualify pulse/output range, direction, limits and disable/stop behavior. At
L4, command position only through the position capability. At L5, compare it with a
separate position observation and test sign, settling, repeatability and limit
behavior using minimum travel first.

Exit gate:

- command generation alone is reported only as E0/E1 and never as reached angle;
- a physical position claim includes fresh measured units and observation source;
- limit and cleanup behavior are bounded; and
- no generic position test contains PWM pins or driver-specific details.

The source now contains the development-only
`bench_single_steering_as5600` profile with separate motor-mode PWM, AS5600 and
controller devices, a generic position actuator endpoint and a separate position
observation endpoint. It is software preparation only: no L2–L5 steering result has
been recorded. Use the [AS5600 motor-mode steering bench runbook](STEERING_AS5600_BENCH_RUNBOOK.md)
to prepare the named fixture; do not elevate an accepted PWM command, a controller
estimate or a LUT report into a physical-angle claim.

## Sensor qualification — L3/L4

Entry gate:

- physical quantity, public unit, range, update rate, criticality and calibration or
  reference instrument are declared; and
- the sensor is reachable read-only without changing an actuator state.

At L3, qualify transport/device conversion, sign, scale, wrap and error behavior. At
L4, read through its typed observation capability and exercise timestamp, validity,
freshness, stale and offline semantics. Compare against an independent reference
when claiming measurement accuracy.

For the AS5600 steering fixture, a valid generic position observation additionally
requires an approved profile LUT **and** an explicit logical reference, and reports
both calibration/reference provenance. The offline 7+7 analysis tool can prepare the
LUT candidate; it is not a measurement-accuracy result or a mechanical-reference
procedure. See the steering runbook for fixture/provenance and evidence requirements.

Exit gate:

- valid range, unit, sign, scale, update rate and repeatability are evidenced;
- stale/offline transitions are distinguishable from valid zero;
- failure of required versus optional hardware has the documented effect; and
- higher-level test logic contains no bus address, register, pin or concrete sensor
  type.

## Closed-loop subsystem — L5

Entry gate:

- actuator endpoint A and observation endpoint B are declared independently;
- each passed its lower-level qualification;
- timestamp/freshness and stopped thresholds are defined; and
- the universal stop/cleanup gate passes.

Apply short bounded setpoints through the actuator capability while sampling the
observation capability. Record command, measured value, source, timestamps, health,
settling, error, saturation and cleanup. Controller-derived feedback is E2; a
separate calibrated sensing path is E3 and supports the stronger physical claim.

Exit gate:

- minimum sample count, tolerance, freshness and settling bounds pass;
- actuator acceptance is not substituted for observation;
- stop is observed within the declared bound; and
- the reusable test does not depend on either endpoint's concrete driver.

Until a suitable application observation boundary exists, a driver-specific
workaround may produce an explicitly limited L3/E2 result, but it cannot close the
generic L5 gate.

## Mobility — L6

Mobility is blocked until the field-ready roadmap's motion-service and minimum-safe-
authority prerequisites are implemented and tested. In particular, the system must
have a body-motion application boundary, one actuation owner, stop precedence,
command TTL/deadman, an explicit active source and operating state, required-health
gating, and no reachable hidden legacy writer. The current legacy `MOVE_VEL` path
does not close this gate.

After every traction, steering and observation endpoint passes its lower-level gate,
progress through the roadmap's unloaded, restrained low-energy and short free-motion
stages. Enter through body motion, never per-wheel commands, and collect E4 evidence
for intended displacement/yaw plus measured stop response.

Exit gate:

- application body commands produce the intended system-level direction and scale;
- command expiry, source loss and explicit stop are physically observed;
- endpoint health and independent physical observations remain within bounds; and
- a supervised short motion stops reliably without bypassing the application path.

## Result gate

Use only `PASS`, `FAIL`, `INCONCLUSIVE` or `ABORTED_FOR_SAFETY`, with the meanings in
the testing guide. A pass requires every selected runbook exit condition, successful
cleanup and enough evidence for the exact claim. Always state what was not verified;
lower-level evidence never implies a higher-level gate passed.
