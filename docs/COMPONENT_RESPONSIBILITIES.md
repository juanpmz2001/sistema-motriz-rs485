# Component responsibilities

## How to read this catalog

This catalog describes the Iteration 4 ownership boundaries. Status is `active`,
`transitional`, `dormant` or `planned`. A named test identifies the intended
executable contract; pass/fail evidence belongs in
[Iteration 4 closeout](ITERATION_4_CLOSEOUT.md).

## Profiles and portable ports

### `robot_profile` — active

- **Purpose:** Define the immutable build-selected board, buses, devices, channels,
  endpoints, limits, criticality and optional application geometry.
- **Owns:** Bounded schema validation and selection of `current_robot` or
  `bench_single_svd48_motor` through Kconfig.
- **Does not own:** Runtime construction, credentials, NVS overrides or a JSON/YAML
  loader.
- **Dependencies:** Standard C/math and portable capability identifiers.
- **Concurrency/ownership:** Read-only static lifetime after selection; validation is
  pure and reentrant.
- **Errors/tests:** Specific `robot_profile_error_t` values; host fixtures cover valid
  and invalid profiles.

### `robot_capabilities` — active

- **Purpose:** Define stable endpoint identity plus typed velocity-RPM and stoppable
  ports.
- **Owns:** Capability limits, availability and criticality metadata, and the bounded
  endpoint registry.
- **Does not own:** Vendor registers, profile selection, authority or command parsing.
- **Dependencies:** Standard C only; no ESP-IDF or legacy facade dependency.
- **Concurrency/ownership:** Registry is built at startup and immutable during active
  use; operation serialization belongs to the application/device layers.
- **Errors/tests:** Portable capability and registry result enums; host-tested through
  coordinator and adapter contracts.

## Bus transport

### `bus_transport` — active

- **Purpose:** Provide a portable serialized request/response transaction port.
- **Owns:** Argument validation, bounded lock acquisition, complete transaction
  exclusion, backend-result propagation through a stable enum and transport
  statistics. Cancellation is a result produced by the backend, not controller-owned
  cancellation state.
- **Does not own:** UART configuration, Modbus framing, retries, device identity or
  motor semantics.
- **Dependencies:** Standard C and an injected backend with bus acquire/release,
  statistics acquire/release and exchange operations.
- **Concurrency/ownership:** One `bus_transport_controller_t` owns one injected
  backend, and multiple bus devices may share its port. Its transaction lock covers
  an entire exchange; injected statistics acquire/release callbacks protect a coherent
  counter snapshot, including 64-bit byte counters.
- **Lifecycle:** Initialize once before devices; reset only after all clients quiesce.
- **Errors/tests:** `OK`, invalid, timeout, busy, I/O, incomplete and cancelled;
  `bus_transport_test` uses a controllable concurrent fake backend.

### `rs485_transport` — active, ESP-IDF adapter

- **Purpose:** Bind `bus_transport` to one ESP-IDF UART/RS485 bus.
- **Owns:** UART installation/configuration, RX flushing, write/read exchange, the
  shared bus mutex, a statistics mutex and cancellation state.
- **Does not own:** SVD48 addresses, registers, retries, polling or endpoint roles.
- **Dependencies:** ESP-IDF UART/FreeRTOS plus `bus_transport`.
- **Concurrency/ownership:** One composition bus slot owns the UART plus static bus
  and statistics mutexes; every device on that bus shares its `bus_transport_t` port.
- **Lifecycle:** Construct before devices and destroy after polling/device teardown.
- **Errors/tests:** Maps UART timeout/short read/I/O/cancel outcomes into portable bus
  results; firmware builds verify the ESP-IDF binding.

## SVD48 device layer

### `svd48_protocol` — active, reusable

- **Purpose:** Build and parse the supported SVD48 frames and CRC16 variant.
- **Owns:** Read-holding, write-single, write-multiple framing, exception parsing,
  CRC validation and actuation-register range classification.
- **Does not own:** Transport, locks, retries, device/channel state or robot mapping.
- **Dependencies:** Standard C only.
- **Concurrency/ownership:** Stateless and reentrant.
- **Errors/tests:** Native protocol tests and Python golden vectors preserve wire
  compatibility.

### `svd48_device` — active

- **Purpose:** Represent exactly one physical dual-channel controller at one RS485
  address.
- **Owns:** Device ID/address, M1/M2 channel objects, register semantics, read retries,
  conservative write retry policy, observations, per-observation freshness,
  communication diagnostics and channel health.
- **Does not own:** UART setup, shared-bus serialization, logical motor indices,
  profile policy, global safety or command authority.
- **Dependencies:** `bus_transport`, `svd48_protocol`, and injected state lock/clock
  ports; no UART or `robot_control` dependency.
- **Concurrency/ownership:** The bus transport serializes wire access; one injected
  device lock protects snapshots, communication state and a whole-poll guard.
  Concurrent polling of the same device returns busy rather than interleaving poll
  cycles. Callers retain the device for the full lifetime of channels/adapters.
- **Lifecycle:** Statically stored by composition, initialized after its bus and
  destroyed after polling stops.
- **Errors/tests:** Device results distinguish complete success, partial observation,
  timeout, busy, I/O, incomplete, cancellation, CRC, exception, bad response and
  unsupported access. `svd48_device_test` uses a fake bus.

### `svd48_channel` — active view

- **Purpose:** Address M1 or M2 of one `svd48_device` without inventing a second
  controller.
- **Owns:** Channel-specific control, target-speed and current register selection;
  enable, stop, clear-fault and observation access.
- **Does not own:** Storage, bus locks, logical motor numbering or endpoint limits.
- **Dependencies:** Parent `svd48_device` only.
- **Concurrency/ownership:** Borrowed immutable view; parent device owns all state.
- **Errors/tests:** Device result enum; tests prove M1/M2 register separation and stop
  ordering.

### `svd48_poll_service` — active

- **Purpose:** Schedule up to four configured physical devices with independent
  periods and backoff.
- **Owns:** Device registration, duplicate/capacity checks, next deadlines,
  consecutive partial/failure accounting and wrap-safe delay calculation from each
  completed device poll.
- **Does not own:** A FreeRTOS task, UART, endpoint policy or omitted devices. Its
  four-device capacity is independent of the wrapper's four channel-binding limit.
- **Dependencies:** `svd48_device` and an injected monotonic clock.
- **Concurrency/ownership:** Called serially by its owner task; it retains borrowed
  device pointers. A device absent from the profile is never registered or failed.
- **Lifecycle:** Initialize, add devices during composition, run after all construction,
  reset after task stop.
- **Errors/tests:** Returns the first meaningful device error/partial result;
  `svd48_poll_service_test` covers N-device scheduling, recovery and freshness.

### `svd48_poll_task` — active, ESP-IDF adapter

- **Purpose:** Drive the pure polling service from one priority-8 FreeRTOS task.
- **Owns:** Task creation, bounded sleep and cooperative stop acknowledgement.
- **Does not own:** Poll policy, device storage or a second legacy poll loop.
- **Dependencies:** FreeRTOS and `svd48_poll_service`.
- **Concurrency/ownership:** Composition owns one task for the shared service; stack is
  4096 bytes and maximum sleep is 50 ms.
- **Lifecycle/errors/tests:** Starts only with at least one device; stop has a bounded
  timeout. Firmware builds cover the task adapter.

## Endpoint and application layer

### `svd48_channel_endpoint_adapter` — active

- **Purpose:** Expose one SVD48 channel as a typed endpoint.
- **Owns:** RPM range enforcement through the endpoint contract, direct target/enable
  and stop operations, best-effort stop after target/enable failure, and driver health
  diagnostics.
- **Does not own:** Logical motor compatibility, bus selection, authority, polling or
  safety policy.
- **Dependencies:** Portable capabilities and `svd48_device`; no `robot_control`.
- **Concurrency/ownership:** Composition owns fixed adapter storage; coordinator and
  bus transport serialize writers.
- **Lifecycle:** Construct after device, register once, deinitialize before device.
- **Errors/tests:** Maps device results to capability errors;
  `svd48_channel_endpoint_adapter_test` covers limits, rollback, stop and M1/M2.

### `actuation_coordinator` — active, partial

- **Purpose:** Serialize migrated velocity and stop operations and return bounded
  per-endpoint reports.
- **Owns:** Endpoint lookup, ordered application, global/individual stop and
  best-effort rollback after critical partial application.
- **Does not own:** Parsing, kinematics, authority, state, vendor protocol or the
  remaining legacy writers.
- **Dependencies:** Portable endpoint registry and injected lock port.
- **Concurrency/ownership:** One static FreeRTOS mutex supplied by composition covers
  each complete operation; acquisition is bounded to 500 ms. No owner task exists.
- **Errors/tests:** Success, partial, failure and lock timeout; host actuation tests.

### `actuation_application_port` — active compatibility boundary

- **Purpose:** Keep serial gateway and safety independent of composition/coordinator
  implementation.
- **Owns:** Profile-dependent legacy-index translation for set speed, stop one, stop
  all, motor count and RPM limits.
- **Does not own:** Hardware, parsing or state policy.
- **Concurrency/ownership:** Immutable operation table backed by composition.
- **Errors/tests:** Portable application results; compatibility tests preserve current
  command behavior.

## Factory and composition

### `robot_driver_factory` — active framework, specialized registry

- **Purpose:** Distinguish schema-valid profiles from profiles executable by available
  factories.
- **Owns:** Factory lookup and pure preflight diagnostics for missing factory,
  incompatible bus and invalid device configuration.
- **Does not own:** ESP-IDF construction storage or a factory for every schema driver.
- **Dependencies:** `robot_profile` only in its portable preflight layer.
- **Concurrency/ownership:** Immutable registry, used single-threaded during boot.
- **Lifecycle/errors/tests:** Factories expose validate/storage/construct/endpoint/
  start/stop/destroy operations. Preflight sums per-device `storage_required`, compares
  `endpoint_count` with endpoint capacity, separately compares it with legacy-binding
  capacity, and validates that endpoints are constructible. Empty bindings, zero or
  unsupported capabilities, inverted limits, unschedulable periods and velocity
  without `STOPPABLE` are rejected before runtime construction. Storage or endpoint
  capacity excess reports `STATIC_CAPACITY_EXCEEDED`; the compatibility bound reports
  `LEGACY_BINDING_LIMIT`. Iteration 4 registers only SVD48; host tests cover supported
  and unsupported profiles.

### `robot_composition` — active composition sub-root

- **Purpose:** Construct the profile-selected actuation runtime below `app_main`.
- **Owns:** Static bus/device/adapter slots, SVD48 executable factory, polling service
  and task, endpoint registry, coordinator mutex, application port, legacy bindings
  and structured diagnostics.
- **Does not own:** NVS/Wi-Fi/OTA/gateway lifecycle, command policy or general dynamic
  allocation. `app_main` remains the complete firmware composition root.
- **Dependencies:** Profile/factory, RS485 transport, SVD48 device/polling/adapter,
  capabilities/coordinator and the transitional legacy wrapper.
- **Concurrency/ownership:** Boot construction is single-threaded. The direct runtime
  path uses one bus-exchange mutex and one statistics mutex per RS485 bus, one state
  mutex per device and the coordinator mutex. The transitional wrapper retains its
  own compatibility state/trace locks.
- **Lifecycle:** Preflight, construct buses by `device.bus_id`, devices, endpoints and
  compatibility view; start polling last; stop in reverse order.
- **Errors/tests:** Diagnostics retain schema/support flag, code, stage and offending
  driver/bus/device/endpoint. More than four compatibility bindings reports
  `LEGACY_BINDING_LIMIT`; for a non-pending OTA image, unsupported preflight enters
  restricted diagnostic startup without outputs. A pending-verification image follows
  rollback handling instead.

## Transitional compatibility

### Legacy `svd48_handle_t` wrapper — transitional

- **Purpose:** Present the new devices/channels through unchanged `svd48` APIs used by
  `robot_control`, gateway maintenance, OTA and safety telemetry.
- **Owns:** Legacy logical-index bindings, trace adaptation and telemetry shape.
- **Does not own:** UART/polling when attached to composed devices; it delegates to
  `svd48_device` and the shared polling service.
- **Dependencies:** New SVD48 devices plus ESP-IDF synchronization for compatibility.
- **Concurrency/ownership:** Composition owns the attached devices; wrapper storage is
  heap-backed and accepts at most four bindings with explicit validation/diagnosis.
- **Removal condition:** All read, maintenance, OTA, safety and remaining write callers
  use typed ports/services.

### `robot_control` — active, legacy

- **Purpose:** Preserve current telemetry, kinematics, maintenance and OTA-facing API.
- **Current writers:** `ENABLE`, `CLEAR_FAULT`, `MOVE_VEL`, OTA preparation and
  maintenance identification/register/configuration helpers.
- **Migrated behavior:** Coordinated speed/stop performs physical writes through direct
  endpoints; `robot_control` only records successful compatible commanded state.
- **Legacy safety predicate:** `robot_control_is_safe_for_ota()` combines commanded
  state with a 5-RPM threshold on online, non-stale legacy telemetry; it skips
  offline/stale samples and the RPM interpretation lacks physical confirmation.
- **Concurrency/ownership:** Shared opaque handle with its own state lock; it borrows
  the legacy SVD48 view.
- **Removal condition:** Each remaining responsibility has a typed, tested replacement.

### `robot_control_endpoint_adapter` — retained but not in the Iteration 4 speed/stop path

- **Purpose:** Historical transitional adapter used by the previous composition.
- **Status:** Source-retained compatibility code, not part of the active composed
  velocity/stop path; direct SVD48 channel adapters now back those endpoints.
- **Removal condition:** No build or test depends on the old adapter.

## External and safety services

### `serial_gateway` — active, mixed

- **Purpose:** UART ASCII framing, parsing, dispatch and response compatibility.
- **Actuation:** `SET_SPEED`, `STOP n` and `STOP ALL` use the application port; enable,
  fault clear, motion and maintenance helpers remain legacy.
- **Diagnostic startup:** Can run with a restricted allowlist and no robot/output
  handles when composition is unsupported.
- **Concurrency:** A command mutex serializes UART commands and maintenance-LAN
  delegation; the priority-6 RX task may synchronously wait on driver/coordinator
  work. Normal mode also owns a priority-4 stream task; diagnostic-only mode does not.

### `robot_safety` — active, mixed

- **Purpose:** Observe RC loss and reported motor faults and request repeated global
  stop through the application port.
- **Limit:** Reads the legacy telemetry projection and does not yet consume complete
  profile-aware degraded/stale/offline health.
- **Concurrency:** Priority-9 task every 20 ms; no network/storage/JSON work.

### `config_manager`, `wifi_manager`, `maintenance_lan`, `ota_manager`, `ota_announce`

- **Purpose:** NVS settings/secrets, low-priority network lifecycle, authenticated
  maintenance envelope and OTA lifecycle.
- **Boundary:** They do not own topology. Maintenance delegates to gateway policy;
  OTA still uses legacy safe-query/preparation until a dedicated application port
  replaces it.
- **Concurrency:** Low-priority tasks only; never execute network/storage work inside
  control or safety tasks.

## Dormant foundations

`robot_state`, `command_authority`, `robot_kinematics` and `control_lan` compile and
have pure-model coverage where applicable, but `app_main` does not integrate them into
active behavior. They remain candidates for the target state, authority, motion and
control-transport layers; compiled does not mean operational.
