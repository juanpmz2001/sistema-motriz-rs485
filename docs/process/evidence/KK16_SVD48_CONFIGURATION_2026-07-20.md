# KK16 SVD48 Configuration - 2026-07-20

## Scope

- Controller: SVD48, Modbus ID 2, software `0x0131`.
- Motors: two UUMOTOR KK16 48 V / 500 W geared wheelbarrow motors.
- Transport: authenticated maintenance LAN through ESP32-S3.
- Mechanical condition: wheels elevated; no floor test.

Sources:

- Local dynamometer report: `docs/Motors/KK16-48V500W.pdf`.
- Manufacturer product page: <https://www.uumotor.com/16-inch-low-speed-geared-wheelbarrow-motor.html>
- SVD48 user and PC software manuals under `docs/controllers/SVD48B50A/`.

## Confirmed Motor Data

| Parameter | Value | Source |
| --- | ---: | --- |
| Rated voltage | 48 V | KK16 report and product page |
| Rated power | 500 W | KK16 report and product page |
| Rated current | 13 A | KK16 report |
| Product-page maximum current | 30 A | Product page |
| Pole pairs | 10 | Product page |
| Gear ratio | 5:1 | Product page |
| No-load speed | 220 RPM | Product page |
| Tire diameter | 400 mm | Product page |

The local dynamometer report measured 249 RPM at near no-load and a maximum test
current close to 28.9 A. These values describe the tested motor, not permission to
raise limits during robot operation.

## Applied And Verified Configuration

| Registers | Meaning | Before | Applied |
| --- | --- | ---: | ---: |
| `0x5018/0x5019` | M1/M2 pole pairs | 24/24 | 10/10 |
| `0x501C/0x501D` | M1/M2 maximum RPM | 100/100 | 10/10 |
| `0x502C/0x502D` | M1/M2 sensor type | Hall/Hall | Hall/Hall |
| `0x5108/0x5109` | M1/M2 acceleration | 45/45 RPM/s | 3/3 RPM/s |
| `0x510C/0x510D` | M1/M2 deceleration | 40/40 RPM/s | 3/3 RPM/s |
| `0x2201` | Wheel diameter | 100 mm | 400 mm |
| `0x5620/0x5621` | Hall installation | 120/120 degrees | unchanged |
| `0x5624/0x5625` | Hall calibration current | 15/15 A | 15/15 A after tests |

Every ordinary parameter write passed immediate readback. Build 15 then wrote
`0x3100=1` with `SAVE_SVD48_CONFIG 2 CONFIRM`; the controller acknowledged the
Modbus write. Register `0x3100` is write-only, so a controller power cycle is still
required to prove persistence independently.

The motor maximum-current registers remain `30/30 A`. They match the product-page
maximum but exceed the 13 A rated current; operating-current policy needs a
separate supervised decision before loaded tests.

Motor KV, phase resistance, phase inductance and PID were not changed. The public
KK16 material is insufficient to derive controller-safe electrical values, and
the geared output RPM must not be treated as rotor KV without confirming how the
SVD48 defines this parameter.

## Hall Calibration Result

M1 automatic Hall calibration was attempted three times:

| Configured current | Observed behavior | Result |
| ---: | --- | --- |
| 3 A | Approximately 5-6 RPM, about 1.0-1.3 A telemetry | Failed (`0x5684=2`) |
| 6 A | Approximately 5-6 RPM, about 2.3-2.7 A telemetry | Failed (`0x5684=2`) |
| 15 A | Approximately 0-3 RPM, peak about 3.4 A telemetry | Failed (`0x5684=2`) |

After every attempt M1 returned to zero command/current/RPM with no motor error.
M2 calibration was deliberately not started after M1 repeatedly failed.

Do not retry remotely by increasing current or changing 60/120-degree mode. Check
the physical UVW-to-Hall phase relationship, Hall signal sequence and connector
integrity first. The SVD48 manual identifies incorrect UVW/Hall order and wrong
motor parameters as causes of startup/calibration problems.

## Final State

- ESP firmware build 15, OTA state valid, partition `ota_1`.
- Robot `SAFE_IDLE`, `MOTION_ACTIVE:0`.
- Both channels online at 0 RPM with raw motor errors zero.
- General configuration written and save command acknowledged.
- Hall calibration unresolved and loaded/floor movement remains blocked.
