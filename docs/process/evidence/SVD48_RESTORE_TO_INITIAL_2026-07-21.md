# SVD48 restoration to the original pre-parameterization baseline

Date: 2026-07-21

Codex thread/session ID: `019f6e72-3486-7ce1-af40-72d240a5f676`

## Correction of source of truth

The first restoration attempt in this session used the wrong reference: the
capture immediately before the latest Hall tests. That intermediate restoration
to 10 RPM, 30 A and 10 pole pairs was not the operator's requested baseline and
is superseded by this report.

The correct source of truth is the read-only inventory taken before any motor
parameterization work:

- `SVD48_ID2_CURRENT_STATE_SUMMARY.md`
- `svd48_id2_inventory_2026-07-20.json`
- raw capture timestamp: `2026-07-21T03:56:12.678785+00:00`
- 187 requested groups, 151 successful reads and 36 preserved unsupported reads

The final post-restoration full catalog is:

- `svd48_id2_xml_inventory_2026-07-21_restored_original_baseline.csv`
- `svd48_id2_xml_inventory_2026-07-21_restored_original_baseline.json`
- 339/339 official SV-Config XML parameters read successfully

No motor movement or calibration was executed during this restoration.

## Values restored to the original capture

All writes below received exact register readback. Float values were restored
atomically as their original two 16-bit words using FC16.

| Parameter | M1 | M2 | Raw/register |
| --- | ---: | ---: | --- |
| Wheel diameter | 100 mm | shared | `0x2201=0x0064` |
| Lq | 0.0002972 H | 0.0002853 H | `0x5000=399B D183`, `0x5002=3995 9451` |
| Ld | 0.0002972 H | 0.0002853 H | `0x5008=399B D183`, `0x500A=3995 9451` |
| Rs | 0.1183 ohm | 0.1054 ohm | `0x5010=3DF2 4746`, `0x5012=3DD7 DBF5` |
| Pole pairs | 24 | 24 | `0x5018/19=24/24` |
| Maximum speed | 100 RPM | 100 RPM | `0x501C/1D=100/100` |
| Maximum current | 30 A | 30 A | `0x5020/21=30/30` |
| KV | 16.6 RPM/V | 16.6 RPM/V | `0x5024/25=166/166`; already matched |
| Direction | reverse | reverse | `0x5028/29=1/1`; already matched |
| Sensor | Hall | Hall | `0x502C/2D=1/1`; already matched |
| Control mode | speed | speed | `0x5100/01=0/0`; already matched |
| Position mode | 1 | 1 | `0x5104/05=1/1`; already matched |
| Acceleration | 45 RPM/s | 45 RPM/s | `0x5108/09=45/45` |
| Deceleration | 40 RPM/s | 40 RPM/s | `0x510C/0D=40/40` |
| S-curve | 100 ms | 100 ms | `0x5110/11=100/100`; already matched |

The PID registers in the original inventory already matched the final state and
were not rewritten:

| PID | M1 | M2 |
| --- | ---: | ---: |
| Speed Kp / Ki / Kd | 0.3 / 0.1 / 2.0 | 0.3 / 0.1 / 2.0 |
| Position Kp / Ki / Kd | 25 / 0 / 10 | 25 / 0 / 10 |
| Current-loop gain | 0.25 | 0.25 |
| Speed feed-forward | 1.0 | 1.0 |
| Speed dead zone | 0 | 0 |

M1's Hall table was restored exactly through the controller's observed Q15 write
encoding:

```text
M1 original/final: [0, 44, 103, 164, 224, 283, 345, 0]
```

The gateway reports a nominal readback mismatch for Hall-table writes because it
sends Q15 turn units but the controller reads back degrees. The subsequent full
eight-register read confirms the exact degree values above.

## Differences that cannot be restored exactly

### M2 Hall table

The original and final values are:

```text
M2 original: [0, 44, 103, 164, 224, 283, 346, 0]
M2 final:    [0, 40,  99, 161, 220, 280, 343, 0]
```

Software `0x0131` rejects individual writes to the affected M2 table registers
and rejects the FC16 table write with invalid-register exception `0x108`. Another
calibration would generate a new table, not restore the captured table, so no
further calibration was run.

### Encoder offsets

The read-only encoder offsets changed from `43/44` to `42/40`. The official XML
marks `0x550C/0x550D` as RO, and the active sensor mode is Hall. They cannot be
restored through the supported parameter-write path.

### Fields absent from the original inventory

The original scanner did not query several addresses later found in the official
SV-Config XML. Their pre-parameterization values are unknown, so claiming they
were restored would be false:

- Gear teeth `0x5030/31/34/35`. Current value is driving/driven `1/5` on both
  channels, but the original capture only probed invalid legacy addresses
  `0x2202/0x2203`.
- M2 Hall calibration current `0x5625`. Current value is 15 A; the original
  scanner probed invalid candidates `0x5605/0x5609`.
- Board communication timeout and undervoltage fields `0x300C..0x300F`.
- Extended acceleration PID and filter fields `0x5248..0x527A`.

These values were left at their current values because there is no captured
original value to restore.

## Register comparison result

The original 151 successful reads were flattened to individual register words
and compared with the final 339-parameter capture.

- Every overlapping `RW` register matches the original capture.
- The complete M1 Hall table matches the original capture.
- Remaining differences are live RO telemetry, M2 Hall table, and RO encoder
  offsets described above.

## Save and stopped state

The explicit FLASH save command was acknowledged:

```text
SAVE_SVD48_CONFIG drive=2 register=0x3100 value=1
OUTCOME=ACKED_UNVERIFIED WRITE_ONLY=1
```

Immediately before save:

- control command: `0/0`
- given speed: `0/0`
- run state: `0/0`
- actual speed raw: `0/0`
- motor errors: `0x00000000/0x00000000`

The save register is write-only. A later physical power cycle plus readback is
still required to prove FLASH persistence.

Final ESP platform status was `SAFE_IDLE`, `MOTION_ACTIVE:0`, `RUNNING:0`,
`FAULTED:0`, with direct SVD command, speed and error registers at zero.

## Firmware outside this restoration

This operation restored SVD48 parameters only. The installed ESP build 19 still
contains the temporary 15 RPM command ceiling added for bench testing. Therefore
the SVD register maximum is back at the original 100 RPM, while commands sent
through the current ESP firmware remain limited to 15 RPM. No firmware rollback
or flash was performed as part of this parameter restoration.

## Confirmed versus not confirmed correct

Confirmed facts:

- The final values above match the original captured register words wherever the
  original inventory had a successful read and the field was writable.
- Controller ID 2 uses software `0x0131`, RS485 at 115200, Hall sensor mode and
  speed control mode.
- Both motors are currently stopped and expose zero raw motor errors.
- Hall calibration terminal state `2` means failed in the official manual.
- Fault `0x00000800` observed during later tests means motor overload.

Not confirmed as technically correct for the connected KK16 motors:

- 24 pole pairs, although that was the original controller value.
- 100 RPM maximum, 30 A current limit, and 45/40 RPM/s ramps.
- Original `Lq/Ld/Rs`, KV, PID and reverse/reverse direction.
- Hall installation type and either Hall angle table.
- Current gear `1/5` as the pre-parameterization value, because it was not read in
  the original capture.
- Physical wheel RPM, acceleration and position scaling.
- Relative wiring order of phases `U/V/W` and Hall signals `HU/HV/HW`.

This restoration recovers the original captured state; it does not establish that
the original state was a valid KK16 tuning.
