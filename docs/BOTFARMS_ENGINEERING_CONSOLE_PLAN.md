# BotFarms Engineering Console — Architecture and Iteration Plan

> **Status:** Initial architecture and implementation roadmap  
> **Scope:** Local engineering console for BotFarms robots and embedded controllers  
> **Primary goal:** Make physical development, parameterization, testing, telemetry capture and hardware qualification useful from the first iteration, while establishing architectural boundaries that can scale to new controllers, sensors, actuators and robot configurations.

---

# 1. Purpose

BotFarms needs an engineering environment that removes the need to develop the robot by manually sending commands through terminals, Postman, ad-hoc UDP packets, raw serial commands or controller-specific scripts.

The Engineering Console must become the main development interface for:

- discovering and connecting to BotFarms controllers;
- inspecting firmware identity, profile, topology and health;
- monitoring live telemetry;
- recording reproducible datasets;
- parameterizing hardware;
- controlling individual actuators during bench qualification;
- calibrating sensors;
- executing repeatable HIL/physical experiments;
- comparing commanded and observed behavior;
- preserving test evidence;
- eventually controlling complete robot motion through the correct firmware application boundary.

The console must be useful **before** the complete long-term architecture exists.

The implementation strategy is therefore:

> define a small set of stable architectural boundaries early, then grow the product through vertical slices that each deliver an immediately usable engineering capability.

The project must not wait until a tenth iteration to become useful.

---

# 2. Relationship with the firmware repository

The Engineering Console and the ESP32 firmware must live in **separate Git repositories**.

Expected local layout:

```text
<botfarms-workspace>/
├── sistema-motriz-rs485/
└── botfarms-engineering-console/
```

The firmware repository owns:

- embedded safety;
- real actuator control;
- bus/device drivers;
- hardware profiles;
- endpoint composition;
- capabilities exposed by the embedded controller;
- observations produced by the firmware;
- maintenance/control/telemetry protocol implementation;
- lifecycle and motion authority;
- real-time priorities;
- hardware-facing validation.

The Engineering Console repository owns:

- developer UX;
- host-side communication clients;
- hardware parameter editors;
- telemetry aggregation;
- dataset recording;
- experiment orchestration;
- visualization;
- presets;
- comparison tools;
- test/session metadata;
- host-side evidence and analysis.

Codex or another development agent may modify both repositories when a vertical slice requires a firmware capability and its corresponding console feature.

A feature must still preserve ownership boundaries.

---

# 3. Central architectural rule

The Console is **not** a second firmware implementation.

Normal high-level features must not control hardware by directly implementing:

- Modbus;
- SVD48 runtime actuation registers;
- PWM;
- I2C;
- CAN frames;
- GPIO logic;
- controller-specific motion behavior.

Those remain firmware responsibilities.

The normal dependency direction is:

```text
Frontend
   ↓
Engineering backend
   ↓
Application-level Robot Client
   ↓
Firmware protocol adapter
   ↓
ESP32 application boundary
   ↓
Capabilities / endpoints
   ↓
Device adapters
   ↓
Drivers / buses
   ↓
Physical hardware
```

Hardware-specific parameterization is an intentional exception:

```text
Svd48ParameterProvider
As5600ParameterProvider
FutureControllerParameterProvider
```

may know vendor-specific parameters and register semantics because their purpose is explicitly to expose the configuration model of that physical device.

That hardware knowledge must remain encapsulated inside the relevant hardware feature.

For example:

```text
Svd48ParameterProvider → SVD48 register semantics
```

is acceptable.

But:

```text
ExperimentEngine → SVD48 register 0x5304
```

is not.

Similarly:

```text
Steering UI → AS5600 raw I2C
```

is not.

---

# 4. Architecture target

The initial conceptual architecture is:

```text
┌──────────────────────────────────────────────────────────┐
│                   Engineering Frontend                   │
│              React + TypeScript + Vite                   │
│                                                          │
│ Overview │ Telemetry │ Parameters │ Experiments │ ...    │
└──────────────────────────┬───────────────────────────────┘
                           │
                    HTTP + WebSocket
                           │
┌──────────────────────────▼───────────────────────────────┐
│                 Engineering Backend                     │
│                  Python + FastAPI                       │
│                                                          │
│ Robot Registry                                           │
│ Robot Client                                             │
│ Parameter Services                                       │
│ Telemetry Hub                                            │
│ Recorder                                                 │
│ Experiment Engine                                        │
│ Evidence / Session Metadata                              │
│                                                          │
│ Firmware Adapters                                        │
│ ├── Maintenance LAN                                      │
│ ├── Serial fallback                                      │
│ ├── Telemetry LAN                                        │
│ └── Future Control LAN                                   │
└──────────────────────────┬───────────────────────────────┘
                           │
                         LAN
                           │
┌──────────────────────────▼───────────────────────────────┐
│                         ESP32                            │
│                                                          │
│ Engineering / Maintenance API                            │
│ Observation / Telemetry API                              │
│ Future Control API                                       │
│             ↓                                            │
│ Application services / capabilities                      │
│             ↓                                            │
│ Device adapters / drivers                                │
│             ↓                                            │
│ RS485 / CAN / PWM / I2C / ...                           │
└──────────────────────────────────────────────────────────┘
```

This target is directional, not a mandate to build every box immediately.

Each iteration should add only the subset required for its vertical feature.

---

# 5. Stable host-side concepts

The first iterations should establish a small “thin waist” that later features reuse.

Avoid prematurely creating a large plugin framework.

The important concepts are:

## 5.1 RobotConnection

Represents connectivity to one embedded controller.

It should hide whether the connection currently uses:

- Maintenance LAN;
- serial;
- direct ESP AP;
- a future transport.

The rest of the console should not repeatedly implement sockets or serial ports.

## 5.2 RobotIdentity

Represents enough identity to know what is being tested.

Examples:

- robot/controller ID;
- IP;
- firmware Git SHA;
- firmware dirty state if available;
- board;
- profile;
- protocol version;
- composition/runtime-ready state.

Every recorded physical session should retain this context.

## 5.3 Endpoint / Capability

Represents semantic hardware behavior.

Examples:

```text
VelocityActuator
PositionActuator
VelocityObservation
PositionObservation
CurrentObservation
HealthObservation
Stoppable
```

High-level UI and experiments should prefer semantic endpoints over hardware-specific device details.

## 5.4 Parameter

Represents something configurable.

Suggested conceptual fields:

```text
key
display_name
description
group
type
unit
current_value
default_value
min
max
enum_values
writable
persistent
requires_stop
requires_confirmation
requires_restart
danger_level
```

Not every parameter needs every field.

A parameter may be firmware-owned, device-owned or host/session-owned.

## 5.5 Observation

Represents a measured value.

Suggested conceptual fields:

```text
robot_timestamp
host_timestamp
endpoint
signal
value
unit
valid
stale
health/status
```

Different physical quantities should preserve their semantics.

Avoid one untyped `SensorValue` contract if it removes useful meaning.

## 5.6 Experiment

Represents a repeatable engineering procedure.

Conceptually:

```text
preconditions
parameter set
baseline capture
commands
duration
observations
assertions
cleanup
evidence
```

The Experiment Engine must operate on application concepts and capabilities.

It must not directly encode controller registers.

---

# 6. Communication architecture

The Console should not require one universal protocol for every responsibility.

Use separate planes when semantics differ.

## 6.1 Engineering / maintenance plane

Purpose:

- discovery;
- identity;
- configuration;
- diagnostics;
- read/write parameters;
- bench-safe commands;
- test support;
- OTA/maintenance actions where appropriate.

Initial implementation should reuse the firmware capabilities that already exist rather than redesign the embedded API before the Console becomes useful.

The backend must hide legacy wire details behind an adapter.

Example:

```text
RobotClient
   ↓
MaintenanceLanAdapter
   ↓
current firmware maintenance protocol
```

The frontend must not know that a semantic operation temporarily maps to an ASCII command.

This makes later protocol evolution possible without rewriting the UI.

## 6.2 Telemetry plane

Telemetry is important from the beginning because physical development needs datasets, not only live gauges.

The target behavior is:

```text
embedded polling/control
        ↓
cached observations
        ↓
low-priority telemetry publisher
        ↓
host TelemetryHub
        ├── live UI
        ├── recorder
        └── experiment engine
```

### Telemetry invariants

1. Telemetry must never outrank control or safety.
2. The UI must not trigger additional physical I/O simply because it wants a faster refresh.
3. Telemetry should consume cached observations produced by the normal firmware polling paths.
4. Telemetry publication must be bounded and low priority.
5. Telemetry must not wait indefinitely for the computer.
6. A slow/disconnected receiver must not block control.
7. Dropping telemetry samples is preferable to blocking robot control.
8. Sequence numbers should allow the host to detect dropped samples.
9. Dataset recording happens on the computer.
10. Disconnecting the Engineering Console must not change robot motion state.
11. Telemetry failure must never prevent STOP.

### Initial transport

A practical first push implementation may use UDP.

A packet should carry enough context to support analysis:

```text
protocol version
sequence
robot monotonic timestamp
observations
validity / health
```

JSON is acceptable initially at modest rates.

Do not introduce CBOR, protobuf or custom binary encoding until measurement demonstrates that JSON is inadequate.

### Frequency separation

Acquisition, transmission and UI rendering do not need the same rate.

Example:

```text
physical polling       ~30 Hz
ESP → daemon stream    ~20–30 Hz
dataset recording      ~20–30 Hz
browser rendering      ~10–15 Hz
```

These are examples, not hard-coded architecture requirements.

The actual values should be measured against firmware load and physical usefulness.

## 6.3 Motion/control plane

Bench engineering commands and complete robot motion are not equivalent.

Early workshop features may reuse the maintenance plane for explicitly authorized bench operations.

Complete robot movement must later use the firmware's safe motion/control path with:

- authority;
- TTL;
- deadman;
- stop precedence;
- lifecycle;
- required-health gating.

The Console architecture should therefore define a `MotionClient` boundary early but defer its final transport implementation until the firmware control plane is ready.

The UI should not need a redesign when that transition occurs.

---

# 7. Telemetry recording and datasets

Recording is a first-class Engineering Console capability.

The objective is to make physical behavior analyzable after the experiment.

Initial storage should remain simple.

Recommended:

```text
<session>.csv
<session>.json
```

The CSV stores time-series observations.

The JSON stores session metadata and events.

Possible long-form CSV fields:

```text
host_timestamp
robot_timestamp_us
sequence
endpoint
signal
value
unit
valid
stale
health
```

Session metadata should include when available:

```text
session_id
robot/hardware identity
PCB revision
firmware SHA
firmware profile
protocol version
console version
start/end
parameter snapshot
experiment definition
operator notes
result
```

High-volume raw data should not be committed to either source repository.

---

# 8. Event recording

Datasets should contain more than sensor samples.

Important engineering events should be recorded alongside observations.

Examples:

```text
PARAMETER_WRITE
PARAMETER_SAVE
COMMAND
STOP
FAULT
HEALTH_CHANGE
TEST_START
TEST_END
CONNECTION_CHANGE
```

This allows later analysis such as:

> How did the velocity transient change immediately after Kp was changed?

The host should therefore conceptually process:

```text
TelemetryHub
├── ObservationEvent
└── SystemEvent
```

Exact implementation can evolve.

---

# 9. Parameter ownership

Three parameter categories must remain distinct.

## 9.1 Firmware/profile topology

Examples:

- which devices exist;
- bus assignments;
- endpoint composition;
- robot geometry.

Initially these remain firmware/build-profile configuration.

Do not make them mutable from the Console simply because a form can be built.

## 9.2 Persistent hardware/runtime parameters

Examples:

- controller PID gains;
- acceleration/deceleration;
- controller current limit;
- motor direction;
- encoder offset;
- steering calibration;
- safe runtime settings.

These may live in:

- external controller registers;
- ESP NVS;
- another hardware device.

The Console may expose them through typed providers.

## 9.3 Experiment/session parameters

Examples:

- requested RPM;
- duration;
- tolerance;
- sample count;
- selected signals;
- sweep range.

These live in the host tooling and should not unnecessarily consume embedded configuration state.

---

# 10. Safety and architectural rules for the Console

The Engineering Console is a development tool, not a safety controller.

It must preserve firmware authority.

Mandatory principles:

- never weaken firmware safety to make a UI operation succeed;
- never treat UI state as physical truth;
- distinguish commanded values from observed values;
- high-level motion features must use semantic firmware boundaries;
- raw register tools must be explicitly marked advanced/low-level;
- operations with physical side effects require appropriate confirmations;
- experiment cleanup must attempt STOP after success, failure, timeout and cancellation;
- host STOP is still best effort and does not replace a physical cutoff;
- recording/plotting must never block firmware control;
- every physical dataset should identify the firmware/hardware tested;
- the Console must clearly communicate when a result is unverified or based only on controller-derived feedback.

---

# 11. Repository structure target

The exact structure may evolve if implementation experience provides a strong technical reason.

Initial direction:

```text
botfarms-engineering-console/
├── AGENTS.md
├── README.md
├── docs/
│   ├── ARCHITECTURE.md
│   └── ENGINEERING_CONSOLE_ROADMAP.md
├── backend/
│   ├── pyproject.toml
│   └── botfarms_console/
│       ├── domain/
│       ├── application/
│       ├── firmware/
│       ├── telemetry/
│       ├── recording/
│       ├── experiments/
│       ├── hardware/
│       │   ├── svd48/
│       │   ├── as5600/
│       │   └── steering/
│       └── api/
└── frontend/
    └── src/
        ├── core/
        ├── api/
        ├── telemetry/
        └── features/
            ├── overview/
            ├── recording/
            ├── svd48/
            ├── experiments/
            ├── encoder/
            └── steering/
```

Do not create empty abstractions/directories merely to satisfy this diagram.

Only create structure as real code requires it.

---

# 12. Technology baseline

Initial technology choice:

| Concern | Choice |
| --- | --- |
| Frontend | React + TypeScript |
| Frontend tooling | Vite |
| Backend | Python 3.12+ |
| HTTP API | FastAPI |
| Browser live updates | WebSocket |
| Engineering ESP communication | Existing maintenance LAN adapter initially |
| Serial | Fallback / compatibility / special HIL use |
| ESP telemetry push | UDP initially when implemented |
| Backend models | Pydantic where useful |
| Experiment/test specs | JSON or YAML; choose one deliberately |
| Recording | CSV + JSON metadata initially |
| Desktop packaging | Deferred |
| ROS integration | Deferred |

Technology changes are allowed if Codex finds a strong technical reason.

Any change to this baseline should document:

- the problem with the proposed baseline;
- alternatives considered;
- why the new option better serves the actual engineering objective;
- migration cost.

---

# 13. Iteration philosophy

Iterations must be **vertical slices**.

Avoid:

```text
Iteration 1: backend
Iteration 2: frontend
Iteration 3: communication
Iteration 4: telemetry
Iteration 5: hardware
```

Prefer:

```text
Iteration 1: I can connect, observe and record
Iteration 2: I can parameterize SVD48
Iteration 3: I can characterize/tune a motor
Iteration 4: I can observe/calibrate AS5600
Iteration 5: I can command and measure steering
```

Each iteration may touch:

- firmware;
- backend;
- frontend;
- docs;
- tests;

but should do so only to deliver its single user-visible engineering capability.

---

# 14. Iteration 1 — Workshop Console + Live Telemetry + Recording

## Status

**PLANNED**

## Primary objective

Deliver the first Engineering Console that is immediately useful in the workshop.

At the end of this iteration the developer should be able to:

1. start the local backend;
2. open the frontend in a browser;
3. discover or manually connect to an ESP32 on the same workshop LAN;
4. verify firmware/profile identity;
5. see useful motor/platform telemetry;
6. send a safe STOP ALL request;
7. record a telemetry session to CSV plus metadata;
8. use an advanced engineering console for currently supported maintenance commands.

The iteration is not complete if it only creates scaffolding.

There must be an end-to-end working vertical feature.

## 14.1 Backend foundation

Implement the smallest durable architecture required for later features.

At minimum evaluate and implement:

```text
RobotConnection
RobotIdentity
FirmwareAdapter / MaintenanceLanAdapter
TelemetrySource
TelemetryHub
Recorder
```

Do not build the full future architecture.

The interfaces should be clear enough that:

- polling can later be replaced by push telemetry;
- serial can act as a fallback;
- SVD48 and AS5600 feature modules can be added without changing HTTP plumbing.

## 14.2 ESP discovery and connection

Use the existing workshop LAN first.

Support:

- discovery if reliably available through current firmware;
- direct/manual IP connection as fallback;
- connection state;
- last-seen/error status.

Do not block the iteration on implementing AP mode or mDNS.

Those can be added later.

## 14.3 Identity / overview page

Show useful information such as:

```text
connection state
IP
firmware SHA/version
profile
board
composition/runtime readiness
safety/platform status
```

Do not invent information the firmware cannot currently provide.

## 14.4 Telemetry

The first release must display useful live telemetry.

Prefer using existing cached firmware observations.

If implementing a bounded low-priority UDP telemetry publisher in firmware is a small and well-contained change, it may be included.

If that significantly expands Iteration 1, ship the first usable release using backend polling over the existing engineering interface and keep `TelemetrySource` abstract so push streaming can replace it without changing the frontend/recorder.

The iteration must prioritize working value over perfect streaming architecture.

## 14.5 Recording

Provide:

```text
Start recording
Stop recording
```

Produce:

```text
session.csv
session.json
```

Record:

- timestamps;
- observations;
- identity;
- console version if available;
- current profile;
- session start/end;
- communication/recording metadata.

Do not store the files in Git.

## 14.6 STOP ALL

Expose a prominent STOP ALL control.

The backend should map this through the normal engineering/application command path, not a raw SVD48 register.

The UI must not imply that software STOP replaces a physical emergency cutoff.

## 14.7 Engineering console

Provide an advanced tool for raw currently supported engineering commands.

This is useful during early development.

Clearly mark it:

```text
Advanced / low-level engineering
```

Do not make raw commands the internal implementation of every frontend feature.

## 14.8 Frontend

Initial pages/features:

```text
Overview / Connection
Live Telemetry
Recording
Engineering Console
```

Do not spend time on visual polish beyond what is needed for clear engineering use.

## 14.9 Tests

At minimum:

- backend unit tests for protocol/adapters;
- telemetry fan-out/recording tests;
- API tests;
- frontend tests for critical states where practical;
- fake/simulated adapter so the application can be developed without physical ESP availability;
- no physical PASS claims without real hardware evidence.

## 14.10 Exit criteria

Iteration 1 can be closed when a developer can use the Console end-to-end for a real or fake-connected controller and:

- identify it;
- see live data;
- record a session;
- send STOP ALL;
- use the engineering console;
- restart the backend/frontend reproducibly.

All known limitations must be documented.

---

# 15. Iteration 2 — SVD48 Parameter Lab

## Status

**PLANNED**

## Primary objective

Make the Console useful for real SVD48 configuration and parameter tuning.

This is a high-priority iteration because controller parameters must be tuned before traction behavior can be properly characterized.

## 15.1 Hardware-specific provider

Implement an encapsulated:

```text
Svd48ParameterProvider
```

or equivalent.

It may know SVD48 register semantics.

The rest of the Console should see typed parameters.

## 15.2 Parameter model

Expose only parameters verified against the exact vendor documentation/current firmware assumptions.

Potential groups may include, when actually supported:

```text
speed / velocity
acceleration
deceleration
current/limits
PID/tuning
direction
gear ratio
controller-specific behavior
```

Never infer undocumented registers from adjacency.

## 15.3 Safe edit lifecycle

Preferred workflow:

```text
read current value
→ archive original
→ user edits
→ validate
→ require safe/stopped state when needed
→ write
→ read back
→ compare
→ optionally persist/save
```

Parameter edits should generate system events for recording.

## 15.4 Parameter snapshots

Support:

```text
export snapshot
compare snapshot
restore known snapshot
```

Import/apply should validate model/controller compatibility.

## 15.5 UI

Create a dedicated SVD48 feature page.

Prefer semantic groups.

Raw register access should remain in an advanced section.

## 15.6 Recording integration

When parameters change during a recording session, record events such as:

```text
PARAM_WRITE
old value
new value
parameter key
timestamp
```

This is necessary for later tuning analysis.

## 15.7 Exit criteria

A developer can parameterize a real SVD48 from the Console, verify readback, retain parameter snapshots and correlate changes with recorded telemetry.

---

# 16. Iteration 3 — Motor Tuning and Experiments

## Status

**PLANNED**

## Primary objective

Turn manual motor testing into reproducible experiments.

At the end of this iteration the Console should support controlled motor characterization using the architecture already built.

## 16.1 Experiment Engine

Implement the minimum host-side orchestration for:

```text
preconditions
baseline
parameter set
command
capture
stop
assertion/summary
evidence
```

Preserve the safety invariants of the existing firmware/HIL tooling.

## 16.2 Initial experiments

Examples:

### Velocity step

```text
0 → +1 RPM
0 → +5 RPM
0 → -1 RPM
0 → -5 RPM
```

### Velocity sweep

Example:

```text
1
2
3
4
5 RPM
```

### Parameter comparison

Run equivalent experiments with parameter preset A/B/C.

## 16.3 Signals

Capture available signals such as:

```text
commanded velocity
observed velocity
current
position
bus voltage
health
faults
```

Do not claim independent physical verification until an independent sensor exists.

## 16.4 Dataset/evidence

Each experiment should produce:

- data;
- parameter snapshot;
- experiment definition;
- firmware identity;
- result;
- cleanup outcome.

## 16.5 UI

Create an Experiments feature where the developer can:

- select an experiment;
- enter bounded parameters;
- see progress;
- see plots;
- stop;
- inspect/save the result.

## 16.6 Exit criteria

A developer can run repeatable traction experiments and compare controller tuning using recorded datasets rather than subjective observation alone.

---

# 17. Iteration 4 — AS5600 Position Observation and Calibration

## Status

**PLANNED**

## Primary objective

Add the first hardware feature that is not SVD48 and validate that the architecture scales without redesigning the Console core.

## 17.1 Firmware vertical slice

Implement the required AS5600 path according to the firmware architecture.

Conceptually:

```text
transport
→ AS5600 device
→ observation adapter
→ PositionObservation capability
→ endpoint
```

Do not couple it directly to the steering UI or servo implementation.

## 17.2 Console hardware feature

Implement an AS5600/encoder feature using the existing:

```text
RobotClient
TelemetryHub
Recorder
Parameter model
```

Do not add another networking architecture.

## 17.3 UI

Display when available:

```text
raw angle
physical angle
zero offset
direction
health
freshness
update rate
```

## 17.4 Calibration

Support the actual calibration capabilities required by the physical setup, such as:

```text
set current as zero
offset
direction inversion
```

Distinguish host visualization transforms from persistent firmware/device calibration.

## 17.5 Recording

Encoder observations should automatically join the existing recording pipeline.

A new recorder must not be written.

## 17.6 Exit criteria

The Console records SVD48 motor signals and AS5600 position observations in the same session, and encoder calibration does not require raw I2C access from the UI/backend core.

---

# 18. Iteration 5 — Steering Actuator and Closed-Loop Characterization

## Status

**PLANNED**

## Primary objective

Create a complete steering-development vertical slice:

```text
position command
→ steering actuator
→ physical mechanism
→ position sensor
→ measured position
```

## 18.1 Firmware

Expose steering actuation through a semantic position capability.

Keep the actuator driver and sensor driver separate where they are physically separate devices.

## 18.2 Console

Add a Steering feature that uses:

```text
PositionActuator
PositionObservation
```

rather than direct PWM or direct encoder APIs.

## 18.3 UI

Display:

```text
requested angle
measured angle
error
health
```

Provide bounded engineering commands.

## 18.4 Parameterization

Expose only parameters that exist for the actual steering actuator/system.

Examples may include:

```text
center offset
min angle
max angle
direction
speed/acceleration
controller PID
```

depending on the real hardware.

## 18.5 Experiments

Examples:

```text
0° → +5°
0° → -5°
0° → +10°
0° → -10°
```

Analyze:

- error;
- overshoot;
- settling;
- repeatability.

## 18.6 Exit criteria

A developer can command a steering position from the Console, independently observe the resulting position and record a reproducible closed-loop dataset.

---

# 19. After Iteration 5

The core should now support incremental hardware features without architectural rewrites.

Examples:

```text
new motor controller
new encoder
new servo
new CAN device
battery telemetry
IMU
robot geometry
motion page
fault injection
field test sessions
```

Future work should continue as vertical slices.

A new hardware feature may introduce:

```text
firmware driver/capability
hardware parameter provider
feature page
experiments
```

but should reuse:

```text
connection
identity
telemetry
recording
experiment engine
API
frontend foundation
```

---

# 20. Deferred work

Do not let the following delay the first useful workshop console unless they become actual blockers:

- desktop packaging;
- Tauri/Electron;
- ROS;
- cloud accounts;
- remote access over the internet;
- large databases;
- authentication systems intended for public deployment;
- plugin marketplace/framework;
- protobuf/CBOR optimization without measurements;
- dynamic firmware profile editing;
- autonomous navigation;
- dashboards unrelated to immediate engineering work;
- perfect design-system polish.

---

# 21. Codex decision policy

This roadmap is directional, not dogmatic.

Codex may improve the implementation plan when there is a strong technical reason.

A meaningful deviation should be documented in this roadmap under the relevant iteration.

The decision record should explain:

```text
problem discovered
original assumption
decision
technical reasoning
alternatives considered
impact
```

Do not change architecture merely from preference.

Changes should serve at least one of:

- immediate workshop usefulness;
- safety;
- testability;
- maintainability;
- hardware interchangeability;
- performance actually required by measurement;
- reduction of clear technical debt that blocks the current vertical slice.

---

# 22. Iteration closeout contract

At the end of **every iteration**, update this document.

Do not leave only the planned scope.

Each iteration must gain a closeout section with:

## Implementation status

```text
DONE
DONE WITH DEFERRED ITEMS
BLOCKED
PARTIAL
```

## What was actually implemented

List concrete features and architecture changes.

## Firmware changes

List changes in `sistema-motriz-rs485`, if any.

Include relevant commit/branch references.

## Console changes

List changes in `botfarms-engineering-console`.

Include relevant commit/branch references.

## Tests executed

Distinguish:

```text
host/unit
integration
fake/simulated
physical
```

Never convert a fake test into a physical PASS claim.

## Physical evidence

State explicitly what was physically tested and what was not.

## Deviations from plan

Explain why.

## Decisions made

Record meaningful architecture or product decisions.

## Additional ideas discovered

Capture ideas that may belong to future iterations.

Do not automatically implement them if they are outside the current objective.

## Deferred items

Describe why they were deferred and whether they block the next iteration.

## Known limitations

Be explicit.

## Recommended next step

State whether the next roadmap iteration is ready.

---

# 23. Iteration progress summary

Update this table after every iteration.

| Iteration | Status | Main delivered value | Remaining gate |
| --- | --- | --- | --- |
| 1 — Workshop Console + Telemetry + Recording | PLANNED | Connect, observe, record | Implementation |
| 2 — SVD48 Parameter Lab | PLANNED | Parameterize controller | Iteration 1 foundation |
| 3 — Motor Tuning & Experiments | PLANNED | Reproducible traction characterization | SVD48 control/telemetry |
| 4 — AS5600 Observation & Calibration | PLANNED | Independent steering position feedback | Actual sensor integration |
| 5 — Steering Closed Loop | PLANNED | Command + independently measure steering | Steering actuator + encoder |

---

# 24. Project success criterion

The project is progressing correctly if each iteration adds an immediately useful engineering capability without requiring the core architecture to be rewritten.

The intended near-term progression is:

```text
connect
→ observe
→ record
→ parameterize SVD48
→ characterize motor response
→ add independent encoder
→ characterize steering closed loop
→ add new hardware features incrementally
```

The Console should become useful before it becomes complete.

The engineering objective is not to build a perfect application.

It is to create a durable development environment that accelerates physical learning while preserving the architectural boundaries that will allow BotFarms robots to grow in complexity.
