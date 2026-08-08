# Current architecture

## Scope and status

This document is the primary source for the architecture implemented by build 19 on
`refactor/svd48-device-composition` after Iteration 4. It is an as-built
description, not the target design. The target and migration rationale live in
[Architecture refactor](ARCHITECTURE_REFACTOR.md).

The firmware is bench-only. `app_main()` is the composition root; component-owned
FreeRTOS tasks perform runtime work after startup, and the main task sleeps. The
runtime topology comes from one immutable, build-selected C profile. There is no
JSON/YAML loader and the executable factory registry supports only SVD48.

## Components and dependencies

```mermaid
flowchart TB
  classDef active fill:#d9f2d9,stroke:#287a28,color:#111
  classDef transition fill:#fff2cc,stroke:#9a7b00,color:#111
  classDef legacy fill:#ffe0d6,stroke:#a33a20,color:#111
  classDef dormant fill:#eeeeee,stroke:#666,stroke-dasharray:5 5,color:#111
  classDef infra fill:#dceeff,stroke:#286a9a,color:#111

  MAIN[app_main lifecycle root]:::active --> PROFILE[robot_profile immutable C profile]:::active
  PROFILE --> PREFLIGHT[robot_driver_factory preflight]:::active
  PREFLIGHT --> COMPOSE[robot_composition]:::transition
  COMPOSE --> RSBUS[one rs485_transport per referenced RS485 bus]:::infra
  COMPOSE --> DEV[one svd48_device per physical controller]:::active
  COMPOSE --> REG[typed endpoint registry]:::active
  DEV --> CHANNEL[M1 and M2 channels]:::active
  CHANNEL --> DIRECT[svd48_channel_endpoint_adapter]:::active
  DIRECT --> REG
  REG --> COORD[actuation_coordinator]:::active
  RSBUS --> UART[ESP-IDF UART]:::infra
  DEV --> BUSPORT[bus_transport port]:::infra --> RSBUS

  SERIAL[serial_gateway]:::transition --> APP[actuation_application_port]:::active
  APP --> COORD
  SAFETY[robot_safety]:::transition --> APP
  MAINT[maintenance_lan]:::transition --> SERIAL
  RC[ibus_receiver / PPM]:::active --> SAFETY

  COMPOSE --> WRAP[legacy svd48_handle_t attached view]:::legacy
  WRAP --> CONTROL[robot_control facade]:::legacy
  SERIAL -->|unmigrated handlers| CONTROL
  CONTROL -->|telemetry| SAFETY
  OTA[ota_manager and ota_announce]:::transition --> CONTROL
  CONFIG[config_manager / NVS]:::infra --> WIFI[wifi_manager]:::infra --> OTA

  STATE[robot_state]:::dormant
  AUTH[command_authority]:::dormant
  KIN[robot_kinematics]:::dormant
  CLAN[control_lan]:::dormant
```

The active speed/stop adapter is the direct SVD48 channel adapter. The
`robot_control_endpoint_adapter` is retained only for host characterization and is
not wired by `robot_composition`. `serial_gateway` depends on the application port
and primitive diagnostic data; it does not depend on profile, composition or the
coordinator implementation.

## `SET_SPEED` sequence

```mermaid
sequenceDiagram
  participant C as Serial or maintenance client
  participant G as serial_gateway
  participant A as actuation_application_port
  participant O as actuation_coordinator
  participant R as endpoint registry
  participant E as direct SVD48 channel adapter
  participant D as svd48_device
  participant B as shared bus_transport
  participant U as RS485 UART
  participant L as legacy commanded-state mirror

  C->>G: SET_SPEED n rpm
  G->>A: validate index and profile RPM range
  A->>O: set endpoint velocity
  O->>O: acquire coordinator mutex (max 500 ms)
  O->>R: resolve endpoint ID
  R-->>O: velocity port
  O->>E: set_velocity_rpm(rpm)
  E->>D: write channel target register
  D->>B: Modbus 0x06 transaction
  B->>B: acquire shared bus mutex
  B->>U: request / response
  U-->>B: acknowledgement
  B-->>D: normalized result
  E->>D: write channel enable command
  D->>B: second serialized transaction
  alt target or enable failed
    E->>D: best-effort target zero then stop
  end
  E-->>O: capability result
  O->>O: release coordinator mutex
  O-->>A: report
  opt complete success
    A->>L: record commanded RPM without physical I/O
  end
  A-->>G: application result
  G-->>C: existing ASCII response
```

The target and enable writes execute while the coordinator mutex is held. The bus
mutex separately serializes this device with polling, maintenance and other devices
on the same RS485 bus. Channel commands use the retries configured by the bus
profile; generic maintenance writes do not retry an ambiguous lost acknowledgement.

## `STOP ALL` sequence

```mermaid
sequenceDiagram
  participant S as Gateway / boot / robot_safety
  participant A as actuation_application_port
  participant O as actuation_coordinator
  participant R as endpoint registry
  participant E as stoppable endpoint adapters
  participant D as SVD48 devices
  participant B as shared RS485 bus

  S->>A: stop_all()
  A->>O: stop all configured endpoints
  O->>O: acquire coordinator mutex (max 500 ms)
  loop each endpoint with STOPPABLE capability
    O->>R: resolve endpoint
    O->>E: stop()
    E->>D: write target RPM zero
    D->>B: serialized transaction
    E->>D: write channel stop command
    D->>B: serialized transaction
    E-->>O: endpoint result
  end
  O->>O: release coordinator mutex
  O-->>A: SUCCESS / PARTIAL / FAILURE report
  opt complete success
    A->>A: mirror all commanded RPM as zero
  end
  A-->>S: application result
```

`STOP n`, boot stop and safety stop use the same boundary. A boot-stop failure still
logs a warning and normal startup continues. The mutex prevents migrated operations
from interleaving, but it is not a priority-aware owner task: a safety stop may wait
up to 500 ms to acquire it and then behind the current bus transaction.

## Profile-driven construction

```mermaid
flowchart TD
  SELECT[Kconfig selects current_robot or bench_single_svd48_motor]
  SCHEMA[robot_profile_validate schema and board resources]
  LOOKUP[lookup executable factory by driver_id]
  CAPACITY[validate factory ops, capabilities, storage and legacy capacity]
  BUSES[construct each referenced RS485 bus]
  DEVICES[construct each device using its bus_id]
  ENDPOINTS[create endpoints in profile array order]
  LEGACY[create legacy index bindings, maximum four]
  APP[initialize registry, coordinator and application port]
  START[start every device and one N-device polling task]
  DIAG[restricted serial diagnostic mode, no outputs]
  FAIL[fail startup / OTA self-test policy]

  SELECT --> SCHEMA
  SCHEMA -->|invalid| FAIL
  SCHEMA --> LOOKUP --> CAPACITY
  CAPACITY -->|schema valid but unsupported| DIAG
  CAPACITY -->|supported| BUSES --> DEVICES --> ENDPOINTS --> LEGACY --> APP --> START
```

The executable registry declares byte capacity for runtime device slots, endpoint
capacity and the transitional legacy binding capacity. Preflight sums each factory's
`storage_required()` result with overflow checks. More than four compatibility
bindings produces `LEGACY_BINDING_LIMIT`; it is not an SVD48 device/channel limit.
The active SVD48 composition also rejects an empty endpoint set, zero or unsupported
capabilities, inverted limits, unschedulable periods and any velocity endpoint that
lacks `STOPPABLE`. These checks complete before a bus or device is constructed.

The SVD48 factory requires one dual-channel physical device and resolves its
transport by `device.bus_id`, never by array position. Because the legacy maintenance
API still identifies a controller only by Modbus address, repeated SVD48 addresses
across buses are rejected until that API carries bus/device identity.

The supported profiles are:

| Profile | RS485 topology | Endpoint topology | Geometry |
| --- | --- | --- | --- |
| `current_robot` | One referenced RS485 bus, devices at addresses 1 and 2 | Four endpoints ordered drive 1 M1/M2, drive 2 M1/M2 | Differential |
| `bench_single_svd48_motor` | One referenced RS485 bus, one device at address 1 | One endpoint at legacy index 0, channel M1 | None |

The bench profile does not invent a second controller. `SET_SPEED 0`, `STOP 0` and
`STOP ALL` are routable; index 1 is invalid and `MOVE_VEL` is unsupported because
there is no application geometry. Both profiles also declare a GPIO RC bus, which
`main` consumes separately from SVD48 composition.

## Polling, observations and health

One `svd48_poll_service` schedules every configured physical device. Position,
speed and current are read on every poll; status, motor temperature, bus voltage,
MOS temperature and error code are added every twentieth poll. A poll is:

- `OK` only when every observation scheduled for that poll succeeds;
- `PARTIAL` when at least one scheduled read succeeds and at least one fails;
- a concrete transport/protocol error when no scheduled read succeeds.

Each channel snapshot carries valid, failed and stale observation masks plus a
timestamp per observation. A success for current cannot update or clear speed. A
channel is `OFFLINE` when recent valid communication is absent, `FAULT` when a fresh
error-code observation is nonzero, `STALE` when an observation has expired or was
never valid, `DEGRADED` after an incomplete result with still-fresh prior data, and
`HEALTHY` only after a complete poll with all observations valid and fresh.

`PARTIAL` is a polling-service failure for backoff. Backoff never shortens the
device's nominal period and is scheduled from the instant that device's poll
finishes. Deadlines use explicit scheduled state and signed modular comparison, so a
wrapped deadline of zero is not confused with an uninitialized entry. A per-device
poll guard prevents the shared task and legacy `POLL_ONCE` from interleaving cycles;
the concurrent request receives `BUS_BUSY` without advancing the poll cycle.

The manufacturer contract names given speed (`0x5304/0x5305`) and observed motor
speed (`0x5410/0x5411`) as RPM. The driver preserves the signed raw register value
without a factor-of-ten conversion. That raw observation already feeds the legacy
5-RPM readiness gate for OTA/maintenance and the motion state reported by
`PLATFORM_STATUS` when it is online and fresh. It is not independent evidence that
the physical axis moved; controlled physical qualification and a reviewed fail-safe
policy remain required.

## Tasks and locks

```mermaid
flowchart LR
  SAFETY[robot_safety P9, stack 4096] --> CM[coordinator mutex, 500 ms]
  GATEWAY[serial_gateway P6, stack 12288] --> CMD[gateway command mutex, 1000 ms] --> CM
  STREAM[gateway_stream P4, stack 4096] --> PRINT[gateway print mutex]
  POLL[svd48_poll P8, stack 4096] --> BM[RS485 bus mutex, 100 ms current / 1000 ms cap]
  CM --> DEV[direct device operations] --> BM --> UART[UART exchange]
  MAINT[legacy maintenance handlers] --> BM
  BM -. released before snapshot update .-> STATE[per-device state mutex]
  BM -. result accounting .-> STATS[transport statistics mutex]
  RC[ibus_rx P5, stack 4096] --> SAFETY
  MAINTLAN[maintenance_lan P2] --> CMD
  BACKGROUND[Wi-Fi reconnect / OTA P2] --> WIFITIMEOUT[wifi_timeout P3, stack 3072, transient]
```

The coordinator mutex is acquired before a migrated device call; the device then
acquires the bus mutex. It prevents two coordinated commands from interleaving.
Polling and legacy maintenance do not acquire the coordinator, so they can run
between the individual bus transactions of a coordinated target/enable or
target-zero/stop sequence even while the coordinator is held. Every individual
RS485 transaction remains serialized. Snapshot state and 64-bit transport
statistics use separate locks, and the bus lock is released before either is
updated. This avoids a bus/state lock cycle, but it does not create stop precedence
or global single-writer authority.

Composition configures a 1000 ms RS485 lock cap. `bus_transport` clips lock
acquisition to the transaction timeout, however, and both current profiles configure
100 ms, so 100 ms is the effective bus-lock acquisition bound for their SVD48 calls.

| Task | Priority | Stack | Active mode |
| --- | ---: | ---: | --- |
| `robot_safety` | 9 | 4096 | Supported normal runtime |
| `svd48_poll` | 8 | 4096 | Supported normal runtime |
| `serial_gateway` | 6 | 12288 | Normal and restricted diagnostic runtime |
| `ibus_rx` | 5 | 4096 | Normal runtime when the RC bus is present |
| `gateway_stream` | 4 | 4096 | Normal runtime only |
| `wifi_timeout` | 3 | 3072 | Transient normal-runtime connection attempts |
| Wi-Fi reconnect | 2 | 4096 | Normal runtime only |
| OTA automatic check | 2 | 8192 | Normal runtime when available |
| OTA announce | 2 | 8192 | Normal runtime when available |
| Maintenance LAN | 2 | 12288 | Normal runtime when available |

## Startup and lifecycle

Normal startup initializes NVS/configuration, initializes Wi-Fi/OTA handles, selects
and composes the profile, creates the legacy facade, attempts boot stop, starts
polling, RC and safety, then starts the serial gateway and optional network tasks.
Shutdown and construction failure unwind tasks, adapters, devices and buses in
reverse order. A poll-task stop timeout preserves every device, lock and transport
dependency; restart is rejected until a later stop call collects the task's completion
signal. This late-completion path is source-contract and firmware-build verified, not
runtime fault-injected under FreeRTOS.

If schema validation succeeds but executable preflight cannot support the profile
(including a missing factory or static-capacity failure), no bus or device output is
constructed. For a non-pending OTA image, `main` starts only the serial RX gateway in
`diagnostic_only` mode. It permits `PING`, `VERSION`, `HELP`, read-only platform,
configuration, Wi-Fi, profile and composition status, plus `STOP ALL`. With no
endpoints, stop returns `ERR STOP_UNAVAILABLE OUTPUTS_NOT_INITIALIZED`; all other
commands return `ERR DIAGNOSTIC_MODE_COMMAND_BLOCKED`. It does not start the stream,
RC, safety, Wi-Fi reconnect, OTA, announce or LAN tasks. A pending OTA image retains
the existing rollback-on-self-test-failure policy instead of accepting diagnostic
mode as a successful self-test.

## Transitional and dormant boundaries

The legacy wrapper and `robot_control` still own `ENABLE`, `MOVE_VEL`, fault clear,
telemetry, trace, OTA preparation and register/configuration maintenance. They also
own `SVD48_IDENTIFY START|STOP`, which writes the physical identification register
and can move a motor. These paths do not all pass through the coordinator.
`robot_safety` emits migrated stops but still consumes legacy telemetry, so the new
health model is not yet the active safety policy.

`robot_state`, `command_authority`, `robot_kinematics` and `control_lan` are compiled
foundations, not active runtime behavior. Their presence must not be described as an
authority, state machine or production control protocol. The next boundary is a
priority-aware single actuation owner with stop precedence; see the
[Roadmap](ROADMAP.md).
