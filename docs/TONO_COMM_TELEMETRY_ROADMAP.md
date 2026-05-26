# Toño Communication, Telemetry and SVD48 Roadmap

Date: 2026-05-12

This document organizes the next features requested for the ESP32-S3 firmware, the SVD48 RS485 driver and the local web interface. It is intentionally a planning document: no hardware communication should be attempted from this document.

Related references:

- `docs/API.md`
- `docs/skills/SVD48B50A_SKILL.md`
- `docs/controllers/SVD48B50A/SV_CONFIG_REPLICATION_NOTES.md`
- `docs/controllers/SVD48B50A/OBSERVED_TOÑO_CONFIG.md`
- `docs/controllers/SVD48B50A/SVD48V30A-user-manual-V2.01.pdf`
- `docs/controllers/SVD48B50A/SVD48V-PC-software-manual-V1.1.pdf`
- `docs/FUTURE_CONTROL_TUNING_UI_NOTES.md`

## Current Development Assumptions

- For firmware-level development, flashing and fast serial diagnostics are easier with the ESP32-S3 connected to the development PC.
- For deployment validation, the ESP32-S3 must be connected to the Jetson and the web container must use the Jetson USB serial device.
- Before any live test, confirm where the ESP32-S3 is connected. The latest request contains both statements: "ESP connected to this computer" and "ESP32S3 not connected". Treat hardware as unavailable until explicitly confirmed for that test.
- Do not change the proven UU Motor CRC behavior unless a captured frame proves a specific alternate path. Current behavior remains CRC16 Modbus polynomial `0xA001`, init `0xFFFF`, transmitted high byte then low byte.

## Feature Inventory

| Feature | Main value | Firmware impact | Web/backend impact | Complexity | Risk |
|---|---|---:|---:|---:|---:|
| Multi-register telemetry reads | Faster polling, less RS485 overhead | Medium | Low | Medium | Medium |
| CSV telemetry recording/replay | Offline analysis and repeatable debugging | Low/none initially | Medium | Medium | Low |
| Chart screenshot export | Shareable diagnostic images | None | Low | Low | Low |
| SV-Config parameter replication | Configure controller without SV-Config | High | Medium | High | High |
| Register documentation tab | Faster field work and future LLM onboarding | None | Medium | Medium | Low |
| Relative X-axis labels `t=-1s` | Better live chart readability | None | Low | Low | Low |
| Future PID/control tuning UI | Tune speed/position/current loops | Medium/High | High | High | High |

## 1. Multi-Register Reads

Current state:

- The driver already supports generic multi-register reads through `svd48_read_registers_by_id()`.
- The serial API already exposes `READ_REG drive_id reg [count]`.
- Background polling currently reads useful telemetry in several small groups:
  - Position: `0x5418`, quantity `4`.
  - Speed: `0x5410`, quantity `2`.
  - Current: `0x5414`, quantity `2`.
  - Slow status/temp/voltage/error groups separately.
- The low-level driver currently limits reads to `quantity <= 16`.

Design direction:

- Keep the existing generic raw read command.
- Add typed telemetry blocks inside the SVD48 driver, not ad hoc parsing in the UI.
- Prefer two safe phases:
  - Phase A: group contiguous useful reads only where the manual and current hardware behavior are already proven.
  - Phase B: test larger blocks and auto-fallback to smaller reads if the controller returns exception, CRC error or timeout.

Candidate blocks:

| Block | Start | Quantity | Contents | Notes |
|---|---:|---:|---|---|
| Fast speed/current | `0x5410` | `6` | speed M1/M2, gap/unused area if applicable, current M1/M2 | Must verify register continuity before relying on it. |
| Fast position | `0x5418` | `4` | position M1/M2 int32 | Already used. |
| Slow thermal/bus/status | `0x5400` | maybe `14` | status, temps, bus voltage, MOS temp | Verify accepted block size. |
| Error block | `0x5420` | `4` | error M1/M2 uint32 | Already used separately. |

Questions:

- What is the target live telemetry rate per selected motor: 10 Hz, 20 Hz, 50 Hz?
- Should the ESP poll all configured drives continuously, or only the motor selected in the web telemetry tab?
- In the current bench mode, should firmware be configurable as "only drive `0x02` M1 active" to avoid wasting time on missing controllers?
- Should manual commands preempt telemetry immediately, or is a short queue delay acceptable?
- What stale threshold should the UI use: 300 ms, 500 ms, 1000 ms?
- Is it acceptable to auto-disable a large block after one exception and persist that fallback until reboot?

Estimated work:

- Driver block reads and fallback: 1-2 focused sessions.
- Hardware validation and tuning: 1 session with the ESP/controller connected.
- Tests for frame parsing and block decoding: 0.5-1 session.

## 2. Telemetry Recording, Replay and Chart Export

Design direction:

- Use CSV as the main interchange format.
- Start with browser-side CSV export/import for speed of development.
- Later add backend-managed session storage on Jetson under `logs/telemetry/`.

Recommended CSV schema:

```csv
session_id,source,iso_time,elapsed_ms,motor,drive_id,channel,rpm,current_da,bus_v,motor_temp_dc,mos_temp_dc,position_counts,encoder_deg,status,online,stale,error_code
```

Where:

- `elapsed_ms` is relative to the beginning of the recording.
- `iso_time` is wall-clock time from browser or backend.
- `current_da` keeps the controller unit of 0.1 A.
- `encoder_deg` is UI-calculated using the configured CPR.
- `source` can be `live`, `imported`, `jetson`, or `pc`.

Replay modes:

- Static replay: load CSV and render full chart immediately.
- Timed replay: play samples according to `elapsed_ms`.
- Compare mode later: overlay two CSV sessions.

Chart export:

- First implementation: export chart canvas as PNG.
- Later: export a report bundle with CSV, PNG and metadata JSON.

Questions:

- Should recordings be saved only from the active telemetry motor, or from all motors being polled?
- Should RS485 trace lines be stored with telemetry, or kept in a separate diagnostics log?
- Should recording start/stop be manual only, or should it auto-start when telemetry starts?
- Where should the canonical recording live: browser download, Jetson disk, or both?
- How long should a normal recording be: seconds, minutes, hours?
- Do we need retention rules on the Jetson, for example keep last 7 days or max 5 GB?
- Should replay include the rotating wheel visualization or only charts?

Estimated work:

- Browser CSV export/import and PNG chart export: 1 session.
- Jetson-backed session storage and listing: 1-2 sessions.
- Replay controls and comparison mode: 1-2 sessions.

## 3. SV-Config Replication

Current confirmed information:

- SV-Config communicates with the controller over serial at `115200`.
- The controller supports Modbus-like RS485 frames with functions `0x03`, `0x06` and `0x10`.
- We can already read/write raw registers from our ESP gateway.
- Prior observations on Toño:
  - `0x5018` active M1 pole pairs read as `24` in an earlier session.
  - `0x502C` active M1 sensor type read as `1/HALL`.
  - `0x2202/0x2203` returned invalid-register exceptions through our path, even though SV-Config exposes gear teeth fields.

Important interpretation:

- It is very plausible that SV-Config can change fields we failed to read because it may use a different register map, `0x10` multi-register writes, an indirect parameter table, mode-gated access, or firmware-version-specific addresses.
- "SV-Config can do it" proves it is reachable over the controller serial interface, but it does not prove that our current individual `READ_REG 0x2202` frame is the correct way to reach it.
- To know exactly what SV-Config does, we need a frame capture of SV-Config talking to the controller while changing one field at a time.

Safe implementation path:

1. Read-only typed parameter schema.
   - Names, register addresses, data type, channel, units, min/max, manual source and confidence level.
   - Types: `uint16`, `int16`, `uint32`, `int32`, `float32_as_2regs`.
   - Commands like `PARAM_LIST`, `PARAM_READ drive channel group`, `PARAM_GET drive channel name`.

2. Profile backup before writes.
   - Read all known parameters into JSON/CSV.
   - Store as "known good" before any write.
   - UI should show before/after diffs.

3. Guarded writes.
   - Require explicit `CONFIRM`.
   - Refuse writes while motors are running unless the parameter is proven live-tunable.
   - Log command, TX hex, RX hex and decoded value.

4. Hall calibration workflow.
   - Stop motor.
   - Verify sensor type `HALL`.
   - Set calibration current if needed.
   - Write calibration command (`0x5600=1` for M1, `0x5601=1` for M2).
   - Poll calibration status (`0x5684/0x5685`) until success/fail/timeout.
   - Read Hall status, Hall angle and angle table.

5. Reverse-engineer missing SV-Config details.
   - Use SV-Config on PC with a USB-RS485 adapter.
   - Add a passive sniffer on A/B lines or capture with a second adapter.
   - Change one field at a time: gear teeth, pole pairs, sensor type, Lq/Ld/Rs, Hall calibration, PID values.
   - Decode and document exact frames.

Questions:

- Do you want to replicate only the SV-Config fields needed for Toño, or eventually all fields visible in SV-Config?
- Can we get a physical RS485 sniffing setup: PC running SV-Config, controller, and a second USB-RS485/logic analyzer listening passively?
- Which fields are highest priority: gear teeth, pole pairs, Hall calibration, Lq/Ld/Rs, PID, current limits, direction?
- Are you comfortable with a write UI that is intentionally conservative and requires `CONFIRM` for every persistent parameter change?
- Should profiles be stored in the firmware repo, the web repo, or Jetson `logs/profiles/`?
- Do we have the exact controller firmware version or any version field from SV-Config?
- For tests, can the wheel be lifted and physically safe when Hall calibration or PID experiments run?

Estimated work:

- Read-only schema and UI table: 1-2 sessions.
- Safe write + backup profile workflow: 2-3 sessions.
- Hall calibration guided workflow: 2-3 sessions plus hardware time.
- SV-Config frame capture and exact replication of missing fields: unknown until capture, likely several focused sessions.

## 4. Register Documentation Tab

Design direction:

- Add a web tab named `Registros` or `Docs`.
- Serve local docs from the Jetson/web backend so the field UI works offline on the LAN.
- Include both:
  - A searchable register table generated from a maintained JSON/MD schema.
  - Links to the original PDFs/manuals.

Recommended structure:

- `docs/registers/svd48-registers.json` as the source of truth.
- Web route `GET /api/docs/registers`.
- Web route/static mount for PDFs.
- UI table columns:
  - Register
  - Name
  - M1/M2 mapping
  - Type
  - Units
  - Access: read-only, write-only, read/write
  - Safe to write while running?
  - Confidence: manual, observed, inferred, needs capture
  - Notes

Questions:

- Do you prefer embedded PDF viewer, markdown/register table, or both?
- Should the UI allow jumping from a telemetry field to its register documentation?
- Should unverified/manual-suspicious registers be visibly marked as risky?
- Should this documentation live only in the firmware repo and be copied into the web container at deploy time, or should the web repo also own a docs copy?

Estimated work:

- Static docs/PDF tab: 0.5-1 session.
- Searchable typed register table: 1-2 sessions.
- Link telemetry/history rows to register docs: 1 session.

## 5. Telemetry X-Axis Relative Time Labels

Design direction:

- In live mode, the newest sample should be at the right edge.
- X-axis labels should show relative age from "now" or from newest sample:
  - `t=0s`, `t=-1s`, `t=-2s`, `t=-3s`.
- In replay/import mode, labels can show either elapsed time or relative-to-cursor time.

Questions:

- Should `t=0s` be displayed at the right edge, or should the labels start at `t=-1s` as requested?
- What default visible time window do you want: 10 s, 30 s, 60 s?
- Should the chart keep a fixed time window even if samples arrive late, or compress only available samples?
- Do you want hover/tooltips with absolute timestamp and raw values?

Estimated work:

- Relative X-axis labels in current canvas chart: 0.5 session.
- Hover/tooltips and replay-aware labels: 1 additional session.

## Recommended Phase Order

### Phase 0: Planning and Versioning Baseline

Goal:

- Freeze this roadmap.
- Decide branch/commit naming.
- Confirm development topology: ESP on PC for firmware iterations, Jetson only for deployment tests.

Deliverables:

- This document.
- `docs/FUTURE_CONTROL_TUNING_UI_NOTES.md`.
- A short checklist for each phase before coding.

Complexity: Low.

### Phase 1: UI-Only Quality Improvements

Goal:

- Improve operator experience without touching RS485 behavior.

Deliverables:

- Relative X-axis labels `t=-1s`, `t=-2s`, etc.
- CSV export/import for the active telemetry chart.
- PNG export of the chart canvas.
- Basic docs tab with PDF links and/or embedded viewer.

Complexity: Low to Medium.

Why first:

- Low hardware risk.
- Gives immediate debugging value.
- Can be developed with recorded/mock data even if the ESP is disconnected.

### Phase 2: Telemetry Data Model and Recording

Goal:

- Make telemetry sessions durable and replayable.

Deliverables:

- Stable CSV schema.
- Browser-side recording controls.
- Optional Jetson-backed session save/load.
- Replay UI for saved CSV.

Complexity: Medium.

### Phase 3: Faster SVD48 Telemetry Polling

Goal:

- Reduce RS485 transactions and stale/active flicker.

Deliverables:

- Configurable telemetry polling scope.
- Multi-register block read strategy with fallback.
- Tests for block decoding.
- Trace output that can show grouped reads when explicitly enabled, but does not pollute normal history.

Complexity: Medium.

Requires:

- ESP and controller connected.
- A clear bench topology: which drive IDs/channels are physically present.

### Phase 4: SV-Config Read-Only Replication

Goal:

- Expose controller parameters safely without writing them yet.

Deliverables:

- Typed register/parameter schema.
- `PARAM_LIST`, `PARAM_READ`, `PARAM_GET`.
- Web register/parameter tab with confidence levels.
- Profile backup format.

Complexity: Medium to High.

### Phase 5: SV-Config Writes, Hall Calibration and Gear Ratio Investigation

Goal:

- Safely replicate selected SV-Config changes.

Deliverables:

- Guarded `PARAM_SET ... CONFIRM`.
- Profile backup and restore workflow.
- Hall calibration guided workflow.
- RS485 capture plan and decoded SV-Config frames for missing fields like gear teeth.

Complexity: High.

Requires:

- Physical safety setup.
- RS485 sniffing/capture setup.
- Clear rollback/recovery procedure for controller parameters.

### Phase 6: Control Tuning UI

Goal:

- Add a dedicated interface for PID/control parameters after the parameter system is safe.

Deliverables:

- Speed/position PID editor.
- Current-loop/feed-forward/dead-zone controls.
- Live plots for response analysis.
- Revert-to-known-good workflow.

Complexity: High.

## Version Control Proposal

- Use one branch per phase:
  - `phase-1-ui-telemetry-tools`
  - `phase-2-telemetry-recording`
  - `phase-3-svd48-block-reads`
  - `phase-4-svconfig-readonly`
  - `phase-5-svconfig-writes-calibration`
- Each phase should end with:
  - Firmware build result.
  - Web build/container result if touched.
  - Hardware test log if hardware was required.
  - Updated `docs/API.md` for any command changes.
  - Updated `docs/skills/SVD48B50A_SKILL.md` for any verified SVD48 behavior.

## Questions To Answer Before Starting Phase 1

1. For local development, should I run the web/backend locally on this PC against the ESP serial port, then deploy to Jetson only after validation?
2. What default chart window should Phase 1 use: 10 s, 30 s or 60 s?
3. Should CSV export start with only the selected telemetry motor, or include all motors present in the ESP `GET_MOTOR` stream?
4. For the docs tab, do you prefer "PDF first", "searchable register table first", or both in the first pass?
5. Should chart PNG export include only the chart, or chart plus current metric cards and wheel visualization?

