# Documentation index

This index separates executable contracts from target design and migration history.
Source code and executable tests take precedence when a document and implementation
disagree.

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

There is no standalone `CONFIGURATION.md`, `CONCURRENCY_MODEL.md` or
`PROFILE_SCHEMA.md` in this iteration. Build profile/configuration is documented in
Architecture and SVD48; ownership and synchronization are documented in Architecture
and Component responsibilities. Do not link to those absent filenames as if they
were implemented contracts.

## Target design

- [Target architecture and migration rationale](ARCHITECTURE_REFACTOR.md) — intended
  authority, state, health and single-owner boundaries; future design, with completed
  Iteration 4 foundations identified explicitly.
- [Roadmap](ROADMAP.md) — ordered work and release exits beyond the current bench
  baseline; future plan, not evidence that a feature exists.
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
  results, accepted limitations and the current manual merge-gate status for this
  iteration.

## Historical material

No `docs/archive/` directory exists at this iteration. Superseded Iteration 0–3
snapshots remain recoverable from Git history; current documents do not treat those
snapshots as executable contracts. If a complete obsolete document must be retained,
archive it with its superseding document, relevant commit and reason, then update all
relative links.
