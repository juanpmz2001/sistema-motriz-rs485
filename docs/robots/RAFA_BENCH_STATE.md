# Rafa — Bench State and Physical Learnings

> Canonical physical bring-up summary after the first real Engineering Console / SVD48 campaign.

## Current state

**A — NOT READY FOR FLOOR MOTION**

Rafa is sufficiently observable for continued controlled bench work, but individual motor behavior still needs physical correlation before broader motion testing.

## Confirmed facts

- Profile: `rafa`.
- Maintenance LAN works.
- Last verified address during the campaign: `192.168.1.194`.
- Last verified firmware: build `24`, SHA beginning `f32c343`.
- Composition active and runtime-ready.
- Platform returned to `SAFE_IDLE`, `MOTION_ACTIVE:0`.
- RC input: GPIO14, PPM, 8 valid/fresh channels, `RC_VALID:1`, `RC_LOSS:0`.
- One SVD48 is physically present.
- The real SVD48 RS485 address is **2**.
- A repeated read-only scan `1..247` found only address 2.
- Firmware profile was corrected to address 2 and deployed by OTA.
- Both M1/M2 provide RPM, current, voltage, position, status/error telemetry.
- `STOP 0`, `STOP 1` and `STOP ALL` work through the Engineering Console/application path.
- Bus voltage observed around 53.8 V.
- No persistent SVD48 controller or communication error was observed.

## RS485 discovery rule

Do not declare a SVD48 disconnected only because the configured address does not answer.

Correct procedure:

1. Verify bus/wiring assumptions.
2. Use safe read-only `SVD48_PROBE <address>`.
3. If necessary, perform a bounded read-only address scan.
4. Repeat the result.
5. Update the immutable profile only after the physical address is established.

## Channel naming

Keep channels neutral as `M1` and `M2` until the operator physically confirms:

- which wheel each channel drives;
- physical positive/negative direction.

An ACK or controller-reported RPM is not enough.

## M1 evidence

Session:

`20260817T051114Z-rafa-m1-enable-plus1-3e048462`

Sequence:

`STOP ALL → ENABLE 0 (blocked) → SET_SPEED 0 1 → STOP 0 → STOP ALL`

Controller-reported M1 behavior:

- `STOPPED → RUNNING → STOPPED`;
- ~+18 RPM;
- current up to ~2.3 A;
- position +7 counts;
- no controller/communication error.

This confirms channel response, not physical wheel identity, true RPM calibration, or physical direction.

## ENABLE semantics

Current firmware behavior:

- `SET_SPEED n rpm` performs channel enable internally.
- If enable fails, firmware attempts STOP and returns an error.
- Separate `ENABLE n` is blocked over Maintenance LAN.
- A separate channel ENABLE is redundant for this bench path.

The UI should explain this instead of teaching a separate ENABLE step.

## M2 anomaly

During/after the first M1 command, M2 reported:

- `STOPPED`;
- near-zero current;
- RPM down to roughly `-216`;
- negative position changes.

A later 30-second no-command session showed both RPM values at zero and stable stopped states.

Possible explanations remain unqualified:

- free-wheel physical movement while lifted;
- mechanical coupling/inertia;
- Hall feedback noise/interpretation;
- controller-specific stopped-channel feedback.

Do not broaden M2 motion testing until direct operator observation resolves the physical behavior.

## PPM

Confirmed:

- GPIO14;
- runtime mode PPM;
- valid/fresh frames;
- 8 channels;
- no RC loss.

Not established:

- stick/channel mapping;
- neutral/min/max per control;
- RC-to-motion authority.

Do not add automatic `PPM → traction` control as part of UI work.

`PPM_CAPTURE` should not be used as a live LAN operation if its blocking behavior can delay STOP.

## Telemetry

Current host polling is about 5 Hz.

Good for:

- status;
- second-scale start/stop observation;
- RPM/current/position/voltage trends;
- error/connectivity observation;
- command traceability.

Not qualified for:

- fast transient characterization;
- accurate settling time;
- PID tuning;
- short spike analysis.

Upgrade telemetry only when a concrete vertical requires it.

## Health semantics

Aggregate health can appear `STALE` while important fast observations are still fresh.

Do not interpret aggregate `STALE` alone as a dead RS485 link. The UI should expose freshness and communication state more clearly.

## Temperature

Motor temperature around `-22.7 °C` is not qualified as a real physical motor temperature.

Raw value was `0xFF1D` interpreted as signed deci-degrees. It may be absent/unconfigured/sentinel data.

MOS temperature around 22–23 °C appeared plausible.

The UI should distinguish unqualified values from trusted physical temperature.

## Parameter Lab

Confirmed:

- `0x2201` available.
- `0x2202` and `0x2203` unavailable on this controller variant.
- The original 11 supported catalog registers read twice consistently.
- Two original read-only snapshots compared with no differences.
- Wheel diameter: 100 mm.
- Pole pairs M1/M2: 24 / 24.
- Sensor type M1/M2: Hall / Hall.
- No parameter write or persistence physically executed.

NEXT-2 float qualification added read-only evidence on 2026-08-18:

- build 24 / `f32c343...`, SVD48 address 2;
- raw two-word reads for M1/M2 Lq, Ld, Rs and several PID/loop fields;
- exact comparison with the trusted historical SV-Config export whose distinctive
  motor values match the installed controller (artifact SHA-256
  `3155dae5ec5c8644ff6c4e4c175f7cda70ddfa1c9f38ea2ed4150b720be6e536`);
- IEEE-754 binary32 confirmed with high word first;
- golden software tests added for the observed word/value pairs;
- no parameter write, PID tuning, save, motion or OTA was issued.

The expanded catalog is implemented in the Console and software-tested against Rafa
and two-controller Toño fakes. Loading the new UI against Rafa still requires the
already-built NEXT-1 inventory firmware to be deployed OTA in a separately authorized
session; current physical firmware remains build 24.

Preserve:

- optional/unavailable registers;
- real error detail;
- snapshot/compare despite partial catalog support;
- no invented adjacent registers.

## Recording

Real sessions confirmed:

- host-side CSV;
- metadata JSON;
- command/STOP events;
- no observed sequence drops.

Recording is a global robot-session concept and should remain active across UI navigation.

## Operational discipline

Before motion:

1. verify identity/profile;
2. verify SVD48 communication;
3. verify safety and PPM;
4. request STOP;
5. start recording;
6. issue a bounded command;
7. STOP individual channel;
8. STOP ALL;
9. inspect evidence.

## OTA-only development

Normal firmware workflow:

`source change → tests → Rafa build → OTA artifact → OTA update → LAN verification`

USB is recovery-only.

## Explicitly not qualified

- floor motion;
- M1/M2 physical side;
- physical positive/negative direction;
- M2 individual command behavior;
- negative-direction testing;
- ±5 RPM characterization;
- dual-motor motion;
- RC driving;
- PID tuning;
- true motor temperature;
- high-frequency transient measurement.
