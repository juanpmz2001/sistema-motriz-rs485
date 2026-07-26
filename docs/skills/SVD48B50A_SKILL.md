# SVD48B50A / SVD48V50A Skill

Read this before editing RS485 code. Public docs identify this controller as UU Motor SVD48V50A / SVD48V Series. One controller drives two motors: M1 and M2. Four-motor robots use two controllers.

Also read `docs/controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md` before changing motor parameters, Hall calibration, PID tuning, or gear-ratio behavior.
Read `docs/controllers/SVD48B50A/FULLING_MANUAL_AUDIT.md` before using Fulling
sources. The 2025 Fulling SVD48 manual is for a related single-axis RC family and
its addresses/procedures are not directly compatible with the dual SVD48V.
For the latest connected-controller evidence, read
`docs/process/evidence/SVD48_BENCH_DIAGNOSIS_2026-07-21.md` and
`docs/process/evidence/SVD48_RESTORE_TO_INITIAL_2026-07-21.md`. The latter is
the current parameter-restoration record; earlier dated captures are snapshots,
not a live-state claim.

## Default Robot Topology

- RS485 bus, 115200 8N1.
- Drive ID 1: MOTOR_0=M1, MOTOR_1=M2.
- Drive ID 2: MOTOR_2=M1, MOTOR_3=M2.
- RS232 examples use fixed slave `0xEE`; RS485 uses configured slave IDs.
- Historical observed bench setup: only controller ID `0x02` and its `M1` channel were configured during the run recorded in `docs/controllers/SVD48B50A/OBSERVED_TOÑO_CONFIG.md`. Do not assume that topology is still physically connected; query inventory/status before interpreting timeouts.

## Protocol

- Function `0x03`: read holding registers.
- Function `0x06`: write single register.
- Function `0x10`: write multiple registers.
- Exception response function is the requested function OR `0x80`: `0x83` for read `0x03`, `0x86` for write-single `0x06`, and `0x90` for write-multiple `0x10`. Documented codes are `01` invalid function, `02` invalid register, and `03` invalid value/length. The driver classifies matching high-bit responses and preserves function, code, and timestamp in motor telemetry; physical controller validation is still required by `TEST-SVD-007`.
- CRC: Modbus CRC16, init `0xFFFF`, poly `0xA001`, calculated over slave through data bytes.
- Important: UU Motor transmits CRC high byte then low byte, matching the manual examples and current repo. This is swapped versus normal Modbus RTU libraries.
- The current main configuration uses a 100 ms response timeout and 2 retries for manual reads and function `0x06` writes. Background telemetry explicitly passes `0` retries per register read, then backs a failed drive off between polling passes. The internal function `0x10` transaction makes one attempt because a missing ACK is ambiguous and must be classified by readback instead of a blind retry. A failed transaction and the serialized bus can still delay stop/other-drive work; see `SAFE-006` and do not claim bounded stop latency until it is measured.
- Background telemetry reads position first on every fast poll, then speed/current. Status, temperature, bus voltage and error registers are slower polls.
- When a telemetry position read times out, back off that drive for 250-1500 ms and keep polling the other drive.

## Useful Registers

- M1/M2 motor pole pairs: M1 `0x5018`, M2 `0x5019`; range `1..128`.
- M1/M2 sensor type: M1 `0x502C`, M2 `0x502D`; `0=encoder`, `1=Hall`, `2=string encoder`.
- Shared wheel diameter is `0x2201` mm. The older manual-derived gear candidates
  `0x2202/0x2203` are invalid on software `0x0131` for both reads and FC `0x10`;
  do not use the legacy `SET_SVD48_GEAR_RATIO` command on this controller.
- The official SV-Config XML and live tests established the real per-channel gear
  fields: M1 driving `0x5030`, M2 driving `0x5031`, M1 driven `0x5034`, M2 driven
  `0x5035`. The observed correct KK16 configuration is driving/driven `1/5` on
  both channels. An inverted `5/1` test caused severe oscillation, motor-overload
  fault `0x00000800`, and loss of RS485 response until power cycle. Never infer
  register order from the verbal ratio alone.
- Before another KK16 Hall calibration, obtain line-line resistance/inductance,
  back-EMF or rotor-side KV, torque constant, rotor inertia, Hall spacing and phase
  sequence. Fulling's third-party motor procedure treats these as prerequisites.
- Hall installation/status: M1/M2 installation `0x5620/0x5621`; calibration
  current `0x5624/0x5625`; calibration status `0x5684/0x5685`; Hall state
  `0x5688/0x5689`; current Hall angle `0x568C/0x568D`. Status values are
  `0=success`, `1=calibrating`, `2=failed`. Both channels ended at `2` in the
  2026-07-21 tests; populated angle tables do not make those calibrations valid.
- Historical physical speed test: with configured pole pairs `48`, command `1 RPM`, measured wheel period `12.75 s/rev`; inferred actual pole pairs are about `10.2`. This is a hypothesis, not a motor datasheet fact: observed configurations have also contained `24` and an experiment used `48`. Verify each robot/motor before staging a pole-pair change; never auto-apply `10` from this note.
- Mode: M1 `0x5100`, M2 `0x5101`; `0=speed`, `1=position`, `2=torque`, `3=voltage`, `4=skateboard`, `5=kart`.
- Accel/decel/smoothing: M1 `0x5108/0x510C/0x5110`; M2 `0x5109/0x510D/0x5111`.
- Control command: M1 `0x5300`, M2 `0x5301`; `0=stop`, `1=start`, `2=clear alarm`. Do not call value `2` brake.
- Given speed RPM: M1 `0x5304`, M2 `0x5305`, signed int16.
- Given current 0.1A: M1 `0x5308`, M2 `0x5309`, signed int16.
- Actual status: M1 `0x5400`, M2 `0x5401`; `0=stop`, `1=running`.
- Motor temp 0.1C: M1 `0x5404`, M2 `0x5405`.
- MOS temp 0.1C: M1 `0x5408`, M2 `0x5409`.
- Bus voltage 0.1V: M1 `0x540C`, M2 `0x540D`.
- Actual speed 0.1 RPM: M1 `0x5410`, M2 `0x5411`. Divide the signed raw value
  by 10 for physical RPM. Build 19 still stores/prints this raw value as `RPM` in
  typed telemetry (`SVD-009`), so `GET_SPEED`/`GET_MOTOR` must not be interpreted
  literally until that contract is migrated.
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
- Treat every requested-function-plus-`0x80` response as a controller exception and preserve both exception function and code. The pure parser and `0x83/0x86/0x90` fixtures are in `svd48_protocol.c` and `tests/host/firmware_contracts_test.c`; retain those tests when extending transactions.
- `svd48_write_registers_by_id()` is a bus primitive, not authorization. The current MVP exposes it indirectly through `WRITE_REGS ... CONFIRM` for elevated bench use only, with an actuation denylist, stopped heuristic, old-value capture, one FC `0x10` attempt and exact readback. Do not broaden that provisional surface or call it production-safe until `robot_state`, exclusive maintenance ownership, typed catalog/ranges, deduplication, persistence, rollback and audit are enforced.
- Decode signed 16-bit values as two's complement.
- Current position/error telemetry decoding uses high register first, then low
  register. Official XML plus 339-field live inventories also confirm
  high-word-first IEEE-754 for the observed PID/electrical fields, but new 32-bit
  families still require catalog evidence rather than a blanket assumption.
- Publish stale telemetry when no valid sample has arrived within the configured stale timeout.
- Build 19 temporarily allows authenticated `maintenance_lan SET_SPEED` up to
  `+/-15 RPM`. It has no TTL/dead-man/authority gate and can leave a command
  active after client/network loss. Use only under the elevated-bench procedure
  with a physical cutoff; remove/gate it before product or floor use (ADR-0004,
  `SAFE-011`).
