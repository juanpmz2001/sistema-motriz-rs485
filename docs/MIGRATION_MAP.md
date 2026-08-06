# Iteration 4 migration map

## Scope and evidence

This map describes the as-built state of
`refactor/svd48-device-composition` after the Iteration 4 closeout work. The
foundation arrived in commit
`ce42f8f0837f01b84b27789b212efd055c4dea40`; subsequent closeout changes added
the missing safety semantics, tests, diagnostics and reproducible builds. It
records executable paths, not the target architecture. Source and executable
verification remain the acceptance evidence; this document does not authorize
hardware actuation.

Status values are `active`, `transitional`, `partial`, `dormant`, and `planned`.

## Migrated speed path

```text
serial_gateway (SET_SPEED)
  -> actuation_application_port
  -> robot_composition application adapter
  -> actuation_coordinator (500 ms mutex acquisition bound)
  -> robot_endpoint registry
  -> svd48_channel_endpoint_adapter
  -> svd48_channel (M1 or M2)
  -> svd48_device (one instance per physical controller)
  -> bus_transport (serialized transaction)
  -> rs485_transport
  -> UART
```

`maintenance_lan` delegates allowed ASCII commands to `serial_gateway`, so its
allowed `SET_SPEED` follows the same path. A successful operation is mirrored
into the legacy commanded-state snapshot; that mirror performs no physical
write.

## Migrated stop paths

| Source | Actual path | State |
| --- | --- | --- |
| `STOP n` | gateway -> application port -> coordinator -> one direct SVD48 channel adapter | Migrated |
| `STOP ALL` | gateway -> application port -> coordinator -> all configured stoppable endpoints | Migrated |
| Boot stop | `main` -> application port -> coordinator -> all configured stoppable endpoints | Migrated; failure still only warns |
| Safety stop | `robot_safety` -> stop subset of application port -> coordinator -> all configured stoppable endpoints | Migrated; health input is still legacy |
| LAN stop | maintenance LAN -> gateway -> the corresponding path above | Migrated, but still lacks authority/lease policy |

Each SVD48 channel stop writes target RPM zero and then the channel stop command.
The coordinator serializes the complete multi-endpoint operation. It is not a
priority-aware owner task, and a safety stop may wait behind another operation.

## Component disposition

| Component | Current state | Destination | Migrated responsibilities | Remaining responsibilities / legacy callers | Removal condition |
| --- | --- | --- | --- | --- | --- |
| `main` | Active composition root | Keep as lifecycle/composition root | Selects profile, invokes executable composition, wires services; starts a serial-only restricted gateway when a schema-valid profile is not composable | Still constructs the legacy `robot_control` facade in the supported runtime | Remaining legacy service ports are explicit and tested |
| `robot_profile` | Active | Versioned immutable embedded profile | Board, buses, devices, channels, endpoints, criticality, geometry and Kconfig selection | No JSON/YAML runtime profile loader or general factory for arbitrary schema drivers; some operational fields are factory-validated | Profile contract and build selection remain reproducible |
| `robot_driver_factory` | Active, specialized | Static executable driver registry | Pure preflight, factory lookup, capability checks, summed `storage_required` validation and capacity diagnostics | Registry has only SVD48 | Every future registered runtime driver has construction and integration evidence |
| `robot_composition` | Active, transitional | Profile-driven construction | RS485 buses by ID, SVD48 devices, endpoints, coordinator, poll service and legacy bindings | Legacy wrapper is still required by `robot_control`; its four-binding capacity is rejected in preflight with `LEGACY_BINDING_LIMIT` | Legacy callers have typed ports and wrapper can be removed |
| `bus_transport` | Active | Shared transport port | Serializes transactions, normalizes results and protects statistics snapshots with a dedicated lock | The ESP-IDF UART timing remains hardware-dependent | Host concurrency/statistics contracts and hardware timing qualification exist |
| `rs485_transport` | Active | ESP-IDF UART backend for `bus_transport` | UART configuration, exchange timeout, cancellation and bus mutex | Hardware timing remains bench-only | Host contracts plus hardware timing qualification exist |
| `svd48_protocol` | Active, reusable | Keep pure and device-independent | CRC, request builders, exception and write-multiple parsing, actuation-register classification; golden vectors cover reads, single/multiple writes, both channels, stops, exception and bad CRC | Protocol timing remains outside the pure module | New protocol behavior retains golden-vector evidence |
| `svd48_device` | Active | Physical dual-channel SVD48 driver | Addressed device transactions, M1/M2 channels, maintenance access, per-observation validity/freshness/failure and complete/partial poll results | Physical RPM interpretation and fault behavior remain unqualified on hardware | Hardware qualification confirms the documented manual-based interpretation |
| `svd48_channel` | Active | Typed device channel | M1/M2 register selection, RPM target, enable, stop, clear fault and current | Some operations are still reached through the legacy facade | All operational callers use reviewed application ports |
| `svd48_poll_service` | Active | Shared N-device polling scheduler | Bounded registry, independent periods, wrap-safe deadlines and error/partial backoff that never shortens the nominal period | Poll task latency remains hardware-dependent | Resource and latency evidence is recorded |
| `svd48_poll_task` | Active | Low-level polling task wrapper | Runs the shared poll service at priority 8 | Timing/stack margin is not hardware-qualified | Resource and latency evidence is recorded |
| `svd48_channel_endpoint_adapter` | Active for speed/stop | Direct typed SVD48 endpoint | Velocity and stop capabilities, range enforcement, rollback stop and per-observation diagnostics | Endpoint availability is configured independently of safety policy | Active safety consumes approved endpoint health |
| Legacy `svd48_handle_t` wrapper | Active, transitional | Remove after callers use devices/ports | Preserves logical indices, telemetry and maintenance compatibility over attached devices | Fixed maximum of four bindings; trace, reads/writes, enable, clear-fault and legacy telemetry still use it | `robot_control`, OTA and gateway no longer require the handle |
| `actuation_coordinator` | Active, partial | One reviewed actuation boundary | Serializes migrated speed/stop and reports partial application | Not the global writer; no priority, authority, TTL or deadman | All writers are migrated and stop precedence is tested |
| `robot_control` | Active, legacy | Split into motion, maintenance and telemetry ports | Commanded-state mirror after coordinated speed/stop | `ENABLE`, `MOVE_VEL`, fault clear, OTA preparation, telemetry, trace and register maintenance | Every responsibility has an executable replacement and compatibility tests |
| `robot_safety` | Active, mixed | Profile-aware health/safety supervisor | Stop output uses the application port | Reads legacy telemetry; stale/offline are not safety faults | Health policy and required/omitted device behavior are approved and tested |
| `serial_gateway` | Active, mixed | Parser/dispatcher over application ports | `SET_SPEED`, `STOP n`, `STOP ALL` use the application port; restricted diagnostic mode exposes read-only status and an explicit unavailable stop without touching legacy handles | Remaining device/maintenance/OTA handlers call `robot_control` in normal mode | Handler compatibility is characterized and concrete dependencies are removed incrementally |
| `maintenance_lan` | Active | Authenticated maintenance adapter | Delegates through gateway policy | Allowed motion/writes have no lease, deadman or replay protection | Production allowlist and authority policy are implemented |
| `ota_manager` / `ota_announce` | Active, legacy edge | OTA application/state ports | Network work remains low priority | Safe query and preparation use `robot_control` | OTA preparation uses reviewed state/stop ports |
| `robot_state`, `command_authority`, `robot_kinematics`, `control_lan` | Dormant foundations | Integrate only in later roadmap slices | Compiled and host-tested models where applicable | Not part of the active runtime | Their dedicated iteration criteria are approved |

## Remaining legacy call classes

| Call class | Classification | Current route |
| --- | --- | --- |
| `ENABLE` | Operational legacy | gateway -> `robot_control` -> legacy wrapper -> channel/device |
| `MOVE_VEL` and servo output | Kinematics/operational legacy | gateway -> `robot_control`; requires the four-motor geometry profile |
| `CLEAR_FAULT` | Maintenance/operational legacy | gateway -> `robot_control` -> legacy wrapper |
| `GET_SPEED`, `GET_MOTOR`, platform status | Telemetry legacy | gateway -> `robot_control` -> wrapper snapshot |
| `READ_REG` | Read-only maintenance legacy | gateway -> `robot_control` -> wrapper -> addressed device |
| `WRITE_REG`, `WRITE_REGS` and SVD48 configuration helpers | Hardware-changing maintenance legacy | gateway -> `robot_control` -> wrapper; runtime actuation registers are blocked |
| `SVD48_IDENTIFY START|STOP` | Physical-motion maintenance legacy | gateway -> `robot_control` -> wrapper; writes identification register `0x5700` outside the coordinator |
| OTA safe query/preparation | OTA legacy | OTA/gateway -> `robot_control`; preparation issues legacy per-motor stops |
| Trace | Diagnostic legacy | gateway -> `robot_control` -> wrapper/device trace hooks |
| Polling | Migrated implementation with legacy access seam | composition-owned N-device poll service; legacy `POLL_ONCE` delegates through `robot_control`/wrapper |
| Safety observation | Safety legacy | `robot_safety` reads `robot_control` telemetry; stop output is migrated |

## Verified implementation status after closeout

| Iteration 4 claim | Status | Evidence |
| --- | --- | --- |
| `bus_transport` | IMPLEMENTED AND HOST-TESTED | Fake-backend tests cover results, statistics, release, cancellation and deterministic two-thread serialization |
| `rs485_transport` | IMPLEMENTED; BUILD-TESTED | Both ESP-IDF profile builds compile the UART backend; no hardware timing claim is made |
| One `svd48_device` per physical controller | IMPLEMENTED AND HOST-TESTED | Device tests plus profile/factory tests cover identity, address, channel and construction contracts |
| Explicit M1/M2 channels | IMPLEMENTED AND HOST-TESTED | Golden request assertions cover separate control, velocity and current registers |
| Polling for N devices | IMPLEMENTED AND HOST-TESTED | Poll-service tests cover one/two devices, periods, partial results, recovery, omission and wraparound |
| Direct channel-to-capability adapter | IMPLEMENTED AND HOST-TESTED | Adapter tests cover capabilities, bounds, M1/M2, rollback stop and diagnostics |
| Executable registry/factory | PARTIAL | SVD48 is registered and preflight capacity is enforced/tested; no other runtime driver factory exists |
| Composition from profile | IMPLEMENTED; HOST- AND BUILD-TESTED | Pure profile/preflight tests and clean ESP-IDF builds cover the static composition inputs and integration |
| `bench_single_svd48_motor` | IMPLEMENTED; HOST- AND BUILD-TESTED | Dedicated shape test and clean firmware build select one device, one M1 endpoint and no geometry |
| Legacy wrapper for unmigrated behavior | IMPLEMENTED; TRANSITIONAL | Firmware builds and application characterization preserve indices, telemetry, trace and maintenance seams; full handlers remain ESP-IDF-bound |

Exact commands, metrics and limitations are recorded in the closeout document;
hardware behavior was not exercised.
