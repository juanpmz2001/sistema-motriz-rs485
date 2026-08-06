# Agent guide

This file is the operating contract for coding agents working in this repository.

## Required reading

Read these files in order before changing code:

1. `README.md`
2. `docs/ARCHITECTURE.md`
3. `docs/SAFETY.md`
4. The domain document relevant to the change: `docs/API.md`, `docs/OTA.md` or
   `docs/SVD48.md`
5. `docs/ROADMAP.md` for planned boundaries and sequencing

Treat source code and executable tests as truth when prose disagrees. Correct the
documentation in the same change whenever behavior or a public contract changes.

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
