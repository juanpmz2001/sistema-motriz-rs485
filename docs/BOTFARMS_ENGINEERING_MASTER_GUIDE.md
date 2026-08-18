# BotFarms Engineering — Master Architecture & Working Guide

> Canonical philosophy for `sistema-motriz-rs485` and `botfarms-engineering-console`.
>
> Every coding agent should read this document before designing a new feature. It explains **how we think**, not just what the current backlog says.

## 1. Purpose

BotFarms is building robots whose hardware will evolve over time.

The software must let us:
- replace controllers, sensors and actuators without rewriting the whole robot;
- test hardware incrementally;
- understand what the physical robot is doing;
- keep control paths bounded and diagnosable;
- use the Engineering Console as a laboratory instrument;
- preserve evidence from physical tests;
- evolve Rafa, Toño and future profiles without profile-specific hacks spreading through the stack.

The goal is not maximum abstraction.

The goal is **the smallest architecture that preserves safety, testability, hardware interchangeability and clear ownership of responsibilities**.

## 2. Source-of-truth hierarchy

When prompt, docs and source disagree, use this order:

1. Current checked-out source code.
2. Executable tests and current runtime identity/evidence.
3. As-built architecture/API documentation.
4. Physical evidence documents.
5. Roadmaps/planning documents.
6. Old prompts and historical prose.

Never implement from a stale prompt without inspecting the current repository.

Before changing either repo:

```bash
git rev-parse --show-toplevel
git status
git branch --show-current
git log --oneline --decorate -15
```

For firmware work, also identify the firmware actually running on the robot over LAN when possible.

## 3. System layering

Preferred dependency direction:

```text
Frontend
  ↓
Engineering Console API / application services
  ↓
Robot/Firmware adapter
  ↓
Firmware application/capability boundary
  ↓
Device/channel adapters
  ↓
Concrete driver
  ↓
Bus transport
  ↓
Physical hardware
```

Rules:
- React must not construct Modbus frames or know register transport details.
- Generic robot control must not know the SVD48 register map.
- Device-specific configuration is allowed in a driver-specific vertical such as `/svd48`.
- Driver code must not own application-level robot intent.
- Transport code must not know robot geometry.
- UI state must not become a second physical-control authority.

## 4. Separate maintenance from continuous control

### 4.1 Maintenance LAN

Maintenance LAN is for bounded engineering/maintenance operations:
- identity/status;
- diagnostics;
- read-only observations;
- reviewed configuration reads/writes;
- snapshots;
- one-shot STOP;
- explicitly reviewed bench operations.

It is **not** the production/continuous joystick transport.

The current design does not provide a continuous motion lease, deadman or replay protection for every Maintenance LAN operation.

Therefore:

> Never solve a control problem by simply making the Maintenance LAN allowlist `allow all`.

Default-deny is intentional. Add only reviewed operations with clear semantics.

### 4.2 Control LAN

Continuous robot control belongs to the dedicated control plane:

```text
Control UI / joystick / keyboard / future RC
        ↓
Control client/backend
        ↓
control_lan
        ↓
command authority
        ↓
motion/application service
        ↓
endpoint capabilities
        ↓
actuators
```

Continuous commands require:
- explicit authority;
- stream/session identity;
- increasing sequence;
- deadman;
- bounded TTL;
- automatic STOP when commands expire;
- source-switch semantics;
- STOP priority.

Loss of browser, backend or Wi-Fi must not leave the last moving command alive indefinitely.

## 5. STOP, ENABLE and DISABLE are different concepts

For SVD48 bench work:

### STOP / panic
Safety cleanup.

Intent:
```text
target → zero
controller → STOP
```

### ENABLE / HOLD ZERO
Useful engineering behavior observed on Rafa:

```text
target = 0 RPM
controller = START
```

The motor actively resists movement to hold zero speed.

Do not claim this behavior for every future controller unless physically verified.

### DISABLE / FREEWHEEL
Observed Rafa behavior:

```text
target = 0
controller = STOP
```

The wheel becomes mechanically much freer.

Suggested UI labels:
- `ENABLE / HOLD 0`
- `DISABLE / FREEWHEEL`
- `PANIC STOP`

The SVD48 adapter owns how START/STOP registers implement those semantics. The frontend must not write controller registers directly.

## 6. Profiles

A profile defines immutable hardware composition and safety-relevant robot defaults:
- buses;
- GPIOs;
- device driver type;
- physical device address;
- channel-to-endpoint binding;
- endpoint capabilities;
- robot geometry when qualified;
- future RC/PPM semantic mapping.

Profiles are compiled firmware artifacts today. Changing topology requires a new firmware build and OTA deployment.

### PPM mapping decision

When RC control is revisited, the **default semantic channel mapping belongs in the robot profile**, because it is part of how that robot is operated.

Example:
```text
throttle      → PPM CH2
steering      → PPM CH1
max_speed     → PPM CH5
acceleration  → PPM CH6
arm           → PPM CH8
```

Do not implement this yet for Rafa while transmitter-loss behavior is unresolved.

A future runtime override may exist, but it should be deliberate rather than the default.

## 7. Robot identity vs legacy indices

Never treat:
```text
motor:0
motor:1
motor:2
```
as the long-term hardware identity.

Prefer:
```text
profile
device_id
driver
bus/address
channel
endpoint_id
endpoint_name
```

Legacy numeric indices can remain compatibility adapters.

Frontend/domain models should increasingly use profile-derived device/channel/endpoint identity.

## 8. Generalization rule

Generalize **contracts and identity** early.

Generalize **unused implementation capacity** only when needed.

Good generalization now:
- dynamic SVD48 controller list;
- dynamic M1/M2 selection;
- typed parameter definitions with channel mapping;
- endpoint-based UI;
- no hardcoded Rafa address in React.

Premature generalization now:
- rewriting every legacy caller solely to support six motors when no current robot needs six;
- generic plugin frameworks;
- runtime arbitrary driver loading;
- distributed multi-robot control.

## 9. SVD48 vertical philosophy

`/svd48` is a **driver engineering workspace**.

It may expose SVD48-specific concepts:
- controller address;
- M1/M2;
- Hall settings;
- control mode;
- acceleration/deceleration;
- max RPM/current;
- PID;
- raw controller errors;
- reviewed registers;
- persistence.

It should not own robot differential-drive logic.

### 9.1 Controller/channel selection

Use one workspace:

```text
Controller: [ address/device ▼ ]
Channel:    [ M1 | M2 ]
```

Changing controller/channel changes the target, not the UI design.

### 9.2 Parameter catalog

All configuration comes from a typed, evidence-backed catalog.

Each parameter definition should contain, as applicable:
- semantic key;
- label;
- group;
- type;
- controller register mapping;
- M1/M2 mapping;
- unit;
- range;
- enum choices;
- read/write support;
- optional/variant support;
- persistence semantics;
- evidence/source;
- operator help text.

Never infer undocumented adjacent registers.

### 9.3 Writes

Safe workflow:

```text
read original
→ validate
→ write
→ independent readback
→ verify
→ record event
```

Persistence is separate.

### 9.4 Float parameters

The official SVD48V manual documents several motor/PID values as `float`.

Do not assume byte/word order.

Before enabling writes:
1. implement a typed multi-register float reader;
2. read physical-controller values;
3. compare them with trusted known values;
4. qualify word/byte order;
5. add golden codec tests;
6. only then enable reviewed float writes/readback.

## 10. Direct SVD48 bench motor control

Simple individual channel motion belongs in `/svd48`, not `/control`, because its purpose is controller qualification.

Use cases:
- requested RPM vs feedback;
- pole-pair plausibility;
- Hall tracking;
- acceleration behavior;
- physical channel identification;
- effect of parameter changes.

Before movement require typing exactly:

```text
motor elevado
```

Then expose:
- controller;
- M1/M2;
- target RPM;
- ENABLE / HOLD 0;
- DISABLE / FREEWHEEL;
- STOP channel;
- PANIC STOP ALL;
- live RPM/current/position/errors.

This is bench engineering, not robot operation.

If bench motion uses Maintenance LAN, keep it explicitly bench-only and bounded. Do not reuse it as `/control`.

## 11. `/control` philosophy

`/control` answers:

> What should the robot do?

It is driver-agnostic.

It should eventually support:
- differential drive;
- keyboard;
- virtual joystick;
- future physical joystick;
- future RC authority;
- future Jetson/ROS source.

It must not expose SVD48 registers or PID.

## 12. Motion limits

There are two layers.

### Controller-level limits

SVD48 parameters such as:
- maximum RPM;
- maximum current;
- acceleration;
- deceleration;
- smoothing;

are high priority and belong in `/svd48`.

### Application-level limits

Future motion service should also support:
- maximum velocity;
- maximum acceleration;
- maximum deceleration;
- ramping.

These are desirable but not required before the SVD48 configuration vertical is complete, provided current testing remains elevated and bounded.

For `/control`, at minimum enforce profile/endpoint limits plus TTL/deadman.

## 13. PPM / RC

PPM observation is useful today.

PPM authority is deferred.

Known issue:
- receiver can continue outputting plausible/fresh PPM after transmitter power loss;
- therefore `fresh PPM` does not prove RF link alive.

Until a safe strategy is chosen:
- monitor PPM;
- do not use PPM as movement authority;
- do not weaken RC-loss safety;
- do not hardcode mappings into application code.

## 14. Evidence levels

Distinguish:
- software test;
- fake adapter test;
- LAN protocol evidence;
- controller-reported feedback;
- operator physical observation;
- independent sensor evidence;
- system outcome.

Examples:
- ACK ≠ wheel moved.
- controller RPM ≠ independently measured RPM.
- PPM fresh ≠ transmitter link alive.
- build succeeds ≠ physical qualification passed.

## 15. Engineering Console product philosophy

The Console is a laboratory instrument.

The operator owns the fast loop:

```text
observe
→ command
→ watch
→ record
→ interpret
→ adjust
```

Codex primarily builds tools, adds instrumentation and fixes evidence-backed bugs.

The Dashboard summarizes. Vertical pages explain and control.

New verticals reuse the shared Linear-inspired Design System.

## 16. Repository ownership

### `sistema-motriz-rs485`
Owns:
- embedded composition;
- driver/device behavior;
- buses;
- safety;
- command authority;
- control LAN;
- profiles;
- typed firmware APIs;
- firmware-side watchdogs/TTL;
- OTA.

### `botfarms-engineering-console`
Owns:
- UI;
- host application services;
- recordings;
- operator workflows;
- semantic API projection;
- charts;
- typed parameter workflow;
- host-side control client.

Host/UI checks improve UX. Firmware owns final protection against host/network disappearance.

## 17. Documentation philosophy

Documentation is part of implementation.

Maintain:
- architecture/master philosophy docs;
- as-built API/driver docs;
- roadmaps;
- physical evidence/robot-state docs.

Rules:
- Prefer one canonical document over duplicated explanations.
- Link to canonical docs instead of copying large sections.
- State `NOT TESTED`, `UNQUALIFIED`, `UNAVAILABLE` explicitly.
- Record firmware SHA/build/profile for physical evidence.
- Never mark a physical gate PASS from CI/fake tests.
- Do not commit secrets or raw datasets unless policy explicitly says so.
- Update docs when source contracts change.

## 18. Git workflow

`main` is canonical in both repos.

Workflow:
1. inspect current status;
2. use a short-lived branch when appropriate;
3. keep commits coherent;
4. run required tests;
5. integrate accepted work into `main`;
6. verify configured remote belongs to `juanpmz2001`;
7. push `main`;
8. report final SHA.

Do not change remotes/Git identity, discard local work or leave accepted work only on a feature branch.

For Rafa firmware:

```text
source → tests → build rafa → OTA → LAN verification
```

USB is recovery-only.

## 19. Agent design behavior

Before implementing:
1. inspect existing abstractions;
2. identify the correct owner/layer;
3. reuse prepared architecture;
4. avoid profile-specific hacks;
5. avoid unrelated broad refactors;
6. test contracts;
7. update as-built docs;
8. distinguish software from physical evidence.

Prefer designs that:
- preserve one owner per responsibility;
- support Rafa/Toño;
- avoid topology hardcoding in UI;
- keep continuous motion fail-safe;
- create the least new abstraction.

## 20. Current priority

1. complete SVD48 configuration/bench vertical;
2. generic multi-SVD controller/channel selection;
3. full reviewed parameter catalog including limits and PID;
4. direct channel bench controls with ENABLE/HOLD, DISABLE/FREEWHEEL and panic STOP;
5. activate safe `/control` using control LAN + command authority + TTL/deadman;
6. communications diagnostics;
7. recording browser;
8. PPM authority only after receiver failsafe strategy is resolved.
