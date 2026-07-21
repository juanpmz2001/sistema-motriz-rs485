# Component Notes

This file used to describe the first ESP-IDF migration with Bluetooth and PPM fallback. The active robot framework is now documented in:

- `README.md`
- `docs/API.md`
- `docs/skills/SVD48B50A_SKILL.md`
- `docs/ROBOT_PROFILES_AND_SVD48_CONFIGURATION_PLAN.md`
- `docs/schemas/robot-profile.schema.json`

## Active Components

- `components/svd48`: SVD48V50A/SVD48B50A RS485 driver with read/write transactions, telemetry polling, logical motor mapping, and UU Motor CRC byte order.
- `components/robot_control`: four-wheel robot abstraction, independent steering kinematics for `MOVE_VEL`, and PWM steering servo outputs.
- `components/robot_safety`: high-priority RC/motor-fault safety supervisor. It never performs Wi-Fi, HTTP, JSON, OTA or NVS work.
- `components/robot_state`: pure operational state model plus a mutex-protected runtime service with single-owner inhibit slots and gate epochs. It compiles, but `main` does not instantiate it and actuator APIs do not enforce it yet.
- `components/command_authority`: pure host-tested `RC > LAN > Bluetooth` mailbox/arbiter model with TTL, dead-man, sequence validation, authority epochs and stop/fault outcomes. Runtime source adapters remain pending.
- `components/robot_kinematics`: pure generic differential strategy with variable motor arrays and proportional RPM saturation. Robot-profile and actuator integration remain pending.
- `components/ibus_receiver`: common FlySky PPM/i-BUS/SBUS receiver facade. The active default is 10-channel PPM on GPIO14; it currently feeds diagnostics and signal-loss safety, not motion commands.
- `components/ppm_decoder`: bounded PPM frame decoder with a pure host-tested model and an ESP32 GPIO interrupt wrapper. The interrupt service is installed without `ESP_INTR_FLAG_IRAM`.
- `components/serial_gateway`: ASCII PC gateway over the ESP-IDF console/USB serial stream, plus the shared command dispatcher used by LAN maintenance.
- `components/config_manager`: NVS-backed Wi-Fi, OTA and LAN maintenance configuration store.
- `components/wifi_manager`: Wi-Fi station manager used by manual OTA, automatic manifest checks, and low-priority auto-connect/reconnect.
- `components/ota_manager`: OTA manifest validation, inactive-slot download verification, manual update, rollback state, and automatic manifest-only checks.
- `components/ota_announce`: authenticated UDP LAN announce listener for no-USB OTA server discovery.
- `components/maintenance_lan`: authenticated UDP LAN maintenance listener for diagnostics, telemetry, `STOP ALL`, register reads and provisional confirmed SVD48 configuration writes without USB. Movement, runtime actuation registers, sensitive ESP configuration mutation and destructive OTA remain blocked.
- `components/control_lan`: separate bounded UDP ingress on port `32322` for typed arm/command/disarm/stop events. It is compiled but deliberately not started or connected to movement yet.

The root `CMakeLists.txt` explicitly includes the project component tree, including the compiled-but-disabled authority, kinematics and control-ingress foundations. The active runtime dependency graph is rooted in `main/CMakeLists.txt`, which requires `svd48`, `robot_control`, `robot_safety`, `robot_state`, `serial_gateway`, `ibus_receiver`, `config_manager`, Wi-Fi/OTA services, and `maintenance_lan`.

## Legacy Components

These remain for reference only and are not required by `main/main.c`:

- `components/motor_controller`
- `components/bluetooth_controller`
- `Codigo_funcional_control_RC_RS232 (1).ino`
- `adelante_atras_un_motor_bt_connection.ino`

Do not copy RS485 behavior from the legacy `motor_controller` without checking `docs/skills/SVD48B50A_SKILL.md` first. The SVD48 manual defines control command value `2` as `clear alarm`, not brake.
