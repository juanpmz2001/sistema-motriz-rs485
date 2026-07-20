# ADR-0002: Canonical JSON Robot Profile

- Status: `ACCEPTED`
- Date: 2026-07-19
- Owners/session IDs: parent `019f6e72-3486-7ce1-af40-72d240a5f676`
- Work-item IDs: `PROF-001..009`, `KIN-001..005`, `SVD-005..007`

## Context

The previous design proposed a fixed C profile with four motors/four servos and
separate robot and SVD48 profile documents. That cannot represent one SVD48 using
M1/M2, two drives with four motors, four drives with one used channel each,
Ackermann linkage, per-wheel steering and independent-steer crab configurations
without accumulating topology-specific structs and migration paths.

The product requirement is that a robot is one editable JSON document containing
all non-secret hardware and control configuration.

## Decision

- The canonical persisted and transported robot profile is versioned JSON.
- It contains board/I/O, controllers/channels, SVD48 desired parameters,
  traction/steering/passive actuators, poses, geometry, kinematics, command
  sources, safety limits and service configuration.
- Secrets remain in dedicated secure/provisioning storage and are referenced by
  identifier; they are not embedded in the robot document.
- The ESP parses and validates the same JSON, then builds an immutable normalized
  C runtime snapshot. The C snapshot is derived data, never the persisted source.
- Arrays are semantically variable length. Firmware-advertised capabilities bound
  resources; a fixed capacity is not a robot topology.
- The JSON is self-contained when staged. Backend authoring templates must be
  resolved before upload.
- Persistence uses validated A/B JSON slots plus small NVS metadata for active and
  known-good pointers.
- SVD48 writes are never automatic at boot. The embedded desired configuration is
  compared to live state and applied only through an explicit guarded operation.

## Alternatives Considered

- Fixed structs and NVS blobs: rejected as the canonical format because layout,
  capacity and migration become firmware-specific and are not directly editable.
- Separate board/robot/SVD files: rejected as the device source of truth because
  revisions can drift. Internal JSON sections and backend templates still provide
  modular authoring without sacrificing a self-contained active document.
- Unlimited dynamic features: rejected. Generic composition still requires a
  supported driver and kinematic strategy advertised by firmware capabilities.

## Consequences

- Firmware needs JSON schema/semantic validation, storage slots and normalization.
- Large profiles require chunked transport and hash verification.
- Runtime code remains deterministic because it consumes a validated immutable
  snapshot instead of parsing JSON in the control loop.
- All known robot combinations use one contract; new supported combinations do
  not require a new top-level profile type.

## Verification and Migration

- Validate the schema and fixtures in host CI.
- Add fixtures for one/two/four SVD48 layouts and all initial kinematics.
- Migrate existing literals into a factory JSON, then prove equivalent target
  output before removing the literals.
- Test corrupt/truncated documents, capacity overflow and power loss at each A/B
  commit stage.

## Supersedes / Superseded By

Supersedes the separate-profile persistence and fixed-capacity profile decisions
in ADR-0001. ADR-0001 remains valid for management/motion boundaries.
