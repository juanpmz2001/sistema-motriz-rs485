# KK16 SVD48 Configuration - 2026-07-20

Historical status: this report records intermediate configuration and tests. It
is superseded for current controller state by
`SVD48_RESTORE_TO_INITIAL_2026-07-21.md`. Post-session XML evidence also showed
that `0x5410/0x5411` is signed `0.1 RPM`; the older firmware printed its raw word
as whole RPM. Speed observations below are corrected to preserve that scale.

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
| 3 A | Firmware raw speed 5-6, approximately 0.5-0.6 RPM; about 1.0-1.3 A | Failed (`0x5684=2`) |
| 6 A | Firmware raw speed 5-6, approximately 0.5-0.6 RPM; about 2.3-2.7 A | Failed (`0x5684=2`) |
| 15 A | Firmware raw speed 0-3, approximately 0-0.3 RPM; peak about 3.4 A | Failed (`0x5684=2`) |

After every attempt M1 returned to zero command/current/RPM with no motor error.
M2 calibration was deliberately not started after M1 repeatedly failed.

Do not retry remotely by increasing current or changing 60/120-degree mode. Check
the physical UVW-to-Hall phase relationship, Hall signal sequence and connector
integrity first. The SVD48 manual identifies incorrect UVW/Hall order and wrong
motor parameters as causes of startup/calibration problems.

## Intermediate State At This Point

- ESP firmware build 15, OTA state valid, partition `ota_1`.
- Robot `SAFE_IDLE`, `MOTION_ACTIVE:0`.
- Both channels online at 0 RPM with raw motor errors zero.
- General configuration written and save command acknowledged.
- Hall calibration unresolved and loaded/floor movement remains blocked.

## Retry After Hall Contact Repair And Power Cycle

After a reported M1 Hall-contact repair, the complete system was power-cycled.
The cycle independently confirmed persistence of the KK16 parameters and cleared
the prior M2 `0x00004000` sensor-input fault. Both 32-bit motor error words read
zero before calibration.

M1 and M2 were then calibrated once each at the persisted 15 A calibration-current
setting. Both produced the same result:

- calibration ran for about five seconds;
- firmware raw speed remained 3-6, approximately `0.3-0.6 RPM`;
- measured current was approximately 2.5-3.5 A;
- the channel stopped without a motor error;
- final calibration status was `2` (failed).

The resulting angle tables were populated and similar:

- M1: `[0, 43, 101, 162, 222, 281, 345, 0]`
- M2: `[0, 40, 100, 162, 220, 280, 344, 0]`

Final state remained `SAFE_IDLE`, zero RPM and raw motor errors zero. A plausible
common cause is the 10 RPM maximum constraining the calibration routine. Testing
with a temporarily higher maximum RPM requires explicit operator approval because
it can make elevated wheels rotate substantially faster. Other unresolved common
causes are incorrect 60/120-degree selection, shared UVW/Hall ordering assumptions,
or incompatible electrical motor parameters.

## Calibration Retry With Temporary 50 RPM Maximum

With explicit operator approval, M1 and M2 maximum speed was temporarily changed
from 10 to 50 RPM. Both channels were calibrated sequentially at 15 A configured
calibration current:

- M1 reached firmware raw speed 33-34, approximately `3.3-3.4 RPM`, and
  2.9-3.7 A telemetry, then failed.
- M2 reached firmware raw speed 33-34, approximately `3.3-3.4 RPM`, and
  2.8-4.0 A telemetry, then failed.
- Both channels stopped cleanly and retained zero raw motor errors.
- Final angle tables remained populated and mutually consistent.

This result rules out the 10 RPM limit as the primary cause. Both maximum-speed
registers were restored to 10 RPM, verified by readback, and `0x3100=1` was
acknowledged to save the final configuration.

The legacy gear candidates `0x2202/0x2203` return unsupported-register exception
`0x108` on software `0x0131`. Later official-XML/live work identified the actual
per-channel fields at `0x5030/31/34/35` and confirmed driving/driven `1/5`. Do not
encode the ratio by falsifying pole pairs or motor KV.

A dedicated write-only-compatible test was later performed with
`SET_SVD48_GEAR_RATIO 2 1 5 CONFIRM`. The controller rejected the FC16 write with
Modbus exception `0x02`, exactly as it rejects reads. Therefore software `0x0131`
does not accept the documented gear registers; no 5:1 value was applied or saved.

## Manual Hall Table Configuration Attempt

Manual writes exposed undocumented conversion and access behavior:

- Hall angle-table reads return degrees.
- Hall angle-table writes expect Q15 turn units, approximately
  `raw = degrees * 32768 / 360`.
- Writing ordinary degree values therefore corrupts the table by scaling values
  down to approximately `raw * 360 / 32768` degrees.
- M1 accepts individual FC06 writes across `0x5640..0x5647`.
- M2 rejects FC06 writes after base address `0x5650` and rejects an eight-word
  FC16 write at `0x5650` with exception `0x02` on software `0x0131`.

M1 was restored from the last captured status-successful table using Q15 values.
Controller rounding produced final degrees `[0,44,102,164,224,282,345,0]` versus
the historical `[0,44,103,164,224,283,345,0]`. M2 remained unchanged at
`[0,40,99,162,220,279,343,0]` because its documented write paths are rejected.

The final configuration was saved, both motors remained stopped with raw errors
zero, and calibration status remained `2/2`. A manually populated table does not
make the read-only calibration status successful and is not sufficient evidence
for a movement test.
