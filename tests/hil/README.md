# Host HIL runner

This executable guide inherits the repository [safety contract](../../docs/SAFETY.md),
[testing architecture](../../docs/TESTING_ARCHITECTURE_GUIDE.md), [physical
runbooks](../../docs/testing/README.md), [evidence
template](../../docs/testing/EVIDENCE_TEMPLATE.md) and this subtree's
[`AGENTS.md`](AGENTS.md). Running a command shown here does not itself authorize a
physical test.

`tools/hil_runner.py` validates and executes capability-oriented JSON manifests over
one persistent gateway connection. Validation uses only the Python standard library;
`pyserial` is imported lazily only by `identify` and `run` when a real port is opened.

Runner schema v1 accepts exactly an L4 velocity-capability claim with E2
controller-derived observation. It cannot obtain L5/L6/L7 or E3/E4 evidence merely
by relabelling a manifest. The included manifest targets exactly one available
endpoint that advertises the required capabilities. It deliberately contains no
controller, bus, register, or channel-position assumptions.

## Commands

Validation performs no serial I/O:

```bash
python3 tools/hil_runner.py validate \
  tests/hil/specs/capabilities/single_endpoint_velocity_l4.json
```

Identify the connected firmware before selecting expectations for a run:

```bash
python3 tools/hil_runner.py identify --port /dev/ttyACM0
```

A motion run requires exact build/profile gates, a clean build, all three explicit
operator confirmations, and an evidence path outside this checkout:

```bash
python3 tools/hil_runner.py run \
  tests/hil/specs/capabilities/single_endpoint_velocity_l4.json \
  --port /dev/ttyACM0 \
  --expect-git-sha <full-git-sha> \
  --expect-git-dirty 0 \
  --expect-profile bench_single_svd48_motor \
  --expect-board botfarms_esp32s3_rev1 \
  --hardware-id <fixture-or-robot-id> \
  --pcb-revision <pcb-revision> \
  --firmware-artifact-sha256 <64-hex-sha256> \
  --firmware-artifact-uri <ci-artifact-or-storage-reference> \
  --set test_rpm=1 \
  --authorize-motion \
  --confirm-unloaded \
  --confirm-cutoff \
  --evidence /absolute/path/outside/repository/hil-evidence.json
```

The runner reads `VERSION`, `PROFILE_STATUS`, and `ENDPOINTS`, enforces the expected
Git SHA, dirty state, profile, board, one-item endpoint inventory, development
criticality, exact logical endpoint, and capabilities, then
sends `STOP ALL` before manifest steps. The L4 spec limits the request to -5 through
5 RPM, uses 1 RPM in this example, bounds total duration, and evaluates RPM
observations with an explicit tolerance before it can report `PASS`. It also checks a
runtime-ready composition, safe-idle platform state, running safety task with no
active RC-loss or motor-fault condition, and a fresh, valid, online, healthy stopped
sample before motion. It observes the requested speed,
stops the endpoint and verifies zero speed. The checked profile and target are the
existing `bench_single_svd48_motor` / endpoint `1` (`bench_motor`) composition.
Both the velocity and endpoint-stop acknowledgements must exactly repeat the target
endpoint; the velocity acknowledgement must also repeat the requested RPM.

After any attempt that may have sent motion, the runner attempts `STOP ALL` again,
waits the manifest cleanup interval and requires a second fresh, online, healthy zero-
speed observation. Failure to acknowledge or verify that final cleanup produces
`ABORTED_FOR_SAFETY`. It then closes the connection after success, command errors,
timeouts, `KeyboardInterrupt`, and handled termination signals. Evidence records the
manifest digest, hardware/PCB identity, port and baud rate, build/profile gates,
operator-supplied firmware artifact SHA-256/reference, observations, commands,
assertions, cleanup errors, and one of `PASS`, `FAIL`,
`INCONCLUSIVE`, or `ABORTED_FOR_SAFETY` as JSON.

The evidence path must not already exist; the runner atomically creates a new record
instead of overwriting prior evidence. Durations, tolerances and command timeouts
must be finite and remain within the runner's bounds. A non-`PASS` result never
prints `OK` or returns a successful CLI status.

Before opening the serial port, the runner creates and fsyncs a sibling
`.NAME.json.reserved` marker, probes same-directory hard-link publication and fsync,
and refuses a pre-existing final path or reservation. Final evidence is published
with a no-clobber hard link and the reservation is removed only after the final entry
is durable. A publication error retains a `FINALIZE_FAILED` marker; an uncatchable
process/host failure may retain `RESERVED`. Either marker means the session has no
valid result and requires operator containment and reconciliation; neither may be
reinterpreted as `PASS`.

The included observation is controller-derived E2 evidence. It cannot establish
independent motor motion, current, thermal behavior, stop latency or any chassis
claim; those gates remain physical and unpassed.

The artifact hash makes the record traceable, but the firmware does not currently
attest its own running image. The operator must hash and flash the named artifact and
retain that external record; Git SHA/profile/board alone do not uniquely identify an
ignored `sdkconfig` or every build input.

The final stop is best effort, not a safety guarantee. `SIGKILL`, host power loss,
cable loss, target failure, or a blocked transport can prevent it. Wheels must remain
unloaded and a working physical power cut-off must stay within reach for every motion
run.

Run the fake-only tests with:

```bash
python3 -m unittest tests.hil.test_hil_runner
```
