# Agent guide

This file is the operating contract for coding agents working in this repository.

## Read and route

Start with `README.md`, then read only the routes the task actually touches. For any
code change, also read `docs/ARCHITECTURE.md` and `docs/SAFETY.md` before editing.

| Task scope | Additional mandatory reading |
| --- | --- |
| Serial/LAN commands or public responses | `docs/API.md` |
| SVD48, RS485, polling or drive units | `docs/SVD48.md` |
| OTA, release or recovery | `docs/OTA.md` |
| Physical tests, HIL or test architecture | `docs/TESTING_ARCHITECTURE_GUIDE.md`, `docs/testing/README.md`, `tests/hil/README.md` and `docs/SAFETY.md` |
| PCB, actuator, sensor, closed-loop or mobility qualification | the preceding physical-test route plus `docs/FIELD_READY_ITERATION_ROADMAP.md` |
| Evidence from a physical session | `docs/testing/EVIDENCE_TEMPLATE.md` |
| Work sequencing toward the first field test | `docs/FIELD_READY_ITERATION_ROADMAP.md` |
| Long-horizon platform, ROS or product qualification planning | `docs/ROADMAP.md`; also read the field-ready roadmap if the decision affects a pre-field milestone |

Do not load every domain document when the task has no dependency on it. When a task
spans routes, combine their reading lists.

Precedence is explicit:

1. source code and executable tests are the implementation truth;
2. current as-built and safety contracts describe that implementation;
3. `docs/TESTING_ARCHITECTURE_GUIDE.md` owns test levels, evidence classes and the
   physical-test lifecycle;
4. `docs/FIELD_READY_ITERATION_ROADMAP.md` owns sequencing from the bench baseline to
   the first field-testable version; and
5. `docs/ROADMAP.md` owns the broader, long-horizon platform sequence.

If the two roadmaps overlap and disagree on pre-field ordering, the field-ready
roadmap wins. No roadmap overrides `docs/SAFETY.md` or proves that planned behavior is
implemented. Correct documentation in the same change whenever behavior or a public
contract changes.

## Current constraints

- The active topology comes from a build-selected immutable C profile. There is no
  JSON/YAML loader or general runtime factory for every declared driver yet.
- The executable factory registry currently supports SVD48 only. It composes both
  the two-controller `current_robot` profile and the one-controller/one-endpoint
  `bench_single_svd48_motor` profile; other driver descriptors are schema fixtures.
- One `svd48_device` represents each physical controller and exposes explicit M1/M2
  channels over a shared serialized RS485 transport and N-device polling service.
- `actuation_coordinator` serializes migrated speed and stop paths with a mutex, but
  several active commands still write through the legacy `robot_control` facade.
- The SVD48 compatibility wrapper is transitional and supports at most four channel
  bindings. `svd48_device` has no logical-motor limit; the poll service has its own
  independent static capacity of four physical devices.
- A schema-valid composition-unsupported startup exposes only the restricted serial
  diagnostic gateway; it must not construct outputs or accept actuation/register-write
  commands. `STOP ALL` must report outputs unavailable when no endpoints exist. A
  pending-verification OTA image takes the startup-failure rollback path instead.
- `robot_state`, `command_authority`, `robot_kinematics` and `control_lan` are not
  part of the active runtime despite being compiled.
- The firmware is bench-only until every release gate in `docs/SAFETY.md` passes.
- Never infer that a servo reached a requested angle without independent feedback.

## Safety rules

- Never run motion, enable, fault-clear, identify or register-write commands unless
  the user explicitly requests a hardware test and confirms the test setup is safe.
- Before hardware actuation, identify the serial port, verify the target and build,
  read safety status, keep wheels unloaded and preserve a physical power cut-off.
- A transport handler must not call a motor driver directly in new code. Commands
  must flow through one authority/coordinator boundary with timeout and stop rules.
- Missing configured-required hardware must inhibit affected capabilities. Hardware
  omitted from a development profile must not create a fault merely because it is
  absent.
- Wi-Fi, JSON, HTTP, OTA, NVS and maintenance work must stay out of high-priority
  control and safety tasks.
- Do not weaken fail-safe behavior to make a bench test pass.

## Secrets and local state

- Never print, commit or copy values from `.env`, NVS tokens, Wi-Fi passwords, SSH
  keys or other credentials.
- `.env.example` documents variable names only.
- Do not commit `build/`, `sdkconfig`, `ota_release/`, captures, generated telemetry,
  caches or editor state.
- Store durable test evidence outside the source repository or in an artifact
  system; summarize only decisions and reproducible procedures here.

## Verification

For hardware-independent changes, run:

```bash
tools/run_host_tests.sh
BOTFARMS_HOST_TEST_SANITIZERS=ON tools/run_host_tests.sh
python3 tools/test_svd48_protocol.py
python3 tools/test_dependency_contracts.py
python3 tools/test_application_compatibility.py
```

For firmware changes, additionally run with ESP-IDF 5.4.1:

```bash
idf.py set-target esp32s3
idf.py build
```

Flashing, OTA, network access and physical tests require explicit task relevance.
Report what was and was not verified. A successful compile is not a safety test.

## Change discipline

- Keep `main` as a composition root; move behavior into components.
- Prefer explicit ports, immutable command messages and pure host-testable models.
- Add a driver through a stable interface and static registry, not conditionals
  spread through control code.
- Validate configuration before starting tasks or touching outputs.
- Preserve serial API compatibility unless the change explicitly versions it.
- Use ESP-IDF facilities and existing repository patterns before adding dependencies.
- Keep one indexed source for each current contract. Clearly label target and
  migration documents; archive a superseded whole document only when traceability
  requires it instead of creating overlapping plans.
