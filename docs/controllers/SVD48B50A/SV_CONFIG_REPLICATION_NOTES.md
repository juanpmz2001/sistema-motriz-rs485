# SV-Config Replication Notes

Date: 2026-05-08

Current constraint: the ESP32-S3/controller is not connected, so these notes are based on the UU Motor manuals, previous observed reads, and physical measurements. Do not attempt live serial communication from this note.

## Pole-Pair Inference From Physical Speed

Observation:

- Controller pole-pair parameter: `48`.
- Commanded speed: `1 RPM`.
- Measured wheel period: `12.75 s/rev`.

Measured wheel speed:

```text
actual_rpm = 60 / 12.75 = 4.7059 RPM
```

If the controller converts between electrical speed and mechanical speed using the configured pole-pair value, then:

```text
actual_rpm = commanded_rpm * configured_pole_pairs / actual_pole_pairs
actual_pole_pairs = configured_pole_pairs * commanded_rpm / actual_rpm
actual_pole_pairs = 48 * 1 / 4.7059 = 10.2
```

Practical conclusion: the wheel motor is very likely a `10` pole-pair motor. The earlier UU Motor family hypothesis of `10` pole pairs remains consistent with the physical timing test.

The smoother motion observed after increasing the pole-pair parameter is probably not evidence that the motor has more poles. It is probably because the controller is making the wheel run faster than the commanded speed. With `48` configured while the motor is really about `10`, a `1 RPM` command becomes about `4.7 RPM` physically, and the Hall transitions arrive much more often.

At true `1 RPM` wheel speed with a `5:1` gearbox and `10` pole pairs:

```text
rotor_speed = 5 RPM
electrical_cycles = 5 * 10 = 50 electrical RPM = 0.833 electrical Hz
3-Hall sector changes = 0.833 * 6 = 5 changes/s
```

That means one Hall sector update about every `200 ms`, which can feel discrete or "golpeado" at very low speed. Increasing the configured pole-pair value masks this by raising the actual wheel speed.

## SV-Config Behavior From Manuals

The PC manual describes SV-Config as a serial tool at `115200` baud with toolbar actions:

- `Read param`: read all parameters from the drive.
- `Write param`: write all parameters to the drive.
- Parameter writes must be done after the motor is stopped.

The software exposes:

- Board parameters: slave ID, RS485/CAN baud, voltage limits, control input.
- General motor parameters: max RPM, max current, direction, sensor type, reduction gears.
- Encoder/Hall parameters and Hall calibration.
- PID parameters that take effect in real time; the manual says they are saved to flash when all motors stop.
- Waveform monitoring and CSV export.

## Relevant Motor General Registers

M1:

- `0x5000`: motor inductance Lq, float, range `0..0.1 H`.
- `0x5008`: motor inductance Ld, float, range `0..0.1 H`.
- `0x5010`: motor internal resistance Rs, float, range `0..127.99 ohm`.
- `0x5018`: motor pole pairs, uint16, range `1..128`.
- `0x501C`: max speed, uint16, RPM.
- `0x5020`: max current, uint16, A.
- `0x5024`: motor KV, uint16, `0.1 RPM/V`.
- `0x5028`: rotation direction, `0=positive`, `1=reverse`.
- `0x502C`: sensor type, `0=encoder`, `1=Hall`, `2=string encoder`.

M2:

- `0x5002`: motor inductance Lq, float.
- `0x500A`: motor inductance Ld, float.
- `0x5012`: motor internal resistance Rs, float.
- `0x5019`: motor pole pairs, uint16.
- `0x501D`: max speed, uint16, RPM.
- `0x5021`: max current, uint16, A.
- `0x5025`: motor KV, uint16, `0.1 RPM/V`.
- `0x5029`: rotation direction, `0=positive`, `1=reverse`.
- `0x502D`: sensor type, `0=encoder`, `1=Hall`, `2=string encoder`.

## PID Registers

M1:

- `0x5200`: speed Kp, float.
- `0x5208`: speed Ki, float.
- `0x5210`: speed Kd, float.
- `0x5218`: position Kp, float.
- `0x5220`: position Ki, float.
- `0x5228`: position Kd, float.
- `0x5230`: current-loop gain, float, range `0..1.0`.
- `0x5238`: speed feed-forward gain, float, range `0..1.0`.
- `0x5240`: speed-loop dead zone, uint16, range `0..100`.

M2:

- `0x5202`: speed Kp, float.
- `0x520A`: speed Ki, float.
- `0x5212`: speed Kd, float.
- `0x521A`: position Kp, float.
- `0x5222`: position Ki, float.
- `0x522A`: position Kd, float.
- `0x5232`: current-loop gain, float.
- `0x523A`: speed feed-forward gain, float.
- `0x5241`: speed-loop dead zone, uint16.

## Hall Calibration Registers

M1:

- `0x5600`: calibration command, `0=no calibration`, `1=perform calibration`.
- `0x5620`: Hall installation, `0=120 deg`, `1=60 deg`.
- `0x5624`: calibration current, uint16, range `0..50 A`.
- `0x5640`: angle table, 8 signed int16 angle values, unit degrees.
- `0x5680`: encoder temperature, read-only, `0.1 C`.
- `0x5684`: calibration status, read-only, `0=success`, `1=calibrating`, `2=failed`.
- `0x5688`: Hall status, read-only, documented range `0..7`.
- `0x568C`: Hall current angle, read-only, signed int16 degrees.

M2:

- `0x5601`: calibration command.
- `0x5621`: Hall installation, `0=120 deg`, `1=60 deg`.
- `0x5605` / `0x5609`: manual lists calibration-current-like fields here; treat this as suspicious until verified against SV-Config traffic.
- `0x5650`: angle table, 8 signed int16 angle values.
- `0x5681`: encoder temperature.
- `0x5685`: calibration status.
- `0x5689`: Hall status.
- `0x568D`: Hall current angle.

## Gear / Vehicle Registers

- `0x2200`: maximum acceleration, unit `A/s`.
- `0x2201`: wheel diameter, unit `mm`.
- `0x2202`: number of motor teeth, range `1..32767`.
- `0x2203`: number of wheel teeth, range `1..32767`.

SV-Config says "Number of driven and driving gears" configures the reduction ratio, and that the given speed is after the reduction ratio.

Observed issue on Toño: controller ID `0x02` accepted reads of `0x2200/0x2201`, but returned invalid-register exceptions for `0x2202/0x2203`. Since SV-Config can change these values, possibilities to verify later:

- SV-Config may use a different address map for this firmware revision.
- SV-Config may write those fields as part of a larger `0x10` multi-register parameter block instead of individual `0x03/0x06` transactions.
- SV-Config may use a parameter-list indirection through the `0x3300..0x330E` parameter register address list.
- Some fields may be write-only, mode-gated, or only valid under a specific control input/interface mode.
- The manual may have a typo or version mismatch for these addresses.

## How To Replicate SV-Config In Our Platform

1. Add a typed parameter schema instead of only raw registers:
   - `uint16`, `int16`, `int32`, and `float` register types.
   - Per-channel aliases: M1/M2.
   - Read/write guardrails: require motor stopped for persistent config writes.

2. Implement SV-Config-like commands:
   - `GET_PARAM drive channel name`.
   - `SET_PARAM drive channel name value CONFIRM`.
   - `READ_PARAM_GROUP drive motor|pid|hall|vehicle`.
   - `SAVE_PROFILE` and `LOAD_PROFILE` for known-good configurations.

3. Implement Hall calibration workflow:
   - Stop motor.
   - Set sensor type to Hall.
   - Set pole pairs, direction, Hall installation and calibration current.
   - Write `0x5600=1` for M1 or `0x5601=1` for M2.
   - Poll `0x5684/0x5685` until success/fail/timeout.
   - Read Hall status, current angle and angle table.
   - Log all TX/RX hex frames.

4. Reverse-engineer the missing SV-Config details:
   - Put a passive sniffer on the PC-to-controller RS485 A/B lines while changing only one SV-Config field at a time.
   - Capture `Read param`, `Write param`, gear teeth changes, Hall calibration, Lq/Ld/Rs writes and PID writes.
   - Decode slave ID, function code, start register, quantity, payload, and CRC.
   - Update `SVD48B50A_SKILL.md` with verified frames.

## Future UI TODO

Build a dedicated local tuning interface for control parameters:

- Live editor for speed PID: Kp, Ki, Kd.
- Live editor for position PID: Kp, Ki, Kd.
- Current-loop gain and speed feed-forward gain controls.
- Speed-loop dead-zone control.
- Before/after profile comparison.
- Revert-to-last-known-good button.
- Live plots of command RPM, actual RPM, current, position, Hall angle and error code.
- Guardrails for motor stopped vs live-tunable parameters.
