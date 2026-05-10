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

## 9.5-C Linker/IRAM Bucket Investigation

Date: 2026-05-09

### Objective

Phase 9.5-B did not recover any reported `IRAM` from the `idf.py size` line:

```text
IRAM: 16383 / 16384 bytes
```

Phase 9.5-C investigated whether this number represents a practical blocker for normal future application code, or a fixed early linker bucket that is already saturated by ESP-IDF startup/low-level code.

No functional source changes were kept. No `sdkconfig.defaults` changes were made. No firmware was flashed.

### Commands Executed

Baseline verification:

```bash
git status --short
git log -3 --oneline
. /home/jp/esp/esp-idf-v5.4.1/export.sh
idf.py build
idf.py size
```

Relevant output:

```text
34e7572 Document OTA memory audit
213223d Enable OTA rollback validation
e2f8051 Add manual OTA update command

ESP-IDF v5.4.1
sistema-motriz-rs485.bin binary size 0xf15f0 bytes
Total image size: 988536 bytes
Flash Code: 709582 bytes
Flash Data: 165668 bytes
DIRAM: 114019 / 341760 bytes, remain 227741
IRAM: 16383 / 16384 bytes, remain 1
RTC FAST: 52 / 8192 bytes
```

Map and symbol extraction:

```bash
grep -n "Memory Configuration" -A80 build/sistema-motriz-rs485.map
grep -n "Linker script and memory map" -A120 build/sistema-motriz-rs485.map
grep -n "\.iram0\.vectors\|\.iram0\.text\|_diram_i_start\|_iram_text_end\|iram0_0_seg\|iram0_2_seg" build/sistema-motriz-rs485.map
xtensa-esp32s3-elf-nm -S --size-sort build/sistema-motriz-rs485.elf
python -m esp_idf_size build/sistema-motriz-rs485.map --format json
python -m esp_idf_size build/sistema-motriz-rs485.map --archives --format json
python -m esp_idf_size build/sistema-motriz-rs485.map --files --format json
```

Notes:

- The shell did not provide `python` until ESP-IDF was exported; `python3` was used for standalone map parsing scripts.
- `xtensa-esp32s3-elf-nm` was available only after sourcing `/home/jp/esp/esp-idf-v5.4.1/export.sh`.

### What The 16 KB Bucket Represents

The linker memory configuration contains:

```text
iram0_0_seg  0x40374000  0x00057700  xr
iram0_2_seg  0x42000020  0x007fffe0  xr
```

The relevant linker symbols are:

```text
_diram_i_start = 0x40378000
.iram0.vectors 0x40374000 0x403
.iram0.text    0x40374404 0x16a73
_iram_text_end = 0x4038af00
```

`idf.py size` splits the executable internal RAM region at `_diram_i_start`:

- `IRAM` is the first 16 KB window: `0x40374000 <= addr < 0x40378000`.
- `DIRAM .text` is the executable internal RAM after that boundary, starting at `0x40378000`.

The reported `IRAM 16383 / 16384` is therefore:

```text
.iram0.vectors: 1027 bytes
.iram0.text before _diram_i_start: 15356 bytes
total: 16383 bytes
```

This is a real fixed 16 KB window, but it is not the whole `.iram0.text` footprint. The full `.iram0.text` is about 92787 bytes. The remaining 77431 bytes are reported as `DIRAM .text`, not as `IRAM`, because they live after `_diram_i_start`.

### Exact Occupants Before `_diram_i_start`

The range `0x40374000-0x40378000` contains 16383 bytes of real linked entries. Top contributors by object:

| Bytes | Object |
|---:|---|
| 1998 | `esp-idf/xtensa/libxtensa.a(xtensa_vectors.S.obj)` |
| 1350 | `esp-idf/esp_system/libesp_system.a(cpu_start.c.obj)` |
| 1017 | `esp-idf/bootloader_support/libbootloader_support.a(bootloader_flash.c.obj)` |
| 1001 | `esp-idf/heap/libheap.a(heap_caps_base.c.obj)` |
| 769 | `esp-idf/esp_hw_support/libesp_hw_support.a(intr_alloc.c.obj)` |
| 759 | `esp-idf/newlib/libnewlib.a(locks.c.obj)` |
| 732 | linker fill/alignment |
| 699 | `esp-idf/esp_system/libesp_system.a(debug_helpers.c.obj)` |
| 662 | `esp-idf/heap/libheap.a(heap_caps.c.obj)` |
| 617 | `esp-idf/esp_system/libesp_system.a(system_internal.c.obj)` |
| 479 | `esp-idf/esp_mm/libesp_mm.a(esp_mmu_map.c.obj)` |
| 468 | `esp-idf/freertos/libfreertos.a(tasks.c.obj)` |
| 335 | `esp-idf/esp_hw_support/libesp_hw_support.a(regi2c_ctrl.c.obj)` |
| 316 | `esp-idf/esp_ringbuf/libesp_ringbuf.a(ringbuf.c.obj)` |
| 275 | `esp-idf/esp_system/libesp_system.a(esp_ipc.c.obj)` |
| 275 | `esp-idf/esp_hw_support/libesp_hw_support.a(periph_ctrl.c.obj)` |
| 274 | `esp-idf/hal/libhal.a(efuse_hal.c.obj)` |

Representative named symbols in this bucket include:

- vector/startup: `_WindowOverflow4`, `_Level2Vector`, `_DebugExceptionVector`, `call_start_cpu0`, `call_start_cpu1`, `do_multicore_settings`
- panic/backtrace/reset: `panicHandler`, `panic_abort`, `esp_backtrace_print_from_frame`, `esp_restart_noos`
- heap: `heap_caps_malloc`, `heap_caps_malloc_default`, `heap_caps_realloc_base`, `heap_caps_free`
- interrupts: `esp_intr_enable`, `esp_intr_disable`, `esp_intr_noniram_enable`, `esp_intr_noniram_disable`, `shared_intr_isr`
- flash/cache/MMU: `bootloader_flash_execute_command_common`, `s_do_mapping`, `s_do_cache_invalidate`, `esp_mmu_paddr_find_caps`
- RTOS/newlib: `ipc_task`, `_lock_acquire`, `_lock_release`, `esp_vApplicationTickHook`

This is mostly ESP-IDF low-level startup, vector, interrupt, heap, flash/cache/MMU, panic and FreeRTOS support code. No Botfarms application component is a top contributor inside this first 16 KB bucket.

### What Lives After `_diram_i_start`

The range `0x40378000-0x4038af00` is still executable internal RAM, but `idf.py size` counts it as `DIRAM .text`. Top contributors by object:

| Bytes | Object |
|---:|---|
| 8929 | `esp-idf/freertos/libfreertos.a(tasks.c.obj)` |
| 4976 | `esp-idf/heap/libheap.a(tlsf.c.obj)` |
| 4284 | `esp-idf/esp_ringbuf/libesp_ringbuf.a(ringbuf.c.obj)` |
| 4016 | `components/esp_wifi/lib/esp32s3/libpp.a(pp.o)` |
| 3767 | `components/esp_wifi/lib/esp32s3/libpp.a(pm.o)` |
| 3172 | `esp-idf/hal/libhal.a(spi_flash_hal_iram.c.obj)` |
| 2823 | `esp-idf/spi_flash/libspi_flash.a(esp_flash_api.c.obj)` |
| 2801 | `esp-idf/spi_flash/libspi_flash.a(spi_flash_chip_generic.c.obj)` |
| 2766 | `esp-idf/freertos/libfreertos.a(queue.c.obj)` |
| 2699 | `components/esp_phy/lib/esp32s3/libphy.a(phy_reg.o)` |
| 2548 | `esp-idf/esp_hw_support/libesp_hw_support.a(rtc_clk.c.obj)` |
| 2195 | `components/esp_wifi/lib/esp32s3/libpp.a(wdev.o)` |
| 1865 | `components/esp_wifi/lib/esp32s3/libnet80211.a(ieee80211_output.o)` |

This explains Phase 9.5-B:

- `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH`
- `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH`
- `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`
- `CONFIG_ESP_WIFI_IRAM_OPT=n`

mostly affected the larger executable internal-RAM area after `_diram_i_start`, which `idf.py size` reports as `DIRAM .text`. They did not reduce the first fixed 16 KB `IRAM` bucket.

### Temporary Normal-Code Experiment

A temporary `main/iram_probe_temp.c` and temporary `main.c` reference were added, built, measured and then removed.

The function was normal C code with no `IRAM_ATTR`.

Result:

| Metric | Baseline | Normal probe | Delta |
|---|---:|---:|---:|
| Binary size | 988656 | 994416 | +5760 |
| Total image size | 988536 | 994292 | +5756 |
| Flash Code | 709582 | 715338 | +5756 |
| Flash Data | 165668 | 165668 | 0 |
| DIRAM | 114019 | 114019 | 0 |
| IRAM | 16383 | 16383 | 0 |

Conclusion:

- Normal C application logic increases `Flash Code`.
- It does not increase the first 16 KB IRAM bucket.
- It does not increase `DIRAM .text` unless the code is explicitly placed into IRAM or comes from an IRAM-safe component/library.

### Temporary `IRAM_ATTR` Experiment

The same temporary probe was changed to use `IRAM_ATTR`.

Result:

| Metric | Baseline | `IRAM_ATTR` probe | Delta |
|---|---:|---:|---:|
| Binary size | 988656 | 989056 | +400 |
| Total image size | 988536 | 988940 | +404 |
| Flash Code | 709582 | 709606 | +24 |
| Flash Data | 165668 | 165668 | 0 |
| DIRAM | 114019 | 114399 | +380 |
| DIRAM `.text` | 77431 | 77811 | +380 |
| IRAM | 16383 | 16383 | 0 |

The probe symbol was placed at:

```text
40377498 00000132 T botfarms_probe_normal
```

That address is inside `0x40374000-0x40378000`, but `used_iram` stayed constant because the first 16 KB bucket was already saturated. The new IRAM function displaced other `.iram0.text` content past `_diram_i_start`, increasing `DIRAM .text`.

Conclusion:

- `IRAM_ATTR` is still dangerous because it consumes executable internal RAM.
- `idf.py size` `IRAM` alone will not show growth once this first 16 KB bucket is already full.
- Future memory gates must track `DIRAM .text`, full `.iram0.text`, and link success, not only `IRAM 16383/16384`.

All temporary files and temporary `main.c`/`main/CMakeLists.txt` changes were removed after the experiments. The final rebuild returned to baseline:

```text
Binary size: 0xf15f0 / 988656 bytes
Total image size: 988536 bytes
Flash Code: 709582 bytes
Flash Data: 165668 bytes
DIRAM: 114019 / 341760 bytes
IRAM: 16383 / 16384 bytes
RTC FAST: 52 / 8192 bytes
```

### Answers To The 9.5-C Questions

1. **What does `16383/16384` represent?**  
   It is the fixed first 16 KB executable IRAM window from `0x40374000` to `_diram_i_start=0x40378000`, containing vectors plus the first part of `.iram0.text`.

2. **What symbols are inside `0x40374000-0x40378000`?**  
   Mostly ESP-IDF vector/startup, CPU start, heap, interrupt allocation, newlib locks, panic/backtrace, flash/cache/MMU and small FreeRTOS support symbols.

3. **What occupies it most?**  
   `xtensa_vectors.S.obj`, `cpu_start.c.obj`, `bootloader_flash.c.obj`, `heap_caps_base.c.obj`, `intr_alloc.c.obj`, `locks.c.obj`, linker fill, `debug_helpers.c.obj`, `heap_caps.c.obj`, and `system_internal.c.obj`.

4. **Why did B2-B5 not reduce it?**  
   Those flags moved or removed code from the larger `.iram0.text` area after `_diram_i_start`, which ESP-IDF reports as `DIRAM .text`, not from the first 16 KB bucket.

5. **Would normal new C logic increase this bucket?**  
   No, based on the temporary normal-code experiment. It increased `Flash Code` by 5756 bytes and did not change `IRAM` or `DIRAM`.

6. **Would `IRAM_ATTR` functions increase it?**  
   They can occupy addresses inside the bucket, but once the bucket is full the reported `used_iram` remains capped. The practical growth appears as increased `DIRAM .text` or eventual linker pressure. New `IRAM_ATTR` should still be treated as dangerous.

7. **What future changes are dangerous for this bucket or executable internal RAM?**  
   New `IRAM_ATTR`, new ISR-safe drivers, `ESP_INTR_FLAG_IRAM`, enabling inactive ISR-heavy components, changing panic/backtrace/debug settings, changing heap/FreeRTOS low-level placement, SPI flash/cache/MMU changes, and enabling `ppm_decoder` without a separate audit.

8. **Can Iteration 10 advance if it only adds normal FreeRTOS task logic?**  
   Yes, with guardrails. A normal low-priority task for periodic manifest checks should compile into flash code, not the critical IRAM bucket, provided it does not add ISR handlers, `IRAM_ATTR`, low-level driver IRAM-safe options, or automatic OTA writes.

9. **What guardrails are required?**  
   See the guardrails section below.

10. **Should the success metric remain “recover 4 KB from this bucket”?**  
    No. The better policy is: do not add new IRAM-critical code; track `IRAM`, `DIRAM .text`, full `.iram0.text`, total image size, and link success every iteration. Recovering 4 KB from the first bucket would require changing low-level ESP-IDF/startup behavior and is not a good target for normal application development.

### Guardrails For Future Iterations

- Do not add `IRAM_ATTR` unless the change has a written justification and map-file evidence.
- Do not add new ISR handlers in Iteration 10.
- Do not use `ESP_INTR_FLAG_IRAM` in Iteration 10.
- Do not enable `ppm_decoder` until it gets its own ISR/IRAM audit.
- Do not enable IRAM-safe driver options casually, such as UART/I2C/I2S/PCNT/RMT ISR-in-IRAM options.
- Do not change SPI flash/cache/MMU, panic, backtrace, heap, FreeRTOS low-level, interrupt allocation or startup settings without a dedicated experiment.
- Run `idf.py build` and `idf.py size` in every iteration.
- Track all of these metrics, not only `IRAM`:
  - `IRAM`
  - `DIRAM .text`
  - total `DIRAM`
  - full `.iram0.text` size from the map
  - `Flash Code`
  - total image size
- If `DIRAM .text` or full `.iram0.text` grows unexpectedly, stop and inspect the map.
- Iteration 10 may add only a normal low-priority FreeRTOS task and non-ISR OTA-check logic.
- Iteration 10 must keep automatic OTA writes/reboots disabled.

### Recommendation After 9.5-C

Iteration 10 can advance with the guardrails above if its scope is limited to automatic `OTA_CHECK` polling only.

Evidence:

- A normal temporary C function increased `Flash Code` by 5756 bytes and did not change the critical IRAM bucket.
- A temporary `IRAM_ATTR` function consumed executable internal RAM and increased `DIRAM .text`, confirming that IRAM placement remains the real danger.
- The current first 16 KB bucket is dominated by ESP-IDF low-level startup/vector/interrupt/heap/flash support, not by Botfarms application logic.
- Phase 9.5-B showed that Wi-Fi/FreeRTOS/heap IRAM knobs affect a different executable internal RAM region than the `16383/16384` bucket.

Minimum tests for Iteration 10:

- `idf.py build`
- `idf.py size`
- map-file check for `.iram0.text`, `DIRAM .text`, and `IRAM`
- `VERSION`
- `PING`
- `WIFI_CONNECT`
- `WIFI_STATUS CONNECTED`
- `OTA_CHECK`
- endpoint-down behavior
- repeated auto-check idle behavior
- `OTA_DOWNLOAD_TEST`
- `OTA_UPDATE` manual between slots
- rollback valid confirmation
- rollback by `NO_CONFIRM_ONCE`
- rollback by `SELF_TEST_FAIL_ONCE`
- `GET_MOTOR 2`
- `STOP 2`
- 12 seconds minimum without `task_wdt`

Do not implement automatic OTA writes in Iteration 10.

## 9.5-C Follow-up: Reverification Of B2-B5 With Correct Metric

After the linker investigation, the B2-B5 candidates were rebuilt in isolated `/tmp` build directories using temporary `/tmp` `SDKCONFIG` files. The repo `sdkconfig.defaults` and functional source were not changed.

The correct metric is not only the first `IRAM 16383 / 16384` bucket. The useful optimization target is executable internal RAM after `_diram_i_start`, reported mostly as `DIRAM .text`, plus the full `.iram0.text` map size.

### Build/Size Recheck

Baseline:

```text
Binary size: 988656
Total image size: 988536
Flash Code: 709582
Flash Data: 165668
DIRAM .text: 77431
DIRAM total: 114019
IRAM: 16383
Full .iram0.text: 92787
```

| Candidate | Change | Bin delta | Total delta | Flash Code delta | DIRAM `.text` delta | DIRAM total delta | Full `.iram0.text` delta | Result |
|---|---|---:|---:|---:|---:|---:|---:|---|
| B2 | `CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y` | +144 | +140 | +7548 | -7408 | -7408 | -7408 | Build OK |
| B3 | `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` | +736 | +736 | +10660 | -9924 | -9924 | -9924 | Build OK, physical OK |
| B4 | `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` | -528 | -520 | +9092 | -9580 | -9580 | -9580 | Build OK, physical OK |
| B5 | `CONFIG_ESP_WIFI_IRAM_OPT=n` | -336 | -336 | +6936 | -7240 | -7240 | -7240 | Build OK |

Interpretation:

- B3 recovers the most executable internal RAM, but it moves FreeRTOS non-ISR functions to flash. That has broader scheduler/timing risk.
- B4 recovers almost the same amount, reduces total image size slightly, and keeps the risk concentrated in Wi-Fi RX throughput/latency.
- B2 and B5 are valid but recover less memory than B3/B4.

### Physical Verification: B3

Candidate:

```text
CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
```

Manifest:

```text
URL: http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b3.bin
size: 989392
sha256: f614c08311662566503b960980eadcc1fd841fedec5964164f1dc5597f9a9f93
```

Validated:

- `VERSION`
- `PING`
- `WIFI_CONNECT`
- `WIFI_STATUS CONNECTED`
- `OTA_CHECK`
- `OTA_DOWNLOAD_TEST`
- `OTA_UPDATE` manual between slots
- post-boot valid confirmation
- rollback by `NO_CONFIRM_ONCE`
- rollback by `SELF_TEST_FAIL_ONCE`
- `GET_MOTOR 2`
- `STOP 2`
- idle without `task_wdt`

Result:

```text
B3 physical verification: PASS
```

### Physical Verification: B4

Candidate:

```text
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
CONFIG_ESP_WIFI_IRAM_OPT=y
```

Manifest:

```text
URL: http://192.168.1.107:8080/firmware/sistema-motriz-rs485-v1.0.0-b3.bin
size: 988128
sha256: c564c536e2dc6707ba573e416d572e096f81a53c8eb7dd9ef896806593ab88e3
```

Boot evidence:

```text
wifi_init: WiFi IRAM OP enabled
```

The `WiFi RX IRAM OP enabled` line no longer appears in the B4 boot logs, confirming the RX IRAM optimization is disabled while the broader Wi-Fi IRAM optimization remains enabled.

Validated:

- `VERSION`
- `PING`
- `WIFI_CONNECT`
- `WIFI_STATUS CONNECTED`
- `OTA_CHECK`
- `OTA_DOWNLOAD_TEST`
- `OTA_UPDATE` manual between slots
- post-boot valid confirmation
- rollback by `NO_CONFIRM_ONCE`
- rollback by `SELF_TEST_FAIL_ONCE`
- `GET_MOTOR 2`
- `STOP 2`
- idle without `task_wdt`

Observed Wi-Fi behavior:

- Wi-Fi connected successfully in repeated boot cycles.
- One initial B4 connection attempt required retries before association/IP, but it recovered within the configured retry window and OTA operations remained stable.
- OTA download and OTA update completed successfully with matching SHA256.

Result:

```text
B4 physical verification: PASS
```

### Recommendation After Reverification

Prefer B4 as the first config optimization to keep:

```text
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
```

Reason:

- It recovers about 9.6 KB of executable internal RAM.
- It reduces total image size slightly.
- It avoids moving FreeRTOS scheduler/task functions out of IRAM.
- Its risk is concentrated in Wi-Fi RX performance, which is acceptable for local OTA checks and manual OTA updates based on the physical validation.

B3 remains a valid second option if more executable internal RAM is needed later, but it should not be combined with B4 until B4 has been committed and observed through at least one normal development iteration.

Consolidation decision:

- B4 is the first optimization to keep permanently.
- `sdkconfig.defaults` now keeps `CONFIG_ESP_WIFI_IRAM_OPT=y`.
- `sdkconfig.defaults` now disables RX IRAM optimization with `# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set`.
- B3 remains the second option if later work needs additional executable internal RAM, but it is not combined with B4 in this phase.
- B2, B3 and B5 remain reverted for now.

Final B4 consolidation build metrics from the repo build:

| Metric | Value |
|---|---:|
| Binary size | 988128 bytes |
| Total image size | 987988 bytes |
| Flash Code | 718674 bytes |
| Flash Data | 165636 bytes |
| DIRAM `.text` | 67851 bytes |
| DIRAM total | 104439 bytes |
| IRAM reported | 16383 / 16384 bytes |
| Full `.iram0.text` | 83207 bytes |
