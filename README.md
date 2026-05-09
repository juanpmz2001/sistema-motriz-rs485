# SVD48 Robot Control and Telemetry Framework

ESP-IDF firmware for an ESP32-S3 connected to UU Motor SVD48V50A/SVD48B50A hub motor controllers over RS485.

The goal is to make the robot controllable from a PC terminal with simple ASCII commands:

```text
GET_SPEED 0
DATA MOTOR_0 RPM:1450 STALE:0 ONLINE:1

MOVE_VEL 1.0 0.0 0.5
OK MOVE_VEL VX:1.000 VY:0.000 WZ:0.500 ...
```

## Start Here

- Controller knowledge base: `docs/skills/SVD48B50A_SKILL.md`
- PC command contract: `docs/API.md`
- Downloaded controller sources: `docs/controllers/SVD48B50A/`
- Python CLI: `tools/robotctl.py`
- Host protocol tests: `tools/test_svd48_protocol.py`

## Firmware Architecture

- `components/svd48`: serialized RS485 driver for two SVD48 drives / four logical motors. Implements reads, writes, telemetry polling, UU Motor CRC byte order, and stale/fault state.
- `components/robot_control`: maps four logical motors into robot commands. Implements `MOVE_VEL vx vy wz` as independent steering kinematics and controls four PWM steering servos.
- `components/serial_gateway`: ASCII gateway over the ESP-IDF console/USB serial stream. Emits `OK`, `ERR`, and `DATA` responses.
- `main`: initializes RS485, starts telemetry polling, starts robot control, and exposes the serial gateway.

The old Arduino sketches and previous ESP-IDF Bluetooth/PPM components are kept as historical reference, but the active firmware path is the SVD48 framework above.

## Default Hardware Assumptions

- ESP32-S3.
- UART2 RS485 TX/RX: GPIO 17 / GPIO 16.
- Two SVD48 drives on RS485 IDs `1` and `2`.
- Logical motor mapping:
  - `MOTOR_0`: drive 1 M1, front-left.
  - `MOTOR_1`: drive 1 M2, front-right.
  - `MOTOR_2`: drive 2 M1, rear-left.
  - `MOTOR_3`: drive 2 M2, rear-right.
- Steering servo GPIO defaults: 4, 5, 6, 7.

Adjust these defaults in `main/main.c` before flashing if the harness differs.

## PC Usage

```bash
python tools/robotctl.py --port COM6 ping
python tools/robotctl.py --port COM6 get-speed 0
python tools/robotctl.py --port COM6 get-motor 0
python tools/robotctl.py --port COM6 move-vel 1.0 0.0 0.5
python tools/robotctl.py --port COM6 watch
```

Install the only PC dependency with:

```bash
python -m pip install pyserial
```

## Build

Use an ESP-IDF shell with the target set to ESP32-S3:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```
