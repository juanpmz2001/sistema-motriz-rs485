# Safe Control Plane v1 — Plan

> **Status (2026-08-20):** implemented and host-tested. The operator has qualified
> Rafa's M1/M2 side/sign and physical differential dimensions; build 33 encodes that
> immutable geometry plus the profile-owned PPM source. Build 33 is installed by OTA
> and runtime reports continuous control `DISARMED`, `SOURCE:NONE`, with TTL 300 ms.
> The first elevated bounded motion smoke and browser/backend/LAN-loss motion evidence
> remain later elevated qualifications.

## Goal

Create `/control` for actual interactive robot motion over LAN.

This is distinct from `/svd48` bench control.

---

# Architecture

```text
Keyboard / arrows / virtual joystick
        ↓
Engineering Console
        ↓
Control client
        ↓
UDP control_lan :32322
        ↓
command_authority
        ↓
motion/application service
        ↓
traction endpoints
```

`main` now starts this path only for a profile whose immutable geometry and endpoint
side/sign mapping validate. Maintenance LAN remains observation and bench maintenance,
not the heartbeat transport.

---

# Control session

States:

```text
DISARMED
ARMED
ACTIVE
EXPIRED / STOPPED
FAULT
```

Required:

- explicit ARM;
- deadman;
- stream ID;
- increasing sequence;
- short TTL;
- DISARM;
- STOP;
- source-switch barrier.

The software candidate is profile-owned 300 ms, with schema validation bounded to
50–500 ms. Rafa workshop evidence must still qualify the exact value and physical
stop latency before it becomes an accepted robot setting.

---

# Input v1

## Keyboard

- W / Up → forward
- S / Down → reverse
- A / Left → left yaw
- D / Right → right yaw
- key release → zero command
- Space → STOP

## Virtual joystick

Normalized input:

```text
forward ∈ [-1, 1]
turn    ∈ [-1, 1]
```

React must not compute SVD48 registers.

---

# Differential motion

One application service computes wheel commands.

Do not duplicate the mixer in:

- keyboard;
- virtual joystick;
- a future PPM source (the Rafa profile now has a separately scoped PPM source;
  it reuses this application/authority path and does not alter the LAN protocol);
- future ROS/Jetson.

All sources publish robot intent.

---

# Safety tests

With Rafa elevated:

1. ARM.
2. Send a bounded forward command.
3. Release controls.
4. Verify zero/stop.
5. Close browser.
6. Verify firmware TTL stops.
7. Kill backend.
8. Verify firmware TTL stops.
9. Remove LAN/Wi-Fi communication.
10. Verify firmware TTL stops.
11. Reconnect.
12. Verify the old stream cannot resume.
13. Require a new ARM.

STOP must remain higher priority than motion.

---

# Limits

Use:

- profile/endpoint absolute RPM limits;
- controller maximum speed/acceleration/deceleration configured through `/svd48`.

Application-level ramping and max acceleration/deceleration may be added if the current architecture already makes them simple.

They are not a blocker for the first elevated `/control` version.

They must be revisited before floor-motion qualification.

---

# UI

`/control` should show:

- connected robot;
- source = LAN;
- ARM/DISARM state;
- deadman/lease state;
- keyboard controls;
- virtual joystick;
- requested robot intent;
- computed M1/M2 targets;
- observed RPM/current;
- prominent STOP.

No SVD48 register configuration belongs on this page.

---

# Explicitly excluded

- a general multi-profile PPM authority framework (Rafa's profile-owned PPM source
  is intentionally narrower and is documented by the as-built architecture);
- torque/current command mode;
- floor motion qualification;
- autonomous control;
- ROS;
- six-motor embedded migration;
- PID tuning.
