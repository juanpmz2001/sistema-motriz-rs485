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
