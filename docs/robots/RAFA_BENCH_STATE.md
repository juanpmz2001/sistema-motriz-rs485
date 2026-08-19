# Rafa — Bench State and Physical Learnings

> Canonical physical bring-up summary through the short 2026-08-19 Engineering
> Console / SVD48 hardening session.

## Current state

**A — NOT READY FOR FLOOR MOTION**

Rafa is sufficiently observable for controlled read-only and parameter work, but the
new typed Bench Control path is not yet accepted for the operator's longer manual
campaign. Its first `HOLD 0 M1` attempt was rejected by the backend before a typed
firmware actuation result was returned. No speed smoke was attempted afterward.

NEXT-3 continuous-control software now exists, but the Rafa profile intentionally
keeps `NO_GEOMETRY` and returns `CONTROL_UNAVAILABLE`. Build 27 was deployed by OTA,
and that unavailable gate plus rejected ARM were verified. Browser-close,
backend-loss and LAN-loss motion evidence was intentionally not attempted. M1/M2 side
and positive direction must be established first; software must not infer them from
endpoint order or controller RPM.

## Confirmed facts

- Profile: `rafa`.
- Maintenance LAN works.
- Last verified address during the campaign: `192.168.1.194`.
- Last verified firmware: version `1.0.0`, build `27`, SHA
  `1f97bd23146c0214b9932b6f30cca37724022ff1`, clean, OTA slot `ota_0`.
- Composition active and runtime-ready.
- Platform returned to `SAFE_IDLE`, `MOTION_ACTIVE:0`.
- RC input remains configured on GPIO14 in PPM mode. The 2026-08-19 transmitter-off
  session reported `RC_SEEN:0`, `RC_VALID:0`, `RC_LOSS:0`; prior valid/fresh receiver
  evidence remains historical context, not evidence for this boot.
- One SVD48 is physically present.
- The real SVD48 RS485 address is **2**.
- A repeated read-only scan `1..247` found only address 2.
- Firmware profile was corrected to address 2 and deployed by OTA.
- Both M1/M2 provide RPM, current, voltage, position, status/error telemetry.
- Legacy `STOP 0`, `STOP 1` and `STOP ALL` worked in the first campaign. Build 27
  again acknowledged global `STOP ALL` through the Console backend.
- Bus voltage observed around 53.7–53.8 V.
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
- No parameter write or persistence was physically executed in the first campaign.

NEXT-2 float qualification added read-only evidence on 2026-08-18:

- build 24 / `f32c343...`, SVD48 address 2;
- raw two-word reads for M1/M2 Lq, Ld, Rs and several PID/loop fields;
- exact comparison with the trusted historical SV-Config export whose distinctive
  motor values match the installed controller (artifact SHA-256
  `3155dae5ec5c8644ff6c4e4c175f7cda70ddfa1c9f38ea2ed4150b720be6e536`);
- IEEE-754 binary32 confirmed with high word first;
- golden software tests added for the observed word/value pairs;
- no parameter write, PID tuning, save, motion or OTA was issued.

The 2026-08-19 build-27 session exercised the expanded Console API against Rafa:

- M1 and M2 each returned one 29-entry semantic catalog covering controller
  configuration, motor basics/limits, motion, PID/loop and Hall diagnostics;
- float fields decoded with the qualified high-word-first codec;
- M1 returned 22 `AVAILABLE`, 5 `READ_ONLY` and 2 `UNAVAILABLE` entries; M2 returned
  22 `AVAILABLE`, 4 `READ_ONLY` and 3 `UNAVAILABLE` entries without aborting either
  snapshot;
- `control_mode` remained visible/read-only as SPEED; direction was not written;
- one volatile benign write changed `wheel_diameter_mm` from 100 to 101, independently
  read back 101, restored 100 and independently read back 100;
- `SAVE_SVD48_CONFIG` was not requested.

Preserve:

- optional/unavailable registers;
- real error detail;
- snapshot/compare despite partial catalog support;
- no invented adjacent registers.

## Build-27 short smoke session

On 2026-08-19 the current Rafa image was built cleanly with ESP-IDF 5.4.1 and
`BOTFARMS_PROFILE_RAFA`, installed by OTA only, and verified over Maintenance LAN.
The deployed binary SHA-256 was
`c8089e72ae364083e726da4454205c91e97bb73114600a59193dc809b0cca17a`.

Read-only evidence:

- exactly one SVD48 controller, device 1 / bus 1 / RS485 address 2;
- M1 → endpoint 1 `rafa_traction_m1` and M2 → endpoint 2
  `rafa_traction_m2`;
- both channels ended `STOPPED`, 0 RPM, 53.7 V, zero controller and communication
  errors, and `HEALTHY`/not stale;
- `PROFILE_STATUS`, `COMPOSITION_STATUS`, `PLATFORM_STATUS` and `SAFETY_STATUS`
  reported Rafa, active/runtime-ready, `SAFE_IDLE`, no motion/fault and a running
  safety task;
- `CONTROL_STATUS` and ARM correctly returned unavailable because Rafa has no
  qualified geometry.

Bench Control did not pass. The first harness attempt stopped before actuation when
the freshly connected inventory was `STALE`. After one bounded cache-settle correction,
the preconditions and both channel snapshots were healthy, but the first
`ENABLE / HOLD 0 M1` API request returned HTTP 400 before a typed firmware command
result was exposed. No M2 HOLD, DISABLE or `+1 RPM` test was attempted. Cleanup and a
separate final check both acknowledged software `STOP ALL`; Rafa ended `SAFE_IDLE`
with both channels `STOPPED`. Do not treat this as motor-path acceptance. Capture the
exact backend rejection on one targeted future attempt before deciding whether the
cause is a freshness race, another pre-actuation gate or a transport error.

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
- typed Bench Control HOLD/DISABLE and `+1 RPM` behavior;
- negative-direction testing;
- ±5 RPM characterization;
- dual-motor motion;
- RC driving;
- PID tuning;
- true motor temperature;
- high-frequency transient measurement.
