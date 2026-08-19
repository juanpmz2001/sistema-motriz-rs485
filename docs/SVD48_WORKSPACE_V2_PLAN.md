# SVD48 Workspace v2 — Design Plan

> **Implementation status (2026-08-19):** NEXT-1 generic inventory, cached channel
> telemetry, controller/channel selection and typed Bench Control are implemented in
> firmware build 27 and the Engineering Console. NEXT-2 is implemented in the Console:
> one semantic per-channel catalog, variant-local `UNAVAILABLE`, snapshot schema 2 and
> qualified float support. Build 27 was deployed to Rafa by OTA; physical inventory,
> channel telemetry, parameter reads and one restored volatile wheel-diameter write
> passed. No save was requested. Typed Bench Control remains unaccepted because the
> first HOLD request was rejected before a firmware actuation result; no speed command
> was attempted. See `docs/robots/RAFA_BENCH_STATE.md` for exact evidence.

## 1. Why this iteration exists

The current SVD48 workspace proved useful on Rafa, but it contains topology assumptions specific to that robot.

The next version must support:

```text
profile
  → configured SVD48 devices
      → controller/address
          → M1
          → M2
```

without changing UI design for each robot.

---

# 2. Target UI

```text
SVD48 Workspace

Controller
[ SVD48 · device 1 · RS485 2 ▼ ]

Channel
[ M1 ▼ ]

[ Live ] [ Parameters ] [ Diagnostics ] [ Bench Control ] [ Advanced ]
```

Controller/channel selection is global to the workspace.

---

# 3. Inventory contract

Add a typed read-only inventory instead of inferring devices from `motor:0`.

Semantic example:

```json
{
  "controllers": [
    {
      "device_id": 1,
      "driver": "SVD48",
      "address": 2,
      "bus_id": 1,
      "channels": [
        {
          "channel": "M1",
          "endpoint_id": 1,
          "endpoint_name": "rafa_traction_m1"
        },
        {
          "channel": "M2",
          "endpoint_id": 2,
          "endpoint_name": "rafa_traction_m2"
        }
      ]
    }
  ]
}
```

Exact wire shape may differ. The identity contract is what matters.

For Toño, the same API should naturally return two SVD48 controllers.

---

# 4. Parameter model

Do not define separate UI rows such as:

```text
m1_speed_kp
m2_speed_kp
```

Define:

```text
speed_kp
```

and map it by selected channel.

Suggested definition fields:

```text
key
label
group
type
unit
minimum
maximum
choices
writable
optional
controller_register or channel register mapping
m1_register
m2_register
evidence
description
```

Controller-wide parameters may have no channel mapping.

---

# 5. Manual-backed traction catalog

The official UUMotor SVD48V Series manual V2.0 documents the following relevant groups.

## M1 basics

- `0x5000` Lq — float
- `0x5008` Ld — float
- `0x5010` Rs — float
- `0x5018` pole pairs
- `0x501C` maximum speed
- `0x5020` maximum current
- `0x5024` motor KV
- `0x5028` direction
- `0x502C` sensor type

M2 uses the corresponding documented channel registers.

## Motion

M1:

- `0x5100` control mode
- `0x5104` position/location mode
- `0x5108` acceleration
- `0x510C` deceleration
- `0x5110` speed smoothing

M2:

- `0x5101`
- `0x5105`
- `0x5109`
- `0x510D`
- `0x5111`

## PID

M1:

- `0x5200` speed Kp
- `0x5208` speed Ki
- `0x5210` speed Kd
- `0x5218` position Kp
- `0x5220` position Ki
- `0x5228` position Kd
- `0x5230` current-loop gain
- `0x5238` speed feed-forward gain
- `0x5240` speed-loop dead zone

M2:

- `0x5202`
- `0x520A`
- `0x5212`
- `0x521A`
- `0x5222`
- `0x522A`
- `0x5232`
- `0x523A`
- `0x5241`

Treat the manual as specification evidence, not proof that every Rafa controller variant implements every register.

Probe/read physical support.

Never infer other registers from adjacency.

---

# 6. Float parameters

Several important motor/PID values are documented as float.

Do not guess encoding.

Qualification sequence:

1. raw read the required register span;
2. implement candidate codec;
3. compare the decoded value against a trusted known value from the physical controller / SV-Config;
4. determine byte and word ordering;
5. add golden codec tests;
6. mark encoding QUALIFIED;
7. expose typed read;
8. only then permit typed write/readback.

Qualification completed read-only on 2026-08-18. Rafa build 24 at SVD48 address 2
returned two-word values matching a trusted SV-Config export across distinctive M1/M2
Lq/Ld/Rs values and multiple loop values. The proven codec is IEEE-754 binary32 with
the high 16-bit word in the lower Modbus register. Reversing word order did not match
the trusted values. Golden host tests retain the observed word/value pairs.

No physical PID/configuration write or `SAVE_SVD48_CONFIG` was issued. The Console may
now use normal pre-read → two-word write → independent exact word readback for an
explicitly authorized future session. A controller variant that rejects a reviewed
float address remains `UNAVAILABLE` without invalidating other values.

Before qualification, the required display was:

```text
UNQUALIFIED_ENCODING
```

instead of a guessed value.

---

# 7. Bench Control tab

Purpose:

> test one physical SVD48 channel, not drive the robot.

Before enabling movement:

```text
Safety confirmation

Robot must be elevated and wheel unloaded.
Independent physical power cutoff must be accessible.

Type:
motor elevado

[ Confirm ]
```

Then show:

```text
Controller: address 2
Channel: M1

Observed
State      STOPPED
RPM        0
Current    0.3 A
Position   ...
Errors     clear

Target RPM
[ 1 ]

[ SET SPEED ]

[ ENABLE / HOLD 0 ]
[ DISABLE / FREEWHEEL ]

[ STOP M1 ]

[ SOFTWARE STOP ALL ]
```

## Semantics

### SET SPEED

Use a typed firmware/backend path.

React must not build raw register commands.

### ENABLE / HOLD 0

SVD48 adapter owns:

```text
target = 0
→ START channel
```

This corresponds to the physically observed active hold-at-zero behavior on Rafa.

### DISABLE / FREEWHEEL

SVD48 adapter owns:

```text
target = 0
→ STOP channel
```

### Software stop all

Global `STOP ALL`.

Must remain available even if the selected controller/channel changes.
It is not an emergency stop and does not replace the physical power cut-off.

---

# 8. Maintenance LAN policy

Do not convert Maintenance LAN to allow-all.

For this vertical, permit/add only typed operations required by the driver workspace.

Examples:

- controller/channel inventory;
- reviewed parameter reads/writes;
- typed ENABLE/HOLD at zero;
- typed DISABLE;
- STOP;
- bounded bench speed operations.

Continuous robot operation remains excluded.

If an operation can create persistent motion without firmware-side expiry, label it BENCH ONLY and never reuse it as the `/control` transport.

---

# 9. Multi-controller behavior

Rafa:

```text
Controller 1 → address 2 → M1/M2
```

Toño:

```text
Controller 1 → address 1 → M1/M2
Controller 2 → address 2 → M1/M2
```

The frontend should not need new components for Toño.

Test Toño through profile/fake fixtures even when not physically connected.

---

# 10. Six-motor decision

Do not perform a full embedded six-motor migration now.

Reason:

- current SVD48 device/poll architecture is already more general;
- the legacy compatibility projection currently limits the composed legacy motor view to four bindings;
- Rafa requires two;
- Toño requires four;
- removing all legacy dependencies is a larger safety/architecture iteration.

Do now:

- design inventory/UI/parameter APIs for N SVD48 devices;
- never hardcode two controllers in frontend;
- document current firmware limit.

Migrate the legacy binding only when a real >4-motor profile is needed.

---

# 11. Acceptance criteria

The iteration is successful when:

- Rafa controller is discovered dynamically rather than hardcoded;
- a two-SVD Toño fixture renders correctly;
- controller/channel selection drives Live/Parameters/Bench tabs;
- Parameter Lab shows one selected channel's semantic parameter set;
- every reviewed parameter can be independently `AVAILABLE`, `UNAVAILABLE` or
  `READ_ONLY`, and snapshot schema 2 preserves that status;
- float words use the Rafa-qualified high-word-first IEEE-754 codec with golden tests;
- direct bench controls are clearly separated from `/control`;
- ENABLE/HOLD and DISABLE/FREEWHEEL semantics are explicit;
- software `STOP ALL` remains global;
- no continuous operational joystick path is built through Maintenance LAN.
