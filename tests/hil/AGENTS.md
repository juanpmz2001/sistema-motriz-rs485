# Host HIL agent contract

This subtree contains host-side HIL orchestration only. Do not add firmware,
driver-specific register knowledge, transport framing, or physical-device access
to its automated tests.

- Keep manifests capability-oriented and validate them before opening a port.
- Runner schema v1 is intentionally one-endpoint L4 velocity with E2 controller
  feedback; do not relabel it as L5/L6/L7 or E3/E4.
- Tests must use fake clients and must never discover or open a real serial port.
- Motion manifests require all three explicit operator confirmations documented in
  `README.md`; never replace them with defaults or environment inference.
- A run must verify build/profile/board identity plus exact endpoint inventory and
  criticality, issue `STOP ALL` before test steps, and attempt `STOP ALL` again while
  closing on every catchable exit path.
- Durable evidence belongs outside the repository. Tests may use their own temporary
  directories. Reserve and validate the no-clobber evidence destination before
  connecting, record firmware artifact provenance, and treat `RESERVED` or
  `FINALIZE_FAILED` sidecars as non-results requiring reconciliation.
- Never report `PASS` from command acknowledgement alone. Require a bounded
  post-command observation assertion, exact stop acknowledgement and a fresh stopped
  observation after final cleanup; otherwise report a non-pass result.
- Verify changes with `python3 -m unittest tests.hil.test_hil_runner`.

Automatic cleanup is best effort. It cannot cover process kill, host failure, cable
loss, or target failure, so documentation and code must preserve the physical
power-cutoff requirement.
