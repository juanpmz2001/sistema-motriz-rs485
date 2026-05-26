# Future Control Tuning UI Notes

Date: 2026-05-12

Purpose: keep a visible note that Toño should eventually have a dedicated interface for experimenting with controller parameters such as proportional, integral and differential gains.

## Future Feature

Build a local UI that allows controlled tuning of SVD48 parameters related to:

- Speed-loop PID:
  - Kp
  - Ki
  - Kd
- Position-loop PID:
  - Kp
  - Ki
  - Kd
- Current-loop gain.
- Speed feed-forward gain.
- Speed-loop dead zone.
- Hall calibration parameters.
- Motor electrical parameters such as Lq, Ld and Rs, only after we confirm exact data type and frame behavior.

## Safety Requirements

- Always read and save the current controller profile before writing any tuning value.
- Require explicit confirmation for persistent writes.
- Clearly separate live-tunable values from values that require motors stopped.
- Provide a revert-to-last-known-good action.
- Log every write with:
  - User-facing parameter name.
  - Register or parameter key.
  - Old value.
  - New value.
  - TX hex frame.
  - RX hex frame.
  - Decoded controller response.
- Do not expose this UI as the first SV-Config replication step. Build the read-only parameter/schema layer first.

## Dependencies

- Typed SVD48 parameter schema.
- Confirmed float encoding for PID/electrical parameters.
- Safe profile backup/restore.
- RS485 frame capture from SV-Config for any parameter that behaves differently from the manual.
- Live telemetry recording and replay, so tuning results can be compared before/after.

## First Practical Milestone

Create a read-only page that displays current PID-related values and marks each field as:

- `verified`: observed and decoded correctly on Toño hardware.
- `manual`: listed in the UU Motor manual but not yet observed.
- `suspect`: manual address/type may be wrong or firmware dependent.
- `captured`: confirmed by sniffing SV-Config traffic.

