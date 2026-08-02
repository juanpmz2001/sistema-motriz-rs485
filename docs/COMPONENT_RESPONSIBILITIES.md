# Component responsibilities

## How to read this catalog

This Phase 0 catalog records the base commit's actual contracts and their intended
destination. “Planned” APIs are responsibility boundaries, not implemented symbols.
Lifecycle teardown assumes callers first quiesce tasks. ESP-IDF services normally
return `esp_err_t`; pure models use domain enums. Exact APIs remain source-owned.

## Active device and control path

### `svd48_protocol` — reusable without changes

- **Purpose/responsibilities:** Pure CRC, frame construction, range rules and response parsing.
- **Not responsible for:** UART, locks, retries, motor identity, safety or robot mapping.
- **Allowed dependencies/API:** C standard integer/types; functions in `svd48_protocol.h`.
- **Thread safety/lifecycle:** Stateless and reentrant; no lifecycle.
- **Errors/tests:** Boolean/size results and parsed exception data; firmware contract and Python reference tests.

### `svd48` — must be divided

- **Purpose/responsibilities:** Currently owns UART, RS485 serialization, request/retry,
  polling/cache, two drive IDs, four logical channels and register/motion commands.
- **Not responsible for (target):** Robot endpoint role, authority, arming, kinematics or global safety.
- **Allowed dependencies/API:** Currently ESP-IDF driver/timer plus protocol; public opaque
  handle in `svd48.h`. Target depends on an RS485 transport port.
- **Thread safety/lifecycle:** Heap handle, mutex-protected bus/cache, polling task;
  initialize before polling and quiesce before deinit.
- **Errors/tests:** `esp_err_t`, `svd48_status_t`, exception diagnostics; protocol is host-tested,
  device/transport behavior requires new fakes and characterization.

### `robot_control` — must be divided

- **Purpose/responsibilities:** Current SVD48/PWM facade, direct speed/stop/configuration,
  motion conversion, telemetry and OTA-safe preparation.
- **Not responsible for (target):** Bus/device details, authority, state or safety policy.
- **Allowed dependencies/API:** Currently concrete `svd48` and ESP-IDF PWM/timer through
  `robot_control.h`; target services consume typed endpoints only.
- **Thread safety/lifecycle:** Opaque shared handle; underlying calls serialize bus access;
  initialize after devices and before gateways/safety.
- **Errors/tests:** `esp_err_t` and boolean snapshots; needs characterization before splitting.
- **Destination:** motion controller, actuation coordinator, endpoint adapters,
  device maintenance and telemetry services.

### `robot_safety` — must be divided

- **Purpose/responsibilities:** Current periodic RC-loss/motor-error observation and repeated stop.
- **Not responsible for (target):** Reading concrete receivers/drivers or applying output.
- **Allowed dependencies/API:** Currently `robot_control`, `ibus_receiver`, FreeRTOS;
  target supervisor consumes health/state ports and issues coordinator requests.
- **Thread safety/lifecycle:** Owns a priority-9 20 ms task and status synchronization.
- **Errors/tests:** Stop result retained in status; existing behavior lacks host policy tests.
- **Destination:** monitors, health aggregator, safety supervisor, state inhibits and stop requests.

## External adapters and infrastructure

### `serial_gateway` — must be divided

- **Purpose/responsibilities:** UART ASCII receive, framing, parsing/dispatch, responses,
  telemetry stream and concrete service command handlers.
- **Not responsible for (target):** Actuation, safety decisions, device implementation or profile policy.
- **Allowed dependencies/API:** Currently all active services; target parser depends only on
  value types and dispatcher ports. Public execute/start/status functions remain compatibility seams.
- **Thread safety/lifecycle:** Owns command and telemetry tasks; initialized after dependencies.
- **Errors/tests:** Framing/result/policy host contracts exist; handler characterization is pending.
- **Destination:** serial transport, parser, dispatcher, operation/maintenance/config/OTA handlers and formatter.

### `maintenance_lan` — reusable with adaptation

- **Purpose/responsibilities:** Authenticated UDP JSON envelope and delegation to gateway policy.
- **Not responsible for (target):** Parsing device semantics, actuator writes or authority bypass.
- **Allowed dependencies/API:** Network/config/auth plus a command-dispatch port.
- **Thread safety/lifecycle:** Low-priority socket task; starts after Wi-Fi and dispatcher.
- **Errors/tests:** UDP/application status; policy has host tests, network behavior is not host-tested.

### `control_lan` — reusable with adaptation, dormant

- **Purpose/responsibilities:** Sequenced control protocol definition/implementation.
- **Not responsible for:** Direct actuator access or an alternate safety path.
- **Allowed dependencies/API:** Network/config/time and future command-input port.
- **Thread safety/lifecycle:** Component compiles but `main` does not initialize it.
- **Errors/tests:** Must gain protocol and authority integration tests before activation.

### `ibus_receiver` and `ppm_decoder`

- **Purpose/responsibilities:** ESP-IDF acquisition, PPM decoding and receiver snapshots;
  `ppm_decoder_model` is pure and reusable unchanged.
- **Not responsible for (target):** Authority, robot state, kinematics or output.
- **Allowed dependencies/API:** Driver/timer below a future RC command adapter.
- **Thread safety/lifecycle:** Receiver/decoder tasks and synchronized snapshots; board profile supplies pins.
- **Errors/tests:** `esp_err_t`; pure PPM model has host tests, hardware acquisition does not.

### `config_manager`, `wifi_manager`, `ota_manager`, `ota_announce`

- **Purpose/responsibilities:** NVS runtime secrets/settings, station lifecycle, OTA
  manifest/download/rollback, and authenticated OTA UDP offers respectively.
- **Not responsible for:** Robot topology or direct actuation. OTA may request a safe
  transition/stop through an application port but cannot own the stop implementation.
- **Allowed dependencies/API:** ESP-IDF network/storage primitives; OTA application/state ports.
- **Thread safety/lifecycle:** Low-priority services created in dependency order; no
  high-priority control task may perform network, JSON, hash or NVS work.
- **Errors/tests:** `esp_err_t` and snapshots; firmware integration tests remain pending.
- **Migration:** Keep config/Wi-Fi mostly intact; replace `robot_control` references in OTA adapters.

## Pure and dormant foundations

### `robot_state_model`

- **Purpose/responsibilities:** Pure operational state, valid transitions, inhibits,
  fault latching and authorization guards.
- **Not responsible for:** Sensors, RC, drivers, output, kinematics or authority selection.
- **Allowed dependencies/API:** Standard C only; `robot_state_model.h` transition functions.
- **Thread safety/lifecycle:** Caller-owned value; reentrant across distinct instances.
- **Errors/tests:** `robot_state_outcome_t`, blockers and actions; host state tests pass.
- **Classification:** Reusable with adaptation; compiled but not active.

### `robot_state_service`

- **Purpose/responsibilities:** Serialize model access, own inhibit-source slots and return snapshots.
- **Not responsible for:** Producing health facts or executing requested actions.
- **Allowed dependencies/API:** State model plus private FreeRTOS synchronization.
- **Thread safety/lifecycle:** Thread-safe opaque handle; callers quiesce before deinit.
- **Errors/tests:** Model outcomes; service concurrency tests are pending.
- **Classification:** Reusable with adaptation; compiled but not active.

### `command_authority_model`

- **Purpose/responsibilities:** Pure sources, priority, lease/TTL, deadman, sequence,
  expiry and handover decisions.
- **Not responsible for:** Hardware, kinematics, state transitions or electrical faults.
- **Allowed dependencies/API:** Standard C and caller-provided time values.
- **Thread safety/lifecycle:** Caller-owned model; service wrapper will serialize live use.
- **Errors/tests:** Domain outcomes with host tests for current model; source policy remains open.
- **Classification:** Reusable with adaptation; compiled but not active.

### `robot_kinematics`

- **Purpose/responsibilities:** Pure differential body-velocity to bounded motor-RPM mapping.
- **Not responsible for:** Applying commands, authority, state, safety or drivers.
- **Allowed dependencies/API:** Standard C/math and explicit SI/RPM types in its header.
- **Thread safety/lifecycle:** Stateless and reentrant.
- **Errors/tests:** Detailed domain enum; host tests cover validation and conversion.
- **Classification:** Reusable with adaptation as one replaceable strategy; dormant.

## Planned ports and services

### Capability ports and endpoint registry

- **Purpose/responsibilities:** Typed velocity/position/torque/binary, enable, stop,
  sensor and health operations; stable endpoint IDs and capability matching.
- **Not responsible for:** Source policy, robot geometry or vendor register exposure.
- **Dependencies/API:** Small C structs/operation tables with explicit units; no ESP-IDF in public contracts.
- **Thread safety/lifecycle/errors/tests:** Fixed composition lifetime; writer rules documented
  per operation; endpoint-level unavailable/range/stale errors; fake-backed host tests required.

### `actuation_coordinator`

- **Purpose/responsibilities:** Sole logical runtime setpoint writer; ordered multi-endpoint
  application, normal/emergency stop, per-endpoint reports and partial-failure handling.
- **Not responsible for:** Parsing transports, kinematics, vendor protocols or deciding safety policy.
- **Dependencies/API:** Capability ports, immutable application requests and bounded result arrays.
- **Thread safety/lifecycle/errors/tests:** One owner task/mailbox or explicit serialization;
  application errors include inhibited/partial/unconfirmed; fakes test ordering and rollback stop.

### Motion controller and command router

- **Purpose/responsibilities:** Validate semantic commands through authority/state,
  select kinematics and submit bounded setpoints to the coordinator.
- **Not responsible for:** Driver calls, bus access or protocol responses.
- **Dependencies/API:** Domain models/strategies, clock port, coordinator command port.
- **Thread safety/lifecycle/errors/tests:** Bounded mailbox and immutable messages;
  tests cover TTL, replay, deadman, handover and unsupported commands.

### Health aggregation and safety supervisor

- **Purpose/responsibilities:** Normalize facts, apply reviewed profile policy, publish
  state inhibits/faults and request stops.
- **Not responsible for:** Register interpretation beyond adapter translation or stop execution.
- **Dependencies/API:** Health providers, profile policy, state service and coordinator stop port.
- **Thread safety/lifecycle/errors/tests:** Snapshot/mailbox model with no network/storage work;
  host tests cover severity, stale/offline, partial apply and E-stop.

### Board/profile composition

- **Purpose/responsibilities:** Declare resources/topology, validate all references and
  capabilities, construct components and start tasks only after successful validation.
- **Not responsible for:** Runtime command policy or credentials.
- **Dependencies/API:** Static board/driver registries and versioned bounded profile data.
- **Thread safety/lifecycle/errors/tests:** Single-threaded boot construction; invalid
  profile keeps outputs disabled; host fixtures cover duplicates, conflicts and missing dependencies.

