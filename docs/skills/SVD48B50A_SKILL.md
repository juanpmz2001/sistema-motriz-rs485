# SVD48B50A / SVD48V50A Skill

Read this before editing RS485 code. Public docs identify this controller as UU Motor SVD48V50A / SVD48V Series. One controller drives two motors: M1 and M2. Four-motor robots use two controllers.

Also read `docs/controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md` before changing motor parameters, Hall calibration, PID tuning, or gear-ratio behavior.

## Default Robot Topology

- RS485 bus, 115200 8N1.
- Drive ID 1: MOTOR_0=M1, MOTOR_1=M2.
- Drive ID 2: MOTOR_2=M1, MOTOR_3=M2.
- RS232 examples use fixed slave `0xEE`; RS485 uses configured slave IDs.
- Current bench setup: only controller ID `0x02` is connected, and only its `M1` channel is configured. Timeouts on ID `0x01` and unused M2 readings are expected in this setup.

## Protocol

- Function `0x03`: read holding registers.
- Function `0x06`: write single register.
- Function `0x10`: write multiple registers.
- Exception response function: `0x90`; codes: `01` invalid function, `02` invalid register, `03` invalid value/length.
- CRC: Modbus CRC16, init `0xFFFF`, poly `0xA001`, calculated over slave through data bytes.
- Important: UU Motor transmits CRC high byte then low byte, matching the manual examples and current repo. This is swapped versus normal Modbus RTU libraries.
- Use 100 ms response timeout on this robot. Manual commands keep 2 retries; background telemetry uses 0 retries so one missing drive does not block the whole bus.
- Background telemetry reads position first on every fast poll, then speed/current. Status, temperature, bus voltage and error registers are slower polls.
- When a telemetry position read times out, back off that drive for 250-1500 ms and keep polling the other drive.

## Useful Registers

- M1/M2 motor pole pairs: M1 `0x5018`, M2 `0x5019`; range `1..128`.
- M1/M2 sensor type: M1 `0x502C`, M2 `0x502D`; `0=encoder`, `1=Hall`, `2=string encoder`.
- Vehicle/wheel parameters used by SV-Config for geared motors: wheel diameter `0x2201` mm, motor teeth `0x2202`, wheel teeth `0x2203`. For PY6514 initial hypothesis use diameter `330`, motor teeth `1`, wheel teeth `5` for ratio `5:1`.
- Observed on Toño hardware: drive ID `2` accepts `0x2200` and `0x2201`, but returns an invalid-register exception for `0x2202/0x2203`. Do not assume every SV-Config field is exposed on every controller firmware revision.
- Hall installation/status: M1/M2 installation `0x5620/0x5621`; Hall status `0x5688/0x5689`; current Hall angle `0x568C/0x568D`.
- Physical speed test: with configured pole pairs `48`, command `1 RPM`, measured wheel period `12.75 s/rev`; inferred actual pole pairs are about `10.2`. Treat the motor as `10` pole pairs unless later measurement contradicts it.
- Mode: M1 `0x5100`, M2 `0x5101`; `0=speed`, `1=position`, `2=torque`, `3=voltage`, `4=skateboard`, `5=kart`.
- Accel/decel/smoothing: M1 `0x5108/0x510C/0x5110`; M2 `0x5109/0x510D/0x5111`.
- Control command: M1 `0x5300`, M2 `0x5301`; `0=stop`, `1=start`, `2=clear alarm`. Do not call value `2` brake.
- Given speed RPM: M1 `0x5304`, M2 `0x5305`, signed int16.
- Given current 0.1A: M1 `0x5308`, M2 `0x5309`, signed int16.
- Actual status: M1 `0x5400`, M2 `0x5401`; `0=stop`, `1=running`.
- Motor temp 0.1C: M1 `0x5404`, M2 `0x5405`.
- Bus voltage 0.1V: M1 `0x5408`, M2 `0x5409`.
- MOS temp 0.1C: M1 `0x540C`, M2 `0x540D`.
- Actual speed RPM: M1 `0x5410`, M2 `0x5411`.
- Actual current 0.1A: M1 `0x5414`, M2 `0x5415`.
- Position/encoder int32: M1 `0x5418`, M2 `0x541A`.
- Error code uint32: M1 `0x5420`, M2 `0x5422`.

## Hex Examples

- Read M1/M2 speed via RS232/example ID: `EE 03 54 10 00 02 61 C3`.
- Read M1/M2 speed on RS485 ID 1: `01 03 54 10 00 02 FE D5`.
- Read M1/M2 current on RS485 ID 1: `01 03 54 14 00 02 3F 94`.
- Read M1/M2 position on RS485 ID 1: `01 03 54 18 00 04 3E D4`.
- Read M1/M2 errors on RS485 ID 1: `01 03 54 20 00 04 F3 55`.
- Start M1 on ID 1: `01 06 53 00 00 01 4E 59`.
- Set M1 speed +100 RPM on ID 1: `01 06 53 04 00 64 A4 D8`.

## Driver Rules

- Keep RS485 as a single-master serialized queue. Never let command handlers and telemetry polling write the UART concurrently.
- Validate response slave ID, function code, byte count, and UU Motor CRC before using data.
- Treat `0x90` exception responses as driver errors and preserve the exception code in telemetry.
- Decode signed 16-bit values as two's complement.
- Decode 32-bit registers from two 16-bit registers in register order: high register first, then low register.
- Publish stale telemetry when no valid sample has arrived within the configured stale timeout.
