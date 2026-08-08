# Field-Ready Iteration Roadmap

> **Roadmap version:** 1.0
> **Status:** Current master sequence from `bench-baseline-v1` to the first
> supervised field-testable version.

## Purpose

This document defines the remaining engineering sequence from the Iteration 4 bench baseline to a **first robot version that can be moved and tested physically with controlled risk**, and then toward a field-testable version.

It is intentionally pragmatic.

The goal is not to complete every desirable architectural improvement before the robot moves.

The goal is:

> eliminate the problems that can create unsafe motion, misleading tests or architecture lock-in, while postponing improvements that do not materially block the next physical milestone.

Future coding agents should use this document to understand **why an iteration exists**, not only what tasks were listed in its prompt.

When implementation details are ambiguous, decisions should be made in favor of the motivation and exit criteria described here.

## Scope and precedence

This document owns ordering for hardware qualification, controlled chassis motion
and the path to the first field test. [Roadmap](ROADMAP.md) remains the long-horizon
platform plan for architectural evolution, Linux/ROS integration and product
qualification. If their ordering overlaps and conflicts before the first field test,
this field-ready roadmap takes precedence.

Neither roadmap describes implemented behavior or supplies verification evidence.
Source code, executable tests, current as-built contracts and [Safety](SAFETY.md)
remain authoritative; a safety gate cannot be relaxed by roadmap ordering.

The testing vocabulary and mandatory lifecycle are versioned in the
[Testing Architecture Guide](TESTING_ARCHITECTURE_GUIDE.md). Use the concise
[physical test runbooks](testing/README.md) to select gates and the
[evidence template](testing/EVIDENCE_TEMPLATE.md) to record a session instead of
duplicating those contracts here.

---

# 1. Guiding principles

## 1.1 Physical progress is a first-class goal

The firmware architecture exists to make the robot easier and safer to evolve.

Architecture work that does not improve:

- safety;
- testability;
- hardware interchangeability;
- diagnosability;
- or the ability to validate the robot;

should not automatically be prioritized over physical testing.

---

## 1.2 Bench motion and robot mobility are different milestones

The fact that a wheel spins safely on a stand does not mean the full robot is ready to move.

We deliberately allow physical testing earlier at lower levels:

```text
device
→ capability
→ subsystem
→ robot motion
→ field behavior
```

Each stage earns the right to proceed to the next one.

---

## 1.3 Do not finish the entire target architecture before testing hardware

The long-term architecture includes:

- state;
- authority;
- health aggregation;
- single-owner actuation;
- additional drivers;
- profile tooling;
- Linux/ROS;
- production security;
- product qualification.

Not all of those are required before the first motor turns.

They become mandatory only when the next physical milestone genuinely depends on them.

---

## 1.4 Do not defer motion-safety fundamentals

Some gaps are not architectural luxury.

Before the complete robot moves on the floor, the system must have:

- one controlled path for motion commands;
- bounded command lifetime;
- a reliable stop path;
- priority for stop over normal motion;
- explicit knowledge of which command source is active;
- required actuator/sensor health gating;
- a known operating state;
- no hidden legacy writer capable of surprising the robot.

Those are prerequisites for chassis motion, not post-production polish.

---

# 2. Current baseline assumptions

Before using this roadmap, every agent must verify the current repository HEAD and current documentation.

The expected Iteration 4 baseline conceptually contains:

- profile-driven buses/devices/endpoints;
- portable bus transport;
- RS485 transport;
- one SVD48 device per physical controller;
- explicit M1/M2 channels;
- direct capability adapters for motor velocity/stop;
- N-device polling;
- per-observation freshness at the SVD48 boundary;
- an executable SVD48 factory;
- a single-SVD48/single-motor bench profile;
- host tests and CI;
- a restricted diagnostic startup.

The following areas are still transitional or incomplete and should be re-verified:

- `robot_control` remains active;
- some physical writers remain legacy;
- `MOVE_VEL` is not yet the final application motion path;
- safety still consumes legacy telemetry in important paths;
- command authority is not active;
- the operating-state model is not active;
- kinematics is not the active motion service;
- the coordinator is mutex-backed rather than a priority-aware owner;
- command TTL/deadman is not globally enforced;
- non-SVD48 factories are not yet broadly available;
- static link capacity has a map-derived 192 KiB shared D/IRAM floor; runtime heap,
  stack and timing margins remain unqualified.

If the code has evolved, update this document rather than following obsolete assumptions.

## 2.1 Verified roadmap state — 2026-08-07

The status words below are deliberate: `DONE`, `IN PROGRESS`, `READY TO START`,
`BLOCKED BY HARDWARE`, `BLOCKED BY DECISION` and `NOT STARTED`. A status may have
additional prerequisite gates explained in its evidence column.

| Milestone / iteration | Status | Evidence or remaining gate |
| --- | --- | --- |
| Milestone 0 — stable Iteration 4 baseline | **DONE** | `bench-baseline-v1` points to `d11306cecb93099d78cb7477cfaf259f9ddaef4c`; post-merge workflow [`31234517124`](https://github.com/juanpmz2001/sistema-motriz-rs485/actions/runs/31234517124) passed host, sanitizer and both ESP-IDF profile builds. This is a bench baseline, not physical evidence. |
| Iteration A — hardware testability | **DONE** | PR [#6](https://github.com/juanpmz2001/sistema-motriz-rs485/pull/6) merged as `7fef8981f78f61c802df63128766a26e9511aaed`; post-merge workflow [`31237789863`](https://github.com/juanpmz2001/sistema-motriz-rs485/actions/runs/31237789863) passed host, sanitizer and both ESP-IDF profile builds with artifacts. No physical test was executed. |
| Iteration B — memory headroom | **DONE** | PR [#8](https://github.com/juanpmz2001/sistema-motriz-rs485/pull/8) merged as `cf726482a1aed453b749b0338c1fad789f0fdc52`; post-merge workflow [`31239836099`](https://github.com/juanpmz2001/sistema-motriz-rs485/actions/runs/31239836099) passed host, sanitizer and both profile builds. Both maps passed with 232,272 B effective headroom against the 196,608 B floor, and both artifacts retain placement evidence. |
| Iteration C — traction qualification | **BLOCKED BY HARDWARE** | This is the first incomplete iteration. No L2–L5 physical result exists. |
| Iteration D — steering + feedback | **BLOCKED BY HARDWARE** | Real actuator/encoder/profile evidence is absent. |
| Iteration E — generic observations | **NOT STARTED** | Iteration A supplies only the minimum typed velocity slice; the broader typed boundary remains. |
| Iteration F — motion service + geometry | **NOT STARTED** | Existing legacy motion path must first be audited against the real geometry. |
| Iteration G — minimum safe authority | **NOT STARTED** | Mandatory before complete-robot floor motion. |
| Iteration H — controlled chassis | **BLOCKED BY HARDWARE** | Also blocked by C–G evidence. |
| Iteration I — pre-field hardening | **NOT STARTED** | Starts only after controlled chassis evidence. |

Main physical gates are currently:

| Physical gate | Status | Required evidence to unblock |
| --- | --- | --- |
| PCB/controller communication L2 | **BLOCKED BY HARDWARE** | Named board/PCB, safe powered setup and repeatable read-only communication evidence. |
| SVD48/device qualification L3 | **BLOCKED BY HARDWARE** | Real controller/channel/sign/scaling/error evidence. |
| Generic single-motor velocity L4 | **BLOCKED BY HARDWARE** | Operator-authorized bounded run through the application endpoint API. |
| Traction closed loop L5 | **BLOCKED BY HARDWARE** | Fresh E2 feedback, then independent E3 sensing for the stronger physical claim. |
| Steering L3–L5 | **BLOCKED BY HARDWARE** | Real actuator plus independent position feedback. |
| Chassis H1/H2/H3 | **BLOCKED BY HARDWARE** | All component, motion-service and minimum-safe-authority gates. |
| Supervised field test | **BLOCKED BY HARDWARE** | Controlled chassis and pre-field hardening evidence. |

No physical gate is marked `PASS`. The first incomplete iteration is C, and its next
exit evidence requires the named bench hardware, an operator-confirmed safe setup
and an explicitly authorized physical session. The repository has the runbook and
read-only interfaces needed to begin that gate, but compilation or simulated
feedback cannot close it.

---

# 3. Milestones

The remaining work should be viewed as milestones rather than one continuous refactor.

## Milestone 0 — Stable Iteration 4 baseline

Goal:

> establish a known firmware version from which physical testing can begin.

This is a merge/release-management milestone, not another architecture iteration.

Required:

- remote CI green;
- Iteration 4 merged;
- reproducible build;
- known commit/tag;
- documentation aligned with code;
- no unreviewed changes mixed into the baseline.

Suggested tag:

```text
bench-baseline-v1
```

or another explicit development tag.

This baseline should remain bench-only.

---

# 4. Iteration A — Hardware testability and agent guidance

## Objective

Create the rules, tooling and interfaces that let developers and LLM agents test hardware **without bypassing the architecture**.

## Why this is necessary now

Physical testing is about to become frequent.

Without a defined approach, each new agent may invent:

- direct Modbus scripts;
- hardcoded motor indices;
- one-off servo commands;
- duplicated test firmware;
- unsafe boot behavior;
- driver-specific mobility tests.

That would recreate the architectural coupling the refactor is trying to remove.

## Main deliverables

### Documentation

Create and integrate:

- the versioned [testing-level and evidence contract](TESTING_ARCHITECTURE_GUIDE.md);
- the concise [PCB, single-motor, servo, sensor, closed-loop and mobility
  runbooks](testing/README.md);
- the versioned [test-evidence template](testing/EVIDENCE_TEMPLATE.md); and
- root routing plus scoped `AGENTS.md` files only where a subtree has additional
  executable invariants.

### Host HIL runner

Create the minimum host-side framework for:

- connect;
- identify firmware/profile;
- read status;
- execute bounded commands;
- collect observations;
- enforce timeout;
- stop in cleanup;
- produce evidence.

Do not build a large test platform.

The first version should be deliberately small.

### Test manifests

Allow physical tests to describe:

- required profile;
- target endpoint/capability;
- command;
- observation;
- tolerance;
- duration;
- safety preconditions;
- cleanup.

### Dependency enforcement

Add automated checks that L4+ HIL tests do not import concrete transport/device APIs.

## Embedded changes

Keep firmware changes minimal.

If generic observations cannot yet be obtained without reading SVD48-specific structures, introduce the smallest stable observation boundary needed by the HIL runner.

Do not implement the entire future telemetry architecture.

## Exit criteria

- an agent can determine where a new test belongs;
- a single-motor physical test can be written without direct RS485/SVD48 writes;
- every motion HIL runner attempts cleanup stop on all catchable exits and requires
  an independent physical cut-off for failures software cannot survive;
- test evidence identifies firmware/profile/hardware;
- repository instructions clearly route new agents.

## Closeout evidence — 2026-08-07

- **What changed:** task routing, the L0–L7/E0–E4 testing contract, physical-test
  runbooks, a typed endpoint/velocity-observation application boundary, reproducible
  firmware identity, and a bounded host-side L4/E2 runner for the single-endpoint
  bench profile.
- **What was tested:** host and sanitizer suites, protocol and dependency contracts,
  HIL/identity fake tests, both ESP-IDF 5.4.1 profiles, and the post-merge workflow
  with downloadable build evidence.
- **What was not tested:** no firmware was flashed and no controller, motor, servo or
  chassis was actuated. These results are software/build evidence only.
- **Current hardware gate:** every L2+ physical gate remains `BLOCKED BY HARDWARE`.
- **Next recommended milestone:** Iteration B, starting with measured IRAM map analysis
  rather than speculative feature removal.

## Explicitly defer

- ROS;
- dashboards;
- generalized laboratory orchestration;
- cloud test storage;
- dynamic runtime profiles;
- test GUIs;
- full production manufacturing tools.

---

# 5. Iteration B — Establish and protect embedded memory headroom

## Objective

Establish and preserve enough effective internal-memory link capacity to continue
firmware development safely.

## Why this is necessary now

The known Iteration 4 `idf.py size` report appeared to show essentially no IRAM
headroom. The linker map demonstrates that this was the dedicated-bank category
boundary, while executable code continues in shared D/IRAM.

An unexplained report that appears nearly full is a poor basis for adding:

- new capabilities;
- motion ownership;
- state/authority integration;
- instrumentation;
- fault handling.

This is not “optimization for elegance.”

It is capacity required for the next safety-critical firmware changes.

## How to think about the task

Do not randomly remove features.

First identify why symbols are in internal executable memory and which physical
memory region they consume.

Use:

- linker map;
- `idf.py size-components`;
- `idf.py size-files`;
- component configs;
- ISR requirements;
- cache-disabled requirements;
- compiler attributes.

Classify internal-memory content:

```text
must remain internal/cache-safe
can move to flash
configuration overhead
unexpected placement
```

If the measured target is not met, prefer moving reviewed non-critical code to
Flash over deleting behavior. Do not move anything merely to alter a misleading
report category.

## Execution decision — 2026-08-07

- **Current problem:** `idf.py size` reports 16,383 of 16,384 bytes in its
  dedicated `IRAM` category, but the ESP32-S3 linker map continues `.iram0.text`
  into dual-mapped D/IRAM. On `05cf1005c731bae6cbd60ae0f570614831213c7e`,
  the conservative shared linker margin is 232,272 bytes; the one-byte value is
  an alignment gap within that category, not the next-link failure point.
- **Motivation and next physical milestone:** preserve measurable capacity for the
  minimum observation slice needed by single-motor qualification, then for motion
  and minimum-authority work before complete-chassis qualification.
- **Minimum required scope:** derive headroom from linker regions and end symbols,
  enforce the same threshold for both CI profiles, retain the size/component/file
  analysis as review evidence, and correct prose that treated the category row as
  total link headroom.
- **Target before implementation:** at least 192 KiB (196,608 bytes) of shared
  D/IRAM linker headroom in both profiles. The measured baseline exceeds this by
  35,664 bytes, leaving a reviewed static-growth budget for Iterations E–G, whose
  new behavior should otherwise remain primarily in flash and small fixed state.
- **Explicit non-goals:** no feature deletion, cache-size change, ISR weakening,
  scheduler/heap relocation, Wi-Fi throughput trade-off or runtime heap/stack claim.
  Runtime resource qualification remains part of Iteration I.

Because the measured baseline already exceeds the target, no placement or
configuration change is justified merely to make the dedicated `IRAM` row look less
full. The implementation scope is a correct automated gate plus aligned evidence.

## Exit criteria

Set an explicit minimum headroom target before coding.

The exact number should be justified from the expected next iterations rather than chosen ceremonially.

At minimum:

- effective shared D/IRAM headroom meets the explicit floor;
- before/after map and placement analysis are retained;
- both profiles build and the gate passes in CI;
- no regression in interrupt/cache-disabled/control requirements.

## Closeout evidence — 2026-08-07

- **Before/after:** effective headroom is 232,272 B before and after; no code or
  configuration was moved. The change corrects the capacity model and prevents
  future regressions with a 196,608 B gate.
- **Evidence retained:** each profile artifact includes the linker map and its
  SHA-256, `size`, `size-components`, `size-files`, generated configuration and
  human/machine-readable gate results.
- **Verified:** 14 focused parser/CLI tests, the normal and sanitizer host suites,
  both ESP-IDF 5.4.1 profile builds and the gate against both maps. PR #8 merged as
  `cf726482a1aed453b749b0338c1fad789f0fdc52`; post-merge workflow
  [`31239836099`](https://github.com/juanpmz2001/sistema-motriz-rs485/actions/runs/31239836099)
  passed all four jobs and uploaded complete evidence for both profiles.
- **Not verified:** no hardware, runtime heap, stack high-water, watchdog, timing or
  endurance claim was tested.
- **Current hardware gate:** Iteration C remains `BLOCKED BY HARDWARE`; no L2–L5
  result is marked `PASS`.
- **Next recommended milestone:** execute the Iteration C read-only L2 communication
  gate on the named single-controller bench setup, then proceed only from real
  evidence and operator authorization.

The placement classification below is based on clean ESP-IDF 5.4.1 maps and
temporary builds outside the repository. Sizes are internal executable bytes from
the baseline component report unless the row states a map-derived delta.

| Candidate/category | Baseline or measured delta | Classification and decision |
| --- | ---: | --- |
| BotFarms components | 0 B internal code | No `IRAM_ATTR`, `DRAM_ATTR`, custom linker fragment or `ESP_INTR_FLAG_IRAM`; nothing project-owned should be moved. |
| Vectors, startup and cache/flash-disabled paths | Linker-required | Must remain internal; moving them would violate boot, interrupt or cache-disabled requirements. |
| FreeRTOS | 16,147 B; placing its reviewed non-ISR subset in Flash recovered 10,240 B in a temporary map | Configurable, but deferred: control/safety latency and cache behavior need runtime evidence, and the floor already passes. |
| Wi-Fi/PHY | `libpp` 6,032 B and PHY 5,053 B; disabling the current Wi-Fi IRAM optimization recovered 7,424 B | Configurable throughput trade-off; defer until network/control-path measurements justify it. |
| Ring buffer | 4,600 B; non-ISR subset can be configured for Flash | Configurable but second-order; no change is needed to meet the target. Keep ISR dependencies internal. |
| SPI flash, HAL, hardware support and heap | 11,505 B, 9,172 B, 7,537 B and 7,327 B | Keep current placement. These paths cross flash/cache, ISR, allocator or timing concerns and are not justified optimization targets. |

Increasing the instruction-cache size would merely reclassify memory while consuming
more physical SRAM. It is not a headroom fix. No cache, FreeRTOS, Wi-Fi, ring-buffer,
heap or ISR setting changed in this iteration.

## Explicitly defer

- micro-optimizing flash size;
- binary-size competitions;
- premature RAM pooling;
- deleting diagnostics solely to save bytes.

---

# 6. Iteration C — Traction hardware qualification

## Objective

Physically prove the traction path from application-level velocity command to motor response.

## Motivation

Before the robot is allowed to drive, we need evidence that:

```text
profile
→ application command
→ endpoint mapping
→ capability
→ driver
→ controller
→ motor
→ observation
```

all agree.

The current single-motor profile exists specifically to make this possible without pretending the entire robot is connected.

## Sequence

### C1 — PCB/controller bring-up

L2 only.

- power;
- firmware identity;
- profile identity;
- RS485 communication;
- read-only SVD48 identity/status data only; the motion-producing
  `SVD48_IDENTIFY` routine is forbidden at L2;
- no motion.

### C2 — SVD48 device qualification

L3.

- M1/M2 mapping;
- RPM sign;
- current;
- voltage;
- temperature;
- position;
- errors;
- stop;
- timeout.

### C3 — Single endpoint capability test

L4.

Use application/capability interfaces.

Commands such as:

```text
+1 RPM
STOP
+5 RPM
STOP
-1 RPM
STOP
-5 RPM
STOP
```

with short bounded durations.

### C4 — Closed-loop traction test

L5.

Compare commanded velocity against:

- controller-reported velocity first;
- independent encoder second, when available.

### C5 — Four-wheel unloaded qualification

Only after one motor is understood.

Test each traction endpoint independently, then small coordinated sets.

## Critical rule

Do not implement these tests by writing SVD48 registers from the HIL test.

L3 may inspect SVD48.

L4/L5 must exercise the generic capability/application path.

## Exit criteria

For every traction endpoint intended for the robot:

- direction known;
- unit known;
- reasonable steady-state response;
- stop works;
- stale/offline detected;
- command timeout behavior understood;
- electrical current within expected range;
- physical evidence recorded.

## Explicitly defer

- maximum speed;
- payload;
- hill climbing;
- regenerative-braking optimization;
- long endurance;
- performance tuning.

---

# 7. Iteration D — Steering actuator and position feedback qualification

## Objective

Create and physically validate the steering path as a first-class position capability.

## Motivation

The production robot cannot safely move simply because traction works.

Steering must be:

- commanded through the architecture;
- measured independently;
- bounded;
- repeatable;
- diagnosable.

If steering is still controlled through legacy direct PWM paths, mobility tests will remain hardware-specific and unsafe.

## Required architecture

Introduce only what is needed:

```text
PositionActuator capability
→ steering actuator adapter/driver
→ physical servo/actuator
```

and:

```text
PositionObservation capability
→ encoder driver
→ physical steering position
```

If the current encoder is AS5600 or another device, treat it as a sensor device rather than embedding it inside servo code.

## Required profiles

At least:

```text
single_steering_actuator
single_steering_actuator_with_encoder
```

or equivalent profiles.

No fake additional steering units.

## Tests

### D1 — Servo output L3

Prove command generation.

### D2 — Encoder L3

Prove raw-to-angle conversion, sign, wrap and freshness.

### D3 — Position capability L4

Command a safe angle through the application boundary.

### D4 — Steering closed loop L5

Compare commanded and independently observed angle.

Measure:

- error;
- sign;
- settling;
- repeatability;
- limit behavior;
- stop/disable behavior.

## Exit criteria

A steering endpoint can be replaced by another implementation without changing the generic position test.

A successful PWM command is never reported as proof that steering physically reached the target.

D1–D4 have been executed on the named hardware with recorded profile/SHA identity,
bounded motion, automatic best-effort cleanup and independent E3 position feedback.
Commanded angle, measured angle, sign, wrap, freshness, settling, repeatability,
limits and stop/disable behavior meet the declared acceptance bounds. Without that
physical evidence, Iteration D remains incomplete.

## Explicitly defer

- sophisticated steering control loops if the physical actuator already closes its loop adequately;
- auto-calibration unless mechanically necessary;
- arbitrary actuator families not used by the current robot.

---

# 8. Iteration E — Generic observation/telemetry boundary

## Objective

Make closed-loop and HIL tests independent of concrete device telemetry.

## Motivation

Without this boundary, high-level tests will gradually depend on:

- SVD48 snapshots;
- AS5600 APIs;
- servo-specific state;
- CAN driver structures.

That would undermine controller interchangeability.

## Required scope

Expose typed observations such as:

```text
velocity
position
current
temperature
health
```

Each observation should include:

- value;
- semantic unit;
- timestamp;
- valid/stale state;
- source endpoint;
- status/error.

Add a small application-facing observation/telemetry port.

Do not create a huge generic sensor abstraction.

## Important design rule

Actuation and observation must remain separate.

A `VelocityActuator` does not automatically imply a `VelocityObservation`.

A motor controller may provide both.

Another actuator may need an external sensor.

## Exit criteria

- the application-facing typed observation port exposes the value, semantic unit,
  timestamp, validity, freshness, source endpoint and status needed by the planned
  L5 tests;
- host tests cover valid, stale, offline and unavailable observations without
  concrete-device imports;
- actuator and observation endpoints remain independent contracts; and
- the same L5 closed-loop test can combine:

```text
actuator endpoint A
observation endpoint B
```

without knowing their drivers. Meeting this software exit does not assert that any
physical observation or closed-loop gate passed.

## Earlier minimal slice

If Iteration A discovers that HIL cannot be done cleanly without this boundary, implement the minimal version there and treat Iteration E as an expansion/cleanup.

Do not duplicate the concept twice.

---

# 9. Iteration F — Motion service and geometry activation

## Objective

Create the application-level motion path required for robot mobility tests.

## Motivation

A mobility test must command robot motion, not four motor registers.

The system should be able to express:

```text
vx
vy
wz
```

or another explicit body-motion contract and transform it into logical endpoint setpoints.

## Required changes

- activate or replace the existing kinematics model through a real motion service;
- map geometry to logical endpoint roles;
- map roles to endpoints using the selected profile;
- migrate `MOVE_VEL` away from direct legacy `robot_control` writes;
- keep hardware-specific details below endpoint capabilities.

## Geometry philosophy

Geometry owns:

```text
robot motion → logical actuator setpoints
```

Geometry does not own:

```text
SVD48
CAN
PWM
Modbus
GPIO
```

## Required host tests

For every geometry:

- zero command;
- forward;
- reverse;
- rotation;
- saturation;
- sign convention;
- steering angles;
- impossible/singular states where applicable.

These should be pure L1 tests.

## Exit criteria

- the body-motion service is active in the intended runtime/profile and maps reviewed
  geometry roles to logical endpoints;
- `MOVE_VEL`, or its explicitly versioned successor, no longer performs direct legacy
  actuator writes;
- the zero, forward, reverse, rotation, saturation, sign, steering and invalid-geometry
  host cases above pass; and
- a mobility test can issue a body command without knowing the installed controller
  type.

This is a software/application-path gate, not evidence that the chassis moved.

## Explicitly defer

- path planning;
- localization;
- ROS;
- autonomous navigation;
- optimization of steering transitions.

---

# 10. Iteration G — Minimum safe motion authority

## Objective

Make full-chassis motion safe enough for controlled floor testing.

## Motivation

At this point the robot has individually qualified traction and steering.

The major remaining risk is not “does the motor work?”

It is:

> what happens when multiple command sources, stale commands, faults and stop requests interact while the complete robot can physically move?

This iteration is required before chassis mobility.

## Minimum required behavior

### One actuation owner

Every physical motion-changing operation must go through one owner/boundary.

Legacy physical writers must be:

- migrated;
- or disabled in the field-test profile.

### Stop precedence

Stop must not wait behind normal queued movement indefinitely.

It needs explicit priority semantics.

### Command lifetime

Motion commands require bounded validity.

A lost source must result in stop.

### Active source

The firmware must know which source currently owns motion.

For the first field version, the number of permitted sources can be intentionally small.

For example:

```text
RC
engineering serial
```

There is no requirement to support every future source immediately.

### Deadman

The chosen active command source must continuously demonstrate that its motion command is alive.

### Minimum state model

Do not build a huge state machine.

A first useful model can be approximately:

```text
BOOT
SAFE_IDLE
ACTIVE
FAULT
MAINTENANCE
```

or another reviewed equivalent.

The key requirement is that outputs do not become active merely because a transport sends a command at the wrong lifecycle stage.

### Required-health gating

Required traction/steering hardware must inhibit motion when:

- offline;
- stale;
- faulted;
- not initialized.

Optional hardware should not unnecessarily block motion.

## Legacy commands

Before field motion, explicitly audit:

- `ENABLE`;
- `MOVE_VEL`;
- clear fault;
- identify;
- maintenance writes;
- OTA preparation;
- any direct servo command.

A hidden writer capable of moving the robot must not remain reachable outside the owner policy.

## Exit criteria

- one source owns motion;
- loss of source stops the robot;
- stop has defined precedence;
- required hardware faults inhibit movement;
- no known legacy path can unexpectedly move the field-test configuration;
- state transition into motion is explicit;
- tests cover contention and timeout.

Before floor motion, executable host/fault tests must also cover:

- active source disappears → stop;
- command TTL expires → stop;
- deadman is lost → stop;
- two sources contend → deterministic result;
- required encoder becomes stale → inhibit;
- controller goes offline → inhibit;
- stop arrives during motion → stop precedence;
- one actuator fails → reviewed safe partial-failure result; and
- boot completes with no spontaneous actuation.

## Explicitly defer

- complex multi-client lease protocols;
- distributed authority;
- ROS arbitration;
- product-certified functional safety.

The first version needs a simple, understandable authority system, not the final general one.

---

# 11. Iteration H — First controlled chassis motion

## Objective

Move the complete robot physically for the first time under a controlled test plan.

## Motivation

This milestone validates that the combined architecture and hardware produce intended body motion.

No major architecture should be invented inside this iteration.

If architecture must be rewritten to execute the test, the prerequisite iterations were incomplete.

## Progression

### H1 — Wheels unloaded

Complete robot assembled.

Test:

- all steering endpoints;
- all traction endpoints;
- body-level forward command;
- body-level rotation command;
- stop;
- command expiry; and
- deadman/source loss.

Observe every endpoint.

### H2 — Robot restrained / low-energy floor test

Use:

- lowest practical speed;
- short duration;
- physical E-stop;
- clear motion envelope.

Verify:

- forward sign;
- reverse;
- steering alignment;
- yaw;
- stop distance;
- current and RPM;
- steering error;
- voltage sag;
- command/stop latency; and
- faults or unexpected current.

### H3 — Short free motion

Only after restrained tests.

Short command sequences with strict max speed and duration.

## Evidence

At least:

- firmware SHA;
- profile;
- command stream;
- endpoint observations;
- fault/health;
- independent physical observation;
- stop response.

## Exit criteria

- prerequisite Iterations C–G and every required endpoint gate have recorded passing
  evidence for the exact assembled hardware;
- H1, H2 and H3 execute in order under an authorized bounded plan, with no bypass of
  the body-motion application path;
- independent E4 evidence confirms the declared body motion, while endpoint health,
  command expiry, deadman/source loss and stop response remain within their bounds;
- every cleanup gate passes and the final result is `PASS`; and
- the record identifies firmware, profile, hardware and what was not verified.

This is the first major “the robot moves” milestone.

---

# 12. Iteration I — Pre-field hardening

## Objective

Turn controlled laboratory/floor mobility into a first field-testable development version.

## Motivation

A robot that moves indoors for a few seconds is not yet ready for greenhouse terrain, operators, long cables, radio loss or extended operation.

This iteration should address risks actually observed during HIL and chassis tests.

## Priorities

### Timing

Instrument and measure:

- stop latency as `T0` condition detected, `T1` firmware requests stop, `T2`
  controller accepts stop and `T3` physical motion is approximately zero;
- command expiry latency;
- bus contention;
- polling delay;
- actuator response.

### Task/resource qualification

Measure:

- stack high-water marks;
- heap;
- watchdog behavior;
- IRAM/DRAM headroom;
- long-duration stability.

### Communication loss

Test:

- RC loss;
- LAN loss if LAN is used;
- serial disconnect;
- controller offline; and
- blocked or delayed bus.

### Fault response

Test:

- encoder missing;
- one motor controller missing;
- steering observation stale;
- required endpoint fault;
- ESP32 reset;
- partial application/actuator response.

### Controlled endurance

Run:

- repeated start/stop;
- steering cycles;
- low-speed drive duration;
- polling over time.

The first endurance gate is a meaningful 30–60 minute bounded session. Longer
hundreds-of-hours campaigns are deferred until that result identifies a need.

### Operational runbook

Document:

- power-up;
- pre-drive checks;
- arm;
- move;
- emergency stop;
- fault reset;
- shutdown;
- evidence/log capture.

## Exit criteria

- quantitative bounds are declared and met for stop/expiry latency, polling and
  actuator response;
- stack, heap, watchdog and IRAM/DRAM measurements meet reviewed minimum margins;
- every required communication-loss and fault-response case above reaches its
  declared safe outcome;
- the bounded 30–60 minute endurance gate completes without an unresolved safety or
  resource failure;
- the operational runbook has a reviewed abort path, evidence procedure and named
  operating envelope; and
- a development operator can follow it to execute a bounded field test with known
  failure responses.

Iteration I remains incomplete if any required measurement is absent, outside its
bound or classified `INCONCLUSIVE`/`ABORTED_FOR_SAFETY`. Its exit is not production
qualification.

## Explicitly defer

- certification;
- public-network security;
- autonomous operation;
- production manufacturing qualification;
- full unattended runtime.

---

# 13. First field version

The first field version is not “production-ready.”

Its purpose is to answer:

- Does the robot physically move in the greenhouse?
- Does steering behave under real ground loads?
- Are currents and temperatures reasonable?
- Does RS485 remain reliable?
- Is the geometry correct?
- Are mechanical ratios adequate?
- Does stop behavior remain acceptable?
- Which assumptions fail outside the bench?

Recommended constraints:

- supervised;
- low speed;
- defined test area;
- physical E-stop;
- trusted/offline control network;
- no autonomous navigation;
- limited session duration;
- logging active;
- explicit operator checklist.

The field version should be treated as a measurement instrument for the next engineering decisions.

---

# 14. Work deliberately deferred until after first field movement

Unless one of these becomes an actual blocker, do not let them delay first field testing.

## ROS / micro-ROS

Useful later.

Not needed to prove traction, steering, safety ownership or basic motion.

---

## Dynamic JSON/YAML firmware profiles

The current build-selected profile model is sufficient for initial physical qualification.

A generator/toolchain can come later.

---

## General factory support for every possible driver

Add the drivers required by actual hardware.

Do not build hypothetical drivers for elegance.

---

## Full CAN architecture

Add CAN when a required physical controller needs it.

Do not implement it solely because the architecture should someday support it.

---

## Complex UI/dashboard

Command-line HIL tooling and structured evidence are enough initially.

---

## Production OTA/security

For controlled bench and supervised field tests, use a trusted network and explicit operating controls.

Before deployment beyond that environment, signed OTA, key management and threat controls become mandatory.

---

## Final ROS transport contract

Do not freeze a ROS-facing API before the embedded state/authority/motion semantics are proven physically.

---

## Exhaustive telemetry

Expose observations needed for testing and safety.

Do not turn the ESP32 into a general analytics platform.

---

## Maximum performance tuning

First prove correctness and safety at low speed.

Then optimize.

---

# 15. Agent decision framework

When a coding agent finds an unexpected problem during one of these iterations, it should ask internally:

### Question 1

Does this issue prevent the current physical milestone from being tested safely or meaningfully?

If yes, fix it.

If no, record it and consider deferral.

### Question 2

Is the proposed fix at the layer that owns the problem?

Examples:

- geometry problem → kinematics;
- SVD48 frame problem → SVD48 driver/protocol;
- wrong endpoint topology → profile/composition;
- motion source conflict → authority/owner;
- HIL reporting problem → host runner.

Do not fix a higher-layer symptom inside a low-level driver.

### Question 3

Will the fix make a generic layer depend on a specific controller?

If yes, redesign the boundary before proceeding.

### Question 4

Is the developer introducing a new abstraction with only one speculative use?

If yes, prefer the smallest contract that solves the current real requirement.

### Question 5

Can this work live on the host instead of consuming embedded resources?

Test orchestration, data analysis, reports and manifests should generally stay host-side.

### Question 6

Does this change alter motion or safety behavior?

If yes:

- update safety documentation;
- add tests;
- describe what was physically verified;
- do not claim field readiness from compilation alone.

---

# 16. What “closing the refactor” should mean

The refactor should not be considered closed when every legacy component is deleted.

It should be considered closed for the first field milestone when:

1. hardware topology is profile-driven;
2. device drivers are behind capabilities;
3. traction and steering can be qualified independently;
4. mobility enters through a hardware-agnostic motion service;
5. all physical motion writers are governed by one authority/owner boundary;
6. commands expire safely;
7. stop has explicit precedence;
8. required hardware health can inhibit motion;
9. the robot has a minimal explicit operating lifecycle;
10. HIL tests can observe behavior without concrete-driver dependencies;
11. new agents have enough documentation to preserve these boundaries.

Some legacy compatibility code may remain after that point if it is unreachable from the field-test operating path.

Deleting legacy code is cleanup.

Eliminating unsafe or architecturally ambiguous behavior is the actual requirement.

---

# 17. Practical priority summary

## Do now

- measure and enforce memory headroom before substantial embedded growth;
- PCB bring-up;
- single-motor traction qualification;
- steering capability + encoder qualification;
- generic observations sufficient for closed-loop tests.

## Do before full robot floor motion

- motion service/kinematics path;
- one motion owner;
- stop precedence;
- TTL/deadman;
- explicit active source;
- minimal state model;
- required-health gating;
- disable/migrate hidden legacy motion writers.

## Do before first greenhouse field test

- low-speed chassis qualification;
- fault/disconnect tests;
- measured stop latency;
- stack/resource qualification;
- operator runbook;
- supervised low-speed envelope.

## Do later

- ROS;
- runtime profile loading;
- generalized profile generator;
- every hypothetical controller;
- production OTA/security;
- autonomy;
- dashboards;
- product certification.

---

# 18. Success criterion

The roadmap is succeeding if each iteration produces a new physical capability that can be tested with stronger evidence while reducing the number of architecture assumptions the test must know.

The intended progression is:

```text
we can communicate
→ we can command one actuator
→ we can measure one actuator
→ we can close one subsystem loop
→ we can command robot motion
→ we can stop robot motion predictably
→ we can move the chassis
→ we can test in the field
```

The team should not optimize for the number of refactor tasks completed.

It should optimize for **safe, interpretable physical learning without accumulating new architectural debt**.
