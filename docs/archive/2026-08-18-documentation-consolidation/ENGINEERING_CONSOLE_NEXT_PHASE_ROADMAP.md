# Engineering Console — Next Phase Roadmap

## Context

UI-A, UI-B and UI-C are complete.

The next priority is no longer Communications Diagnostics.

The highest-value work is:

1. finish the SVD48 engineering vertical;
2. then activate a real safe motion-control plane.

PPM movement authority is explicitly deferred.

---

# NEXT-1 — SVD48 Workspace v2: Generic topology + bench control

## Goal

Remove Rafa-specific hardcoding from `/svd48` and make it the reusable engineering workspace for one or many SVD48 controllers.

## Deliver

### Generic inventory

Expose configured SVD48 devices through a typed read-only contract.

Each controller should expose:

- profile device ID;
- bus/address;
- driver/model;
- M1/M2 channels;
- endpoint binding when present;
- availability/health.

The frontend must not contain topology assumptions such as:

```text
const CONTROLLER_ADDRESS = 2
motor:0
motor:1
```

### Workspace selector

```text
Controller: [ SVD48 @ address 2 ▼ ]
Channel:    [ M1 ▼ ]
```

Changing controller/channel changes the target while the page design stays the same.

### Bench control

Add an explicitly bench-only individual motor test panel.

Before motion controls unlock, require:

```text
motor elevado
```

Controls:

- target RPM;
- SET SPEED;
- ENABLE / HOLD 0;
- DISABLE / FREEWHEEL;
- STOP channel;
- PANIC STOP ALL.

Always show:

- target;
- observed RPM;
- current;
- position;
- state;
- errors.

Do not call this robot operation.

Do not use it as the future `/control` transport.

### Tests

Cover:

- Rafa: one controller / two channels;
- Toño profile: two controllers / four channels;
- selector routing;
- no address-2 hardcoding;
- no left/right assumptions unless profile evidence supplies them.

## 6+ motors

Do **not** perform the full legacy-wrapper removal yet.

Design UI/contracts for N controllers, but preserve the documented current embedded composition limit where applicable.

A six-motor embedded migration becomes its own iteration when a real profile requires it.

---

# NEXT-2 — SVD48 Configuration Complete v1

## Goal

Make `/svd48` the primary place to inspect and configure the controller.

## Parameter groups

At minimum include reviewed manual-backed parameters relevant to traction.

### Motor basics

Per M1/M2 where documented:

- Lq;
- Ld;
- Rs;
- pole pairs;
- maximum speed;
- maximum current;
- motor KV;
- direction;
- sensor type.

### Motion parameters

- control mode;
- position mode;
- acceleration;
- deceleration;
- speed smoothing.

### PID / loops

- speed Kp;
- speed Ki;
- speed Kd;
- position Kp;
- position Ki;
- position Kd;
- current-loop gain;
- speed feed-forward gain;
- speed-loop dead zone.

### Existing config/diagnostics

Preserve:

- Hall parameters;
- wheel diameter;
- optional gear-related registers;
- snapshots;
- compare;
- restore;
- save.

## Typed catalog

Refactor definitions so one semantic parameter maps to M1/M2 register addresses.

Concept:

```text
parameter: speed_kp
type: float32
m1_register: 0x5200
m2_register: 0x5202
range: 0..127.999
```

The UI should not show separate M1/M2 rows simultaneously.

## Float qualification gate

Do not enable PID float writes until physical encoding is qualified.

Sequence:

1. implement raw multi-register read;
2. implement candidate float codec;
3. read Rafa;
4. compare with known controller/SV-Config values;
5. confirm byte/word ordering;
6. add golden tests;
7. enable read-only display;
8. only after qualification enable writes/readback.

## Variant handling

Any reviewed parameter can be:

- AVAILABLE;
- UNAVAILABLE on this controller variant;
- READ_ONLY;
- UNQUALIFIED_ENCODING.

One unavailable parameter must not break the entire controller snapshot.

---

# NEXT-3 — Safe Motion Control v1 (`/control`)

## Goal

Activate the architecture already prepared for continuous LAN control.

## Required embedded path

```text
control_lan
→ command_authority
→ application/motion service
→ traction endpoints
```

The current build contains `control_lan` but does not start it.

## Safety contract

Continuous movement requires:

- ARM/DISARM;
- stream ID;
- increasing sequence;
- deadman;
- bounded TTL;
- automatic stop on source expiry;
- STOP priority;
- source-switch barrier.

Bench evidence should include:

- key release produces zero/stop;
- browser close cannot leave a stale command alive;
- backend failure cannot leave a stale command alive;
- Wi-Fi loss causes firmware-side expiry and stop;
- reconnect does not silently resume the old stream.

## UI

Add `/control`.

Initial inputs:

- WASD/arrows;
- virtual differential joystick.

Display:

- control state;
- source;
- deadman/lease state;
- command;
- computed wheel targets;
- observed wheel feedback;
- global STOP.

## Limits

Use current profile/endpoint bounds.

Controller-level maximum speed/acceleration/deceleration are configured in `/svd48`.

Application ramping/max acceleration can be added here if it is trivial/reuses existing architecture; otherwise defer it.

## PPM

Do not give PPM command authority in this iteration.

---

# Deferred

## PPM mapping and authority

Default semantic mapping belongs in the robot profile, but implementation waits for a resolved transmitter/receiver failsafe strategy.

## Torque/current actuation

Controller configuration may expose mode selection when qualified, but actual torque/current commands should become a typed capability later.

Do not overload velocity with amperes.

## Full 6+ motor embedded refactor

Defer until a real robot profile needs more than the current legacy binding capacity.

## UI-D Diagnostics

Still valuable, but after SVD48 configuration and safe control.
