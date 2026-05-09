# UU Motor Product Page Summary

Source: https://www.uumotor.com/multi-function-rs485-can-encoder-hall-sensors-brushless-dc-dual-control-driver.html

The page identifies the product as a multi-function RS485/CAN encoder/hall sensor brushless DC dual controller with models `SVD48V30A` and `SVD48V50A`.

Relevant facts for this project:

- Operating voltage: 24 V to 48 V DC.
- SVD48V50A maximum input continuous current: 40 A.
- SVD48V50A highest output Iq current: 50 A.
- SVD48V50A adapted motor power: 400 W to 800 W.
- Control modes: speed mode, position mode, torque mode.
- Control interfaces: RS485, RS232, CAN, PWM, analog input.
- Encoder inputs include A/B/Z, A/B + HALL, and Hall sensor options.
- One controller can drive two motors; two controllers can be used for four motors.
- The page links the official user manual and PC programming software.

Important implementation note from the product page comments:

- A 2025 discussion points out that the controller's CRC byte order is swapped relative to standard Modbus RTU libraries. UU Motor replies that integrators should follow the manual/demo code. This repository therefore implements the documented UU Motor byte order: CRC high byte first, then low byte.
