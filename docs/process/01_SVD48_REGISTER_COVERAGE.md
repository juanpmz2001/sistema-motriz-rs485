# SVD48 Register Coverage and Implementation Plan

Date: 2026-07-17

Status: implementation in progress with protocol evidence `E1/E2`; hardware verification remains incomplete.

Primary sources:

- `docs/controllers/SVD48B50A/SVD48V30A-user-manual-V2.01.pdf`
- `docs/controllers/SVD48B50A/SVD48V-PC-software-manual-V1.1.pdf`
- `docs/controllers/SVD48B50A/OBSERVED_TOÑO_CONFIG.md`
- `docs/controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md`
- `components/svd48/svd48.c`
- `components/serial_gateway/serial_gateway.c`

The public manuals refer to SVD48V30A/SVD48V50A family behavior. Local hardware is treated as compatible only where observation confirms it.

## Confidence Labels

- `IMPLEMENTED`: dedicated typed firmware behavior exists.
- `OBSERVED`: read/write behavior has been recorded on local hardware.
- `MANUAL_ONLY`: described by the manual but not verified locally.
- `SUSPECT`: contradictory address/type/access or local invalid-register response.
- `UNSUPPORTED`: deliberately excluded from the current product scope.

Confidence and access are independent. A manual `RW` field remains read-only in our platform until it satisfies write evidence.

## Current Protocol Coverage

Implemented:

- UU Motor CRC16 and high-byte/low-byte CRC ordering.
- Function `0x03` holding-register reads.
- Function `0x06` single-register writes.
- Size-bounded function `0x10` request construction plus an internal UART transaction that validates CRC, slave, function, echoed start register, and echoed quantity.
- UART serialization, timeout, response CRC and shape validation. Reads/function `0x06` retain configured retries; function `0x10` makes one attempt so an ambiguous write is not repeated blindly.
- Matching high-bit exception parsing (`0x83`, `0x86`, `0x90`) with preserved function, code, and timestamp.
- Two fixed drives with two channels each.
- Typed start/stop/clear alarm, given RPM, and given current.
- Polling and typed decoding for status, speed, current, temperatures, bus voltage, position, and error code.
- Raw `READ_REG`, confirmed `WRITE_REG`, and confirmed/bounded `WRITE_REGS`
  gateway commands over USB and authenticated maintenance LAN.
- Provisional write containment: runtime-actuation denylist, stopped heuristic,
  old-value pre-read, exact readback, and explicit ambiguous-outcome text.
- Partial `GET_SVD48_CONFIG`/`APPLY_PY6514_CONFIG` support.

Missing or unsafe:

- Typed float and generic 32-bit codecs.
- Physical evidence that each local controller firmware emits the documented exception forms/codes.
- Parameter metadata, range/access validation, persistence semantics, and controller-firmware capability detection.
- A formal `MAINTENANCE` state, exclusive operation ownership, expiry and
  request deduplication. The current stopped check is the OTA heuristic, not a
  state-machine permit.
- Typed change-set validation, complete hard ceilings, rollback and audit. The
  provisional raw path only captures old/readback words for one command.
- Controller save (`0x3100`) and power-cycle verification workflow.
- Calibration workflows.
- Reliable health meaning for `svd48_poll_once()`: it can return success even if all drives fail.

## Register Matrix

### Full ID 2 capture (2026-07-20)

`tools/svd48_read_inventory.py` queried 187 documented groups through
maintenance LAN against controller ID 2: 151 succeeded and 36 returned preserved
invalid-register/read exceptions. No write or movement command was sent.

Evidence:

- `docs/process/evidence/svd48_id2_inventory_2026-07-20.json`
- `docs/process/evidence/svd48_id2_inventory_2026-07-20.md`
- `docs/process/evidence/SVD48_ID2_CURRENT_STATE_SUMMARY.md`

The capture confirms board/software identity, all documented motor/PID/motion
fields, live telemetry, encoder fields and most Hall fields. It also confirms
that this firmware revision does not expose gear teeth, controller-direct PPM or
CAN/RS232 active-upload configuration. Later write/read testing confirmed M2 Hall
calibration current at symmetric address `0x5625`; manual addresses `0x5605` and
`0x5609` are unsupported on this revision.
Raw float patterns strongly support high-word-first IEEE-754, but an independent
SV-Config comparison is still required before PID writes.

### Vehicle, throttle, brake, and remote control

| Registers          | Meaning                                       | Type/access in manual | Current use                              | Confidence    | MVP decision                                                   |
| ------------------ | --------------------------------------------- | --------------------- | ---------------------------------------- | ------------- | -------------------------------------------------------------- |
| `0x2200`           | Maximum acceleration                          | `u16 RW`              | Raw access; observed read                | `OBSERVED`    | Catalog/read; write only if needed                             |
| `0x2201`           | Wheel diameter mm                             | `u16 RW`              | Partial typed read/write                 | `OBSERVED`    | Catalog/read; validate whether controller or ESP owns geometry |
| `0x2202`, `0x2203` | Motor/wheel gear teeth                        | `u16 RW`              | Partial typed attempts; rejected locally | `SUSPECT`     | Read-only/unsupported until captured                           |
| `0x2280..0x2283`   | Input type and PPM baselines                  | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer unless controller-direct PPM is required                 |
| `0x2000..0x2002`   | Throttle dead zone/curve/point count          | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x2010..0x2013`   | Normal-mode throttle force/ramp/speed         | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x2020..0x2023`   | Sport-mode throttle force/ramp/speed          | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x2030..0x2033`   | Turbo-mode throttle force/ramp/speed          | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x2060..0x207F`   | 32-point throttle curve                       | 32 x `u16 RW`         | Read command max 16; no bulk write       | `MANUAL_ONLY` | Defer                                                          |
| `0x2080..0x2082`   | Brake dead zone/curve/point count             | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x2090..0x2092`   | Normal-mode brake parameters                  | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x20A0..0x20A2`   | Sport-mode brake parameters                   | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x20B0..0x20B2`   | Turbo-mode brake parameters                   | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer                                                          |
| `0x20E0..0x20FF`   | 32-point brake curve                          | 32 x `u16 RW`         | Read command max 16; no bulk write       | `MANUAL_ONLY` | Defer                                                          |
| `0x2100..0x2103`   | Remote mode, direction, throttle/brake stroke | `u16 RW`              | Raw only                                 | `MANUAL_ONLY` | Defer; ESP RC should remain authoritative                      |
| `0x2130..0x2132`   | Vehicle speed, battery, power                 | inconsistent access   | Raw only                                 | `SUSPECT`     | Read experiment only                                           |
| `0x2133..0x2135`   | M1/M2/drive temperature                       | `i16 RO`              | Not polled through this group            | `MANUAL_ONLY` | Optional read-only inventory                                   |

### Board and persistence

| Registers | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x3001` | Slave ID | `u16 RO` in table, but SV-Config suggests configuration | Raw read | `SUSPECT` | Read only; migration capture required |
| `0x3002..0x3005` | Software, hardware, bootloader, product versions | `u16 RO` | Raw read only | `MANUAL_ONLY` | Typed controller inventory |
| `0x3006` | RS485 baud enum | `u16 RW` | Raw only | `MANUAL_ONLY` | Defer write; recovery risk |
| `0x3007` | CAN baud enum | `u16 RW` | Raw only | `MANUAL_ONLY` | Defer |
| `0x3008` | Control input source | `u16 RW` | Raw only | `MANUAL_ONLY` | Read; write only after authority design |
| `0x3009` | Maximum bus voltage in 0.1 V | `u16 RW` | Raw only | `MANUAL_ONLY` | High-priority read; write needs battery-specific ceilings |
| `0x300A` | Overload timeout ms | `u16 RW` | Raw only | `MANUAL_ONLY` | Read first |
| `0x300B` | Power-on encoder calibration | manual table unclear | Raw only | `SUSPECT` | Read experiment only |
| `0x3100` | Save parameters to flash | `u16 WO` | Raw write possible, no workflow | `MANUAL_ONLY` | Required for verified persistence workflow |
| `0x3180` | In-position/heartbeat flag | `u16 RO` | Raw only | `MANUAL_ONLY` | Optional read diagnostic |

Never expose writes to `0x3001`, `0x3006`, or `0x3008` until a recovery path exists for a controller that disappears from the current bus settings.

### Active upload configuration

| Registers | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x3200,0x3202,...0x3216` | Twelve CAN address/period records | nominally `u32 RW` | Raw words only | `MANUAL_ONLY` | Defer |
| `0x3300..0x330E` | RS232 upload count/period/register list | `u16 RW` | Raw only | `SUSPECT` due omitted/garbled addresses | Defer |

These are not the right first mechanism for web telemetry. The ESP already owns polling and should publish typed samples.

### Motor electrical and general parameters

M1/M2 pairs are shown together.

| M1 / M2 | Meaning | Type/range | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5000` / `0x5002` | Lq | `float32 0..0.1 H` | Raw words | `MANUAL_ONLY` | Read after float verification; defer write |
| `0x5008` / `0x500A` | Ld | `float32 0..0.1 H` | Raw words | `MANUAL_ONLY` | Read after float verification; defer write |
| `0x5010` / `0x5012` | Rs | `float32 0..127.99 ohm` | Raw words | `MANUAL_ONLY` | Read after float verification; defer write |
| `0x5018` / `0x5019` | Pole pairs | `u16 1..128` | Partial typed read/write | `OBSERVED` for local channel | First guarded write candidate |
| `0x501C` / `0x501D` | Maximum speed RPM | `u16` | Raw only | `MANUAL_ONLY` | First guarded write candidate after readback |
| `0x5020` / `0x5021` | Maximum Iq current A | `u16 0..512` | Raw only | `MANUAL_ONLY` | Candidate with compiled ceiling well below manual max |
| `0x5024` / `0x5025` | Motor KV, 0.1 RPM/V | `u16` | Raw only | `MANUAL_ONLY` | Read first |
| `0x5028` / `0x5029` | Rotation direction | enum `u16` | Raw only | `MANUAL_ONLY` | Candidate only after wheel-off sign test |
| `0x502C` / `0x502D` | Sensor type | enum `u16` | Partial typed read/write | `OBSERVED` for local channel | Guarded write candidate |

### Motion parameters

| M1 / M2 | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5100` / `0x5101` | Speed/position/torque/voltage/skateboard/kart mode | enum `u16 RW` | Raw only | `MANUAL_ONLY` | Read; MVP permits speed mode only |
| `0x5104` / `0x5105` | Absolute/relative position mode | enum `u16 RW` | Raw only | `MANUAL_ONLY` | Defer write |
| `0x5108` / `0x5109` | Maximum acceleration RPM/s | `u16 RW` | Raw only | `MANUAL_ONLY` | Guarded write candidate |
| `0x510C` / `0x510D` | Maximum deceleration RPM/s | `u16 RW` | Raw only | `MANUAL_ONLY` | Guarded write candidate |
| `0x5110` / `0x5111` | S-curve smoothing time | `u16 RW` | Raw only | `MANUAL_ONLY` | Guarded write candidate |

### PID parameters

| M1 / M2 | Meaning | Type/range | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5200` / `0x5202` | Speed Kp | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | MVP guarded write after float/persistence evidence |
| `0x5208` / `0x520A` | Speed Ki | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | MVP guarded write after float/persistence evidence |
| `0x5210` / `0x5212` | Speed Kd | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | MVP guarded write after float/persistence evidence |
| `0x5218` / `0x521A` | Position Kp | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | Defer write |
| `0x5220` / `0x5222` | Position Ki | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | Defer write |
| `0x5228` / `0x522A` | Position Kd | `float32 0..127.999` | Raw words | `MANUAL_ONLY` | Defer write |
| `0x5230` / `0x5232` | Current-loop gain | `float32 0..1` | Raw words | `MANUAL_ONLY` | Defer write |
| `0x5238` / `0x523A` | Speed feed-forward gain | `float32 0..1` | Raw words | `MANUAL_ONLY` | Defer write |
| `0x5240` / `0x5241` | Speed-loop dead zone | `u16 0..100` | Raw only | `MANUAL_ONLY` | MVP guarded write candidate after hardware evidence |

The PC manual says PID writes act immediately and are saved when motors stop. This persistence behavior must be observed; it is not sufficient evidence for production writes.

The manuals further claim PID parameters are persisted when both motors stop and Hall calibration data is saved automatically after calibration. Treat both as unverified controller-firmware behavior until power-cycle experiments confirm them. Other RW fields have no reliable persistence contract beyond the explicit `0x3100=1` save command.

### Commands and telemetry

| M1 / M2 | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5300` / `0x5301` | Stop/start/clear alarm | `u16 RW` | Typed | `IMPLEMENTED` | Retain behind safety authority |
| `0x5304` / `0x5305` | Given speed RPM | `i16 RW` | Typed | `IMPLEMENTED` | Retain behind safety authority |
| `0x5308` / `0x5309` | Given current, 0.1 A | `i16 RW` | Typed | `IMPLEMENTED` | Restrict to approved modes |
| `0x530C` / `0x530E` | Given position | `i32 RW` | No typed write | `MANUAL_ONLY` | Defer |
| `0x5400/01` | Motor status | `i16 RO` | Polled | `IMPLEMENTED` | Retain |
| `0x5404/05` | Motor temperature, 0.1 C | `i16 RO` | Polled | `IMPLEMENTED` | Retain |
| `0x5408/09` | Bus voltage, 0.1 V | `u16 RO` | Polled | `IMPLEMENTED` | Add configured warning/fault interpretation |
| `0x540C/0D` | MOS temperature, 0.1 C | `i16 RO` | Polled | `IMPLEMENTED` | Retain |
| `0x5410/11` | Actual speed RPM | `i16 RO` | Fast polled | `IMPLEMENTED` | Retain |
| `0x5414/15` | Actual current, 0.1 A | `i16 RO` | Fast polled | `IMPLEMENTED` | Retain |
| `0x5418..0x541B` | Positions | `i32 RO` | Polled high-word first | `IMPLEMENTED` | Verify physical scaling per motor/profile |
| `0x5420..0x5423` | Error codes | `u32 RO` | Polled | `IMPLEMENTED` | Add decoded error catalog |

### Encoder parameters

| M1 / M2 | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5500` / `0x5501` | Calibration command | `u16 RW` | Raw only | `MANUAL_ONLY` | Hazardous workflow; defer |
| `0x5504` / manual also says `0x5504` | Encoder lines/bits | `u16 RW` | Raw only | `SUSPECT` shared address | Read/capture only |
| `0x5508` / `0x5509` | Installation direction | enum `u16 RW` | Raw only | `MANUAL_ONLY` | Defer write |
| `0x550C` / `0x550D` | Encoder bias degrees | signed field | Raw only | `MANUAL_ONLY` | Read first |
| `0x5580` / `0x5581` | Encoder temperature | `i16 RO` | Raw only | `MANUAL_ONLY` | Read-only |
| `0x5584` / `0x5585` | Calibration status | enum `u16 RO` | Raw only | `MANUAL_ONLY` | Required for later state machine |

### Hall parameters

| M1 / M2 | Meaning | Type/access | Current use | Confidence | MVP decision |
| --- | --- | --- | --- | --- | --- |
| `0x5600` / `0x5601` | Calibration command | `u16 RW` | Raw only | `MANUAL_ONLY` | Hazardous workflow; defer |
| `0x5620` / `0x5621` | 120/60 degree installation | enum `u16 RW` | Partial typed read | partly `OBSERVED` | Read; guarded write later |
| `0x5624` / `0x5605` or `0x5609` | Calibration current | `u16 RW` | Raw only | M2 `SUSPECT` | No write until sniffed |
| `0x5640..47` / `0x5650..57` | Eight-angle table | 8 x `i16 RW` | Raw reads limited; no bulk write | `MANUAL_ONLY` | Read after validation; defer write |
| `0x5680` / `0x5681` | Sensor temperature | `i16 RO` | Raw only | `MANUAL_ONLY` | Read-only |
| `0x5684` / `0x5685` | Calibration status | enum `u16 RO` | Raw only | `MANUAL_ONLY` | Required later |
| `0x5688` / `0x5689` | Hall status | `u16 RO` | Partial typed read | local values conflict with documented `0..7` | `SUSPECT` interpretation |
| `0x568C` / `0x568D` | Hall current angle | `i16 RO` | Partial typed read | `OBSERVED` raw | Read-only |

### Card reader / software upgrade

The manual describes separate framing and upgrade behavior without a normal holding-register map usable by the current driver. It is `UNSUPPORTED` for this program and must remain separate from application OTA.

## Error Code Interpretation

The user manual documents bits in the 32-bit motor error field around `0x5420/0x5422`, including current sampling/circuit/wiring faults, bus high/low, thermal sensing, 12 V/5 V rails, motor open circuit, drive/motor overtemperature, overcurrent, overload, overvoltage, undervoltage, encoder, and firmware/hardware mismatch. The manual's LED-code table does not align perfectly and introduces/reorders conditions such as E-stop. Preserve the raw 32-bit value and mark decoded labels with manual/version confidence; never discard unknown bits.

## Required Hardware Experiments

| ID | Experiment | Preconditions | Expected evidence | Write risk |
| --- | --- | --- | --- | --- |
| `SVD-EXP-001` | Sniff SV-Config reading one known float | Motor stopped/power isolated | Exact request, response, word/byte order, decoded value | none |
| `SVD-EXP-002` | Sniff one PID float write and readback | Restrained motor, saved baseline | `0x06` vs `0x10`, ordering, immediate/persistent behavior | high |
| `SVD-EXP-003` | Probe `0x2202/03` with `0x03`, `0x06`, `0x10`, modes varied | Full backup; stopped | Valid/exception frames and firmware version | medium |
| `SVD-EXP-004` | Trigger invalid read/write | No movement | Actual exception functions/codes (`0x90`, `0x83`, `0x86`) | low |
| `SVD-EXP-005` | Verify M2 encoder and Hall disputed addresses | Stopped | Address-by-address read and SV-Config capture | low until write |
| `SVD-EXP-006` | Save benign parameter with `0x3100=1`, power cycle, readback | Backup and power control | Persistence timeline and recovery | medium |
| `SVD-EXP-007` | Observe automatic PID persistence after all motors stop | Restrained | Before/write/stop/power-cycle values | high |
| `SVD-EXP-008` | Validate position counts per motor and wheel revolution | Wheel marked/elevated | Counts/rev at motor and output; gearbox relationship | medium |
| `SVD-EXP-009` | Capture slave ID/baud change through SV-Config | Spare controller/recovery adapter | Exact safe migration sequence | high |
| `SVD-EXP-010` | Map error bits using controlled faults | Manufacturer guidance and restrained setup | Error bit, trigger, clear/recovery behavior | high |

Every capture must record controller product/software/hardware version, drive ID, power voltage, motor channel, exact old/new value, complete TX/RX bytes, and whether a power cycle occurred.

## Implementation Order

1. `SVD-001/002`: parser/error fidelity and tests.
2. `SVD-003`: `0x10` framing, ACK parser and one-attempt transaction with host golden vectors. It is provisionally exposed by `WRITE_REGS ... CONFIRM`; physical SVD48 evidence remains pending.
3. Execute `SVD-EXP-001/004/005`.
4. `SVD-004/005`: typed codecs and catalog with confidence metadata.
5. `SVD-006/007`: read-only inventory/group reads/drift.
6. Expose read-only firmware/backend contracts and compare USB/LAN output.
7. Implement `SVD-020/021` maintenance/change-set infrastructure.
8. Execute persistence/write experiments on a backed-up restrained controller.
9. Enable only verified allowlisted fields under `SVD-023`.
10. Enable speed PID under `SVD-025` after float and persistence experiments;
    position/current PID, calibration and board communication remain later work.

## Test Requirements

Host/golden tests:

- CRC and request frames for `0x03`, `0x06`, `0x10`.
- Valid/truncated/oversized/wrong-slave/wrong-function/wrong-byte-count responses.
- CRC mismatch and timeout/retry exhaustion.
- `0x90`, `0x83`, `0x86`, and unknown exception functions with preserved exception code.
- `0x10` writes of 1, 2, 8, and 32 words, including request-size limits, echoed start/count, truncation, and readback fixtures.
- Signed 16/32 boundaries and all verified float fixtures.
- Group reads crossing M1/M2 and noncontiguous parameters.
- Catalog access/range/scope/channel validation.
- Write rejection for RO, uncertain, out-of-range, unsafe, moving, stale, offline, and faulted conditions.
- Change-set partial failure, readback mismatch, timeout, cancellation, job deadline, and rollback-data generation.
- Group-summary behavior when optional Hall/gear reads return exceptions; missing values must remain unknown/unsupported rather than false zero.

Hardware tests are tracked in `04_OFF_GROUND_TEST_MATRIX.md` and cannot be replaced by raw-register command success.

## Definition of Read Support

A parameter is not `DONE` until:

- address and width are known for the controller firmware under test;
- decoding has a golden fixture and live read;
- unit/scale/range/access/confidence are in the catalog;
- unsupported/exception behavior is structured;
- serial and LAN return equivalent typed values;
- UI displays source timestamp, stale state, and confidence.

## Definition of Write Support

A parameter is not writable until all read criteria plus:

- a motor-stopped/maintenance policy is assigned;
- compiled hard ceiling and semantic validation exist;
- old value is captured;
- write encoding has a captured/golden frame;
- readback equality/tolerance is defined;
- controller persistence behavior and power-loss limitation are documented;
- audit output excludes secrets and includes firmware/controller/profile identity;
- restrained hardware success, rejection, readback mismatch, and recovery tests pass.
