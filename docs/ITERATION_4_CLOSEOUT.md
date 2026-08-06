# Iteration 4 closeout record

## Record scope

This record covers `refactor/svd48-device-composition` on 2026-08-06. It closes the
hardware-independent verification of Iteration 4 and records the remaining bench and
legacy limits. It does not authorize motor actuation, flashing, OTA, network tests or
register writes.

The repository state recovered at the start preserved the expected base and Iteration
4 commit, but the shared branch had already advanced with earlier closeout work:

- base `origin/main`: `4c8e1b24a344500a73ad77190ce533bace632834`;
- initial Iteration 4 commit:
  `ce42f8f0837f01b84b27789b212efd055c4dea40`;
- initial branch and remote branch both pointed to
  `c18e7d9c5f9b2a86959afe28832de75c0953b2c3`; and
- `origin/main` did not advance during local closeout.

No commit was rewritten and no force push was used. The pre-existing Iteration 4
commit remains in branch history.

## Closeout commits

| Commit | Intent |
| --- | --- |
| `ffc64f9` | Harden shared transport, SVD48 protocol/device observation semantics, poll scheduling and per-device poll serialization |
| `f98c444` | Enforce executable preflight and add output-free restricted diagnostic startup |
| `b0e0779` | Add host fakes, driver/factory/profile/application tests and protocol/dependency coverage |
| `ca7bf0b` | Add reproducible host, sanitizer and ESP-IDF 5.4.1 profile CI |
| `7adbbb5` | Align the as-built architecture, safety, API, SVD48 and migration documents |
| `8e18a4d` | Record the local Iteration 4 closeout evidence |
| `be72f92` | Link the Iteration 4 pull request and remote verification gate |
| `c18e7d9` | Declare a manual workflow trigger for use once the workflow exists on the default branch |
| `7929b6a` | Preserve poll-task dependencies and make late stop completion safely collectible |
| `16e42c7` | Make sanitizers fail-fast and close factory, bench, health and concurrency coverage gaps |
| `db0df13` | Reconcile the manual merge gate, final local metrics and lifecycle documentation |
| `e0dd148` | Index the final Iteration 4 lifecycle hardening in this closeout record |

## Added files

The complete iteration added the following groups relative to the base commit:

- portable and ESP-IDF transport components: `bus_transport` and
  `rs485_transport`;
- executable composition support: `robot_driver_factory`;
- physical SVD48 device, channel, poll service and poll task files;
- the direct `svd48_channel_endpoint_adapter`;
- reusable fake transport plus host tests for transport, device, polling, adapter,
  factory and both selected profiles;
- `tools/test_application_compatibility.py`;
- `.github/workflows/firmware-ci.yml` and two profile defaults fragments; and
- the documentation index, `docs/README.md`.

No document was archived. The reviewed documents were still useful as current or
target-design sources and were made explicit instead of being duplicated or moved.
The documentation index records that no `docs/archive/` history exists at this
revision.

## Implemented architecture

The active topology is selected from an immutable C profile. Executable preflight
validates the available SVD48 factory, bus compatibility, runtime storage, endpoint
and legacy capacities, schedulable periods, endpoint constructability and the safety
invariant that a velocity endpoint is also stoppable. All preflight checks complete
before a UART bus or device is constructed.

Normal composition creates one `rs485_transport` for each referenced RS485 bus, one
`svd48_device` per physical two-channel controller, explicit M1/M2 channels, direct
typed endpoint adapters, one N-device polling service, the coordinator/application
port and a transitional legacy view. Bus selection uses `device.bus_id`, not profile
array position.

`SET_SPEED`, `STOP n`, `STOP ALL`, boot stop and safety stop use the application
port/coordinator/direct adapter path. The coordinator is not a global writer: legacy
maintenance, enable, clear-fault, identify, OTA and motion paths remain.

For a schema-valid but non-executable profile, a non-pending OTA image starts only
the restricted serial RX gateway. No bus or device output is constructed, streaming
and network/OTA tasks are not started, and the allowlist contains only status,
version/help and `STOP ALL`. The stop command reports that outputs were not
initialized rather than claiming a physical stop.

## Acceptance evidence

| Criterion | Status | Evidence | Notes |
| --- | --- | --- | --- |
| Recover exact branch/base context | PASS | Remote refs and history matched `4c8e1b` / `ce42f8f`; `origin/main` remained unchanged | No branch recreation or history rewrite |
| Map migrated and legacy routes | PASS | `docs/ARCHITECTURE.md` and `docs/MIGRATION_MAP.md`; source call-site audit | Coordinator is not described as the only global writer |
| Shared bus abstraction and concurrent statistics | PASS | `bus_transport_test`; separate production bus/statistics locks | Individual transactions are serialized |
| One SVD48 device with explicit M1/M2 channels | PASS | `svd48_device_test`, protocol vectors and profile tests | One physical address owns both channels |
| Per-observation validity, freshness and health | PASS | Device and poll-service regressions for partial/stale/current-vs-speed behavior | Unrelated success cannot refresh a failed speed observation |
| Poll result/backoff and N-device scheduling | PASS | Poll tests cover one/two devices, periods, partial, recovery, wraparound and completion-relative deadlines | `PARTIAL` receives failure backoff |
| Concurrent legacy/service polling | PASS | Deterministic pthread regression | Second poll returns busy; cycles and `poll_count` do not interleave |
| Direct endpoint adapter | PASS | Adapter tests cover ranges, capabilities, rollback stop, diagnostics and M1/M2 | Velocity without stoppable is rejected in preflight |
| Executable factory and pure preflight | PASS | Factory tests cover missing/incompatible factories, duplicate IDs/names/channels/addresses, missing references, invalid devices/endpoints, multibus diagnostic identity and all static limits | The real SVD48 registration is source-characterized and covered by both integration builds |
| `current_robot` profile | PASS | Host shape test and clean ESP-IDF 5.4.1 build | Two devices and four ordered endpoints |
| `bench_single_svd48_motor` profile | PASS | Host shape test, application characterization and clean ESP-IDF 5.4.1 build | Only index 0 is exposed; `MOVE_VEL` is unsupported; speed and stop routes remain available |
| Protocol compatibility | PASS | Six Python tests plus C request/response tests | Covers reads, single/multiple writes, M1/M2 targets/stops, exception and bad CRC |
| Application compatibility | PASS | Thirteen source-level characterization tests | Preserves syntax/results/routes for speed, stop, bench indexing, telemetry and maintenance commands; not a hardware runtime test |
| Safe diagnostic startup | PASS | Policy tests, source characterization, independent code audit and both firmware builds | Pending OTA verification still follows rollback policy |
| Local CI-equivalent matrix | PASS | Host, sanitizer, Python and both isolated profile builds executed with the workflow commands | External GitHub runner execution remains the manual merge gate below |
| GitHub-hosted PR checks | NOT VERIFIED | [PR #5](https://github.com/juanpmz2001/sistema-motriz-rs485/pull/5) reported zero GitHub Actions runs on the final head during the [active Actions incident](https://www.githubstatus.com/incidents/qcvjkzcs7j74) on 2026-08-06 | Must become PASS on the final head before merge; the platform outage is not a waiver |
| As-built documentation and links | PASS | Documentation audit, Mermaid review and local link validation | No obsolete file required archival |
| Hardware response, timing and physical RPM interpretation | NOT VERIFIED | Deliberately not exercised | Requires separately authorized off-ground evidence |

## Tests executed

The final production tree was verified with:

```text
tools/run_host_tests.sh
BOTFARMS_HOST_TEST_SANITIZERS=ON tools/run_host_tests.sh
python3 tools/test_svd48_protocol.py
python3 tools/test_dependency_contracts.py
python3 tools/test_application_compatibility.py
```

Results:

- host CTest matrix: 17/17 targets passed;
- ASan/UBSan CTest matrix: 17/17 targets passed;
- protocol reference: 6/6 passed;
- dependency contracts: 10/10 passed; and
- application compatibility characterization: 13/13 passed.

The host suite has a ten-second per-test timeout. Concurrency tests use explicit
condition-variable gates rather than long timing sleeps. ASan and UBSan are configured
to halt on the first detected error. The application test is honestly limited to
source-level characterization because the full gateway is bound to ESP-IDF/FreeRTOS;
firmware integration is covered by both builds.

## Clean firmware builds and resources

Both profiles were configured in new temporary directories with explicit CMake
`SDKCONFIG` and `SDKCONFIG_DEFAULTS` paths. This was also checked to leave the local
ignored repository `sdkconfig` unchanged. ESP-IDF was exactly `v5.4.1`, target
`esp32s3`; `idf.py build` and `idf.py size` passed with zero compiler warning lines.

| Metric | `current_robot` | `bench_single_svd48_motor` |
| --- | ---: | ---: |
| Application binary | 1,074,704 B (`0x106610`) | 1,074,640 B (`0x1065d0`) |
| Flash code | 789,758 B | 789,758 B |
| Flash data | 180,612 B | 180,548 B |
| Total image reported by `idf.py size` | 1,074,580 B | 1,074,516 B |
| DIRAM used / free | 109,135 / 232,625 B | 109,135 / 232,625 B |
| DIRAM BSS | 21,336 B | 21,336 B |
| DIRAM data / text | 19,444 / 68,355 B | 19,444 / 68,355 B |
| IRAM used / free | 16,383 / **1 B** | 16,383 / **1 B** |
| Application partition free | 5,216,752 B (83%) | 5,216,816 B (83%) |
| Warning lines | 0 | 0 |

The one-byte IRAM margin is a release risk even though both builds link. No further
IRAM growth should be accepted without an explicit configuration/linker review.

### Resource deltas

Against the initial `ce42f8f` branch baseline:

| Metric | `current_robot` delta | Bench delta |
| --- | ---: | ---: |
| Application binary | +4,592 B | +4,576 B |
| Flash code | +3,364 B | +3,364 B |
| Flash data | +1,104 B | +1,088 B |
| DIRAM / BSS | +904 B | +904 B |
| IRAM | 0 B | 0 B |

`origin/main` `4c8e1b` was also built cleanly for `current_robot` with the same toolchain.
The final branch adds 15,152 B of binary, 13,480 B of flash code, 1,664 B of flash
data and 3,408 B of DIRAM/BSS; IRAM is unchanged. A comparable main bench metric is
not available because that base profile fails compilation with
`CURRENT defined but not used` under `-Werror`.

### Tasks and stacks

Iteration 4 adds the shared `svd48_poll` task at priority 8 with a 4,096-byte stack.
Relevant active tasks remain:

| Task | Priority | Stack |
| --- | ---: | ---: |
| `robot_safety` | 9 | 4,096 B |
| `svd48_poll` | 8 | 4,096 B |
| `serial_gateway` RX | 6 | 12,288 B |
| `gateway_stream` | 4 | 4,096 B |

Restricted diagnostic startup creates only serial RX from this group; it does not
create the stream or polling/safety tasks.

## Defects corrected during closeout

- Preserved validity, failure and timestamp independently for all eight observations.
- Added explicit complete/partial/concrete-failure poll results and stale health.
- Scheduled period/backoff from poll completion and made wraparound deadline zero
  unambiguous.
- Serialized whole polls per device so `POLL_ONCE` cannot race the shared poll task.
- Made poll-task teardown collect a late completion after timeout, reject premature
  restart and preserve devices, locks and UART until the worker is quiescent.
- Rejected invalid/overflowing Modbus read ranges before any transport call.
- Restricted retries to operations whose outcome is safe to retry.
- Corrected raw speed naming/handling to RPM without invented factor-of-ten scaling.
- Protected 64-bit transport statistics with a dedicated lock.
- Made factory storage metadata enforce static capacity and diagnosed the four-binding
  legacy limit explicitly.
- Rejected invalid slave IDs, retry counts, duplicate legacy addresses,
  unschedulable periods, empty/nonconstructible endpoints and velocity without stop
  before output construction.
- Corrected endpoint failure diagnostics to report the resolved driver, bus, device
  and endpoint.
- Removed array-position bus selection from the legacy attachment path.
- Added restricted output-free diagnostic startup and made its initial help advertise
  only its allowlist.

## Accepted limitations and deferred work

These items do not invalidate the hardware-independent Iteration 4 contracts, but
they keep the firmware bench-only:

- Raw observed RPM already feeds the legacy 5-RPM OTA/maintenance readiness predicate
  and `PLATFORM_STATUS`. It is not independent proof of physical motion and remains
  unqualified on hardware.
- `robot_safety` emits migrated stops but still consumes the legacy telemetry view;
  the new per-observation health is not yet the approved active safety policy.
- `ENABLE`, `MOVE_VEL`, clear-fault, register/configuration maintenance,
  `SVD48_IDENTIFY START|STOP`, OTA preparation, trace and some polling/telemetry
  commands still traverse `robot_control`/the legacy wrapper. Identify can cause
  physical motion outside the coordinator.
- The coordinator has a mutex but no priority owner task, stop precedence, authority
  lease, TTL or deadman. Boot continues after a logged best-effort boot-stop failure.
- The legacy wrapper supports at most four endpoint bindings and its maintenance API
  identifies a controller by Modbus address without bus identity.
- UART/RS485 timing, controller exception behavior, task stack high-water marks and
  physical stop/RPM behavior were not measured.
- The poll-task timeout/recollection path is covered by source contracts, independent
  review and both firmware builds, but was not fault-injected in a FreeRTOS runtime;
  its inter-task stop flag remains the existing ESP-IDF-style `volatile bool`.
- IRAM has one byte of link-time headroom.

Recommended next work is a bounded safety/authority iteration: establish one
priority-aware actuation owner, migrate the remaining physical writers, make approved
per-observation health inhibit required capabilities, then perform separately
authorized off-ground SVD48 timing/RPM/stop qualification. It must not be folded into
this closeout.

## CI and merge gate

The workflow is configured to run on pull requests to `main` and pushes to
`refactor/**`. It executes the host matrix, ASan/UBSan, all Python contracts and an
ESP-IDF 5.4.1 matrix for both profiles. Each firmware job stores build logs, generated
configuration and size evidence as a 14-day artifact and writes metrics to the job
summary. No secrets or hardware are used.

At the remote state audited on 2026-08-06, Actions was enabled with all actions
allowed. GitHub received the branch push event, the workflow existed at the pushed
SHA and `refactor/svd48-device-composition` matched its `refactor/**` filter, but no
GitHub Actions run was created. GitHub simultaneously reported a major Actions outage
and stated that webhook triggers were throttled to approximately 15%, so many pushes
and pull requests were not triggering runs. This is an external gate failure, not
evidence that the workflow passed.

The `workflow_dispatch` declaration cannot be invoked while the workflow exists only
on the feature branch because GitHub requires it on the default branch. A later
substantive push may retry the natural `push` trigger, but neither a fabricated check
nor a merge during the outage is acceptable.

Review and the manual merge gate are tracked in
[PR #5](https://github.com/juanpmz2001/sistema-motriz-rs485/pull/5). Merge is
prohibited by project policy until its workflow checks pass, the branch remains
conflict-free and clean, and the final diff review has no blocking finding. `main`
currently has no branch protection or required-status-check rule, so GitHub does not
enforce this gate. The preferred integration is squash merge; rollback is a revert of
that single `main` commit. The feature branch should be retained until post-merge CI
and documentation visibility are confirmed.

## Current closeout classification

**NOT READY TO MERGE.** The local hardware-independent code, test, documentation and
build criteria are satisfied, but the required GitHub-hosted checks have not run due
to the active GitHub Actions outage. The outage does not relax the merge gate. The
iteration may be reclassified as `CLOSED WITH EXPLICIT DEFERRED ITEMS` only after the
checks pass on the final PR head and final review has no blocker. Hardware
qualification and the listed legacy/safety migrations remain explicitly deferred;
the firmware remains bench-only.
