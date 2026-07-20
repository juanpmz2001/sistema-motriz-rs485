# Compatibility Matrix

Update this file only from observed build/release evidence. Source inspection alone is `UNVERIFIED`.

## Schema and Protocol Versions

| Surface | Current observed/source value | Target MVP | Compatibility rule | Status |
| --- | --- | --- | --- | --- |
| Firmware project/build | `sistema-motriz-rs485`, source build number `12` | monotonically increasing build with source/artifact provenance | OTA rejects same/lower build; never reuse build number for different bytes | `E2_BUILD_VERIFIED` |
| ESP target/flash | `esp32s3`, 16 MB OTA layout | explicit canonical-JSON `board` compatibility | partition/flash/target must match physical module | `HARDWARE_PREVIOUSLY_OBSERVED` |
| Maintenance LAN protocol | implicit v1 UDP JSON | management protocol `2.x` plus capabilities | reject unknown major; feature-gate by capabilities | `PROTOTYPE` |
| Serial command protocol | unversioned ASCII lines | correlated structured result plus diagnostic compatibility adapter | no success inference from arbitrary lines | `PROTOTYPE` |
| Canonical robot JSON schema | documentation/schema `1.0.0`; runtime absent | `1.x` self-contained board/controllers/actuators/kinematics/sources/SVD desired config | migrate known older minor; reject unknown major; preserve exact active/known-good JSON | `IN_PROGRESS_DESIGN_ONLY` |
| SVD48 parameter catalog | absent | versioned catalog consumed by embedded controller configuration sections | catalog confidence and controller identity constrain readable/writable keys | `NOT_STARTED` |
| Web package | source `0.1.0` | versioned API/event consumer | negotiate device capabilities; do not key behavior only on build | `SOURCE_ONLY` |

## Validated Combinations

Add one row per complete validation. Do not overwrite prior rows.

| Date | Robot profile/revision | Board/ESP | Firmware build/commit | Protocol | Web version/commit | SVD48 IDs and product/software/hardware versions | Transport | Evidence level | Result/notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-07-17 | none | ESP32-S3 source configuration | build `10`, commit base `d932792` plus dirty changes | maintenance v1 | `0.1.0`, commit base `0d55f8b` plus dirty changes | not queried in this audit | none | `E0/E2 build only` | Planning baseline, not a validated robot combination |

## Required Compatibility Checks

- Web major protocol support versus ESP advertised major/minor.
- Feature/capability presence for every visible action.
- Canonical robot JSON schema plus embedded `board.variant/revision`.
- Active firmware partition/build and profile migration support.
- SVD48 product/software/hardware versions against parameter catalog confidence.
- Controller count, IDs, channels, and expected-profile fingerprint.
- OTA artifact signature/project/target/build/profile-schema range.
- Rollback image support for active NVS/profile schema.

## Release Entry Template

```text
Date:
Robot/profile:
ESP board/flash:
Firmware build, commit, binary SHA-256, partition:
Management protocol/capabilities:
Web version, commit, image digest:
SVD48 identity per drive:
USB tests:
LAN tests:
Elevated tests:
Known limitations:
Rollback pair tested:
Codex/human session ID:
```
