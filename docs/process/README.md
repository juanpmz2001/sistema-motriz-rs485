# Firmware Process Documentation

This directory is the execution record for the multi-robot configuration program. It complements design and API documentation; it does not replace source code, tests, or hardware evidence.

## Documents

- `00_MASTER_PLAN.md`: cross-surface roadmap, ordering, gates, ownership, and release criteria.
- `01_SVD48_REGISTER_COVERAGE.md`: register inventory, implementation gaps, confidence, and controller experiments.
- `02_ROBOT_PROFILES_KINEMATICS_SAFETY.md`: canonical JSON profile, generic
  topology/kinematics, simultaneous `RC > LAN > Bluetooth` authority and safety
  migration.
- `03_TRANSPORT_AND_API_CONTRACT.md`: USB/LAN/backend contract, authentication, correlation, sizing, and compatibility.
- `04_OFF_GROUND_TEST_MATRIX.md`: complete tests before the robot is allowed to move on the floor.
- `05_SAFE_CONFIGURATION_WRITE_PLAN.md`: prioritized path to guarded SVD48 writes,
  JSON pin/profile activation, firmware contract and backend-first delivery.
- `06_SIMULATED_QA_FAULT_INJECTION_PLAN.md`: deterministic host QA, fake bus/clock, safety invariants, and communication fault matrix.
- `COMPATIBILITY_MATRIX.md`: proven firmware/web/profile/controller combinations and schema rules.
- `SESSION_LOG.md`: immutable chronological handoffs from Codex sessions and human test runs.
- `adr/`: cross-repository architecture decisions and their rationale.
- `templates/WORK_ITEM.md`: required format for a new work item or feature slice.
- `templates/SESSION_ENTRY.md`: required handoff format for each implementation session.

The web repository owns its own consumer-side plan under `web_controll_esp_svd48/docs/process/`.

The canonical machine-readable profile contract is
`../schemas/robot-profile.schema.json`. Examples under `../examples/` marked
`activation_allowed:false` are topology drafts, not measured hardware profiles.
The schema is currently a design artifact; firmware parsing/persistence remains
`PROF-002..005`.

For a non-C explanation of current and target behavior, read `../FIRMWARE_LOGIC_HUMAN_FRIENDLY.md`.

## Status Vocabulary

Use exactly one status per work item:

- `NOT_STARTED`: no implementation has been attempted.
- `IN_PROGRESS`: implementation exists locally but acceptance criteria are not all met.
- `BLOCKED`: progress requires named evidence, hardware, decision, or dependency.
- `FAILED`: an attempted test or approach failed; retain the evidence and next hypothesis.
- `DONE`: implementation, documentation, and required tests have objective evidence.
- `DEFERRED`: intentionally outside the MVP; include reason and revisit trigger.
- `SUPERSEDED`: replaced by another named work item or ADR.

`DONE` never means "code was written." It requires links or paths to the implementation and exact test evidence. Hardware-dependent work cannot be marked `DONE` from compilation alone.

## Stable IDs

Use these prefixes in plans, tests, code comments, commits, and session entries:

- `SAFE-*`: safety, arming, command authority, and emergency behavior.
- `PROF-*`: robot profile and persistence.
- `KIN-*`: kinematics and actuator mapping.
- `SVD-*`: SVD48 protocol and parameter catalog.
- `TRANS-*`: serial/LAN transport and command contract.
- `AUTH-*`: authentication, authorization, replay, and secrets.
- `WEB-*`: backend/frontend work owned by the web repository.
- `TEST-*`: cross-cutting bench or hardware validation.
- `OPS-*`: release, migration, logging, and deployment.

Example code comment for a deliberate MVP limitation:

```c
// TODO(SVD-042): Enable only after float word order is captured from SV-Config and covered by a golden frame test.
```

Do not add comments that only narrate the code. Comments must explain a safety invariant, protocol ambiguity, hardware dependency, or intentional MVP limitation and reference its process ID.

## Agent Workflow

Before editing:

1. Read this file, `00_MASTER_PLAN.md`, the relevant topic document, and the latest `SESSION_LOG.md` entries.
2. Record the Codex session/thread ID. In a local Codex shell, use `printenv CODEX_THREAD_ID`; never dump the complete environment.
3. Record branch, commit, dirty-worktree status, attached hardware, and which work item IDs are being claimed.
4. Re-read the actual implementation. A previous `DONE` status is a claim to verify, not an instruction to trust.

During work:

1. Keep changes scoped to claimed IDs.
2. Update each work item as evidence changes; do not postpone all documentation until the end.
3. Record failed approaches and unexpected hardware behavior instead of erasing them.
4. Redact Wi-Fi passwords, OTA tokens, maintenance tokens, private keys, and full device secrets.
5. If behavior or compatibility changes, update the normative API/skill documentation in the same change.

At handoff:

1. Add one `SESSION_LOG.md` entry using the template.
2. List exact files changed and tests with commands/results.
3. Distinguish simulated, mocked, USB hardware, LAN-only, motor-connected, and robot-elevated evidence.
4. State rollback steps and unresolved risks.
5. Leave no running test/server/monitor process unless the handoff explicitly identifies it.

## Evidence Levels

- `E0`: code inspection or manual statement only.
- `E1`: host unit/golden test.
- `E2`: firmware build or web static/integration test.
- `E3`: ESP32 test without SVD48/motors.
- `E4`: SVD48 connected, motors mechanically restrained or wheels removed.
- `E5`: complete robot elevated with wheels free and an operator at physical power cutoff.
- `E6`: supervised floor test in a controlled area.

Each acceptance criterion declares its minimum evidence level. Never substitute a higher-risk test for missing lower-level deterministic tests.
