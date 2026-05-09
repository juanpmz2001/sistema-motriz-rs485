# Component Notes

This file used to describe the first ESP-IDF migration with Bluetooth and PPM fallback. The active robot framework is now documented in:

- `README.md`
- `docs/API.md`
- `docs/skills/SVD48B50A_SKILL.md`

## Active Components

- `components/svd48`: SVD48V50A/SVD48B50A RS485 driver with read/write transactions, telemetry polling, logical motor mapping, and UU Motor CRC byte order.
- `components/robot_control`: four-wheel robot abstraction, independent steering kinematics for `MOVE_VEL`, and PWM steering servo outputs.
- `components/serial_gateway`: ASCII PC gateway over the ESP-IDF console/USB serial stream.

## Legacy Components

These remain for reference only and are not required by `main/main.c`:

- `components/motor_controller`
- `components/ppm_decoder`
- `components/bluetooth_controller`
- `Codigo_funcional_control_RC_RS232 (1).ino`
- `adelante_atras_un_motor_bt_connection.ino`

Do not copy RS485 behavior from the legacy `motor_controller` without checking `docs/skills/SVD48B50A_SKILL.md` first. The SVD48 manual defines control command value `2` as `clear alarm`, not brake.
