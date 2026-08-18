# Agent guide

This file is the operating contract for coding agents working in this repository.

## Mandatory shared philosophy

Read [the BotFarms Engineering Master Guide](docs/BOTFARMS_ENGINEERING_MASTER_GUIDE.md)
before planning or implementing **every** task. It is the shared design philosophy for
this firmware and the Engineering Console: choose the smallest correct architecture,
preserve clear ownership, keep dependencies layered, and distinguish maintenance from
continuous control. It is not an as-built source of truth.

Apply it in practice:

1. Inspect current source, tests and runtime evidence before trusting a prompt or a
   plan.
2. Identify the owner and public boundary of the behavior before editing. Keep the
   dependency direction `UI → service/adapter → firmware capability → device →
   driver → transport`.
3. Put robot intent in an application/service layer, device protocol in the driver
   vertical, and transport framing in transport. Do not make a shortcut around the
   correct owner merely to finish a task faster.
4. Generalize contracts and identity when a real use needs them; do not introduce
   unused framework capacity, profile-specific UI assumptions, or broad refactors.
5. Test at the layer being changed, update the current contract, and label software,
   controller and physical evidence honestly.

The firmware remains the final safety authority. Maintenance LAN is bounded bench
tooling, not continuous motion control; any continuous source needs the dedicated
authority, session, sequence, TTL/deadman and STOP-priority path described in the
Master Guide.

## Documentation selection

Start with `README.md` and [the documentation index](docs/README.md). Then use this
catalog to decide which focused document is worth reading. Read every matching row;
do not load unrelated domain material.

| Document | Read it when… | It tells you… |
| --- | --- | --- |
| `docs/BOTFARMS_ENGINEERING_MASTER_GUIDE.md` | always, before design or implementation | Shared layering, ownership, evidence, control-plane and cross-repository philosophy. |
| `README.md` | always | Current product boundary, supported profiles, setup and known blockers. |
| `docs/README.md` | always | The active documentation map and where historical material lives. |
| `docs/ARCHITECTURE.md` | changing code, startup, lifecycle, composition, tasks or ownership | As-built component graph, active/transitional/dormant boundaries and data/actuation paths. |
| `docs/SAFETY.md` | changing any code; mandatory for hardware, commands, safety, endpoints or OTA | Current safeguards, bypasses, release gates and non-negotiable invariants. |
| `docs/API.md` | changing serial/LAN commands, responses or a firmware client | Public grammar, compatibility behavior, diagnostic mode and maintenance/control boundaries. |
| `docs/SVD48.md` | changing SVD48, RS485, polling, units, registers or device/channel adapters | Driver/transport contract, register evidence and legacy compatibility limits. |
| `docs/OTA.md` | release, recovery, update or OTA work | Trusted release, rollback and recovery procedure. |
| `docs/RAFA_BOOTSTRAP.md` | Rafa provisioning or recovery | One-time USB-to-OTA handoff; normal Rafa changes are OTA-only. |
| `docs/robots/RAFA_BENCH_STATE.md` | deciding Rafa behavior, evidence or a bench session | Actual installed-hardware observations, unresolved facts and allowed conclusions. |
| `docs/TESTING_ARCHITECTURE_GUIDE.md` | adding tests, HIL or any physical qualification | L0–L7 levels, evidence classes and which layer a test may exercise. |
| `docs/testing/README.md` | preparing a physical session | Concise entry/exit gates and the appropriate runbook. |
| `docs/testing/STEERING_AS5600_BENCH_RUNBOOK.md` | AS5600 or steering bench work | The isolated steering fixture, explicit-reference procedure and evidence limits. |
| `docs/testing/EVIDENCE_TEMPLATE.md` | recording physical evidence | Required identity, preconditions, observations, cleanup and result format. |
| `tests/hil/README.md` | using or changing the host HIL runner | Executable manifest workflow, identity gates and explicit motion confirmations. |
| `docs/NEXT_STEPS.md` | choosing a new firmware scope or evaluating open gates | Compact as-built handoff; it does not authorize a feature or physical operation. |
| `docs/SVD48_WORKSPACE_V2_PLAN.md` | implementing generic controller/channel inventory, SVD48 UI contracts or bench controls | Future multi-controller workspace scope, typed catalog shape and acceptance criteria. |
| `docs/SAFE_CONTROL_PLANE_V1_PLAN.md` | implementing `/control`, LAN motion or host controls | Future control-plane scope: authority, TTL/deadman, mixer and elevated-test criteria. |
| `docs/archive/README.md` and `docs/archive/` | tracing a historical decision | Superseded plans and closeouts; never an active contract or evidence source. |

Plans describe intent, not implementation. When a plan conflicts with code, tests,
as-built documents or current physical evidence, the latter win.

## Required routes

For any code change, read `docs/ARCHITECTURE.md` and `docs/SAFETY.md` before editing,
then add the matching rows below.

| Task scope | Additional required reading |
| --- | --- |
| Serial/LAN commands or public responses | `docs/API.md` |
| SVD48, RS485, polling or drive units | `docs/SVD48.md` |
| OTA, release or recovery | `docs/OTA.md` |
| Physical tests, HIL or test architecture | `docs/TESTING_ARCHITECTURE_GUIDE.md`, `docs/testing/README.md`, `tests/hil/README.md` and `docs/SAFETY.md` |
| PCB, actuator, sensor, closed-loop or mobility qualification | the preceding physical-test route, `docs/NEXT_STEPS.md`, and the relevant robot/fixture evidence |
| Evidence from a physical session | `docs/testing/EVIDENCE_TEMPLATE.md` |
| Future SVD48 workspace or bench-control work | `docs/SVD48_WORKSPACE_V2_PLAN.md`, plus API/SVD48/safety routes as applicable |
| Future continuous `/control` work | `docs/SAFE_CONTROL_PLANE_V1_PLAN.md`, `docs/API.md`, `docs/NEXT_STEPS.md` and the Master Guide control-plane sections |

Before changing either repository, inspect its top-level path, status, branch and
recent log. For firmware work, identify the running firmware over LAN when safely
available; never infer it from a local checkout.

Precedence is explicit: current source first, executable tests and runtime evidence
second, as-built and API/safety contracts third, physical evidence fourth, plans
fifth and historical prose last. No plan, handoff or archived document overrides
`docs/SAFETY.md` or proves that planned behavior is implemented. Correct documentation
in the same change whenever behavior or a public contract changes.

## Current constraints

- The active topology comes from a build-selected immutable C profile. There is no
  JSON/YAML loader or general runtime factory for every declared driver yet.
- The executable factory registry composes the two-controller `current_robot` and
  one-controller/one-endpoint `bench_single_svd48_motor` SVD48 profiles plus the
  isolated development `bench_single_steering_as5600` chain (motor-mode PWM, AS5600
  and steering controller). The steering profile is unqualified bench software, not
  a general driver framework or a physical test result.
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
- A cyclic sensor phase is not a mechanical zero. Do not add auto-home/reference
  behavior to motor-mode steering; explicit reference and 7+7 calibration capture
  are separately authorized maintenance/physical-test operations.
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
