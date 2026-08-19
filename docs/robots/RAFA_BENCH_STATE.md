# Rafa — Bench State and Physical Learnings

> Canonical physical bring-up summary through the short 2026-08-19 Engineering
> Console / SVD48 hardening session.

## Current state

**A — NOT READY FOR FLOOR MOTION**

Rafa is sufficiently observable for controlled read-only and parameter work, but the
new typed Bench Control path is not yet accepted for the operator's longer manual
campaign. A focused follow-up proved one traced `HOLD 0 M1` end to end, but the
required M1 HOLD/DISABLE and M2 HOLD/DISABLE acceptance sequence was blocked by
intermittent HTTP 400 responses from read-only Console preflight endpoints before
further HOLD requests were sent. No speed smoke was attempted.

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

The initial short smoke did not pass Bench Control: one attempt stopped on a `STALE`
inventory and the next M1 HOLD returned an opaque HTTP 400 because the temporary
harness raised before preserving the response body.

A focused follow-up then traced one exact request:

```text
POST /api/svd48/controllers/1/channels/M1/bench/hold
{"confirmation":"motor elevado"}
```

All layers passed. The backend emitted `SVD48_BENCH_HOLD 1 M1`; firmware returned
`OK SVD48_BENCH_HOLD DEVICE_ID:1 CHANNEL:M1 ENDPOINT_ID:1 RPM_TARGET:0 MODE:ACTIVE`;
cached controller telemetry changed to `RUNNING`, 0 RPM, healthy, with zero controller
and communication errors. Software `STOP ALL` then returned M1 to `STOPPED`/0 RPM.
This also confirms that `CONTROL_UNAVAILABLE` is not treated as a bench conflict.

No production change was justified by that successful reproduction. The requested
acceptance sequence was then attempted, but two separate runs stopped before any new
HOLD: first `/api/overview/refresh`, then `/api/svd48/inventory`, returned HTTP 400.
The temporary acceptance harness again failed to retain those bodies. Both runs had
already acknowledged initial `STOP ALL`; neither sent a new HOLD. Per the bounded-test
stop rule, M1 DISABLE and all M2 operations remain untested. Do not infer an exact
cause from the status code alone; capture the complete read-only error response before
changing production or resuming motor acceptance.

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
- typed M1 HOLD→DISABLE sequence, all typed M2 bench behavior and `+1 RPM` behavior;
- negative-direction testing;
- ±5 RPM characterization;
- dual-motor motion;
- RC driving;
- PID tuning;
- true motor temperature;
- high-frequency transient measurement.
