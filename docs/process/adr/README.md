# Architecture Decision Records

Use ADRs for decisions that change safety invariants, protocol compatibility, profile ownership, persistent storage, trust/authentication, or cross-repository responsibilities.

File names: `NNNN-short-kebab-title.md`.

Statuses: `PROPOSED`, `ACCEPTED`, `SUPERSEDED`, `REJECTED`.

Required fields:

```markdown
# ADR-NNNN: Title

- Status:
- Date:
- Owners/session IDs:
- Work-item IDs:

## Context

## Decision

## Alternatives Considered

## Consequences

## Verification and Migration

## Supersedes / Superseded By
```

An ADR records why a decision was made. The process work item and tests still record whether it was implemented successfully.

Current records:

- `0001-management-and-motion-boundaries.md`: accepted target boundary.
- `0002-canonical-json-robot-profile.md`: accepted profile ownership/model.
- `0003-simultaneous-command-source-arbitration.md`: accepted target authority.
- `0004-temporary-maintenance-lan-bench-actuation.md`: temporary build-19
  engineering exception; prohibited for floor/product use and tracked by
  `SAFE-011`.
