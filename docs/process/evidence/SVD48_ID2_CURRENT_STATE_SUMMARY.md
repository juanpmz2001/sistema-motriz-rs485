# SVD48 ID 2 State Snapshot Before Parameterization

Capture date: 2026-07-20

This is an immutable interpretation of the pre-parameterization snapshot, not a
claim about the controller's current state. The 2026-07-21 experiments and final
restoration are documented in `SVD48_BENCH_DIAGNOSIS_2026-07-21.md` and
`SVD48_RESTORE_TO_INITIAL_2026-07-21.md`. Later XML evidence also corrected the
gear addresses, M2 Hall-current address, MOS/bus labels and actual-speed scale as
noted below.

Source files:

- `svd48_id2_inventory_2026-07-20.json`: complete machine-readable capture.
- `svd48_id2_inventory_2026-07-20.md`: every request, raw word and exception.

The capture executed 187 documented read groups through ESP maintenance LAN:
151 succeeded and 36 returned the controller's invalid-register/read exception.
No write, enable or movement command was sent. The controller remained stopped.

## Identity And Communication

| Parameter | Register | Raw | Interpretation |
| --- | --- | --- | --- |
| Slave ID | `0x3001` | `0x0002` | ID 2 |
| Software | `0x3002` | `0x0131` | likely V1.3.1 per manual encoding |
| Hardware | `0x3003` | `0x0300` | likely V3.0.0 |
| Bootloader | `0x3004` | `0x0103` | likely V1.0.3 |
| Product | `0x3005` | `0x0101` | raw product ID 0x0101 |
| RS485 baud | `0x3006` | `4` | 115200 baud |
| CAN baud | `0x3007` | `6` | 1000 Kbit/s according to manual |
| Control input | `0x3008` | `1` | RS485 |
| Maximum bus voltage | `0x3009` | `600` | 60.0 V |
| Overload timeout | `0x300A` | `1000` | 1000 ms |
| Power-on encoder calibration | `0x300B` | `1` | enabled/meaning requires confirmation |

`0x3100` is write-only and correctly rejected a read. `0x3180` returned its
documented fixed heartbeat value `1`.

## Motor Configuration

Both channels are configured symmetrically unless noted.

| Parameter | M1 | M2 | Notes |
| --- | ---: | ---: | --- |
| Pole pairs | 24 | 24 | Hall motor configuration |
| Maximum speed | 100 RPM | 100 RPM | Above the 30 RPM initial bench target |
| Maximum current | 30 A | 30 A | High until motor/controller/battery limits are confirmed |
| KV | 16.6 RPM/V | 16.6 RPM/V | Raw value 166 in 0.1 RPM/V |
| Direction | 1 | 1 | Reverse according to manual |
| Sensor | 1 | 1 | Hall |
| Control mode | 0 | 0 | Speed mode |
| Position mode | 1 | 1 | Relevant only in position mode |
| Acceleration | 45 RPM/s | 45 RPM/s | Conservative initial ramp |
| Deceleration | 40 RPM/s | 40 RPM/s | Slower than the 160 RPM/s `origin/lucho` reference |
| S-curve smoothing | 100 ms | 100 ms | |

Electrical float values strongly select high-word-first order because the other
order produces impossible magnitudes:

| Parameter | M1 | M2 |
| --- | ---: | ---: |
| Lq | 0.0002972 H | 0.0002853 H |
| Ld | 0.0002972 H | 0.0002853 H |
| Rs | 0.1183 ohm | 0.1054 ohm |

The controller wheel diameter is `100 mm`. The current RAFA firmware reference
uses wheel radius `0.10 m`, equivalent to `200 mm` diameter. This is a material
configuration mismatch that must be measured before changing either side.
Legacy registers `0x2202/0x2203` are unsupported on this firmware. The original
scanner therefore did not capture gear, but later official-XML/live evidence
located it at `0x5030/31/34/35`. Its pre-parameterization value remains unknown.

## PID Values

The raw word patterns consistently support high-word-first IEEE-754. Later
official-XML inventories and exact raw restoration confirmed that order for
these observed PID/electrical fields.

| Loop parameter | M1 | M2 |
| --- | ---: | ---: |
| Speed Kp | 0.3 | 0.3 |
| Speed Ki | 0.1 | 0.1 |
| Speed Kd | 2.0 | 2.0 |
| Position Kp | 25.0 | 25.0 |
| Position Ki | 0.0 | 0.0 |
| Position Kd | 10.0 | 10.0 |
| Current-loop gain | 0.25 | 0.25 |
| Speed feed-forward | 1.0 | 1.0 |
| Speed dead zone | 0 | 0 |

## Hall And Encoder State

- Hall installation is `0/0`, documented as 120-degree installation.
- Hall calibration status is `0/0`, documented as success.
- Hall calibration current is `15 A` on both channels. M2 is `0x5625`; the
  original scanner's legacy candidates were invalid.
- M1 angle table: `0, 44, 103, 164, 224, 283, 345, 0` degrees.
- M2 angle table: `0, 44, 103, 164, 224, 283, 346, 0` degrees.
- Encoder lines/bits register is `1024`; installation directions are `0/0` and
  biases are `43/44` degrees. These may be inactive because sensor mode is Hall.
- Hall status returned `103/103`, outside the manual's documented `0..7`; do not
  decode it as healthy/faulted until the firmware-specific meaning is known.

## Live Stopped State

- Control commands: `0/0` (stop).
- Given speed: `0/0 RPM`.
- Given current: `0/0` during the inventory capture.
- Actual speed raw: `0/0` in `0.1 RPM`, therefore `0.0/0.0 RPM`.
- Motor errors: `0x00000000 / 0x00000000`.
- MOS temperature during capture: approximately `22.4/22.5 C` (`0x5408/09`).
- Bus voltage: approximately `53.6 V` (`0x540C/0x540D`).

Values requiring caution:

- Motor temperature is `-22.7 C` on both channels and likely means missing or
  unsupported sensing rather than real temperature.
- Actual current later reported `-0.1 A`, likely a zero offset/sentinel.
- Position was `-227/-27`; position scale and signed word semantics need a manual
  rotation experiment.
- Unsupported-read exceptions remain visible in telemetry as the last Modbus
  exception even after successful polling; `COMM_ERR:0` and raw motor error zero
  are the relevant current-health fields.

## Unsupported On This Controller Revision

- Legacy gear candidates: `0x2202/0x2203`. Actual gear fields
  `0x5030/31/34/35` were absent from this scanner and discovered later.
- Controller-direct throttle/PPM: `0x2280..0x2283`.
- Save command readback: `0x3100` because it is write-only.
- CAN active-upload records: `0x3200..0x3216`.
- RS232 active-upload configuration: `0x3300..0x330E`.
- Legacy M2 Hall calibration-current candidates: `0x5605/0x5609`; actual field
  `0x5625` was discovered later.

Every unsupported response is retained verbatim in the complete JSON/Markdown
capture. No unsupported field should be shown as zero in the UI.

## Decisions Before Writing

1. Measure actual wheel diameter and determine whether geometry belongs in the
   ESP profile, SVD48, or both.
2. Confirm battery chemistry/maximum regenerative voltage before reducing the
   current `60.0 V` overvoltage threshold.
3. Establish a conservative current ceiling well below the current `30 A` until
   motor, cable, connector and battery limits are known.
4. Compare PID values and float word order with SV-Config.
5. Keep ID 2 and 115200 unchanged until a tested address/baud recovery procedure
   exists.
6. Do not run Hall/encoder calibration until a dedicated state machine and
   elevated-wheel procedure are available.
