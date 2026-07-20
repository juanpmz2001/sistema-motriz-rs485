# ADR-0001: Separate Management and Motion Boundaries

- Status: `ACCEPTED`
- Date: 2026-07-17
- Owners/session IDs: parent `019f6e72-3486-7ce1-af40-72d240a5f676`; audits listed in `../SESSION_LOG.md`
- Work-item IDs: `SAFE-001`, `TRANS-002`, `PROF-001`, `SVD-005`, `WEB-001`

## Context

The current serial gateway combines text parsing, movement, raw SVD48 access, network/OTA configuration, diagnostics, and output rendering. Maintenance LAN invokes that parser and captures text. The web frontend then infers transaction results from lines. Adding robot profiles and writable controller configuration directly to this path would couple safety decisions to transport syntax and make USB/LAN behavior difficult to keep equivalent.

## Decision

- Motion commands pass through command-authority and latched-safety enforcement below all transports.
- Configuration/diagnostic operations use a typed firmware management service.
- USB serial and maintenance LAN adapt framing/authentication to the same management operations.
- Profile authoring remains modular, while the canonical device profile is the
  self-contained JSON defined by ADR-0002.
- The web backend is the browser bridge and consumes advertised capabilities/typed results; the browser does not own register semantics.
- Writable configuration is unavailable until the safety, authentication, typed-read, session, change-set, and readback gates pass.

## Alternatives Considered

- Continue adding ASCII commands and parsing output lines: rejected because it loses semantic status/correlation and scales poorly for typed/large data.
- Let the browser talk directly to the ESP: rejected for the MVP because it duplicates transport/authentication/scheduling and exposes device credentials.
- Store an unversioned opaque combined blob: rejected. ADR-0002 later selected a
  versioned self-contained JSON with typed internal sections, explicit apply
  policy and live-controller drift reporting.
- Automatically rewrite SVD48 values at boot: rejected because controller identity/support may be unknown and an incorrect write can create unsafe movement.

## Consequences

- Initial work is larger than adding a form, but failures become structured and policies remain consistent across USB/LAN.
- Compatibility adapters are required while existing ASCII tools remain in use.
- Profile and protocol versions must be negotiated and tested.
- The read-only configuration page can ship before write support.
- Safety and transport containment become prerequisites, not optional cleanup.

## Verification and Migration

- Follow `00_MASTER_PLAN.md` delivery slices.
- Validate contracts using `03_TRANSPORT_AND_API_CONTRACT.md` and shared fixtures.
- Validate physical behavior using `04_OFF_GROUND_TEST_MATRIX.md`.
- Record each proven firmware/web/controller combination in `COMPATIBILITY_MATRIX.md`.

## Supersedes / Superseded By

Profile ownership and persistence portions are superseded by ADR-0002. The
management/motion boundary remains accepted.
