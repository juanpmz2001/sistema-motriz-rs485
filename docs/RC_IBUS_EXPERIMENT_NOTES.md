# RC i-BUS Experiment Notes

Date: 2026-06-20
Branch: `experiment/rc-ibus-fsia10b`

## Hardware Under Test

- Transmitter: FlySky `FS-i6`.
- Receiver: FlySky `FS-iA10B`, paired to the transmitter.
- Receiver data output connected through a 5 V to 3.3 V level shifter.
- Receiver and ESP32-S3 have common GND.
- ESP32-S3 input under test: GPIO18, UART1 RX.

## Sources Checked

- Official FlySky FS-i6 product page: confirms `AFHDS 2A`, 6 channels, and links official user manual.
  `https://www.flysky-cn.com/fsi6`
- Official FlySky FS-iA10B specifications: confirms `AFHDS 2A`, 10 PWM channels, power range, RSSI support, and data port `PWM / PPM / i.bus / s.bus`.
  `https://www.flysky-cn.com/ia10b-canshu`
- Common i-BUS frame references agree on: UART `115200`, frame header `0x20 0x40`, 14 little-endian channel values, 32 byte frame, checksum `0xFFFF - sum(bytes[0..29])`.

## Firmware Experiment Added

New diagnostic-only component:

- `components/ibus_receiver`

New serial commands:

- `IBUS_MODE [IBUS|IBUS_INV|IBUS_8N2|IBUS_INV_8N2|SBUS|SBUS_NOINV]`
- `IBUS_STATUS`
- `IBUS_CHANNELS`
- `IBUS_RAW`

Python CLI wrappers:

- `python3 tools/robotctl.py --port /dev/ttyACM0 ibus-mode IBUS`
- `python3 tools/robotctl.py --port /dev/ttyACM0 ibus-status`
- `python3 tools/robotctl.py --port /dev/ttyACM0 ibus-channels`

This path is diagnostic only. It does not drive motors and is not command authority.

## Observed Results

The ESP32-S3 serial gateway is reachable:

- `PING`: `OK PONG`
- `VERSION`: build `5`, partition `ota_0`, state `VALID`
- `PLATFORM_STATUS`: `SAFE_IDLE`, `SAFE_FOR_OTA:1`

GPIO18/UART1 receives bytes, but no valid i-BUS frames in any tested mode:

- `IBUS`: no `0x20 0x40` frame sync.
- `IBUS_INV`: more structured bytes, but no valid checksum.
- `IBUS_8N2`: no valid frame.
- `IBUS_INV_8N2`: occasional candidate frame, checksum invalid.
- `SBUS`: no valid i-BUS frame, used only as raw UART diagnostic.
- `SBUS_NOINV`: no valid i-BUS frame.

Internal pull-up on GPIO18 did not eliminate the bytes, but still no valid frame appeared.

## Interpretation

Current evidence does not show a valid FlySky servo i-BUS stream on GPIO18.

Most likely causes to check next:

- The receiver output is configured as PPM or S.BUS instead of i-BUS.
- The wire is connected to the i-BUS sensor/telemetry port rather than the servo i-BUS output.
- The FS-i6 menu is in `i-BUS setup` for telemetry modules, not receiver servo output.
- The level shifter or wiring is not passing a clean UART waveform.
- GPIO18 is not connected to the expected receiver signal after the level shifter.

Expected healthy i-BUS symptom:

- `IBUS_STATUS` should show `STATUS:OK`, fast-increasing `VALID`, and channels around `1000..2000`.
- `IBUS_RAW` should repeatedly contain frames starting with `20 40`.

## Next Hardware Checks

1. Confirm the exact FS-iA10B pin used. Use the servo/channel i-BUS output if present, not only the sensor telemetry bus.
2. On the FS-i6, check `System Setup -> RX Setup`.
3. If available, disable PPM output or select i-BUS/S.BUS output mode according to the receiver/transmitter firmware.
4. With a logic analyzer or oscilloscope on the ESP side of the level shifter, confirm:
   - idle level,
   - real transitions,
   - baud near 115200 for i-BUS,
   - repeated `0x20 0x40` frame starts if decoded as UART 115200 8N1.
5. If the transmitter cannot produce servo i-BUS with this receiver/firmware combination, test PPM first or use a receiver known to expose servo i-BUS directly.
