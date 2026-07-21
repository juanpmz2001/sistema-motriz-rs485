# SVD48 KK16 Motor Identification - 2026-07-20

## Setup

- ESP32 firmware: build 17, target `esp32s3`, partition `ota_1`, OTA valid.
- SVD48: drive ID 2, software `0x0131`.
- Motors: KK16-compatible profile, M1 and M2 physically connected.
- Robot: wheels elevated, `SAFE_IDLE`, zero reported RPM, no motor faults.
- Transport: authenticated maintenance LAN at `192.168.1.185`.

The official SV-Config protocol was recovered from the manufacturer's package.
Identification starts with function `0x10` at `0x5700/0x5701`; status and results
are read from `0x5710..0x571D`. No unknown register was written.

## Results

| Channel | Final state | Rs raw | Ld raw | Lq raw | Decoded Rs | Decoded Ld/Lq |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| M1 | 2 | 1191 | 2474 | 2474 | 0.1191 ohm | 247.4 uH |
| M2 | 2 | 1311 | 2428 | 2428 | 0.1311 ohm | 242.8 uH |

The scale is inferred from the official XML units and from correspondence with
the previous float parameters: Rs raw uses `1e-4 ohm`; Ld/Lq raw uses `0.1 uH`.
State 2 is treated as success because values became complete and stable, matching
the PC manual's successful identification workflow.

## Applied Configuration

The identified values were manually copied to the normal float32 parameters,
using high-word-first ordering, then verified by readback:

| Parameter | Register | Previous | Applied words | Applied value |
| --- | --- | ---: | --- | ---: |
| M1 Lq | `0x5000` | 297.2 uH | `0x3981 0xB577` | 247.4 uH |
| M1 Ld | `0x5008` | 297.2 uH | `0x3981 0xB577` | 247.4 uH |
| M1 Rs | `0x5010` | 0.1183 ohm | `0x3DF3 0xEAB3` | 0.1191 ohm |
| M2 Lq | `0x5002` | 285.3 uH | `0x397E 0x9821` | 242.8 uH |
| M2 Ld | `0x500A` | 285.3 uH | `0x397E 0x9821` | 242.8 uH |
| M2 Rs | `0x5012` | 0.1054 ohm | `0x3E06 0x3F14` | 0.1311 ohm |

`SAVE_SVD48_CONFIG 2 CONFIRM` was acknowledged. Register `0x3100` is write-only,
so persistence still requires a controlled power-cycle readback test.

KV was not changed. Both channels read `166`, displayed by SV-Config as
`16.6 rpm/V`, matching the official KK16-compatible profile. The electrical
identification routine does not measure KV.

## Post-check

- M1/M2 actual RPM: 0.
- M1/M2 error: `0x00000000`.
- M1/M2 communication exception: none.
- Bus voltage: approximately 53.7 V (`0x540C/0x540D`).
- MOS temperature: approximately 22.8-23.0 C (`0x5408/0x5409`).
- Motor temperature reads -22.7 C and is considered an absent/invalid sensor,
  not a trustworthy physical temperature.

The original build 17 `GET_MOTOR` output had bus voltage and MOS temperature
labels inverted. Direct reads and the official SV-Config XML proved the correct
mapping above; firmware build 18 fixes it. This correction does not alter the
electrical-identification results.

## Remaining Tests

1. Power-cycle the SVD48 and confirm the six float registers persist exactly.
2. Retry Hall calibration after electrical parameters are correct.
3. Run low-speed off-ground motion in both directions and monitor current,
   error bits and Hall state before any floor test.
4. Compare the result display in SV-Config once an RS232 adapter is available,
   confirming the inferred raw-result scale and state labels.
