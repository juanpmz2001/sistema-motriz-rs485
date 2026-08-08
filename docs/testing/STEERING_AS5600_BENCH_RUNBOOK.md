# AS5600 motor-mode steering bench runbook

> **Status:** Prepared operator procedure only. No physical session, calibration
> capture, steering motion or qualification result is recorded by this document.

This runbook narrows the general [physical-test lifecycle](../TESTING_ARCHITECTURE_GUIDE.md#5-mandatory-physical-test-lifecycle)
for the one-axis `bench_single_steering_as5600` development profile. It does not
authorize hardware actuation; an operator must explicitly request and confirm the
named safe setup before any motion-capable step.

## Scope and evidence boundary

The profile composes three separate devices:

| Role | Development-profile item | Public endpoint |
| --- | --- | --- |
| Bounded output | Motor-mode PWM | None directly |
| Position sensor | AS5600 on I2C | ID 2, `bench_steering_position_feedback` (`POSITION_OBSERVATION`) |
| Closed-loop policy | Steering position controller | ID 1, `bench_steering_position` (`POSITION`, `POSITION_REFERENCE`, `STOPPABLE`) |

PWM in this configuration means direction/speed, not a servo angle. The controller
is its only normal PWM writer; tests above L3 must use endpoints rather than GPIO,
I2C, AS5600 registers or pulse widths.

The historical `origin/ensayo-nueva-pata` experiment is empirical design input for
the provisional calibration and control parameters. It is **not** evidence that the
current build, profile, wiring or fixture passed a test. In particular, source-level
host tests, a LUT candidate and an accepted position request are not physical PASS
evidence.

## Entry gate

Before read-only power-up, attach a fresh copy of the
[evidence template](EVIDENCE_TEMPLATE.md) and record:

- repository SHA, dirty state, build artifact hash, ESP-IDF version, selected board
  and active `bench_single_steering_as5600` profile;
- exact ESP32-S3, PCB revision, Dockit motor-mode servo, AS5600, magnet, shaft,
  linkage and fixture identities; the LUT is invalid for a changed sensor, magnet
  alignment/gap, shaft or steering geometry until reviewed again;
- known supply limits, polarity/grounding inspection, mechanical travel envelope and
  an accessible person-operated power cut-off;
- unloaded or restrained linkage, no person in the envelope, and a current/thermal
  abort threshold; and
- read-only identity/composition/endpoint information and the initial AS5600 health,
  magnet status, timestamp and calibration state.

Do not proceed to a motion-capable step if composition is not runtime-ready, the
endpoint list differs from the table above, calibration is absent/unapproved, the
sensor is stale/offline/degraded unexpectedly, the mechanism is not contained, or
the physical cut-off is unavailable. The only profile-scoped degraded exception is
the known `ML:1` warning while `MD:1`, `MH:0` and the latest primary poll are healthy;
it is development-only and must be recorded. Missing `MD`, `MH:1`, a partial
diagnostic result or an invalid primary poll are NO-GO conditions.

## Explicit mechanical reference — no automatic home

An AS5600 cyclic phase is not a steering zero. The controller starts `UNHOMED` and
holds neutral. A normal position request must not establish a reference implicitly.

After an explicitly authorized, stopped and contained session has placed the linkage
at a physically verified pose within the configured range, the full serial interface
may use:

```text
SET_ENDPOINT_POSITION_REFERENCE 1 <verified-degrees> CONFIRM
```

This maintenance operation stops the endpoint before mapping the **current fresh
sensor sample** to the stated logical coordinate. It does not drive the actuator,
seek a stop, infer zero or prove the physical pose. Record the physical reference
method, the independent instrument/mark used, the endpoint-2 observation and the
command/result. A subsequent generic position observation is valid only when both
`CALIBRATED:1` and `REFERENCED:1`; the latter says that this logical mapping exists,
not that the operator selected the mechanically correct pose. This operation is
unavailable through LAN-safe and restricted-diagnostic modes, and it cannot clear or
re-arm a latched steering fault. Treat a latched fault as a NO-GO until a separately
reviewed fault-recovery policy exists.

If the verified pose, calibration identity or sensor freshness is uncertain, do not
set reference and do not command position. Resolve the setup as an L2/L3 issue.

## Offline 7+7 linearity candidate

The repository tool is deliberately host-only:

```bash
python3 tools/analyze_as5600_linearity.py capture.csv --out candidate.json \
  --calibration-id <reviewed-id> --hardware-identity <fixture-id>
python3 tools/analyze_as5600_linearity.py --validate-candidate-report candidate.json
python3 tools/analyze_as5600_linearity.py validation.csv \
  --cross-validate-candidate candidate.json --out validation.json
```

It expects one CSV with `time_ms`, `raw_angle`, `unwrapped_deg`, `i2c_valid`,
`valid`, `glitch`, and `direction` or `phase`. A motion-capable capture is a
separate approved maintenance session; this analyzer neither connects to hardware
nor sends a command. Keep raw captures outside the repository.

The input must contain positive and negative passes with at least seven complete
turns each. The analyzer rejects incomplete capture, meaningful unflagged reversal,
poor repeatability, incoherent bidirectional curves, invalid raw data, speed variation
by turn above its declared limit and a LUT that is not strictly monotonic across all
4096 AS5600 codes. Its candidate report contains the input SHA-256, pass/filter
counts, quality values, 128 signed-centidegree cyclic corrections and a LUT SHA-256;
it intentionally does not retain raw rows. Cross-validation applies a fixed candidate
to a distinct capture and reports residuals; it never refits or replaces the LUT.

The result is a reviewable **linearity candidate**, not a measurement of mechanical
zero, absolute steering angle, linkage backlash or closed-loop performance. Before a
candidate becomes a profile LUT, review the fixture identity and hashes, preserve the
external evidence location, inspect quality/residuals, update the static profile in a
separate reviewed change, and rebuild/re-identify the artifact. Never substitute a
runtime-generated file for profile provenance.

The time-to-angle method assumes sufficiently stable rotation. Per-turn repeatability
or speed checks cannot distinguish every intra-turn velocity ripple from AS5600/magnet
nonlinearity. Treat a candidate as provisional until an L3 reference instrument
checks it on the named hardware.

### Historical candidate limit

The static development-profile LUT is byte-identical to the historical
`origin/ensayo-nueva-pata` candidate. Its separate historical validation capture
reported a combined post-correction RMSE of **3.597°**, P95 absolute residual of
**7.925°**, and maximum absolute residual of **15.924°** (versus 8.171° pre-correction
RMSE). Those are not current-fixture evidence, and that validation still inferred its
reference from motion at assumed constant speed. In particular, the controller's
`[0°, +3°]` arrival band is a local neutral/drive policy, **not** a 3° angular-accuracy
claim or an L4/L5 acceptance tolerance.

Do not declare a position tolerance from this LUT. First run L3 against a suitable
independent angular reference, document repeatability and direction effects, and then
choose any L4/L5 tolerance from that physical evidence. The 7+7 session itself is a
separate controlled maintenance capture; this source tree supplies offline analysis
only, not a direct-PWM or automatic multi-turn calibration command.

## Staged operator evidence

Perform only the next authorized stage, stop on an unexpected result, and record the
selected test level/evidence class rather than combining claims.

| Stage | Claim and route | Required observation / result |
| --- | --- | --- |
| L2 | Powered, read-only I2C/AS5600 communication | Device identity, magnetic status, raw-phase freshness and timeout behavior; no motion. Use the device-scoped diagnostics command when it is present; never poll/register-write from a generic endpoint test. |
| L3 sensor | AS5600 conversion and approved-LUT behavior | Sign, wrap, scale, direction effects and calibration provenance against a suitable independent angular reference; raw phase without an approved LUT and explicit logical reference is not valid generic logical feedback |
| L3 actuator | Bounded controller-owned motor-mode output | Direction, neutral, limits and output-failure response while contained; a PWM result is not angle evidence |
| L4 | `SET_ENDPOINT_POSITION` through endpoint 1 after explicit reference | Bounded request, finite 650 ms lease, stop/TTL result and generic endpoint contract; no direct PWM/I2C dependency |
| L5 | Endpoint 1 actuator plus endpoint 2 position observation | Timestamped error, settling, repeatability, limits and physical stop evidence; state exactly what is independent of the PWM path and what was externally checked |

For every motion-capable stage, issue and record the normal `STOP ALL` before the
first command and after each attempt. The current controller design neutralizes after
120 ms without a fresh sensor sample, latches a sensor-timeout fault at 400 ms, and
neutralizes on TTL expiry, explicit stop, output failure or move timeout. A one-shot
AS5600 diagnostic failure is a latched NO-GO condition; it has no automatic recovery.
PWM construction/deinit source behavior is neutral-duty attach followed by
neutral/LEDC-stop/GPIO-release on teardown, but its electrical waveform remains an L3
observation to capture. Treat all of these as software requests until the fixture
demonstrates the observed physical response; they are not an emergency-stop substitute.

## Result and next gate

Attach command/observation timestamps, endpoint IDs,
source/calibrated/referenced/health/stale fields, current/voltage where available,
reference method, operator decisions, cleanup result and external capture hashes to
the evidence record. Classify an uncertain sensor/reference/cleanup result as `INCONCLUSIVE` or
`ABORTED_FOR_SAFETY`, never `PASS`.

This runbook prepares Iteration D software and bench evidence only. The canonical
first physically incomplete iteration remains Iteration C traction qualification;
the steering L2–L5 gates and all closed-loop/chassis gates remain blocked by hardware
until real operator evidence is reviewed.
