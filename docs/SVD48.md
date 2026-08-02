# SVD48 integration

## Hardware boundary

Build 19 assumes an ESP32-S3 connected through an auto-direction UART-to-RS485
converter to two Fulling SVD48 dual-channel drives:

| Setting | Value |
| --- | --- |
| UART | UART2 |
| TX / RX | GPIO17 / GPIO16 |
| Baud | 115200 |
| Drive IDs | 1 and 2 |
| Response timeout / retries | 100 ms / 2 |
| Telemetry period / stale threshold | 30 ms / 1000 ms |
| ESP-IDF RS485 half-duplex mode | Disabled; adapter handles direction |

Logical mapping is fixed in the current driver:

| Logical motor | Drive | Channel |
| ---: | ---: | --- |
| 0 | 1 | M1 |
| 1 | 1 | M2 |
| 2 | 2 | M1 |
| 3 | 2 | M2 |

This topology is compiled into `main/main.c` and `svd48.h`; no runtime robot
profile exists yet.

## Wire protocol

The driver implements holding-register read (`0x03`), single-register write
(`0x06`) and multiple-register write (`0x10`). It uses the Modbus polynomial
`0xA001`, but this drive family transmits the computed CRC high byte first and low
byte second. Standard Modbus libraries that append low then high are not directly
compatible with the observed device protocol.

All RS485 requests share one mutex. Responses are checked for slave ID, function,
length, exception frames and CRC. Polling backs off per drive after repeated
failures so an absent controller does not continuously saturate the bus.

Protocol-only behavior is covered by native C tests and
`tools/test_svd48_protocol.py`; keep those tests independent from ESP-IDF hardware.

## Registers used by runtime

| Purpose | M1 | M2 |
| --- | ---: | ---: |
| Control command | `0x5300` | `0x5301` |
| Given speed | `0x5304` | `0x5305` |
| Given current | `0x5308` | `0x5309` |

Telemetry begins at these M1 addresses and reads adjacent M1/M2 pairs:

| Field | Start register | Representation |
| --- | ---: | --- |
| Status | `0x5400` | 0 stopped, 1 running |
| Motor temperature | `0x5404` | signed, 0.1 C |
| MOS temperature | `0x5408` | signed, 0.1 C |
| Bus voltage | `0x540C` | 0.1 V |
| Actual speed | `0x5410` | signed raw value, documented as 0.1 RPM |
| Actual current | `0x5414` | signed, 0.1 A |
| Position | `0x5418` | 32-bit value per channel |
| Error code | `0x5420` | 32-bit value per channel |

Configuration/diagnostic helpers also use `0x2201..0x2203`, `0x5018..0x5019`,
`0x502C..0x502D`, `0x5620..0x5621` and `0x5688..0x568D`. Do not extrapolate
undocumented addresses from adjacency; verify against the exact drive model and
firmware before adding a register.

## Known unit defect

`svd48.c` currently copies the actual-speed register directly into
`actual_rpm`, and gateways label it `RPM`. Bench evidence indicates the register
scale is 0.1 RPM. This creates a factor-of-ten ambiguity in telemetry and control
validation. Fix the conversion at the driver boundary, rename raw fields where
needed and add positive/negative unit tests before using speed feedback for safety.

## Configuration writes

Serial and maintenance APIs expose confirmed single/multiple writes and several
SVD48-specific helpers. `CONFIRM` prevents accidental invocation but does not make
an address safe. Persistent configuration can change feedback, motor direction,
current limits or communication behavior and can make a drive unreachable.

For any write campaign:

1. Keep the mechanism unloaded and preserve an independent power disconnect.
2. Read and archive the original values outside this source repository.
3. Verify drive ID, channel, model and register meaning.
4. Change the smallest possible set, read it back and power-cycle when required.
5. Confirm stop behavior and telemetry before any low-speed motion.
6. Restore the known baseline if any observation differs from expectation.

Do not use `APPLY_PY6514_CONFIG` as a universal production profile. It is a
hardware-specific bench helper retained in source and requires explicit review.

## Driver evolution

New controllers must not add model-specific conditionals to `robot_control`. Each
driver should expose small typed capabilities such as velocity command, stop,
position command and feedback. A profile factory should map configured devices to
endpoints, while a single coordinator applies limits and command ownership.

Driver tests should cover framing, scaling, signed values, timeout, stale state,
exceptions, retries and idempotent stop. Hardware tests then validate electrical
and timing assumptions without replacing host coverage.

## Vendor references

Manual PDFs and downloaded product pages are intentionally not stored in this
repository. Obtain the exact manual for the controller model and firmware from the
manufacturer, then archive its title, revision, checksum and retrieval date in the
company artifact system. A mutable product URL is not a permanent specification.
