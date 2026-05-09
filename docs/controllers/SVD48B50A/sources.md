# SVD48B50A / SVD48V50A Controller Sources

The public documentation for `SVD48B50A` is not indexed under that exact model name. This repository treats the controller as the documented UU Motor `SVD48V50A / SVD48V Series`, which matches the register addresses already used by the original firmware.

## Local Files

- `SVD48V30A-user-manual-V2.01.pdf`: official UU Motor SVD48V Series user manual. Includes RS232/RS485 protocol, Modbus-style frames, register map, examples, CRC routine, and troubleshooting.
- `SVD48V-PC-software-manual-V1.1.pdf`: official SV-Config PC software manual. Useful for drive setup, motor parameter workflow, waveform monitoring, and firmware/configuration guidance.
- `uumotor-product-page.html`: captured product page for the SVD48V30A/SVD48V50A dual hub motor controller.

## Upstream Links

- Official user manual: https://www.uumotor.com/wp-content/uploads/2022/04/SVD48V30A-user-manual-V2.01.pdf
- Official product page: https://www.uumotor.com/multi-function-rs485-can-encoder-hall-sensors-brushless-dc-dual-control-driver.html
- Official PC software manual: https://www.uumotor.com/en/wp-content/uploads/2022/04/SVD48V-PC-software-manual-V1.1.pdf
- ManualsLib mirror: https://www.manualslib.com/products/Uumotor-Svd48v50a-12879845.html

## Integration Notes

- One SVD48V50A controller drives two motors. A four wheel robot uses two controllers on the same RS485 bus.
- The official page says the supported control interfaces include RS485, RS232, CAN, PWM, and analog input.
- The manual examples use a non-standard Modbus CRC byte order: high byte first, low byte second. The existing Arduino/ESP-IDF code in this repo already follows that order.
- The RS232 examples use slave ID `0xEE`; RS485 deployments should use the configured drive addresses, defaulting here to drive IDs `1` and `2`.
