# OTA Memory Audit

Date: 2026-05-09  
Firmware repo: `/mnt/windows/Users/juanp/OneDriveShouldDie/Documents/BotFarms/sistema-motriz-rs485`  
Baseline commit: `213223d Enable OTA rollback validation`  
Scope: Iteration 9.5-A documentation only.

## 1. Executive Summary

The firmware after Iteration 9 is functional for lab use: manual OTA works, rollback is enabled, post-boot validation is proven, RS485 control still works, and the 16 MB OTA partition layout has very large flash margin.

The main blocker before Iteration 10 is not flash size. The main blocker is reported IRAM pressure:

```text
IRAM: 16383 / 16384 bytes, 99.99%, 1 byte free
```

This does not mean the entire ESP32-S3 internal RAM is exhausted. The map file shows a larger executable internal memory region, and `idf.py size` reports a specific IRAM bucket. However, the reported bucket is effectively full, so any new code or configuration that places functions into early IRAM, ISR-safe code, Wi-Fi IRAM, flash/cache routines, heap, or FreeRTOS IRAM can break the build.

The audit shows that the active Botfarms application components are not directly consuming `.iram0.text`. The pressure comes almost entirely from ESP-IDF, especially FreeRTOS, Wi-Fi, flash/HAL, heap, PHY and support code.

Recommendation: do not start Iteration 10 yet. First run Phase 9.5-B as separate configuration experiments, one change per commit, until at least 4 KB of reported IRAM is recovered. An 8 KB or larger margin is the preferred target.

## 2. Commands and Evidence

### 2.1 Toolchain Verification

Commands:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py --version
python -m esptool --help
python - <<'PY'
import serial
print("pyserial OK")
PY
export PATH=/home/jp/.local/node-local/bin:$PATH
node --version
npm --version
```

Results:

```text
ESP-IDF v5.4.1
esptool.py v4.11.0
pyserial OK
node v22.22.2
npm 10.9.7
```

No persistent toolchain installation or workaround was required during this audit. One shell command that used `python` without sourcing ESP-IDF failed because `python` was not in that shell. The command was rerun after `export.sh`, and the ESP-IDF Python environment worked correctly.

### 2.2 Repository State

Commands:

```bash
git status --short
git log -1 --oneline
```

Results:

```text
git status --short: clean
git log -1 --oneline: 213223d Enable OTA rollback validation
```

Backend repo status was also clean during the initial audit:

```text
26b9e52 Harden local firmware backend serving
```

The backend `.env`, generated firmware binaries and generated manifest are ignored and must remain untracked.

### 2.3 Build and Size

Commands:

```bash
idf.py build
idf.py size
idf.py size-components
idf.py size-files
python -m esp_idf_size build/sistema-motriz-rs485.map --format json > /tmp/botfarms_size.json
python -m esp_idf_size build/sistema-motriz-rs485.map --archives --format json > /tmp/botfarms_size_archives.json
python -m esp_idf_size build/sistema-motriz-rs485.map --files --format json > /tmp/botfarms_size_files.json
```

Build result:

```text
Build succeeded.
sistema-motriz-rs485.bin binary size: 0xf15f0 bytes / 988656 bytes
Smallest app partition: 0x600000 bytes
Free in smallest app partition: 0x50ea10 bytes, 84%
```

Size result:

```text
Total image size: 988536 bytes
Flash Code: 709582 bytes
Flash Data: 165668 bytes
DIRAM: 114019 / 341760 bytes, 33.36%
IRAM: 16383 / 16384 bytes, 99.99%
RTC FAST: 52 / 8192 bytes, 0.63%
```

JSON summary:

```text
total_size: 988508
used_iram: 16383
iram_total: 16384
iram_remain: 1
iram_text: 15356
iram_vectors: 1027
used_diram: 114019
diram_total: 341760
diram_remain: 227741
diram_text: 77431
diram_data: 19444
diram_bss: 17144
flash_code: 709582
flash_rodata: 165412
flash_other: 256
```

### 2.4 Map and Symbol Inspection

Commands:

```bash
xtensa-esp32s3-elf-nm -S --size-sort build/sistema-motriz-rs485.elf > /tmp/botfarms_symbols_size_sorted.txt
xtensa-esp32s3-elf-nm -S --size-sort build/sistema-motriz-rs485.elf \
  | grep -Ei 'iram|isr|intr|uart|wifi|esp_|svd48|serial|ota|http|mbedtls|sha|json|robot' \
  > /tmp/botfarms_symbols_iram_candidates.txt
grep -n ".iram0.text\|iram" build/sistema-motriz-rs485.map | head -80
grep -n ".iram0.text\|iram" build/sistema-motriz-rs485.map | tail -120
```

Memory configuration excerpt:

```text
iram0_0_seg      0x40374000         0x00057700         xr
iram0_2_seg      0x42000020         0x007fffe0         xr
dram0_0_seg      0x3fc88000         0x00053700         rw
drom0_0_seg      0x3c000020         0x01ffffe0         r
```

Important linker markers:

```text
.iram0.vectors  0x40374000      0x403
.iram0.text     0x40374404    0x16a73
_diram_i_start  0x40378000
_iram_text_end  0x4038af00
```

The symbols before `_diram_i_start` are early boot, interrupt, heap, intr-alloc, reset and flash command functions. The larger region after `_diram_i_start` includes many FreeRTOS, flash, HAL, Wi-Fi and PHY routines. `idf.py size` reports the first IRAM bucket as nearly full, while the map file contains additional executable internal RAM sections.

### 2.5 Top IRAM Contributors

Top `.iram0.text` contributors by archive from `esp_idf_size`:

```text
15976 IRAM  libfreertos.a
13572 IRAM  libpp.a
11505 IRAM  libspi_flash.a
 9172 IRAM  libhal.a
 7537 IRAM  libesp_hw_support.a
 7327 IRAM  libheap.a
 5053 IRAM  libphy.a
 4600 IRAM  libesp_ringbuf.a
 4431 IRAM  libesp_system.a
 3863 IRAM  libnet80211.a
 2481 IRAM  libxtensa.a
 1647 IRAM  libnewlib.a
 1179 IRAM  libesp_timer.a
 1027 IRAM  libbootloader_support.a
  679 IRAM  libesp_mm.a
  462 IRAM  libesp_rom.a
  405 IRAM  libxt_hal.a
  349 IRAM  libesp_wifi.a
  273 IRAM  liblog.a
  243 IRAM  libesp_phy.a
```

Top `.iram0.text` contributors by file:

```text
9397 IRAM  libfreertos.a:tasks.c.obj
5144 IRAM  libheap.a:tlsf.c.obj
4600 IRAM  libesp_ringbuf.a:ringbuf.c.obj
4016 IRAM  libpp.a:pp.o
3767 IRAM  libpp.a:pm.o
3204 IRAM  libhal.a:spi_flash_hal_iram.c.obj
2966 IRAM  libfreertos.a:queue.c.obj
2939 IRAM  libspi_flash.a:esp_flash_api.c.obj
2853 IRAM  libspi_flash.a:spi_flash_chip_generic.c.obj
2744 IRAM  libesp_hw_support.a:rtc_clk.c.obj
2699 IRAM  libphy.a:phy_reg.o
2195 IRAM  libpp.a:wdev.o
1998 IRAM  libxtensa.a:xtensa_vectors.S.obj
1865 IRAM  libnet80211.a:ieee80211_output.o
1770 IRAM  libhal.a:spi_flash_hal_gpspi.c.obj
1634 IRAM  libspi_flash.a:spi_flash_chip_mxic_opi.c.obj
1354 IRAM  libnet80211.a:ieee80211_sta.o
1350 IRAM  libesp_system.a:cpu_start.c.obj
1259 IRAM  libpp.a:lmac.o
1179 IRAM  libfreertos.a:port.c.obj
```

Active Botfarms app components:

```text
0 IRAM  libmain.a
0 IRAM  libsvd48.a
0 IRAM  librobot_control.a
0 IRAM  libserial_gateway.a
0 IRAM  libconfig_manager.a
0 IRAM  libwifi_manager.a
0 IRAM  libota_manager.a
```

This means the current application logic is not the direct source of reported IRAM pressure.

### 2.6 Relevant sdkconfig Findings

Current important settings:

```text
CONFIG_COMPILER_OPTIMIZATION_DEBUG=y
CONFIG_COMPILER_OPTIMIZATION_LEVEL_DEBUG=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_ESP_WIFI_IRAM_OPT=y
CONFIG_ESP_WIFI_RX_IRAM_OPT=y
# CONFIG_ESP_WIFI_EXTRA_IRAM_OPT is not set
# CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is not set
# CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH is not set
CONFIG_SPI_MASTER_ISR_IN_IRAM=y
CONFIG_SPI_SLAVE_ISR_IN_IRAM=y
CONFIG_PERIPH_CTRL_FUNC_IN_IRAM=y
CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y
CONFIG_ESP_SPI_BUS_LOCK_ISR_FUNCS_IN_IRAM=y
CONFIG_HAL_SPI_MASTER_FUNC_IN_IRAM=y
CONFIG_HAL_SPI_SLAVE_FUNC_IN_IRAM=y
CONFIG_LWIP_IPV6=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=y
CONFIG_MBEDTLS_ERROR_STRINGS=y
```

ESP-IDF Kconfig notes:

```text
CONFIG_ESP_WIFI_IRAM_OPT:
  Disabling can save more than 10 KB IRAM, with lower Wi-Fi throughput.

CONFIG_ESP_WIFI_RX_IRAM_OPT:
  Disabling can save more than 17 KB IRAM, with lower Wi-Fi RX performance.

CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH:
  Can save up to 8 KB IRAM by moving selected non-ISR FreeRTOS functions to flash.

CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH:
  Can save RAM by placing heap component functions in flash.
  It is safe only if heap functions are not called from ISR.
```

## 3. Component Diagnosis

### main

No direct IRAM contribution. Main participates in rollback self-test sequencing and should not be refactored for memory first. Keep the current validation order intact.

### serial_gateway

No direct IRAM contribution. It consumes flash and runtime stack, but not the reported IRAM bucket. Do not reduce serial diagnostics until the OTA path is fully field-proven because this is the recovery/control interface.

### config_manager

No direct IRAM contribution. NVS config storage is not the immediate issue. Password redaction must remain unchanged.

### wifi_manager

No direct app IRAM contribution, but enabling Wi-Fi brings in `libpp`, `libnet80211`, `libesp_wifi`, PHY and LwIP. Wi-Fi IRAM optimization flags are the largest plausible low/medium risk knobs.

### ota_manager

No direct app IRAM contribution. OTA pulls in HTTP client, JSON/cJSON, app_update and mbedTLS/SHA256. These affect flash, rodata, heap and stack more than the reported IRAM bucket. Do not weaken SHA256, manifest validation or rollback.

### robot_control

No direct IRAM contribution. It is safety-critical and should not be changed for memory unless a later stack/high-water audit proves a specific issue.

### svd48

No direct IRAM contribution. RS485 is already physically proven. Do not change CRC, UART behavior, register order or timeout behavior as part of memory reduction.

### ESP-IDF Wi-Fi/LwIP

Wi-Fi-related archives consume measurable IRAM. Disabling `ESP_WIFI_RX_IRAM_OPT` and possibly `ESP_WIFI_IRAM_OPT` are good candidates if lower-risk compiler/heap/FreeRTOS experiments do not recover enough margin.

LwIP currently has IPv6, DHCP server and generous TCP defaults enabled. These are more likely flash/DRAM/heap levers than IRAM levers.

### HTTP client, JSON and mbedTLS/SHA256

These are not the immediate IRAM source, but they are large flash/heap contributors. Because current local OTA is HTTP, HTTPS-specific options could be reduced later if flash/heap becomes the problem. Do not remove SHA256 verification.

### rollback/app_update

Rollback is required and already validated. Do not disable or loosen it to recover memory.

### logging

Verbose maximum logging and dynamic log control likely cost flash/rodata/DRAM more than IRAM. Reducing maximum log level is reasonable after the memory experiments, but not as the first IRAM fix because diagnostics are still valuable.

### inactive components

`components/ppm_decoder` contains an `IRAM_ATTR` ISR, but it is not active in the current build. Do not activate it until IRAM has margin and its ISR path is reviewed. `bluetooth_controller` and `motor_controller` are also inactive.

## 4. Triage Table

| ID | Action | Category | Expected memory effect | Functional impact | Risk | Complexity | Tests | Recommendation |
|---|---|---|---|---|---|---|---|---|
| M1 | Document audit results | easy win | none | none | low | low | docs diff only | implement now |
| M2 | Set compiler optimization to size | low-risk config | Flash, some DIRAM/IRAM possible | lower debug friendliness | low/medium | low | full firmware + OTA suite | implement first in 9.5-B |
| M3 | Enable heap functions in flash | medium-risk config | IRAM reduction likely | heap calls from ISR would be unsafe | medium | low | full suite, no ISR heap evidence | second experiment |
| M4 | Enable FreeRTOS non-ISR functions in flash | medium-risk config | up to ~8 KB IRAM | timing/debug behavior changes | medium | low | full suite, WDT idle check | third experiment |
| M5 | Disable Wi-Fi RX IRAM optimization | low/medium config | potentially large IRAM gain | reduced Wi-Fi RX throughput | medium | low | Wi-Fi + OTA timing | fourth experiment if needed |
| M6 | Disable Wi-Fi IRAM optimization | medium config | potentially large IRAM gain | reduced Wi-Fi throughput/latency | medium | low | Wi-Fi + OTA timing | fifth experiment if still critical |
| M7 | Reduce HTTPS/mbedTLS features for HTTP-local build | low-risk config | Flash/heap, little IRAM | delays HTTPS migration | low/medium | medium | OTA HTTP, future HTTPS doc | plan later |
| M8 | Reduce max log level from VERBOSE | easy win | Flash/rodata/DRAM | less debug output | low | low | serial diagnostics | plan later |
| M9 | Measure task stack high-water marks | investigation | stack/heap tuning | extra diagnostics | low | medium | runtime telemetry | investigate after IRAM |
| M10 | Trim component `REQUIRES driver` to specific driver components | medium-risk refactor | flash/link reduction, uncertain IRAM | build dependency risk | medium | medium | build + hardware | investigate later |
| M11 | Change SPI flash/HAL IRAM options | high-risk/postpone | uncertain | can break flash/OTA/cache safety | high | medium | deep validation | do not do now |
| M12 | Remove rollback, SHA256, manifest validation or serial recovery | not worth it | possible flash/heap | unsafe OTA | high | low | N/A | do not do |

## 5. Prioritization Rules

Prioritize changes that:

- Recover reported IRAM without changing RS485 behavior.
- Are reversible and isolated to one config knob.
- Keep OTA manual, rollback and SHA256 verification intact.
- Preserve serial recovery commands.
- Keep errors visible.
- Can be validated with build, flash, serial, Wi-Fi, OTA and rollback tests.

Do not prioritize changes that:

- Touch SVD48 CRC/register behavior.
- Weaken OTA validation.
- Disable rollback.
- Hide failures by reducing critical logs.
- Modify partitions.
- Combine multiple memory changes in one commit.

## 6. Do Not Touch Without Strong Reason

- RS485 frame format, CRC order, UART2 setup, SVD48 register behavior.
- `OTA_UPDATE` manual safety path.
- Rollback post-boot validation.
- SHA256 and size verification.
- Manifest validation and localhost rejection.
- Password redaction.
- `partitions_ota_16mb.csv`.
- Serial recovery commands.
- Robot STOP/safety preparation before reboot.

## 7. Phase 9.5-B Experiment Plan

Phase 9.5-B must run one experiment per commit. If an experiment fails, revert it before trying the next one.

Common commands for every experiment:

```bash
. /home/jp/esp/esp-idf-v5.4.1/export.sh
git status --short
idf.py build
idf.py size
```

Every experiment must report:

```text
Total image size
bin size
Flash Code
Flash Data
DIRAM
IRAM
RTC FAST
```

Flash only if build and size pass.

Physical validation for every accepted experiment:

```text
VERSION
PING
WIFI_CONNECT
WIFI_STATUS CONNECTED
OTA_CHECK
OTA_DOWNLOAD_TEST
OTA_UPDATE manual between slots
rollback valid app confirmation
rollback by OTA_ROLLBACK_TEST NO_CONFIRM_ONCE
rollback by OTA_ROLLBACK_TEST SELF_TEST_FAIL_ONCE
GET_MOTOR 2
STOP 2
12 seconds idle without task_wdt
```

Success criteria:

```text
Initial target: recover at least 4 KB reported IRAM.
Preferred target: recover 8 KB or more reported IRAM.
No regression in manual OTA, rollback, Wi-Fi, RS485 or serial recovery.
```

### Experiment 9.5-B1: Compiler Optimization for Size

Objective:

- Change only compiler optimization from debug/default to size.

Expected effect:

- Lower flash size and possibly reduce some IRAM/DIRAM.

Risk:

- Reduced debug friendliness.
- Possible timing changes, though this is usually low risk.

Acceptance:

- Full test suite passes.
- IRAM improves or remains stable.

Rollback:

- Revert the single config change.

### Experiment 9.5-B2: Heap Functions in Flash

Objective:

- Enable `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` if Kconfig allows it.

Expected effect:

- Recover IRAM from heap/TLSF code.

Risk:

- Unsafe if heap APIs are called from ISR.
- Current active code search did not find app ISR heap usage, but ESP-IDF and future code must still be considered.

Acceptance:

- Full test suite passes.
- No heap/assert/panic under Wi-Fi, OTA download/write or RS485 use.

Rollback:

- Revert the single config change.

### Experiment 9.5-B3: FreeRTOS Non-ISR Functions in Flash

Objective:

- Enable `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`.

Expected effect:

- ESP-IDF documents up to roughly 8 KB IRAM savings depending on used functions.

Risk:

- Non-ISR FreeRTOS functions execute from flash.
- Timing behavior can change.

Acceptance:

- Full test suite passes.
- No task watchdog after idle and OTA workloads.

Rollback:

- Revert the single config change.

### Experiment 9.5-B4: Disable Wi-Fi RX IRAM Optimization

Objective:

- Set `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`.

Expected effect:

- ESP-IDF documents more than 17 KB IRAM savings, with reduced Wi-Fi RX performance.

Risk:

- Slower HTTP download or less Wi-Fi throughput.
- OTA may take longer but should remain acceptable for local lab updates.

Acceptance:

- Full test suite passes.
- OTA download/update time remains acceptable.
- Wi-Fi remains stable.

Rollback:

- Revert the single config change.

### Experiment 9.5-B5: Disable Wi-Fi IRAM Optimization

Objective:

- Set `CONFIG_ESP_WIFI_IRAM_OPT=n`.

Expected effect:

- ESP-IDF documents more than 10 KB IRAM savings, with reduced Wi-Fi throughput.

Risk:

- Broader Wi-Fi performance impact than RX-only optimization.

Acceptance:

- Full test suite passes.
- Wi-Fi and OTA remain reliable.

Rollback:

- Revert the single config change.

## 8. Recommendation

Do not advance to Iteration 10 until Phase 9.5-B has been run and reviewed.

Run experiments in this exact order:

1. `CONFIG_COMPILER_OPTIMIZATION_SIZE`
2. `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`
3. `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`
4. `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`
5. `CONFIG_ESP_WIFI_IRAM_OPT=n`

Stop as soon as the firmware has at least 4 KB reported IRAM free and the full manual OTA/rollback/RS485 validation suite passes. Continue toward 8 KB only if the next experiment has acceptable risk and the previous experiment did not create instability.

If no safe experiment recovers enough IRAM, keep OTA automatic disabled, forbid new `IRAM_ATTR`/ISR-heavy features, keep manual OTA only, and add a mandatory memory gate to every future iteration.
