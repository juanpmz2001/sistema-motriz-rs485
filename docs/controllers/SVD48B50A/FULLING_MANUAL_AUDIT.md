# Fulling Motor Manual Audit

Date: 2026-07-20

## Sources Reviewed

- Fulling Motor official SVD4812RC-AA product page:
  <https://www.fullingmotor.com/en/product/76_233>
- Fulling Motor official SVD4822RC-AA product page:
  <https://www.fullingmotor.com/en/product/76_232>
- Fulling Motor official manual URL:
  <https://www.fullingmotor.com/en/Uploads/file/20250122/1737543938891611.pdf>
- Local copy:
  `Fulling_SVD48_Series_Servo_Driver_User_Manual_V1.00_2025.pdf`
- Local SHA256:
  `bc76ce9e6412f2ea6461dc66b2a917c088c3e62c18e3dc0c680f93be67663335`
- Official SVD4812RC-AA datasheet URL:
  <https://www.fullingmotor.com/en/Uploads/file/20250122/1737543933622349.pdf>
- Local datasheet:
  `Fulling_SVD4812RC-AA_Servo_Driver_Datasheet_2025.pdf`
- Datasheet SHA256:
  `2a90b1f882012b8ab358c771915b6b228b3d1a6158b9fd3a9928fec09e12c54f`

Fulling publishes a second PDF at `1737543253809856.pdf`. Its extracted text is
byte-for-byte identical to the retained manual; only PDF binary metadata differs,
so the repository stores one copy.

## Applicability Boundary

The Fulling manual is for the modern, single-axis SVD48 family represented by
SVD4812RC-AA/SVD4822RC-AA. The connected controller is the older dual-motor
SVD48V30A/SVD48V50A-style unit, software `0x0131`, documented by UUMOTOR.
The separate three-page Fulling datasheet confirms SVD4812RC-AA is a 20-56 V,
single-axis servo drive rated 12 Arms continuous without auxiliary cooling.

Do not copy Fulling UART/CANopen object addresses, error codes, current conversion
formulas, initialization commands, connector pinouts or firmware procedures into
the dual SVD48V implementation. The product families share a name prefix but not
the observed protocol map or hardware topology.

| Area | Fulling SVD4812/22RC | Connected SVD48V dual | Reusable? |
| --- | --- | --- | --- |
| Axes | One motor | M1 and M2 | No |
| Feedback | Incremental/communication encoder | Hall/encoder/string encoder | Concept only |
| Parameter protocol | Fulling object dictionary/UART/CANopen | Modbus-like `0x03/0x06/0x10` | No |
| Motor initialization | `SaveMot`, reboot, `InitCtrl`, `SaveCtrl` | SVD48V register workflow | Concept only |
| Third-party motor data | Explicit complete list | Partial corresponding fields exist | Yes, as requirements |
| Mechanical gearbox | No matching SVD48V tooth registers | `0x2202/0x2203` documented but rejected | No new solution |

## Useful Additional Information

Appendix I provides a stronger third-party motor configuration checklist than the
older UUMOTOR manual. Before motor initialization/self-tuning, Fulling requires:

- feedback type and resolution;
- motor pole pairs;
- rated/IIT current and maximum current;
- line-to-line inductance;
- line-to-line resistance;
- reverse electromotive force (back-EMF);
- torque constant;
- rotor inertia;
- excitation current/time and current-loop bandwidth.

Its sequence is also explicit:

1. Enter all motor data.
2. Store motor parameters and restart the drive.
3. Initialize the motor and store control parameters.
4. Run Hall-angle self-tuning and wait for completion.
5. Restart again.
6. Perform the first test with a 5 A peak current limit.
7. If tuning repeatedly fails, verify or change motor direction and repeat only
   after storing/restarting.

This sequence is not directly executable on the connected SVD48V, but it is
strong evidence that Hall calibration should not be considered independent from
the electrical motor model.

## Consequences For KK16

Known KK16 data is insufficient for a defensible SVD48V motor model:

| Required item | Current evidence |
| --- | --- |
| Voltage | 48 V, confirmed |
| Rated current | 13 A, confirmed by local dynamometer report |
| Maximum current | 30 A, manufacturer product page |
| Pole pairs | 10, manufacturer product page |
| Mechanical gearbox | 5:1, manufacturer product page |
| Line-to-line resistance | Missing |
| Line-to-line inductance | Missing |
| Back-EMF/KV definition and value | Missing |
| Torque constant | Missing |
| Rotor inertia | Missing |
| Hall spacing (60/120 degrees) | Not independently confirmed |
| Phase-to-Hall sequence | Not independently confirmed |

The SVD48 currently contains plausible but inherited `Lq/Ld/Rs/KV` values. They
must not be relabeled as KK16 specifications. The public no-load wheel speed cannot
be substituted directly for motor KV because the KK16 has a 5:1 gearbox and the
SVD48 manual does not establish whether its KV field expects rotor-side or
output-side speed.

Repeated Hall calibration failures at 3, 6 and 15 A and with 10/50 RPM limits are
therefore consistent with an incomplete or wrong electrical motor model. The next
configuration step is to request the missing data or a controller parameter file
from UUMOTOR/Fulling, then verify units and float word order before writing.

## Gear-Ratio Finding

The Fulling manual contains an electronic gear ratio for scaling command pulses.
That is not the KK16 mechanical gearbox and does not map to SVD48V
`0x2202/0x2203`. The connected software `0x0131` rejected those two registers for
both read and FC16 write with Modbus exception `0x02`. No Fulling source found in
this audit provides an alternate compatible address for the dual SVD48V.

## Recommended Manufacturer Request

Ask UUMOTOR/Fulling for a KK16 + SVD48V30A/50A parameter export or, at minimum:

- exact KK16 variant/serial and Hall spacing;
- phase-to-phase resistance at a stated temperature;
- phase-to-phase inductance and test frequency;
- rotor-side KV/back-EMF constant and its units;
- torque constant and rotor inertia;
- recommended Hall calibration current;
- known-good Hall angle tables for both wiring directions;
- the correct SVD48V software/firmware version and register map for reduction ratio;
- whether motor parameters must be saved/rebooted before Hall calibration.

Until that information is available, keep wheels elevated, retain the 10 RPM
limit, and do not perform a loaded movement test based only on populated Hall
tables.
