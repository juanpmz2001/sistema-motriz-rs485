# Documentation index

This index separates executable contracts from target design and migration history.
Source code and executable tests take precedence when a document and implementation
disagree.

For overlapping plans, [Field-ready iteration roadmap](FIELD_READY_ITERATION_ROADMAP.md)
owns the sequence to the first supervised field test; [Platform roadmap](ROADMAP.md)
owns the longer-horizon architecture and product sequence. Neither overrides the
current [Safety](SAFETY.md) contract or counts as verification evidence.

## Current sources

- [Architecture](ARCHITECTURE.md) — as-built component graph, startup, actuation,
  polling, tasks and locks for firmware maintainers.
- [Safety](SAFETY.md) — implemented safety behavior, known bypasses and production
  release gates; it describes current firmware and mandatory future constraints.
- [Command and LAN API](API.md) — current serial grammar, compatibility responses and
  maintenance LAN boundary for operators and client authors.
- [SVD48 integration](SVD48.md) — current RS485 transport, device/channel model,
  registers, polling, units and legacy compatibility for driver maintainers.
- [OTA runbook](OTA.md) — current provisioning, release, update and recovery procedure
  for trusted development networks.

## Testing and physical qualification

- [Testing architecture guide](TESTING_ARCHITECTURE_GUIDE.md) — versioned contract
  for test levels L0–L7, evidence classes E0–E4, test specifications, mandatory
  cleanup and architectural dependency rules.
- [Physical test runbooks](testing/README.md) — concise entry/exit gates for PCB,
  single-motor, servo, sensor, closed-loop and mobility sessions; it links to rather
  than restates the testing contract.
- [AS5600 motor-mode steering bench runbook](testing/STEERING_AS5600_BENCH_RUNBOOK.md)
  — prepared fixture/provenance, explicit-reference and staged-evidence procedure
  for the isolated steering development profile; it records no physical pass.
- [Evidence template](testing/EVIDENCE_TEMPLATE.md) — versioned record for identity,
  safety preconditions, observations, cleanup, result and verification boundaries.
- [Host HIL runner](../tests/hil/README.md) — executable `validate`, `identify` and
  bounded `run` workflow, required identity gates and explicit motion confirmations.

These documents define how future physical evidence must be produced. A minimal host
HIL runner, one capability-oriented L4 velocity manifest, typed velocity and position
observation slices, and an offline AS5600 calibration-candidate analyzer now exist;
they do not mean that any physical milestone passed or that the broader observation
architecture is complete. Raw captures and durable evidence remain external
artifacts.

There is no standalone `CONFIGURATION.md`, `CONCURRENCY_MODEL.md` or
`PROFILE_SCHEMA.md` in this iteration. Build profile/configuration is documented in
Architecture and SVD48; ownership and synchronization are documented in Architecture
and Component responsibilities. Do not link to those absent filenames as if they
were implemented contracts.

## Target design

- [Target architecture and migration rationale](ARCHITECTURE_REFACTOR.md) — intended
  authority, state, health and single-owner boundaries; future design, with completed
  Iteration 4 foundations identified explicitly.
- [Field-ready iteration roadmap](FIELD_READY_ITERATION_ROADMAP.md) — master ordering
  for hardware qualification, controlled chassis motion and the first supervised
  field-testable version.
- [Platform roadmap](ROADMAP.md) — longer-horizon architectural and product slices,
  including Linux/ROS integration and qualification beyond the first field path.
- Open safety and architecture decisions are tracked in
  [Questions, assumptions and decisions](QUESTIONS_ASSUMPTIONS_DECISIONS.md), which
  is also cataloged below as the migration decision log.

## Migration and decisions

- [Migration map](MIGRATION_MAP.md) — current disposition of legacy responsibilities,
  callers and removal conditions for reviewers.
- [Component responsibilities](COMPONENT_RESPONSIBILITIES.md) — active, transitional
  and planned ownership/dependency catalog for maintainers.
- [Questions, assumptions and decisions](QUESTIONS_ASSUMPTIONS_DECISIONS.md) — dated
  open safety questions and architectural decisions across the migration.
- [Iteration 4 closeout](ITERATION_4_CLOSEOUT.md) — verification evidence, build
  results, accepted limitations and final integration evidence for this iteration.

## Historical material

No `docs/archive/` directory exists at this iteration. Superseded Iteration 0–3
snapshots remain recoverable from Git history; current documents do not treat those
snapshots as executable contracts. If a complete obsolete document must be retained,
archive it with its superseding document, relevant commit and reason, then update all
relative links.
