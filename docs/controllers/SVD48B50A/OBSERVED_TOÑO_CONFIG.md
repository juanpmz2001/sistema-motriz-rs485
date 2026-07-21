# Observed Toño SVD48 Configuration

Date: 2026-05-07

Read through the ESP32-S3 USB serial gateway after flashing the firmware with raw register commands.

Current bench setup: only controller ID `0x02` is connected, and only its `M1` channel is configured.

## Commands Used

```text
GET_SVD48_CONFIG 1
GET_SVD48_CONFIG 2
GET_SVD48_CONFIG 0x02 M1
READ_REG 2 0x5018 2
READ_REG 2 0x502C 2
READ_REG 2 0x2200 1
READ_REG 2 0x2201 1
READ_REG 2 0x2202 1
READ_REG 2 0x2203 1
READ_REG 2 0x5620 2
READ_REG 2 0x5688 2
READ_REG 2 0x568C 2
```

## Results

- Drive ID `1` did not respond to configuration reads at `0x5018`.
- Drive ID `2` responded.
- Drive ID `2`, active M1 pole-pair register `0x5018`: `24`.
- Drive ID `2`, active M1 sensor-type register `0x502C`: `1`, which is `HALL`.
- Drive ID `2`, M2 also read back `24` pole pairs and `1/HALL`, but this channel is not physically configured in the current bench setup.
- Drive ID `2`, `0x2200`: `400`.
- Drive ID `2`, wheel diameter `0x2201`: `100 mm`.
- Drive ID `2`, motor teeth `0x2202`: invalid register exception.
- Drive ID `2`, wheel teeth `0x2203`: invalid register exception.
- Drive ID `2`, Hall installation `0x5620/0x5621`: `0/0`.
- Drive ID `2`, Hall status `0x5688/0x5689`: `345/0`.
- Drive ID `2`, Hall angle/status area `0x568C/0x568D`: `283/0`.
- Channel-specific read after adding channel-aware commands:
  `DATA SVD48_CONFIG DRIVE:2 CHANNEL:M1 POLES:24 SENSOR:1/HALL WHEEL_DIAM_MM:100 MOTOR_TEETH:NA/0 WHEEL_TEETH:NA/0 GEAR_RATIO:NA/0.000 HALL_INSTALL:0 HALL_STATUS:345 HALL_ANGLE:283`

## Interpretation

The active responding controller M1 channel is configured for Hall sensors, but not for the current PY6514/PYD6514 hypothesis of `10` pole pairs. Its M1 pole-pair register reports `24`.

The manual and SV-Config mention reduction-ratio parameters via motor/wheel teeth, but this observed controller rejected `0x2202/0x2203` over RS485. Treat the `5:1` gear ratio as a robot-side calibration unless these registers are confirmed on another firmware revision or through a different register map.

The measured UI calibration of about `62.1` counts per wheel turn is consistent with a telemetry position stream that is not exposing all theoretical three-Hall edge transitions after the wheel gearbox. Continue using physical calibration for wheel angle/distance until the controller's exact position counter semantics are confirmed.

## 2026-07-20 Full LAN Inventory

A later read-only capture with the ESP powered from the assembled hardware found
both ID 2 channels online. The complete evidence is stored in:

- `docs/process/evidence/svd48_id2_inventory_2026-07-20.json`
- `docs/process/evidence/svd48_id2_inventory_2026-07-20.md`
- `docs/process/evidence/SVD48_ID2_CURRENT_STATE_SUMMARY.md`

The capture queried 187 documented register groups: 151 succeeded and 36 returned
invalid-register/read exceptions. It sent no write or movement command.

Important updates versus the earlier capture:

- Both M1 and M2 now poll online; both report 24 pole pairs, Hall sensors, speed
  mode, reverse direction, 100 RPM maximum and 30 A maximum.
- Raw float patterns consistently support high-word-first values: speed PID
  `Kp=0.3`, `Ki=0.1`, `Kd=2.0` for both channels.
- ID 2 reports software `0x0131`, hardware `0x0300`, bootloader `0x0103`, RS485
  enum 4/115200, RS485 control input, 60.0 V maximum bus and 1000 ms overload.
- The controller wheel diameter is 100 mm, while the current RAFA firmware
  reference radius implies 200 mm diameter. Resolve this mismatch by measurement.
- The current Hall status read is `103/103`, not the earlier `345/0`; this field
  is dynamic and outside the documented `0..7`, so its interpretation remains
  suspect.
- Gear teeth, controller-direct PPM, CAN/RS232 active-upload blocks and the two
  suspect M2 calibration-current addresses are unsupported on this revision.

## 2026-07-20 KK16 Electrical Identification

Build 17 reproduced the official SV-Config motor-identification transaction over
RS485. Both channels reached state 2 without motor errors. The identified values
were applied with exact float readback and acknowledged save:

- M1: `Rs=0.1191 ohm`, `Ld=Lq=247.4 uH`.
- M2: `Rs=0.1311 ohm`, `Ld=Lq=242.8 uH`.
- KV remained `16.6 rpm/V`; this routine does not identify KV.

Full commands, raw results, old/new words and remaining persistence test are in
`docs/process/evidence/SVD48_KK16_MOTOR_IDENTIFICATION_2026-07-20.md`.
