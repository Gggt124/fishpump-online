# ESP32-S3 Native Firmware Migration & Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the fish environment control firmware from `scratch/fish-env-control-system` into the main `firmware/` directory, completely purge ESP32 Classic legacy code, tune performance exclusively for ESP32-S3 WROOM-1-N16R8 (240MHz, 8MB Octal PSRAM @ 80MHz, 16MB Flash, SPI DMA), preserve all 24/7 industrial-grade reliability safeguards (Brownout L3, TWDT 10s, Consecutive OOM strike auto-reboot, Relay Safe Boot, Sockets 32, OTA rollback), and establish a decoupled `telemetry_hub` observer for future ThingsBoard / VPS MQTT integration.

**Architecture:** A clean ESP-IDF v5 modular project structure in `firmware/`. Tasks are pinned across dual cores: Core 0 handles networking (Wi-Fi, HTTP Server, SSE, DNS) and flash operations, while Core 1 handles real-time pump timing, 1-Wire DS18B20 temperature reads, cooling relay protection, and ST7789 TFT display updates via SPI DMA. A lightweight publish-subscribe `telemetry_hub` coordinates state broadcasts.

**Tech Stack:** ESP-IDF v5.x, C99, FreeRTOS, ESP32-S3 Xtensa LX7, Octal PSRAM (OPI), ST7789 SPI DMA display driver, LwIP, embedded HTTP server with gzip compression.

## Global Constraints

- Target Chip: ESP32-S3 exclusively (`CONFIG_IDF_TARGET="esp32s3"`)
- Module Target: ESP32-S3-WROOM-1-N16R8 (16 MB Flash, 8 MB Octal PSRAM)
- No ESP32 Classic logic: Zero `#ifdef CONFIG_IDF_TARGET_ESP32` or classic pin mappings
- Flash Layout: 16 MB custom partition table with dual 3.5 MB OTA slots
- Performance: CPU @ 240 MHz, Octal PSRAM @ 80 MHz, Compiler Optimization `-O2`
- 24/7 Safeguards: Brownout Level 3 (~2.80V), TWDT 10s panic on CPU0+CPU1, OOM strike auto-reboot after 3 consecutive failures (<10KB free heap or <5KB largest block) with NVS restart detail, Relay Safe Boot, Sockets 32, OTA rollback

---

### Task 1: Project Scaffolding, 16MB Partitions, and Merged S3 `sdkconfig.defaults`

**Files:**
- Create: `firmware/partitions.csv`
- Modify: `firmware/CMakeLists.txt`
- Modify: `firmware/sdkconfig.defaults`
- Modify: `firmware/version.txt`

**Interfaces:**
- Consumes: ESP-IDF build system environment (`$ENV{IDF_PATH}`)
- Produces: `partitions.csv` (16MB layout), `sdkconfig.defaults` (S3 240MHz, 8MB OPI PSRAM, Brownout L3, TWDT, Canary, Sockets 32)

- [ ] **Step 1: Write `firmware/partitions.csv` with 16MB layout**

Write the custom partition table allocating 3.5 MB for each OTA slot, along with NVS, PHY, NVS keys, and coredump partitions:

```csv
# Name,      Type, SubType,  Offset,    Size,      Flags
nvs,         data, nvs,      0x11000,   0x6000,
otadata,     data, ota,      0x17000,   0x2000,
phy_init,    data, phy,      0x19000,   0x1000,
ota_0,       app,  ota_0,    0x20000,   0x380000,
ota_1,       app,  ota_1,    0x3A0000,  0x380000,
nvs_keys,    data, nvs_keys, 0x720000,  0x1000,
coredump,    data, coredump, 0x721000,  0x40000,
```

- [ ] **Step 2: Update `firmware/CMakeLists.txt`**

Set the project name to `fishpump_firmware` and configure extra component directories:

```cmake
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fishpump_firmware VERSION 0.3.2)

add_compile_definitions(APP_TEMPLATE_FIRMWARE_VERSION="v${PROJECT_VERSION}")
```

- [ ] **Step 3: Update `firmware/sdkconfig.defaults` with full S3 tuning and 24/7 reliability flags**

Merge all chip-agnostic and S3-specific parameters into a single unified file:

```ini
CONFIG_IDF_TARGET="esp32s3"

# 16 MB Flash Configuration
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHFREQ="80m"

# CPU Clock: 240 MHz Max Performance
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240

# Custom Partition Table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_OFFSET=0x10000

# 8 MB Octal PSRAM (OPI) Configuration
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_CAPS_ALLOC=y

# 24/7 Reliability: Hardware Brownout Detector Level 3 (~2.80V)
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_BROWNOUT_DET_LVL_SEL_3=y

# 24/7 Reliability: Task Watchdog Timer (TWDT)
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y

# 24/7 Reliability: Stack Canary Check & Overflow Protection
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_FREERTOS_HZ=1000

# 24/7 Reliability: Network Sockets Headroom
CONFIG_LWIP_MAX_SOCKETS=32
CONFIG_LWIP_MAX_ACTIVE_TCP=32

# 24/7 Reliability: OTA Rollback & Coredump
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y

# Performance Optimization: -O2
CONFIG_COMPILER_OPTIMIZATION_PERF=y

# Console Output: Default UART0
CONFIG_ESP_CONSOLE_UART_DEFAULT=y

# Task Stack Sizing
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4608
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096

# Wi-Fi Task Core Pinning
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y

# Logging
CONFIG_LOG_DEFAULT_LEVEL_WARN=y
CONFIG_LOG_DEFAULT_LEVEL=2
CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=n
CONFIG_LOG_MAXIMUM_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL=3
```

- [ ] **Step 4: Update `firmware/version.txt`**

Write `0.3.2` into `firmware/version.txt`.

- [ ] **Step 5: Verify partition table syntax**

Run: `pwsh -Command "python $env:IDF_PATH/components/partition_table/gen_esp32part.py firmware/partitions.csv"`
Expected: Clean partition table summary printed without syntax errors.

- [ ] **Step 6: Commit Task 1 changes**

```bash
git add firmware/partitions.csv firmware/CMakeLists.txt firmware/sdkconfig.defaults firmware/version.txt
git commit -m "feat(firmware): configure ESP32-S3 16MB partitions and unified sdkconfig"
```

---

### Task 2: Web Asset Pipeline & S3 Build Scripts

**Files:**
- Create: `firmware/scripts/optimize_assets.py`
- Create: `firmware/scripts/build.ps1`

**Interfaces:**
- Consumes: Static web assets in `firmware/main/static/`
- Produces: Minified and gzipped assets in `firmware/main/static_compressed/`
- Command: `.\firmware\scripts\build.ps1` for direct S3 builds

- [ ] **Step 1: Create `firmware/scripts/optimize_assets.py`**

Copy and adapt `scratch/fish-env-control-system/scripts/optimize_assets.py` to `firmware/scripts/optimize_assets.py`. The script reads `index.html`, `style.css`, and `app.js`, strips excess whitespace/comments, and generates corresponding `.gz` files with maximum gzip compression (level 9).

- [ ] **Step 2: Create `firmware/scripts/build.ps1`**

Create a clean, dedicated ESP32-S3 build script:

```powershell
<#
.SYNOPSIS
    Build firmware for ESP32-S3 (DevKitC-1 N16R8).
.PARAMETER FullClean
    Perform a clean rebuild (idf.py fullclean).
#>
param(
    [switch]$FullClean
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FirmwareDir = Split-Path -Parent $ScriptDir

Set-Location $FirmwareDir

if ($FullClean) {
    Write-Host "Performing fullclean..." -ForegroundColor Yellow
    idf.py fullclean
}

Write-Host "Building firmware for ESP32-S3 (WROOM-1-N16R8)..." -ForegroundColor Cyan
idf.py build

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nESP32-S3 Firmware Build Successful!" -ForegroundColor Green
} else {
    Write-Error "Build failed with exit code $LASTEXITCODE"
}
```

- [ ] **Step 3: Test asset optimizer script syntax**

Run: `python firmware/scripts/optimize_assets.py --help` (or run python syntax check `python -m py_compile firmware/scripts/optimize_assets.py`)
Expected: Exit code 0 with no syntax errors.

- [ ] **Step 4: Commit Task 2 changes**

```bash
git add firmware/scripts/optimize_assets.py firmware/scripts/build.ps1
git commit -m "feat(firmware): add asset optimization pipeline and S3 build script"
```

---

### Task 3: Migrate Independent Base Components (NVS, Safe Relay Init, Session, DNS)

**Files:**
- Create: `firmware/components/nvs_store/`
- Create: `firmware/components/relay_safe_init/`
- Create: `firmware/components/session/`
- Create: `firmware/components/dns_server/`

**Interfaces:**
- Consumes: ESP-IDF `nvs_flash`, `esp_system`, `esp_netif`, `driver/gpio`
- Produces:
  - `nvs_store`: Key-value storage for Wi-Fi, pump config, cooling thresholds, restart detail (`nvs_store_set_restart_detail`)
  - `relay_safe_init`: `relay_safe_init_pins()` ensuring zero relay chatter on boot
  - `session`: `session_authenticate()`, token generation for web security
  - `dns_server`: `dns_server_start()` for captive portal interception

- [ ] **Step 1: Copy `nvs_store` component**

Copy from `scratch/fish-env-control-system/components/nvs_store` to `firmware/components/nvs_store`.
Verify `CMakeLists.txt` registers `SRCS "nvs_store.c"` and `INCLUDE_DIRS "include"`.

- [ ] **Step 2: Copy `relay_safe_init` component**

Copy from `scratch/fish-env-control-system/components/relay_safe_init` to `firmware/components/relay_safe_init`.
Verify `CMakeLists.txt` registers `SRCS "relay_safe_init.c"` and `INCLUDE_DIRS "include"`.

- [ ] **Step 3: Copy `session` component**

Copy from `scratch/fish-env-control-system/components/session` to `firmware/components/session`.
Verify `CMakeLists.txt` registers `SRCS "session.c"` and `INCLUDE_DIRS "include"`.

- [ ] **Step 4: Copy `dns_server` component**

Copy from `scratch/fish-env-control-system/components/dns_server` to `firmware/components/dns_server`.
Verify `CMakeLists.txt` registers `SRCS "dns_server.c"` and `INCLUDE_DIRS "include"`.

- [ ] **Step 5: Verify component headers exist and compile without syntax errors**

Run: `pwsh -Command "Test-Path firmware/components/nvs_store/include/nvs_store.h, firmware/components/relay_safe_init/include/relay_safe_init.h, firmware/components/session/include/session.h, firmware/components/dns_server/include/dns_server.h"`
Expected: Output `True` for all paths.

- [ ] **Step 6: Commit Task 3 changes**

```bash
git add firmware/components/nvs_store firmware/components/relay_safe_init firmware/components/session firmware/components/dns_server
git commit -m "feat(firmware): migrate base components (nvs, relay_safe_init, session, dns)"
```

---

### Task 4: Hardware Map Component ESP32 Classic Purge

**Files:**
- Create: `firmware/components/hardware_map/CMakeLists.txt`
- Create: `firmware/components/hardware_map/include/hardware_map.h`
- Create: `firmware/components/hardware_map/hardware_map.c`

**Interfaces:**
- Consumes: ESP-IDF `driver/gpio`, `esp_err.h`
- Produces:
  - `hardware_map_init()`
  - `hardware_map_get_active_pin(hardware_role_t role)`
  - `hardware_map_is_pin_allowed(gpio_num_t pin)` (Allowlist restricted strictly to S3 safe GPIOs: 1, 2, 5, 6, 7, 8, 15, 16, 17, 18)

- [ ] **Step 1: Write `hardware_map.h` without legacy classic definitions**

Create `firmware/components/hardware_map/include/hardware_map.h` declaring the role enums (`HARDWARE_ROLE_FLOAT`, `HARDWARE_ROLE_PUMP1`, `HARDWARE_ROLE_PUMP2`, `HARDWARE_ROLE_TEMP_DATA`, `HARDWARE_ROLE_COOLING`) and API functions.

- [ ] **Step 2: Write `hardware_map.c` purged of all ESP32 Classic logic**

Write `firmware/components/hardware_map/hardware_map.c`:
- Remove `#ifdef CONFIG_IDF_TARGET_ESP32` and `#ifndef CONFIG_IDF_TARGET_ESP32S3` completely.
- Use only the ESP32-S3 pin descriptor table containing GPIO1, 2, 5, 6, 7, 8, 15, 16, 17, 18.
- Default pin mapping:
  - `HARDWARE_ROLE_FLOAT`: GPIO16
  - `HARDWARE_ROLE_PUMP1`: GPIO5
  - `HARDWARE_ROLE_PUMP2`: GPIO6
  - `HARDWARE_ROLE_TEMP_DATA`: GPIO7
  - `HARDWARE_ROLE_COOLING`: GPIO8

- [ ] **Step 3: Write `hardware_map/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "hardware_map.c"
    INCLUDE_DIRS "include"
    REQUIRES driver nvs_store
)
```

- [ ] **Step 4: Verify that no ESP32 Classic references remain in `hardware_map`**

Run: `pwsh -Command "Select-String -Path firmware/components/hardware_map/*.* -Pattern 'CONFIG_IDF_TARGET_ESP32\b'"`
Expected: No matches returned.

- [ ] **Step 5: Commit Task 4 changes**

```bash
git add firmware/components/hardware_map
git commit -m "feat(firmware): implement ESP32-S3 native hardware map with classic code purged"
```

---

### Task 5: Implement Decoupled `telemetry_hub` Component

**Files:**
- Create: `firmware/components/telemetry_hub/CMakeLists.txt`
- Create: `firmware/components/telemetry_hub/include/telemetry_hub.h`
- Create: `firmware/components/telemetry_hub/telemetry_hub.c`

**Interfaces:**
- Consumes: FreeRTOS `semphr.h`
- Produces:
  - `telemetry_hub_init()`
  - `telemetry_hub_subscribe(telemetry_callback_t cb, void *user_ctx)`
  - `telemetry_hub_publish(const telemetry_event_t *event)`
  - `telemetry_hub_get_latest(telemetry_snapshot_t *snapshot)`

- [ ] **Step 1: Write `telemetry_hub.h`**

Define event types and data payloads:

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TELEMETRY_EVT_PUMP,
    TELEMETRY_EVT_FLOAT,
    TELEMETRY_EVT_COOLING,
    TELEMETRY_EVT_HEALTH
} telemetry_event_type_t;

typedef struct {
    uint8_t active_pump;
    bool is_running;
    uint32_t elapsed_sec;
    uint32_t remaining_sec;
    bool relay_1_active;
    bool relay_2_active;
} telemetry_pump_payload_t;

typedef struct {
    bool float_is_high;
} telemetry_float_payload_t;

typedef struct {
    float current_temp_c;
    bool is_sensor_valid;
    bool cooling_relay_active;
    uint32_t lockout_remaining_sec;
} telemetry_cooling_payload_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t free_psram_bytes;
    int8_t wifi_rssi;
    uint32_t uptime_sec;
    uint8_t oom_strikes;
} telemetry_health_payload_t;

typedef struct {
    telemetry_event_type_t type;
    union {
        telemetry_pump_payload_t pump;
        telemetry_float_payload_t float_state;
        telemetry_cooling_payload_t cooling;
        telemetry_health_payload_t health;
    } data;
} telemetry_event_t;

typedef void (*telemetry_callback_t)(const telemetry_event_t *event, void *user_ctx);

esp_err_t telemetry_hub_init(void);
esp_err_t telemetry_hub_subscribe(telemetry_callback_t cb, void *user_ctx);
esp_err_t telemetry_hub_publish(const telemetry_event_t *event);
```

- [ ] **Step 2: Write `telemetry_hub.c`**

Implement thread-safe event publishing protected by a FreeRTOS mutex, with up to 4 registered subscribers (Web SSE, TFT, future ThingsBoard client):

```c
#include "telemetry_hub.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SUBSCRIBERS 4
static const char *TAG = "telemetry_hub";

typedef struct {
    telemetry_callback_t cb;
    void *user_ctx;
} subscriber_entry_t;

static subscriber_entry_t s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t telemetry_hub_init(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    s_subscriber_count = 0;
    return ESP_OK;
}

esp_err_t telemetry_hub_subscribe(telemetry_callback_t cb, void *user_ctx) {
    if (!cb || !s_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_subscriber_count >= MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_subscribers[s_subscriber_count++] = (subscriber_entry_t){ .cb = cb, .user_ctx = user_ctx };
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t telemetry_hub_publish(const telemetry_event_t *event) {
    if (!event || !s_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].cb) {
            s_subscribers[i].cb(event, s_subscribers[i].user_ctx);
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
```

- [ ] **Step 3: Write `telemetry_hub/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "telemetry_hub.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_common
)
```

- [ ] **Step 4: Commit Task 5 changes**

```bash
git add firmware/components/telemetry_hub
git commit -m "feat(firmware): add telemetry_hub observer component for decoupled event dispatch"
```

---

### Task 6: Application Config & Control Components (`app_config`, `pump_control`, `cooling_control`)

**Files:**
- Create: `firmware/components/app_config/`
- Create: `firmware/components/pump_control/`
- Create: `firmware/components/cooling_control/`

**Interfaces:**
- Consumes: `hardware_map`, `nvs_store`, `telemetry_hub`
- Produces:
  - `app_config.h`: Locked board name `"ESP32-S3-DevKitC-1 WROOM-1-N16R8"`, OOM strike thresholds, safe GPIO pool
  - `pump_control`: Real-time dual timer pump state machine running on Core 1, publishing `TELEMETRY_EVT_PUMP` and `TELEMETRY_EVT_FLOAT`
  - `cooling_control`: DS18B20 1-Wire temperature sensor task + relay hysteresis running on Core 1, publishing `TELEMETRY_EVT_COOLING`

- [ ] **Step 1: Write `app_config.h`**

Adapt `scratch/fish-env-control-system/components/app_config/include/app_config.h` strictly for ESP32-S3:
- Set `APP_TEMPLATE_BOARD_NAME "ESP32-S3-DevKitC-1 WROOM-1-N16R8"`
- Set `APP_CONFIG_OOM_MIN_FREE_HEAP_BYTES 10240` (10KB)
- Set `APP_CONFIG_OOM_MIN_LARGEST_BLOCK_BYTES 5120` (5KB)
- Set `APP_CONFIG_OOM_RECOVER_FREE_HEAP_BYTES 15360` (15KB)
- Set `APP_CONFIG_OOM_RECOVER_LARGEST_BLOCK_BYTES 8192` (8KB)
- Set `APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART 3`

- [ ] **Step 2: Migrate `pump_control` and wire telemetry publishing**

Copy `scratch/fish-env-control-system/components/pump_control/` to `firmware/components/pump_control/`.
In `pump_control.c`, whenever the active pump, running phase, or float switch state transitions, construct a `telemetry_event_t` and call `telemetry_hub_publish()`.
Ensure `CMakeLists.txt` includes `telemetry_hub` in `REQUIRES`.

- [ ] **Step 3: Migrate `cooling_control` and wire telemetry publishing**

Copy `scratch/fish-env-control-system/components/cooling_control/` to `firmware/components/cooling_control/`.
In `cooling_control.c`, after reading temperature and evaluating relay status, construct a `telemetry_event_t` with `TELEMETRY_EVT_COOLING` and call `telemetry_hub_publish()`.
Ensure `CMakeLists.txt` includes `telemetry_hub` in `REQUIRES`.

- [ ] **Step 4: Commit Task 6 changes**

```bash
git add firmware/components/app_config firmware/components/pump_control firmware/components/cooling_control
git commit -m "feat(firmware): migrate app_config, pump_control, and cooling_control with telemetry dispatch"
```

---

### Task 7: Display & Wi-Fi Components (`tft_display`, `wifi_manager`)

**Files:**
- Create: `firmware/components/tft_display/`
- Create: `firmware/components/wifi_manager/`

**Interfaces:**
- Consumes: `telemetry_hub`, `hardware_map`, `nvs_store`, `esp_wifi`
- Produces:
  - `tft_display`: ST7789 display controller on Core 1 using SPI2_HOST with DMA, subscribing to `telemetry_hub` for push-based redraw
  - `wifi_manager`: Robust Wi-Fi STA with auto-reconnect + AP fallback running on Core 0

- [ ] **Step 1: Migrate `tft_display` component**

Copy from `scratch/fish-env-control-system/components/tft_display/` to `firmware/components/tft_display/`.
Ensure SPI host is configured for `SPI2_HOST` (FSPI) which does not conflict with S3 Octal PSRAM on `SPI3_HOST`.
Register a subscriber callback with `telemetry_hub_subscribe()` to trigger display screen updates immediately when pump/temperature state changes.

- [ ] **Step 2: Migrate `wifi_manager` component**

Copy from `scratch/fish-env-control-system/components/wifi_manager/` to `firmware/components/wifi_manager/`.
Ensure event loop stack and buffer sizing reflect the 24/7 reliability headroom (4096 stack size).

- [ ] **Step 3: Commit Task 7 changes**

```bash
git add firmware/components/tft_display firmware/components/wifi_manager
git commit -m "feat(firmware): migrate tft_display with SPI DMA and wifi_manager"
```

---

### Task 8: Main Application, Web Server, and 24/7 Reliability Guards

**Files:**
- Create: `firmware/main/CMakeLists.txt`
- Create: `firmware/main/app_main.h`
- Create: `firmware/main/app_main.c`
- Create: `firmware/main/web_server.h`
- Create: `firmware/main/web_server.c`
- Create: `firmware/main/static/` (HTML/CSS/JS web assets & embedded fonts)

**Interfaces:**
- Consumes: All components (`pump_control`, `cooling_control`, `telemetry_hub`, `hardware_map`, `wifi_manager`, `nvs_store`, `relay_safe_init`)
- Produces:
  - `app_main()`: Initializes hardware, safely drives relays, configures TWDT 10s, registers OOM strike loop, and launches Core 0 (Network) and Core 1 (Control) tasks
  - `web_server`: REST API endpoints, captive portal interception, SSE telemetry stream

- [ ] **Step 1: Copy static assets and embedded Thai fonts**

Copy `scratch/fish-env-control-system/main/static/` into `firmware/main/static/`.

- [ ] **Step 2: Write `firmware/main/CMakeLists.txt`**

Configure custom asset compression command (`optimize_assets.py`), embed generated compressed files (`index.html.gz`, `style.css.gz`, `app.js.gz`) and fonts (`.woff2`), and register all component dependencies:

```cmake
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(STATIC_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/static")
    set(STATIC_OUT_DIR "${CMAKE_CURRENT_LIST_DIR}/static_compressed")
    file(MAKE_DIRECTORY "${STATIC_OUT_DIR}")

    add_custom_command(
        OUTPUT
            "${STATIC_OUT_DIR}/index.html"
            "${STATIC_OUT_DIR}/index.html.gz"
            "${STATIC_OUT_DIR}/style.css"
            "${STATIC_OUT_DIR}/style.css.gz"
            "${STATIC_OUT_DIR}/app.js"
            "${STATIC_OUT_DIR}/app.js.gz"
        COMMAND python "${CMAKE_CURRENT_LIST_DIR}/../scripts/optimize_assets.py"
        DEPENDS
            "${STATIC_SRC_DIR}/index.html"
            "${STATIC_SRC_DIR}/style.css"
            "${STATIC_SRC_DIR}/app.js"
            "${CMAKE_CURRENT_LIST_DIR}/../scripts/optimize_assets.py"
        COMMENT "Minifying and compressing static web assets..."
        VERBATIM
    )
    add_custom_target(generate_static_assets
        DEPENDS
            "${STATIC_OUT_DIR}/index.html.gz"
            "${STATIC_OUT_DIR}/style.css.gz"
            "${STATIC_OUT_DIR}/app.js.gz"
    )
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "web_server.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "static_compressed/index.html"
        "static_compressed/index.html.gz"
        "static_compressed/style.css"
        "static_compressed/style.css.gz"
        "static_compressed/app.js"
        "static_compressed/app.js.gz"
        "static/ibm_plex_sans_thai_400_latin.woff2"
        "static/ibm_plex_sans_thai_400_thai.woff2"
        "static/ibm_plex_sans_thai_700_latin.woff2"
        "static/ibm_plex_sans_thai_700_thai.woff2"
        "static/jetbrains_mono_400_latin.woff2"
    REQUIRES
        app_update
        app_config
        cooling_control
        dns_server
        hardware_map
        nvs_flash
        nvs_store
        pump_control
        relay_safe_init
        session
        telemetry_hub
        tft_display
        wifi_manager
        esp_http_server
        esp_wifi
        cjson
        mdns
        mbedtls
        led_strip
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} generate_static_assets)
endif()
```

- [ ] **Step 3: Migrate `web_server.c` and connect SSE to `telemetry_hub`**

Copy `scratch/fish-env-control-system/main/web_server.c` and `web_server.h` to `firmware/main/`.
Subscribe `web_server` SSE handler to `telemetry_hub` so incoming events are pushed directly to connected web clients over Server-Sent Events.

- [ ] **Step 4: Implement `app_main.c` with Dual-Core Pinning, TWDT, and OOM Strike Guard**

Adapt `app_main.c`:
1. Call `relay_safe_init_pins()` before any driver initialization.
2. Initialize NVS and print system banner (ESP32-S3 WROOM-1-N16R8, 240MHz, 16MB Flash, 8MB PSRAM).
3. Initialize `telemetry_hub`.
4. Configure Task Watchdog (TWDT) for 10 seconds.
5. Create control tasks pinned to **Core 1**:
   - `pump_control_start_task()` -> pinned to Core 1
   - `cooling_control_start_task()` -> pinned to Core 1
   - `tft_display_start_task()` -> pinned to Core 1
6. Create network tasks pinned to **Core 0**:
   - `wifi_manager_init()` / `start()` -> Core 0
   - `web_server_start()` -> Core 0
   - `dns_server_start()` -> Core 0
7. In the system health monitor loop:
   - Check `free_heap` and `largest_block`.
   - If below threshold (`< 10KB` or `< 5KB`), increment `oom_strike_count`.
   - If `oom_strike_count >= 3`, log warning, save `nvs_store_set_restart_detail("OOM")`, and invoke `esp_restart()`.

- [ ] **Step 5: Commit Task 8 changes**

```bash
git add firmware/main
git commit -m "feat(firmware): implement app_main with dual-core pinning, 24/7 OOM guard, and web server"
```

---

### Task 9: Full ESP-IDF Build Verification & Artifact Validation

**Files:**
- Read: `firmware/build/` artifacts

**Interfaces:**
- Consumes: Complete `firmware/` codebase
- Produces: Compiled `fishpump_firmware.bin`, `partitions.bin`, `bootloader.bin`

- [ ] **Step 1: Execute clean ESP-IDF build**

Run: `pwsh -Command "idf.py -C firmware fullclean; idf.py -C firmware build"`
Expected: Build finishes with exit code 0 and reports `Project build complete`.

- [ ] **Step 2: Validate binary outputs**

Run: `pwsh -Command "Test-Path firmware/build/fishpump_firmware.bin, firmware/build/partitions.bin, firmware/build/bootloader/bootloader.bin"`
Expected: All output `True`.

- [ ] **Step 3: Verify binary flash size and partition alignment**

Run: `pwsh -Command "idf.py -C firmware size"`
Expected: Prints memory footprint showing DRAM and Flash code usage comfortably within the 16MB / 8MB limits.

- [ ] **Step 4: Commit build verification notes and clean up any untracked files**

```bash
git status
git commit --allow-empty -m "chore(firmware): verify clean ESP32-S3 build passing with 24/7 reliability safeguards"
```
