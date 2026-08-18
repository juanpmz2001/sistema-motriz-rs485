# Documentation archive

The material under `2026-08-18-documentation-consolidation/` was removed from the
active reading path after a code-and-test review of firmware `main` at `627c263`.
It remains for decision traceability only. Current source, executable tests and the
documents indexed in `../README.md` take precedence.

| Archived material | Why it is archived | Active replacement |
| --- | --- | --- |
| Architecture refactor, component catalog and migration map | Target/migration prose duplicated the as-built architecture and safety contract. | `../ARCHITECTURE.md`, `../SAFETY.md` |
| Iteration 4 closeout and questions/decisions log | Historical implementation and decision evidence. | Current code/tests; retain only when tracing a decision. |
| Field-ready and platform roadmaps | Long plans contained completed milestones and overlapping sequencing. | `../NEXT_STEPS.md` plus the task-specific contracts. |
| Engineering Console and control/SVD48 workspace plans | Cross-repository target designs duplicated the Console's active handoff. | Console `docs/NEXT_STEPS.md` and its architecture. |

Archived content must not be used to claim a physical pass, broaden a requested
iteration, or override the active safety contract.
