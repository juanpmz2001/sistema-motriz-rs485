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

## Integration Notes

- One SVD48V50A controller drives two motors. A four wheel robot uses two controllers on the same RS485 bus.
- The official page says the supported control interfaces include RS485, RS232, CAN, PWM, and analog input.
- The manual examples use a non-standard Modbus CRC byte order: high byte first, low byte second. The existing Arduino/ESP-IDF code in this repo already follows that order.
- The RS232 examples use slave ID `0xEE`; RS485 deployments should use the configured drive addresses, defaulting here to drive IDs `1` and `2`.
- Fulling's current SVD48RC documentation is not a replacement register map for
  the dual SVD48V controller. Its most useful transferable result is the complete
  set of electrical motor data required before third-party motor initialization.
