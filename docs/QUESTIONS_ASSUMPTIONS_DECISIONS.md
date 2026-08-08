# Questions, assumptions and decisions

This is the decision log for the layered-architecture migration. It describes
uncertainty rather than silently turning it into firmware behavior. It begins at
commit `ce5f1e2e5b4e784b0b877366be6f68b6778f14d1` (build 19) and records decisions
through Iteration 4. [Architecture](ARCHITECTURE.md), source and executable tests
define the current implementation. No item here authorizes hardware actuation.

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
- **Context:** Clean Iteration 4 builds report one byte free in the dedicated IRAM
  category. Iteration B map analysis showed 232,272 B of effective shared D/IRAM
  linker headroom and set a 192 KiB CI floor. Task stack high-water marks, runtime
  heap, bus timing and their acceptable budgets are still not qualified.
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
- **Evidence:** `main/main.c`, component APIs and the host/protocol contract suites;
  exact Iteration 4 results belong in the closeout record.
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

**SUPERSEDED BY D-009.** The coordinator adds no FreeRTOS task and performs no
per-command allocation, but its operations are not reentrant in firmware. Iteration 3
added one injected mutex covering each complete operation. Device/bus serialization
remains a separate lower-layer responsibility. Ordering/authority policy remains
outside this iteration.

### D-008 — First vertical slice

Only individual `SET_SPEED` and global stops from boot, serial/LAN delegation and
safety migrate. Q-001 through Q-005 remain OPEN and no authority/state/TTL policy is activated.

Iteration B supersedes Q-008's earlier interpretation of the Iteration 4 size row:
the one-byte dedicated-bank remainder is not linker capacity. Reproducible ESP-IDF
5.4.1 map analysis gives 232,272 B of effective shared D/IRAM headroom for both
profiles, with a 192 KiB CI floor. Q-008 remains open because runtime heap/stack,
bus-blocking and scheduling budgets and evidence are still undefined.

## Iteration 3 hardening decisions

- **D-009 Coordinator serialization:** an injected lock port keeps the core portable;
  the firmware composition supplies a static FreeRTOS mutex with a 500 ms acquire bound.
- **D-010 Stop semantics:** physical endpoint stops occur once; only after total success
  an isolated legacy hook clears body velocity, wheel RPM, steering command, sequence
  and timestamp state. Partial stop retains a distinguishable partial commanded state.
- **D-011 Neutral profile model:** board, buses, devices, channels, endpoints and optional
  application geometry replace the fixed RS485/four-motor shape.
- **D-012 Board validation:** explicit valid/reserved and input/output/PWM resource masks
  replace numeric GPIO-range-only validation.
- **D-013 Build selection:** Kconfig chooses a compiled profile and boot logs its name.
- **D-014 Gateway inversion:** serial and maintenance paths depend on an application port;
  safety depends on its stop subset, not composition or coordinator implementations.
- **D-015 Transitional composition:** fixed slots own buses, devices and endpoint
  adapters; the compatibility wrapper may still allocate at startup. This component
  remains an actuation sub-composition rather than the full system composition root.

Q-001 through Q-005 remain OPEN. No new operational safety policy was inferred.

## Iteration 4 composition decisions

### D-016 — One serialized transport per physical bus

- **Context:** Multiple SVD48 controllers share one UART-to-RS485 link.
- **Decision:** `rs485_transport` owns the UART and implements a portable
  `bus_transport` backend. The bus lock covers a complete request/response exchange;
  devices never include UART headers or own duplicate UART instances.
- **Consequences:** Device tests can use a fake transport and two controllers cannot
  interleave frames. Transport statistics are updated and copied under a dedicated
  statistics lock, separate from the bus-exchange lock.
- **Status:** IMPLEMENTED in Iteration 4.

### D-017 — One device per controller with explicit M1/M2 channels

- **Context:** An SVD48 address identifies one physical controller with two channels,
  not two independent bus devices.
- **Decision:** Construct one `svd48_device` for each configured controller address and
  expose borrowed M1/M2 channel views. Endpoints bind to an explicit device and
  channel.
- **Consequences:** Driver logic has no four-motor topology; profile endpoint order is
  used only at the legacy-index compatibility edge.
- **Status:** IMPLEMENTED in Iteration 4.

### D-018 — Shared N-device polling and observation freshness

- **Context:** A single fixed two-drive poll loop cannot represent single-controller
  or future bounded multi-controller profiles, and one successful field must not make
  failed speed data fresh.
- **Decision:** One polling service schedules every configured device with independent
  periods and backoff. Freshness, validity and timestamps are retained per observation.
  A cycle is complete only when every observation scheduled for that fast/slow cycle
  succeeds; otherwise it is partial or failed. Partial participates in backoff rather
  than being recorded as full success.
- **Consequences:** Healthy requires every configured SVD48 observation to be valid
  and fresh. Stale, offline, degraded and controller fault remain distinct. The active
  safety task does not yet consume the full profile-aware model.
- **Status:** IMPLEMENTED at the device/polling boundary; safety integration PENDING.

### D-019 — Executable factory registry starts with SVD48 only

- **Context:** The profile schema describes more driver IDs than the runtime can
  construct.
- **Decision:** Keep the schema descriptor registry separate from the executable
  factory registry. Iteration 4 registers only the SVD48/RS485 factory and preflight
  reports a missing or incompatible factory before touching outputs.
- **Consequences:** “Schema valid” does not imply “composition supported”. New drivers
  require a complete validate/storage/construct/endpoint/start/stop/destroy factory,
  capacity checks and tests; scattered driver conditionals are not accepted.
- **Status:** IMPLEMENTED for SVD48; general multi-driver composition remains PARTIAL.

### D-020 — Legacy SVD48 view is bounded to four bindings

- **Context:** `robot_control`, maintenance, OTA and safety telemetry still consume the
  legacy logical-motor API.
- **Decision:** Attach the new device/channel objects to a temporary compatibility
  wrapper. Validate and diagnose its maximum of four bindings explicitly.
- **Consequences:** The limit applies only to wrapper channel bindings, not
  `svd48_device` or channel endpoints. The polling service separately has a static
  capacity of four physical devices. Profiles needing more legacy endpoints require
  those compatibility callers to migrate first.
- **Status:** TRANSITIONAL; removal depends on migrating all remaining legacy callers.

### D-021 — SVD48 speed registers are raw RPM

- **Context:** Earlier prose and fields asserted a 0.1 RPM scale without durable
  evidence. The manufacturer register table labels given speed `0x5304/0x5305` and
  motor speed `0x5410/0x5411` as RPM.
- **Decision:** Preserve the signed register value as RPM with no artificial scaling
  and name public observations accordingly.
- **Consequences:** Existing ASCII syntax and `RPM` label remain compatible. A future
  controlled physical test must confirm the interpretation. The unconfirmed raw value
  already feeds the legacy 5-RPM OTA/maintenance readiness predicate and platform
  motion status; those checks remain unqualified and must not be expanded as safety
  policy merely because the field was renamed. Uncertainty is not represented by
  silently scaling the value.
- **Status:** IMPLEMENTED contract; physical confirmation OPEN.

### D-022 — Safe diagnostic startup for unsupported composition

- **Context:** Preflight can find a schema-valid profile whose factory/bus/device
  composition is unsupported. Returning before the gateway leaves no field diagnosis.
- **Decision:** Do not construct actuator outputs. Retain only the minimum serial
  configuration and start the gateway in an explicit restricted diagnostic mode with
  an immutable profile/composition failure snapshot. Allow `PING`, `VERSION`, `HELP`,
  `PLATFORM_STATUS`, `CONFIG_STATUS`, `WIFI_STATUS`, `PROFILE_STATUS`,
  `COMPOSITION_STATUS` and exactly `STOP ALL`; reject everything else.
- **Consequences:** `STOP ALL` cannot claim a physical stop when no endpoints exist; it
  reports outputs unavailable. This mode is a recovery aid, not an armed operating
  state. Invalid schema and failures after outputs are constructed remain fail-closed
  startup errors unless separately designed and tested. A pending-verification OTA
  image follows rollback handling instead of entering this mode.
- **Status:** IMPLEMENTED as the minimum Iteration 4 diagnostic fallback.

### D-023 — Both Kconfig SVD48 profiles are executable

- **Context:** The previous legacy backend required four bindings even though the
  schema admitted a one-endpoint profile.
- **Decision:** Support `current_robot` with two devices/four endpoints and
  `bench_single_svd48_motor` with one device, M1 endpoint and legacy index `0` only.
  The bench profile has no application geometry; `MOVE_VEL` is unsupported.
- **Consequences:** Omitted controller/channel endpoints are not reported failed.
  `SET_SPEED 0`, `STOP 0` and `STOP ALL` remain routable; index `1` is rejected.
- **Status:** IMPLEMENTED; build/test evidence belongs in Iteration 4 closeout.

### D-024 — Merge Iteration 4 only from verified review state

- **Context:** The original architecture commit is large and the closeout adds fixes,
  tests, CI and documentation.
- **Decision:** Preserve focused closeout commits on the feature branch, integrate any
  newer `main` with a normal merge if needed, and use a reviewed pull request. Squash
  merge is preferred when one coherent Iteration 4 change on `main` is desired; a
  merge commit is acceptable when full branch history is required.
- **Consequences:** No direct silent merge or force push. Host tests, sanitizers,
  protocol/dependency tests, both ESP-IDF profiles, resource evidence and clean-tree
  checks are mandatory before merge.
- **Status:** ACCEPTED process; final evidence belongs in Iteration 4 closeout.
