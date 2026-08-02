# Questions, assumptions and decisions

This is the decision log for the layered-architecture migration. It describes
uncertainty rather than silently turning it into firmware behavior. The recorded
baseline is commit `ce5f1e2e5b4e784b0b877366be6f68b6778f14d1` (build 19), captured
on 2026-08-02. No item in this document authorizes hardware actuation.

## A. Open questions

### Q-001 — Command precedence and handover

- **Date:** 2026-08-02
- **Context:** RC, serial and maintenance LAN exist; dormant control LAN exists;
  ROS, micro-ROS and Bluetooth are future adapters.
- **Question:** What precedence, preemption and handover rules apply to every
  command source?
- **Why it matters:** Ambiguous ownership can sustain or unexpectedly initiate motion.
- **Impact if unanswered:** The new coordinator cannot safely integrate live sources.
- **Temporary decision:** Integrate no new motion source; require an explicit,
  unexpired lease and deadman before a source can be routed.
- **State:** OPEN

### Q-002 — Arming, deadman and maximum TTL

- **Date:** 2026-08-02
- **Context:** The dormant state and authority models are not wired into runtime.
- **Question:** What physical/semantic event arms the robot, what constitutes a
  deadman, and what maximum TTL is acceptable per source?
- **Why it matters:** These define when motion is legal and when expiry must stop it.
- **Impact if unanswered:** Existing serial/LAN speed commands remain bench-only.
- **Temporary decision:** Initial state remains non-authoritative; future activation
  defaults to disarmed, rejects missing deadman, and clamps TTL through profile policy.
- **State:** OPEN

### Q-003 — Required hardware and stale/offline policy

- **Date:** 2026-08-02
- **Context:** Current safety ignores offline/stale SVD48 telemetry.
- **Question:** Which endpoints are critical in each profile, and when do offline or
  stale observations become degraded, motion-inhibiting, or latched faults?
- **Why it matters:** Treating absence too weakly is unsafe; treating omitted bench
  hardware as failed prevents valid development profiles.
- **Impact if unanswered:** Profile-aware safety cannot be activated.
- **Temporary decision:** Configured-required endpoints inhibit dependent modes;
  omitted endpoints do not exist; no arbitrary freshness threshold is added.
- **State:** OPEN

### Q-004 — E-stop and confirmed stop

- **Date:** 2026-08-02
- **Context:** Software `STOP ALL` has no independent position/velocity confirmation.
- **Question:** What physical E-stop input/output exists, and what feedback and
  deadline constitute a confirmed stop for each actuator type?
- **Why it matters:** A successful bus write does not prove physical cessation.
- **Impact if unanswered:** Production safety qualification is blocked.
- **Temporary decision:** Preserve the existing best-effort stop, label it
  unconfirmed, and require an independent power cut-off for bench work.
- **State:** OPEN

### Q-005 — Partial application

- **Date:** 2026-08-02
- **Context:** Multi-actuator commands cannot be physically atomic.
- **Question:** Which partial-apply outcomes require normal stop, emergency stop,
  fault latching, retry, or degraded operation?
- **Why it matters:** Asymmetric traction or steering may create hazardous motion.
- **Impact if unanswered:** Coordinator policy cannot be finalized.
- **Temporary decision:** Treat any critical endpoint partial application as an
  inhibit and request stop of every endpoint already attempted.
- **State:** OPEN

### Q-006 — Profile source and persistence

- **Date:** 2026-08-02
- **Context:** Hardware topology is C constants; NVS stores operational settings.
- **Question:** Should production profiles be generated from YAML, embedded as C,
  signed, or selected from a compiled set; which fields may NVS override?
- **Why it matters:** Mutable hardware mappings can defeat build-time validation.
- **Impact if unanswered:** Only a conservative build-selected profile can proceed.
- **Temporary decision:** Use versioned, build-selected immutable C data first;
  keep credentials and approved runtime settings separate in NVS.
- **State:** OPEN

### Q-007 — Maintenance compatibility and authorization

- **Date:** 2026-08-02
- **Context:** Serial/LAN expose motion and persistent SVD48 writes without a lease.
- **Question:** Which commands must remain compatible, and which states and operator
  authorization are required for direct endpoint/register maintenance?
- **Why it matters:** Compatibility must not preserve an unsafe bypass indefinitely.
- **Impact if unanswered:** The existing API remains explicitly bench-only.
- **Temporary decision:** Preserve syntax during characterization; do not expand
  allowlists; later route motion through authority and gate writes by maintenance state.
- **State:** OPEN

### Q-008 — Allocation and timing budgets

- **Date:** 2026-08-02
- **Context:** The clean build has one byte of reported IRAM headroom; task timing is
  not qualified.
- **Question:** What static memory, stack, bus-blocking and scheduling budgets apply?
- **Why it matters:** New indirection must not undermine determinism or memory safety.
- **Impact if unanswered:** Firmware integration must remain bounded and incremental.
- **Temporary decision:** Prefer fixed-capacity C structures and startup allocation;
  measure before activating additional runtime services.
- **State:** OPEN

## B. Assumptions

### A-001 — Current behavior is the compatibility oracle

- **Assumption:** Source and executable tests at the base commit define current behavior.
- **Reason:** Prose explicitly distinguishes active from planned architecture.
- **Evidence:** `main/main.c`, component APIs and seven host tests.
- **Risk:** Hardware-only behavior is incompletely characterized.
- **How isolated:** Migration map marks characterization gaps and adapters preserve APIs.
- **What changes if false:** Add hardware evidence and contract tests before migration.
- **State:** ACTIVE

### A-002 — C capability ports are sufficient

- **Assumption:** Small operation tables/opaque handles in C can express the needed ports.
- **Reason:** Existing firmware is C and does not require inheritance, RTTI or exceptions.
- **Evidence:** Current opaque component handles and pure C domain models.
- **Risk:** Poorly designed tables can erase type safety or add indirect-call overhead.
- **How isolated:** One interface per capability with explicit units and fixed lifetimes.
- **What changes if false:** Introduce narrowly scoped C++ only behind the same C ABI.
- **State:** ACTIVE

### A-003 — Build-selected profiles precede runtime profiles

- **Assumption:** Initial profiles can be immutable C data selected at build time.
- **Reason:** This reduces parser and mutable-configuration risk in the first migration.
- **Evidence:** Roadmap requires a compiler/validator before runtime adoption.
- **Risk:** Slower iteration and profile firmware variants.
- **How isolated:** Profile model is serialization-neutral and versioned.
- **What changes if false:** A validated generated representation can replace C literals.
- **State:** ACTIVE

## C. Architectural decisions

### D-001 — Incremental layered migration

- **Title:** Add seams before replacing active behavior.
- **Context:** Active bench behavior spans tightly coupled components.
- **Options considered:** Rewrite; flag-day migration; characterization plus adapters.
- **Decision:** Characterize, introduce pure contracts, wrap existing behavior, then
  reroute one vertical slice at a time.
- **Justification:** Preserves diagnostic value and keeps regressions attributable.
- **Positive consequences:** Small reviewable commits and continuous host verification.
- **Negative consequences:** Temporary duplication and explicitly partial boundaries.
- **Alternatives discarded:** Rewrite and flag-day migration due to safety/regression risk.
- **Reversibility:** High; each slice can be reverted independently.
- **Files affected:** Initially documentation only; later component-specific commits.

### D-002 — Capability ports and one coordinator

- **Title:** Typed capabilities with a single logical output owner.
- **Context:** SVD48 is a dual-channel device and actuators differ in supported operations.
- **Options considered:** Universal Motor class; direct driver calls; small capability ports.
- **Decision:** Use typed C capability ports and route normal setpoints/stops through one
  `actuation_coordinator` application service.
- **Justification:** Keeps domain independent of devices and makes partial application visible.
- **Positive consequences:** CAN or servo adapters can be added without domain changes.
- **Negative consequences:** Requires staged removal of existing direct calls.
- **Alternatives discarded:** Universal Motor and transport-owned actuation.
- **Reversibility:** Medium; public capability contracts will be versioned.
- **Files affected:** Planned new ports/application components and adapters.

### D-003 — ROS remains external

- **Title:** ROS is an adapter, not a domain dependency.
- **Context:** ESP32 must retain local authority, expiry and safety policy.
- **Options considered:** Embed ROS types in domain; micro-ROS everywhere; neutral command port.
- **Decision:** Future ROS adapters translate into the same transport-neutral command model.
- **Justification:** The firmware remains usable and testable without ROS.
- **Positive consequences:** One safety path for RC, LAN, serial and ROS.
- **Negative consequences:** Requires an explicit embedded transport contract later.
- **Alternatives discarded:** Driver access from ROS and ROS message types in domain.
- **Reversibility:** High at adapter level.
- **Files affected:** Documentation now; future external adapter only.


## Iteration 2 decisions

### D-004 — Ports are owned by the ports component

Stable endpoint/capability contracts are portable C and implementation-neutral;
application code consumes them while adapters implement them.

### D-005 — Legacy adapter is a temporary strangler seam

`robot_control_endpoint_adapter` maps endpoint IDs to unchanged logical motor indices.
Only composition binds it to `robot_control`; later device adapters replace this seam.

### D-006 — Initial build profile

Q-006 is **ANSWERED FOR INITIAL IMPLEMENTATION** only: an immutable, schema-versioned
C profile selected at build is validated before actuator construction. Production
serialization, signing and field mutability remain OPEN.

### D-007 — Synchronous coordinator

The coordinator adds no FreeRTOS task and performs no per-command allocation. Its
functions are reentrant over an immutable registry; device adapters retain bus
serialization. Ordering/authority policy remains outside this iteration.

### D-008 — First vertical slice

Only individual `SET_SPEED` and global stops from boot, serial/LAN delegation and
safety migrate. Q-001 through Q-005 remain OPEN and no authority/state/TTL policy is activated.

Q-008's previous one-byte IRAM statement is an unverified inherited assertion in
this environment. It requires a reproducible ESP-IDF 5.4.1 build/map comparison.
