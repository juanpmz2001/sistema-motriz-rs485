# Testing Architecture Guide

> **Contract version:** 1.0
> **Status:** Current testing contract from `bench-baseline-v1`, including the
> unqualified steering-development preparation. It does not record a physical pass.
> **Operational companion:** [Physical test runbooks](testing/README.md) and
> [evidence template](testing/EVIDENCE_TEMPLATE.md).

## Purpose

This document defines **how to think about testing this firmware and its physical hardware**, not just which test cases to write.

Its primary audience is:

- coding agents such as Codex starting with little or no session context;
- firmware developers adding or validating hardware;
- engineers commissioning a new PCB or robot configuration;
- reviewers deciding whether a test is exercising the correct architectural layer.

The goal is to prevent a common failure mode in a growing robotics repository:

> a developer wants to prove that something works, so they bypass the architecture and call the lowest-level driver that makes the test easy.

A test that passes by bypassing the real application path can create false confidence. A wheel may spin when a Modbus register is written directly while the actual application path, safety boundary, profile mapping, coordinator, timeout semantics, or unit conversions are broken.

Tests must therefore be designed around **what is being validated**, **which layer owns that behavior**, and **what evidence is required to claim success**.

Sections 1–5 and 14 define the normative vocabulary and lifecycle for new tests.
The remaining sections guide commissioning and target architecture. A minimal JSON
manifest runner and typed velocity-observation slice now exist; they do not imply that
every listed observation type, hardware profile or physical gate exists or passed.
Verify available behavior in source, executable tests and the current API before
planning a run.

Increment the major contract version if the meanings of levels, evidence classes,
result states or mandatory cleanup change incompatibly. Use a minor version for
additive guidance that preserves existing test records.

---

# 1. Core testing principle

Every test must answer five questions before code is written:

1. **What behavior are we trying to prove?**
2. **What is the System Under Test (SUT)?**
3. **Through which public boundary should the test stimulate the SUT?**
4. **Through which independent observation should the result be verified?**
5. **Which lower layers is this test explicitly forbidden from bypassing into?**

The default rule is:

> Enter through the public boundary immediately above the behavior being tested. Do not bypass the layer whose behavior the test is supposed to validate.

Examples:

| Test intent | Correct entry point | Hardware-specific knowledge allowed? |
| --- | --- | --- |
| Validate RS485 exchange | `bus_transport` / RS485 diagnostic tooling | Yes |
| Validate SVD48 protocol/device mapping | `svd48_device` | Yes |
| Validate SVD48 M1/M2 channel behavior | channel adapter / device API | Yes |
| Validate “this traction endpoint runs at 5 RPM” | velocity capability / application actuation port | No, ideally |
| Validate “this steering endpoint reaches 30°” | position capability / application actuation port | No, ideally |
| Validate an encoder | observation capability | No transport details in the test logic |
| Validate steering closed loop | steering/position application behavior + position observation | No controller-specific register access |
| Validate robot mobility | motion application service | No SVD48/PWM/CAN assumptions |

A low-level test may know the low-level hardware when that low-level component is the object under test.

A high-level test must not descend into low-level implementation details simply because that is convenient.

---

# 2. Test levels

All physical and software tests should declare a level.

## L0 — Static and configuration validation

No hardware and no firmware runtime required.

Purpose:

- detect impossible profiles;
- detect invalid references;
- verify dependency rules;
- verify supported factories and capabilities;
- detect resource conflicts before hardware is touched.

Typical checks:

- profile schema;
- endpoint uniqueness;
- bus/device/channel references;
- pin conflicts;
- capability compatibility;
- device factory availability;
- geometry validity;
- build-profile selection;
- dependency-contract tests;
- compiler/static analysis.

An L0 failure should normally prevent later hardware tests.

---

## L1 — Host tests

No ESP32 or physical hardware.

Purpose:

- prove deterministic logic independently from FreeRTOS and hardware;
- reproduce failures cheaply;
- verify error and concurrency semantics.

Typical SUTs:

- protocol parsing/building;
- transport contracts with fakes;
- device drivers with fake transports;
- polling;
- capabilities;
- profile/preflight;
- kinematics;
- state models;
- command authority models;
- coordinator behavior;
- application compatibility.

Whenever practical, a new component should have an L1 fake-backed test before it is physically exercised.

---

## L2 — Electrical and communication bring-up

Physical board is powered, but **actuation must remain disabled**.

Purpose:

- determine whether the PCB, wiring, firmware image and buses are healthy enough for further testing.

Examples:

- verify supply rails;
- verify firmware identity and selected profile;
- initialize UART/TWAI/I2C/SPI;
- confirm bus pins and baud rate;
- find expected devices;
- read safe identification/version registers;
- inspect CRC/timeouts;
- inspect communication statistics;
- verify expected devices are present;
- verify intentionally omitted devices are not treated as failures.

At this level it is acceptable to use transport/device diagnostics because those are the layers being validated.

Forbidden by default:

- motor enable;
- non-zero motion setpoints;
- servo motion;
- fault-clear if it changes device state;
- identify routines that can cause motion;
- configuration writes;
- automatic actuation during boot.

---

## L3 — Device and driver qualification

Physical hardware is present and mechanically made safe.

Purpose:

- prove that a concrete hardware driver correctly represents a physical device.

This level may intentionally be hardware-specific.

Examples for an SVD48:

- address mapping;
- M1 versus M2 register mapping;
- signed speed values;
- current/voltage/temperature scaling;
- stop register behavior;
- controller error codes;
- timeout and retry behavior;
- stale/offline transitions;
- polling behavior;
- Modbus exceptions.

Examples for a future CAN motor controller:

- arbitration IDs;
- command encoding;
- status frames;
- watchdog behavior;
- channel mapping.

Examples for a servo driver:

- PWM pulse range;
- direction;
- min/max command;
- disable behavior.

The output of L3 is confidence in a **driver/device integration**, not yet in the robot behavior.

---

## L4 — Capability qualification

This is the first hardware layer that should normally be **agnostic to the specific controller**.

Purpose:

- prove that a configured endpoint implements its declared capability.

Examples:

- velocity actuator reaches requested RPM;
- position actuator accepts position commands;
- stoppable endpoint actually stops;
- position sensor returns valid physical units;
- current observation is available and fresh.

A generic L4 velocity test should be reusable for:

- an SVD48 channel;
- a future CAN controller;
- another motor controller;

provided all of them expose the same velocity capability.

The test must not contain:

- Modbus addresses;
- SVD48 registers;
- CAN frame IDs;
- PWM pin numbers.

Those belong in the profile and the driver.

---

## L5 — Closed-loop subsystem qualification

Purpose:

- prove that actuation produces the expected physical response by comparing an actuator with one or more observations.

Examples:

- motor velocity command + motor/controller velocity feedback;
- motor velocity command + independent encoder;
- steering position command + steering encoder;
- motor command + current response;
- wheel rotation + external position measurement.

The test must describe actuator and observation endpoints independently.

Do not assume that actuator and sensor are provided by the same device.

A future system may use:

```text
CAN motor controller -> VelocityActuator
AS5600/RS485 encoder -> PositionObservation
```

while another configuration may use:

```text
SVD48 -> VelocityActuator + VelocityObservation
```

The test logic should remain reusable.

---

## L6 — Robot behavior / mobility

Purpose:

- prove a robot-level behavior.

Examples:

- forward velocity;
- reverse;
- yaw rotation;
- crab steering;
- steering alignment;
- short trajectory;
- commanded stop from motion.

The test must enter through a **motion/application-level interface**, not through wheel controllers.

For example:

```text
desired body motion
    ↓
motion service / kinematics
    ↓
logical actuator setpoints
    ↓
capability endpoints
    ↓
drivers
```

A mobility test that writes four SVD48 registers directly is invalid as an L6 test.

It may demonstrate that four motors spin, but it does not demonstrate that the robot motion architecture works.

---

## L7 — Fault injection, endurance and qualification

Purpose:

- prove behavior under faults and over time.

Examples:

- unplug an encoder;
- remove a motor controller;
- introduce bus delays;
- force CRC errors;
- expire a command;
- lose LAN/RC;
- trigger E-stop;
- hold a controller offline;
- long-duration polling;
- reboot during operation;
- measure stack high-water marks;
- measure worst-case stop latency.

L7 belongs after the basic hardware path is proven.

It should not be the first way a new PCB or actuator is tested.

---

# 3. Evidence classes

A test must state what evidence is required to claim success.

## E0 — Software acceptance

The software accepted the command.

Example:

```text
SET velocity endpoint = 5 RPM
result = SUCCESS
```

This proves only that the software path accepted the request.

It does not prove physical motion.

---

## E1 — Device command echo / target state

The physical controller confirms the requested target.

Example:

```text
requested target = 5 RPM
controller target register = 5 RPM
```

This proves communication and command mapping.

It still does not prove that the actuator moved.

---

## E2 — Controller-derived physical observation

The controller reports physical feedback.

Example:

```text
observed velocity = 4.9 RPM
```

This is useful evidence, but it is not always independent because the same controller may generate both actuation and observation.

---

## E3 — Independent sensor evidence

A separate sensing path confirms the result.

Example:

```text
motor controller command = 5 RPM
external encoder = 4.8 RPM
```

This is the preferred evidence for physical closed-loop tests.

---

## E4 — System-level physical result

The robot itself demonstrates the intended behavior.

Examples:

- measured distance;
- measured yaw;
- wheel displacement;
- steering angle at the wheel;
- external localization.

E4 is required for high-confidence mobility qualification.

---

# 4. Test specification contract

Every HIL test should be defined before execution using a small declarative specification.

The exact format may be YAML, JSON or another host-side format.

This is **test configuration**, not firmware runtime topology.

Conceptual example:

```yaml
id: traction_velocity_5rpm
level: L4
profile: bench_single_svd48_motor

target:
  endpoint_id: "1"
  endpoint_name: bench_motor
  capability: VELOCITY_RPM

command:
  value: 5
  duration_ms: 3000

observation:
  endpoint_id: "1"
  capability: VELOCITY_OBSERVATION
  evidence_class: E2

acceptance:
  settle_timeout_ms: 2000
  tolerance_rpm: 1.0
  minimum_samples: 10
  maximum_stale_ms: 300

safety:
  unloaded: true
  physical_cutoff_required: true
  maximum_test_duration_ms: 5000

cleanup:
  stop: true
  verify_stopped: true
```

An L4 test specification must not contain implementation details such as:

```yaml
register: 0x5304
modbus_address: 1
uart_num: 1
```

Those details are appropriate only in an L2/L3 hardware-specific test.

---

# 5. Mandatory physical-test lifecycle

Every physical test that can cause motion must follow the same lifecycle.

## 5.1 Identify

Record:

- repository commit SHA;
- firmware version;
- ESP-IDF version;
- build profile;
- PCB revision;
- robot/test fixture identity;
- test-specification version.

Never execute an unknown binary.

---

## 5.2 Validate profile

Before motion:

- confirm the selected profile;
- enumerate expected endpoints;
- enumerate capabilities;
- verify limits;
- verify required/optional hardware;
- verify composition is runtime-ready.

Do not modify code because the wrong profile was selected.

Select or create the correct profile.

---

## 5.3 Establish safe physical conditions

Examples:

- wheel unloaded/off ground;
- robot mechanically restrained;
- steering linkage safe;
- current-limited supply where appropriate;
- physical emergency power cut-off accessible;
- no people in the motion envelope;
- test duration bounded.

The test software must not substitute for physical containment.

---

## 5.4 Read-only baseline

Capture before actuation:

- health;
- faults;
- stale/offline state;
- voltage;
- temperatures;
- sensor position;
- communication statistics.

Unexpected required-hardware faults must block motion.

---

## 5.5 Explicit stop before motion

Issue the normal high-level stop command.

Record the result.

This proves the stop path is reachable before requesting movement.

---

## 5.6 Minimum-energy command

Begin with the smallest useful command and shortest useful duration.

Do not begin qualification at production speeds.

---

## 5.7 Observe continuously

Capture:

- commanded value;
- measured value;
- timestamps;
- freshness;
- health;
- errors;
- current;
- bus statistics where relevant.

---

## 5.8 Automatic cleanup

Every motion test must define cleanup.

At minimum:

```text
STOP
verify STOP response
verify observed motion decreases/stops
record final state
```

The runner must attempt cleanup after every exit it can control:

- success;
- assertion failure;
- timeout;
- keyboard interruption;
- unexpected exception.

A runner cannot guarantee software cleanup after `SIGKILL`, host power loss, cable
loss, a blocked transport or target failure. Physical containment and an independent
person-operated power cut-off therefore remain mandatory. A test that omits an
automatic best-effort stop on catchable exits is not acceptable, and a failed or
unverified cleanup forbids `PASS`.

---

## 5.9 Report evidence

The result must distinguish:

```text
PASS
FAIL
INCONCLUSIVE
ABORTED_FOR_SAFETY
```

- `PASS` means every declared acceptance condition and cleanup gate passed with the
  required evidence for exactly the stated claim.
- `FAIL` means the test executed validly and observed behavior outside an acceptance
  bound or an explicitly failed assertion.
- `INCONCLUSIVE` means transport, observation, provenance or other evidence was
  insufficient to decide the claim even though cleanup and physical containment were
  verified safe; it is preferable to invented confidence.
- `ABORTED_FOR_SAFETY` means a precondition, identity gate, interruption, unsafe
  condition, or failed/unverified cleanup required the run to stop or prevented a
  safe continuation.

---

# 6. New PCB commissioning

A new PCB should follow a reusable commissioning pipeline.

## Stage A — Unpowered inspection

Verify:

- PCB revision;
- assembly/BOM revision;
- polarity;
- connector pinout;
- continuity;
- shorts;
- bus termination;
- common grounds;
- emergency-stop wiring.

---

## Stage B — Power bring-up

Prefer a current-limited source for initial bring-up.

Measure:

- main supply;
- 5 V;
- 3.3 V;
- idle current;
- regulator temperature;
- brownout/reset behavior;
- boot log.

No actuators should move.

---

## Stage C — Firmware identity

Firmware must expose enough information to identify:

- git SHA;
- build profile;
- board profile;
- firmware version;
- expected buses;
- expected devices;
- endpoint/capability inventory;
- normal versus diagnostic startup.

---

## Stage D — Bus qualification

One bus at a time:

- initialization;
- pins;
- bitrate/baud;
- communication;
- timeouts;
- CRC/errors;
- expected devices.

No movement.

---

## Stage E — Read-only device qualification

Read:

- controller version;
- bus voltage;
- temperatures;
- position;
- errors;
- status;
- sensor values.

Verify freshness and stale/offline behavior.

---

## Stage F — Minimum controlled actuation

Use the highest valid application/capability layer available.

Keep the actuator physically unloaded.

---

## Stage G — Closed-loop qualification

Add independent observations where possible.

Validate:

- sign;
- scale;
- repeatability;
- settling time;
- saturation;
- stop;
- fault behavior.

A board should not proceed to robot-level mobility before these stages pass for required hardware.

---

# 7. Single-motor test philosophy

The first useful physical traction profile should contain only the hardware required for that test.

A single-motor test should not instantiate missing motors merely because the production robot has four.

At capability level, the test should look like:

```text
test runner
→ application actuation port
→ velocity endpoint
→ physical motor
→ velocity observation
→ test assertion
```

A separate L3 SVD48 qualification may use:

```text
svd48_device
→ M1
→ Modbus/RS485
```

Do not combine both purposes into one test.

Recommended progression:

1. read-only L2;
2. L3 controller/channel test;
3. L4 +1 RPM;
4. stop;
5. L4 +5 RPM;
6. stop;
7. negative RPM;
8. stop;
9. E2 feedback comparison;
10. E3 independent encoder comparison;
11. timeout/fault behavior.

---

# 8. Servo test philosophy

A position actuator test must distinguish:

```text
commanded position
```

from:

```text
measured physical position
```

Sending PWM corresponding to 30° proves only command generation.

It does **not** prove the mechanism reached 30°.

Preferred architecture:

```text
PositionActuator endpoint
        ↓
servo driver
        ↓
physical steering
        ↓
independent encoder
        ↓
PositionObservation endpoint
```

A generic position test should operate on the two capabilities, not on PWM.

If independent feedback is absent, the report must explicitly limit the claim to command generation.

---

# 9. Adding a sensor

Before implementing a sensor, the agent must answer:

1. What physical quantity is measured?
2. What public unit is used?
3. What is the valid range?
4. What is the expected update rate?
5. What does `valid` mean?
6. What does `stale` mean?
7. What does `offline` mean?
8. Is the observation safety-relevant?
9. Which bus/transport is used?
10. Is the physical device single-channel or multi-channel?
11. Which observation capability does it expose?
12. Is the endpoint required, optional or development-only?
13. How is the driver host-tested?
14. How is the sensor physically qualified?
15. What independent reference can validate its measurement?

For a cyclic position sensor, keep three claims separate: raw phase acquisition,
linearity correction, and mechanical reference. A bidirectional multi-turn analysis
may produce a versioned correction candidate only when its complete passes are
coherent and monotonic; it must retain fixture/input provenance and reject an
incomplete capture. That candidate does not establish physical zero or absolute
angle. Mechanical reference is a separate, explicitly authorized maintenance action,
and raw capture data remains an external evidence artifact.

Preferred dependency direction:

```text
transport
→ device driver
→ device/channel adapter
→ observation capability
→ endpoint registry
→ telemetry/application port
```

Avoid:

```text
gateway → I2C driver
safety → concrete encoder driver
motion → concrete sensor driver
```

---

# 10. Adding an actuator/controller

A new actuator controller should not be integrated by adding conditionals throughout the application.

Required thinking sequence:

1. Identify the physical device and channels.
2. Reuse or add the transport.
3. Implement the device driver.
4. Define which existing capabilities are honestly supported.
5. Add endpoint adapters.
6. Add executable factory/profile support.
7. Add host tests.
8. Add L2/L3 hardware qualification.
9. Reuse existing L4 capability tests.
10. Only after qualification should higher-level robot tests use it.

The architecture is successful if replacing:

```text
SVD48
```

with:

```text
another motor controller
```

requires changes mainly in driver/factory/profile code while velocity and mobility tests remain largely unchanged.

---

# 11. Adding a geometry

Geometry must not know about:

- Modbus;
- SVD48;
- CAN IDs;
- PWM pins;
- UARTs;
- concrete motor driver types.

A geometry maps robot-level intent to logical actuator setpoints.

Conceptually:

```text
body command
(vx, vy, wz)
    ↓
kinematics / geometry
    ↓
wheel velocity setpoints
steering position setpoints
    ↓
logical endpoint roles
```

The profile maps logical roles to physical endpoints.

Tests should be split:

### Geometry L1

Pure host tests.

Example:

```text
vx = 0.2 m/s
vy = 0
wz = 0
→ expected wheel/steering setpoints
```

No hardware.

### Geometry L6

Physical behavior.

Send body-level motion and observe robot-level result.

Do not test geometry by manually commanding four motors.

---

# 12. Host-side HIL runner

Hardware test orchestration should live on the development host, not inside the firmware.

Current minimal structure:

```text
tests/
├── host/
├── fakes/
└── hil/
    ├── AGENTS.md
    ├── README.md
    └── specs/
        └── capabilities/
            └── single_endpoint_velocity_l4.json
tools/
├── hil_runner.py
└── serial_gateway_client.py
```

Add `bringup/`, `devices/`, `closed_loop/` or `mobility/` only when a real test at
that level exists. Do not create empty framework layers.

The current minimal runner is responsible for:

- connecting;
- identifying firmware/profile;
- checking preconditions;
- executing bounded commands;
- reading observations;
- evaluating tolerances;
- enforcing timeouts;
- cleanup stop;
- producing evidence.

Its executable v1 manifest is deliberately narrower than the conceptual schema
above: it accepts only a generic velocity-capability claim at `L4` with
controller-derived `E2` observation. Higher levels and independent evidence classes
require a future runner path that can actually stimulate and observe those layers;
changing only the manifest labels must never strengthen the claim. A v1 motion run
requires a full 40-hex clean firmware SHA, exact board/profile,
exact endpoint inventory, target name/ID/criticality, declared actuation,
observation and stop capabilities, finite duration/timeouts, and all three explicit
operator confirmations. It also requires the composition to be runtime-ready, the
platform to be safe-idle, and the running safety task to report neither motor fault
nor active RC loss before the command. L4 requires a fresh post-command observation whose tolerance
can distinguish the minimum nonzero request from zero. `STOP ALL` must return its
exact acknowledgement, and any motion attempt also requires a fresh stopped
observation during final best-effort cleanup. The run record includes an
operator-supplied firmware artifact SHA-256/reference; because this is traceability,
not on-target image attestation, it must accompany the retained flash/artifact
record. Evidence paths are external and never overwritten.

Do not add firmware-only “test modes” unless a physical behavior is impossible to expose safely through normal engineering/application interfaces.

---

# 13. Generic observation boundary

Closed-loop tests should not depend directly on SVD48 snapshots, AS5600 registers or any concrete sensor API.

The architecture should converge on typed observation capabilities such as:

```text
VelocityObservation
PositionObservation
CurrentObservation
TemperatureObservation
HealthObservation
```

An observation should carry at least:

```text
value
unit/semantic type
timestamp
valid
stale
error/status
source endpoint
```

Avoid one universal untyped `Sensor` interface.

Different physical quantities have different semantics.

The application exposes typed velocity and position-observation ports without
requiring the caller to know the driver. A position observation carries degrees,
timestamp, valid/stale/online state, source endpoint/source class, health,
acquisition status and explicit calibration/reference provenance. `valid` means a
fresh measurement usable in the actuator's logical coordinate system, not merely
that a sensor returned a raw phase. For example, an AS5600 adapter must leave the
generic observation invalid without both a profile-approved LUT and an explicit
operator-established reference. Neither provenance field proves that the physical
reference is correct.

The executable HIL runner remains limited to its velocity manifest. Iteration E is
therefore still incomplete: position HIL/L5 reuse, current/temperature/health
expansion and physical evidence remain open. Do not turn either typed slice into a
universal sensor or let a closed-loop test import a concrete sensor API.

Until that boundary exists, hardware-specific L3 tests may use driver telemetry, but L4/L5 tests should not normalize that workaround into the permanent architecture.

---

# 14. Agent rules for test implementation

An agent creating or modifying tests must obey these rules.

## Rule 1 — State the test layer

Every new test must identify its level L0–L7.

If the layer is unclear, the test design is incomplete.

## Rule 2 — Do not bypass the SUT

If testing motion, do not call the driver directly.

If testing a driver, do not bypass the driver and write the transport directly.

## Rule 3 — Use profiles instead of code edits

If only one motor is connected, use a one-motor profile.

Do not edit loops or comment out errors in production code just to make the test bench look valid.

## Rule 4 — Omitted hardware is not failed hardware

A development profile may intentionally omit devices.

Only hardware declared required by the active profile should be expected.

## Rule 5 — Never weaken safety to make a test pass

Do not:

- lengthen watchdogs without justification;
- suppress faults;
- bypass stop;
- ignore stale observations;
- raise limits;
- convert errors into warnings;

just to obtain a green test.

## Rule 6 — Test through capabilities at L4+

L4, L5 and L6 tests must not import concrete driver/transport interfaces.

This is enforced for current HIL Python and L4+ manifests by the dependency-contract
tests and must remain executable in CI.

## Rule 7 — Every motion test owns cleanup

A motion runner must attempt high-level stop before motion and again on every
catchable exit. Because software cannot survive every failure mode, an independent
physical cut-off is also mandatory; do not describe cleanup as guaranteed.

## Rule 8 — Separate commanded state from observed state

Do not assert:

```text
command succeeded == physical action succeeded
```

## Rule 9 — Evidence must be reproducible

Record:

- test version;
- firmware SHA;
- profile;
- hardware identity;
- relevant measurements;
- result.

Raw high-volume captures should generally be stored as external artifacts, not committed to source control.

## Rule 10 — Report verification boundaries

Every test report must say what was **not** verified.

---

# 15. Documentation routing for agents

The repository root [`AGENTS.md`](../AGENTS.md) is the single task router. HIL work
also inherits the narrower [`tests/hil/AGENTS.md`](../tests/hil/AGENTS.md) contract.
Add another local guide only when a subtree has executable invariants that genuinely
differ; do not copy the root routing table into this guide.

---

# 16. Definition of a good test architecture

The test architecture is working when all of the following are true:

- a new developer can identify the correct layer before writing a test;
- a one-motor bench does not require pretending that four motors exist;
- a new controller can reuse capability tests;
- a mobility test does not know which motor controller is installed;
- a sensor can be changed without rewriting motion tests;
- actuator commands and observations are separate;
- every failed test attempts best-effort stop and cannot report `PASS` unless the
  stopped state and cleanup gate are verified;
- every result identifies the exact firmware/profile/hardware tested;
- agents cannot make a test pass by casually bypassing architecture boundaries;
- physical evidence becomes progressively stronger from L2 to L7.

The purpose of testing is not merely to make hardware move.

The purpose is to build evidence, layer by layer, that the software abstraction, electronics, device integration, physical actuator and robot behavior all agree about what a command means.
