# SVD48B50A / SVD48V50A Controller Sources

The public documentation for `SVD48B50A` is not indexed under that exact model name. This repository treats the controller as the documented UU Motor `SVD48V50A / SVD48V Series`, which matches the register addresses already used by the original firmware.

## Local Files

- `SVD48V30A-user-manual-V2.01.pdf`: official UU Motor SVD48V Series user manual. Includes RS232/RS485 protocol, Modbus-style frames, register map, examples, CRC routine, and troubleshooting.
- `SVD48V-PC-software-manual-V1.1.pdf`: official SV-Config PC software manual. Useful for drive setup, motor parameter workflow, waveform monitoring, and firmware/configuration guidance.
- `uumotor-product-page.html`: captured product page for the SVD48V30A/SVD48V50A dual hub motor controller.
- `Fulling_SVD48_Series_Servo_Driver_User_Manual_V1.00_2025.pdf`: official
  Fulling manual for SVD4812RC-AA/SVD4822RC-AA. It is a related but incompatible
  single-axis family; use its third-party motor checklist only through the
  applicability analysis in `FULLING_MANUAL_AUDIT.md`.
- `Fulling_SVD4812RC-AA_Servo_Driver_Datasheet_2025.pdf`: official three-page
  product datasheet confirming the related drive's model, current and topology.
- `FULLING_MANUAL_AUDIT.md`: comparison, applicability boundary, KK16 data gaps,
  and manufacturer information request.

## Upstream Links

- Official user manual: https://www.uumotor.com/wp-content/uploads/2022/04/SVD48V30A-user-manual-V2.01.pdf
- Official product page: https://www.uumotor.com/multi-function-rs485-can-encoder-hall-sensors-brushless-dc-dual-control-driver.html
- Official PC software manual: https://www.uumotor.com/en/wp-content/uploads/2022/04/SVD48V-PC-software-manual-V1.1.pdf
- ManualsLib mirror: https://www.manualslib.com/products/Uumotor-Svd48v50a-12879845.html
- Fulling official SVD48 manual: https://www.fullingmotor.com/en/Uploads/file/20250122/1737543938891611.pdf
- Fulling official SVD4812RC-AA datasheet: https://www.fullingmotor.com/en/Uploads/file/20250122/1737543933622349.pdf
- Fulling SVD4812RC-AA product page: https://www.fullingmotor.com/en/product/76_233
- Fulling SVD4822RC-AA product page: https://www.fullingmotor.com/en/product/76_232
- Official SV-Config download: https://retail.uumotor.com/wp-content/uploads/2022/08/software20220727.zip
- Official KK16-compatible motor profile: https://retail.uumotor.com/wp-content/uploads/2022/08/uuMotor_48V50A_1100rpm.zip

## Integration Notes

- One SVD48V50A controller drives two motors. A four wheel robot uses two controllers on the same RS485 bus.
- The official page says the supported control interfaces include RS485, RS232, CAN, PWM, and analog input.
- The manual examples use a non-standard Modbus CRC byte order: high byte first, low byte second. The existing Arduino/ESP-IDF code in this repo already follows that order.
- The RS232 examples use slave ID `0xEE`; RS485 deployments should use the configured drive addresses, defaulting here to drive IDs `1` and `2`.
- Fulling's current SVD48RC documentation is not a replacement register map for
  the dual SVD48V controller. Its most useful transferable result is the complete
  set of electrical motor data required before third-party motor initialization.

## KK16 Profile And Motor Identification

UUMOTOR explicitly lists `uuMotor_48V50A_1100rpm` for KK5WS, KK5WT, KK6156 and
KK16 motors. Its motor values for both channels are: 10 pole pairs, Hall sensor,
`KV=166` in SV-Config units, `Rs=0.06972 ohm`, `Ld=Lq=0.00013348 H`, 60 A,
1100 motor RPM and a 1:5 driving/driven tooth ratio. Limits such as 60 A and
1100 RPM are manufacturer-profile values, not safe robot operating limits.

Static analysis of the official SV-Config package confirms its identification
protocol for this dual SVD48V family:

- start/stop M1: function `0x10`, register `0x5700`, one word, value `1/0`;
- start/stop M2: function `0x10`, register `0x5701`, one word, value `1/0`;
- state: `0x5710/0x5711`;
- identified Rs: `0x5714/0x5715`;
- identified Ld: `0x5718/0x5719`;
- identified Lq: `0x571C/0x571D`.

The official PC manual says the operation detects phase resistance and phase
inductance. Neither the manual nor the SV-Config parameter map identifies KV.
Do not describe this as KV calibration. The result-register scale is not
published in the XML and must be validated against SV-Config before applying
raw results to the persistent motor parameters.
