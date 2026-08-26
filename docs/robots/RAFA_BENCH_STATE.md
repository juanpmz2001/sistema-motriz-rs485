# Rafa — Bench State and Physical Learnings

> Canonical physical bring-up summary through the 2026-08-25 build-35 OTA
> verification.

## Current state

**A — NOT READY FOR FLOOR MOTION**

Rafa is sufficiently observable for controlled read-only and parameter work, but the
new typed Bench Control path is not yet accepted for the operator's longer manual
campaign. A focused follow-up proved one traced `HOLD 0 M1` end to end, but the
required M1 HOLD/DISABLE and M2 HOLD/DISABLE acceptance sequence was blocked by
intermittent HTTP 400 responses from read-only Console preflight endpoints before
further HOLD requests were sent. No speed smoke was attempted.

Build 35 is the verified installed artifact. It retains the typed paired SVD48
bench-speed command and adds the profile-owned geometry/limits, PPM axis calibration,
CH6 speed scale and typed Hall-calibration path described below. Its OTA session
verified download/hash, reboot, valid image state, active Rafa composition,
`SAFE_IDLE`, healthy zero-RPM endpoints and the new reported control/endpoint limits;
it did not issue a paired command, Hall calibration, ARM or motor-motion command.
Continuous control remains `DISARMED`, `SOURCE:NONE`, `TTL_MS:300`; the bounded
elevated smoke and browser-close, backend-loss and LAN-loss motion evidence still
require separate operator-authorized sessions.

## Confirmed facts

- Profile: `rafa`.
- Maintenance LAN works.
- Last verified address during the campaign: `192.168.1.194`.
- Last verified firmware: version `1.0.0`, build `35`, SHA
  `45c3f38a236047801951abe427aca2d827b774f7`, clean, OTA slot `ota_0`, OTA state
  `VALID`, with no pending verification.
- Composition active and runtime-ready.
- Platform returned to `SAFE_IDLE`, `MOTION_ACTIVE:0`.
- `CONTROL_STATUS` returned `TASK:RUNNING`, `STATE:DISARMED`, `SOURCE:NONE`,
  `TTL_MS:300`, `DETAIL:EXPLICIT_STOP`; M1 and M2 observations were healthy at 0 RPM.
- RC input remains configured on GPIO14 in PPM mode. The 2026-08-19 transmitter-off
  session reported `RC_SEEN:0`, `RC_VALID:0`, `RC_LOSS:0`; prior valid/fresh receiver
  evidence remains historical context, not evidence for this boot.
- One SVD48 is physically present.
- The real SVD48 RS485 address is **2**.
- A repeated read-only scan `1..247` found only address 2.
- Firmware profile was corrected to address 2 and deployed by OTA.
- Both M1/M2 provide RPM, current, voltage, position, status/error telemetry and
  reported healthy, stopped observations after build-35 boot.
- Build 35 reports `MAX_VX_MPS:0.8000`, `MAX_WZ_RADPS:0.5236`, a 300-ms control TTL,
  and M1/M2 endpoint limits `-40..40 RPM`; this is controller/runtime status evidence,
  not a physical wheel-motion result.
- Legacy `STOP 0`, `STOP 1` and `STOP ALL` worked in the first campaign. Build 27
  again acknowledged global `STOP ALL` through the Console backend.
- Bus voltage observed around 53.7–53.8 V.
- No persistent SVD48 controller or communication error was observed.

## Operator-qualified differential geometry

Canonical operator inputs encoded in OTA-verified build 35:

- M1 is the right wheel; positive controller RPM produces robot-forward wheel
  rotation, so `motion_direction_sign = +1`.
- M2 is the left wheel; positive controller RPM does not produce robot-forward wheel
  rotation, so `motion_direction_sign = -1`.
- wheel radius: 0.20 m (physical diameter 0.40 m);
- wheel-center track width: 1.52 m;
- direct drive: `motor_to_wheel_ratio = 1.0`;
- overall reported chassis dimensions: 1.5 m wide and 0.9 m long. Differential v1
  does not consume wheelbase, so the profile does not substitute the 0.9 m length
  into the kinematics.

These inputs are installed source/profile evidence only. They do not by themselves
prove wheel motion, deployed target direction, TTL stop latency or floor behavior.

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

Build-35 PPM source configuration (installed, not physically qualified yet):

- CH2 high: forward;
- CH4 high: right;
- CH5≤1500us: PPM priority;
- neutral: 1500±30us before PPM arm;
- PPM TTL: 300 ms;
- validity envelope: 750–2250us;
- CH2 and CH4 axis calibration: 1000/1500/2000us; values inside the validity
  envelope beyond those endpoints clamp to full scale;
- CH6 dynamic speed scale: 1000us=0.50, 1500us=0.75, 2000us=1.00.

The associated software path is `ibus_receiver → ppm_motion_source →
motion_application → command_authority → robot_kinematics → traction endpoints`.
Build 35 has been built and deployed by OTA; it still requires elevated testing before
any floor-motion conclusion.

### RC/LAN priority policy

The operator qualified the receiver failsafe for Rafa: with the RC transmitter
disconnected, the receiver emits a valid PPM frame with `CH5=2000us`. The static
Rafa profile therefore treats valid CH5≤1500us as `PPM_PRIORITY` and valid
CH5>1500us as `RC_FAILSAFE`.

This is deliberately not PPM traction authority. `PPM_PRIORITY` must stop/revoke
an existing LAN session and prevent LAN ARM while it remains valid. `RC_FAILSAFE`
and no first valid PPM frame permit only a fresh explicit LAN ARM. If valid PPM is
lost after it held priority, the state is `PPM_LOST`; the prior LAN stream remains
retired. The firmware publishes the interlock separately in `CONTROL_STATUS` so the
Console can expose the firmware decision without risking truncation of the control
session detail. Physical verification of a CH5≤1500us transition still belongs to a
separate, explicitly authorized bench observation; it is not implied by the
receiver-failsafe observation above.

On 2026-08-20, build 31 (`ca1fa79`) was installed by OTA. The post-reboot LAN
snapshot reported the `rafa` profile, active composition, `SAFE_IDLE`, two zero-RPM
traction endpoints, `RC_INTERLOCK:RC_FAILSAFE`, `RC_CH5_US:2000`,
`LAN_ELIGIBLE:1`, and a separate `DATA CONTROL_AUTHORITY ...` line with
`DETAIL:READY` preserved on the session line. No physical CH5≤1500us priority
transition or motor motion was performed as part of that OTA verification.

On 2026-08-20, build 33 (`2f43d4d`) superseded build 31 by OTA after a successful
manifest check and download/hash verification. Post-reboot evidence was: valid clean
image, `rafa` profile schema-valid, active composition, `SAFE_IDLE`, two endpoints at
0 RPM, `RC_INTERLOCK:RC_FAILSAFE`, `RC_CH5_US:2000`, `LAN_ELIGIBLE:1`, and a
300-ms `DISARMED` control session. No PPM-priority transition, ARM or motion was
sent during this verification.

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

The aggregate snapshot can still carry `STALE` when a lower-rate diagnostic
observation expires. Build 33 separates this from velocity-channel communication:
only position, speed and current freshness determine whether a traction endpoint is
`HEALTHY`, `STALE` or `DEGRADED`; a fresh nonzero controller error remains `FAULT`.

The post-OTA `CONTROL_STATUS` was read twice four seconds apart and reported both
M1/M2 `ONLINE:1 STALE:0 HEALTH:HEALTHY`. Do not interpret a diagnostic-field stale
bit alone as a dead RS485 link; surface its field mask separately in the Console.

## Temperature

Motor temperature around `-22.7 °C` is not qualified as a real physical motor temperature.

Raw value was `0xFF1D` interpreted as signed deci-degrees. It may be absent/unconfigured/sentinel data.

MOS temperature around 22–23 °C appeared plausible.

The UI should distinguish unqualified values from trusted physical temperature.

## Parameter Lab

Confirmed:

- `0x2201` available.
- `0x2202` and `0x2203` unavailable on this controller variant (`ERR:0x108`); the
  reviewed manual classifies them as controller-wide `uint16`, range `1..32767`, but
  the Console must retain `UNAVAILABLE` and the controller detail rather than invent a
  value.
- The original 11 supported catalog registers read twice consistently.
- Two original read-only snapshots compared with no differences.
- Wheel diameter: 100 mm.
- Pole pairs M1/M2: 24 / 24.
- Sensor type M1/M2: Hall / Hall.
- No parameter write or persistence was physically executed in the first campaign.

Build 35 adds a typed Hall calibration trigger/status path for the Console; it is
`NOT TESTED` on Rafa. It does not use generic register write/readback or save, and no
Hall calibration is authorized by this evidence alone.

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

## Build-29 Control LAN boundary fix

On 2026-08-20 build 29 (`0061665e34375a74b268bd3f79e62224b3388c0e`) was
built cleanly for `BOTFARMS_PROFILE_RAFA` and installed by OTA only. The Console
had already demonstrated successful ARM and command ingress through the dedicated UDP
control path, but an exact full-scale decimal request could fail with
`BAD_VELOCITY`: the advertised `0.0200` m/s limit is stored as `float`, whose widened
value is slightly below JSON `0.02`.

The firmware now quantizes finite JSON velocity values to the control `float`
representation before comparing the immutable profile limit. This does not increase
Rafa's 0.02 m/s or 0.20 rad/s limits, change TTL, or weaken deadman/authority checks.

The OTA announcer did not receive a reply during the reboot, but subsequent LAN
identity verified build 29, clean Git SHA, `OTA_STATE:VALID`, profile `rafa`, active
composition/runtime, `SAFE_IDLE`, running safety task, `CONTROL_STATUS:DISARMED`
with `TTL_MS:300`, and both traction endpoints healthy at 0 RPM. This is firmware and
controller evidence only. It is not an elevated motion acceptance: the operator must
reconnect the Console and perform the next authorized physical test separately.

## Explicitly not qualified

- floor motion;
- deployed build-29 `/control` target signs and physical elevated forward/turn smoke;
- M2 individual command behavior;
- typed M1 HOLD→DISABLE sequence, all typed M2 bench behavior and `+1 RPM` behavior;
- negative-direction testing;
- ±5 RPM characterization;
- dual-motor motion;
- RC driving;
- PID tuning;
- true motor temperature;
- high-frequency transient measurement.
