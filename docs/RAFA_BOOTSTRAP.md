# Rafa USB bootstrap and OTA handoff

## Scope and safety boundary

This runbook commissions the ESP32-S3 that will later be installed in Rafa. The
board is bare during this procedure. It authorizes only a complete USB flash,
credential/token provisioning, read-only status checks and an OTA round trip.
It does not authorize an SVD48 transaction, motor command, PPM capture, parameter
write or motion test.

Rafa is a sprayer with this immutable build profile:

- board `botfarms_esp32s3_rev1` / target `esp32s3`;
- one RS485 bus on UART2, TX GPIO17, RX GPIO16, 115200 baud;
- one required SVD48 at address 2, with `rafa_traction_m1` and
  `rafa_traction_m2` endpoints limited to `-15..15 RPM`;
- one PPM RC input on GPIO14 through `IBUS_RECEIVER_MODE_PPM`;
- no second SVD48, servo, AS5600, steering endpoint or application geometry.
- native USB Serial/JTAG as the primary stdin/stdout console for this final
  provisioning session; post-bootstrap operation uses Maintenance LAN.

The endpoint names intentionally do not infer left/right. An unplugged SVD48 and
PPM receiver are expected during bootstrap and must remain visible as unavailable;
they are not physical qualification results.

## Reproducible profile build

Use ESP-IDF 5.4.1 and isolated build/config paths. This avoids a developer's
`menuconfig` state becoming part of the released identity.

```bash
source /path/to/esp-idf-v5.4.1/export.sh
idf.py -D SDKCONFIG=/tmp/sdkconfig-rafa \
  -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults;$PWD/ci/sdkconfig.rafa.defaults" \
  -B /tmp/build-rafa set-target esp32s3
grep -Fx 'CONFIG_BOTFARMS_PROFILE_RAFA=y' /tmp/sdkconfig-rafa
idf.py -D SDKCONFIG=/tmp/sdkconfig-rafa \
  -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults;$PWD/ci/sdkconfig.rafa.defaults" \
  -B /tmp/build-rafa build
python3 tools/check_memory_headroom.py /tmp/build-rafa/sistema-motriz-rs485.map
```

Build from a clean commit so `VERSION` reports the real 40-character source SHA
and `GIT_DIRTY:0`. Record the ESP-IDF version, build number, binary SHA-256 and
partition table before flashing.

## One-time USB provisioning

Resolve the exact USB by-id path and confirm the chip identity before writing.
The first `flash` must write the bootloader, 16 MB partition table, OTA data and
factory application; `app-flash` is insufficient.

Provision only through the trusted serial gateway. Never put the Wi-Fi password,
maintenance token or OTA token in source, command transcripts or durable logs.

```text
WIFI_SET "ssid" "password"
MAINT_TOKEN_SET <maintenance-token>
OTA_ANNOUNCE_TOKEN_SET <ota-token>
WIFI_CONNECT
WIFI_STATUS
MAINT_LAN_STATUS
OTA_ANNOUNCE_STATUS
OTA_CONFIG
OTA_ROLLBACK_STATUS
```

Both tokens are independent and persist in NVS. Automatic installation must remain
off. After provisioning, reboot once and stop using serial as evidence: discovery,
identity, profile, composition, platform, safety and OTA status must all be obtained
over the LAN.

## Mandatory OTA round trip

Prepare a second clean Rafa image with a strictly higher `FW_BUILD_NUMBER`; do not
add an unrelated functional change just to distinguish it. Generate the manifest
with `tools/ota_prepare_release.py`, serve it on a LAN-reachable address, then use
the authenticated announce channel in this order:

1. `check` — manifest/project/target/build acceptance;
2. `download_test` — complete download, size and SHA-256 without selecting a slot;
3. `update` — inactive-slot write, boot selection and reboot;
4. LAN rediscovery and read-only post-update checks.

The post-update evidence must show the higher build, profile `rafa`, supported and
ready composition, resolved pending verification, no unexpected rollback and Wi-Fi/
Maintenance LAN reconnection without serial access. A UDP acknowledgement alone is
not proof that the new image booted successfully.

## Handoff gates

- Rafa profile host test and clean ESP-IDF build: required.
- Complete USB flash and correct dual-slot partition layout: required.
- Persistent Wi-Fi, Maintenance LAN and OTA announce provisioning: required.
- Engineering Console authenticated LAN identity/status: required.
- OTA check, download test and higher-build round trip: required.
- Physical SVD48 qualification: **NOT TESTED**.
- Physical PPM qualification: **NOT TESTED**.
- Motor motion: **NOT TESTED**.
- Left/right channel mapping: **NOT TESTED**.

Do not install the controller in Rafa until all non-physical bootstrap gates above
are recorded as passed. Stop after handoff; installed hardware bring-up belongs to
a later, separately authorized iteration.

## Commissioning record — 2026-08-16

**Result: PASS for standalone ESP32 bootstrap and OTA handoff.** This record contains
no SVD48, PPM or motion evidence.

- Controller: ESP32-S3 QFN56 rev 0.2, native USB Serial/JTAG, MAC
  `14:c1:9f:2b:8e:cc`, detected 16 MB flash. Secure boot and flash encryption were
  disabled; those remain future production-security decisions rather than hidden
  claims of this trusted-LAN bootstrap.
- USB image: build 21 from clean source
  `5c3136791d3f895440635f4a00494cbdd10cd159`, application SHA-256
  `9e0c959818f9ca818a9e515edcb9ce596b7dcaa4a27fa48e1683ac522224844b`,
  1,100,608 bytes. Full flash wrote bootloader, partition table, initial OTA data
  and `ota_0`; every esptool write verified its hash.
- Partition layout: NVS, OTA metadata, PHY, coredump, two 6 MB application slots
  and FAT storage. The initial app reported `OTA_STATE:VALID`,
  `PENDING_VERIFY:0` and `GIT_DIRTY:0`.
- Build resource gate: ESP-IDF v5.4.1; 227,296 bytes effective shared D/IRAM
  headroom against the 196,608-byte floor. The map SHA-256 was
  `5eeffdb0bb63147ae60852616d0d5a6bc9824e85c6f7340f3c8bd42773bea7ef`.
- Provisioning: Wi-Fi, Maintenance LAN token and independent OTA announce token
  were persisted in NVS. Secrets were neither printed nor committed. A reset caused
  a measured LAN outage and automatic reconnect; all acceptance queries after that
  reset used LAN only. The observed DHCP address was `192.168.1.194` and is not a
  static identity.
- Initial LAN state: profile `rafa`, board `botfarms_esp32s3_rev1`, schema valid,
  composition supported and runtime ready. Platform was `SAFE_IDLE`, motion inactive
  and safe for OTA. Both configured traction observations were offline/stale because
  no SVD48 was connected.
- Engineering Console: authenticated discovery and direct-IP connection both found
  the controller; overview reported build 21/profile `rafa`, and cached telemetry
  contained 20 rows with `OFFLINE` health rather than invented controller data.
- OTA check: build 22 was reported `UPDATE_AVAILABLE` from build 21.
- OTA download test: 1,100,608 bytes were downloaded and SHA-256 verified against
  the manifest in inactive `ota_1` without selecting it.
- OTA update: installed build 22 from clean source
  `ef55ddae457a345f0c53662f419f87c56ac6d4e3`, application SHA-256
  `ba2729e00ab6200efbf7ef446bc535e030b8242fbc386eb32449a56a033a4788`.
  The controller returned over LAN on `ota_1` with `OTA_STATE:VALID`,
  `PENDING_VERIFY:0`, rollback test mode `NONE`, profile `rafa`, runtime-ready
  composition and automatic installation still disabled.
- Post-OTA Console: connected to build 22/SHA `ef55dda...`, profile `rafa`, runtime
  ready, with the same honest `OFFLINE` telemetry health.

Physical SVD48 qualification, physical PPM qualification, motor motion and M1/M2
left/right mapping remain **NOT TESTED**. Those gates require the controller to be
installed and a new, explicitly authorized iteration.
