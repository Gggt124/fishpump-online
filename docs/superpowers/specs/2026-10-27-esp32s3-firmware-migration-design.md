# Design Specification: ESP32-S3 Native Firmware Migration & Optimization

- **Date:** 2026-10-27
- **Target Chip:** ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz)
- **Target Module:** ESP32-S3-WROOM-1-N16R8 (16 MB Flash + 8 MB Octal PSRAM)
- **Status:** Approved by User, Ready for Implementation Planning

---

## 1. Executive Summary & Goals

This specification defines the migration of the fish tank pump and environment control firmware from `scratch/fish-env-control-system` into the main `firmware/` directory of the `fishpump-online` project.

### Core Objectives:
1. **Target ESP32-S3 Exclusively:** Purge all legacy ESP32 Classic targets, conditional compilation macros, and unused board configs.
2. **Maximize Hardware Performance:** Boost CPU clock to 240 MHz, enable Octal Flash and Octal PSRAM (OPI 80 MHz), configure SPI DMA for display, and optimize compilation with `-O2`.
3. **Dedicated Dual-Core Task Concurrency:** Isolate network and storage I/O on Core 0 while isolating deterministic real-time control, 1-Wire sensor timing, and display rendering on Core 1.
4. **Preserve 24/7 Reliability Systems:** Retain and consolidate hardware brownout protection, dual-core Task Watchdog (TWDT), consecutive OOM strike auto-reboot, Relay Safe Boot, and OTA rollback protection.
5. **Decoupled Telemetry Architecture:** Implement an internal lightweight `telemetry_hub` observer to publish system events (pump state, float level, temperature, health metrics) to current local consumers (Web SSE, TFT) while providing a zero-friction extension point for future ThingsBoard / VPS MQTT integration.

---

## 2. Hardware Profile & Pin Configuration

### 2.1 Hardware Specifications
- **SoC:** ESP32-S3
- **CPU:** Dual-core Xtensa 32-bit LX7, running at 240 MHz
- **Flash:** 16 MB SPI Flash in DIO mode @ 80 MHz
- **PSRAM:** 8 MB Octal SPI (OPI) @ 80 MHz
- **Console / Flashing:** UART0 (via on-board CP2102/CH340 USB-to-UART chip)

### 2.2 Dedicated ESP32-S3 Pin Mapping
All pin assignments are locked to the verified ESP32-S3 DevKitC-1 profile:

| Peripheral / Role | ESP32-S3 Pin | Function / Electrical Notes |
|:---|:---:|:---|
| **Float Switch Input** | GPIO16 | Active-low binary input, internal pull-up + external 4.7kΩ pull-up |
| **Pump Relay 1** | GPIO5 | Channel 1 pump relay control (safe initialized) |
| **Pump Relay 2** | GPIO6 | Channel 2 pump relay control (safe initialized) |
| **DS18B20 One-Wire** | GPIO7 | Digital temperature data line with external 4.7kΩ pull-up |
| **Cooling Relay** | GPIO8 | Compressor / cooling fan relay control |
| **TFT CS** | GPIO10 | ST7789 SPI Chip Select |
| **TFT DC** | GPIO13 | Data / Command selector |
| **TFT RESET** | GPIO14 | LCD hardware reset |
| **TFT MOSI** | GPIO11 | SPI2_HOST (FSPI) MOSI with DMA |
| **TFT SCK** | GPIO12 | SPI2_HOST (FSPI) Clock |
| **TFT Backlight** | GPIO9 | LEDC PWM backlight dimming |
| **Status LED (Board)**| GPIO48 | On-board WS2812 addressable RGB LED |
| **External Button** | GPIO38 | Optional physical push button |
| **External LED** | GPIO39 | Optional front-panel status LED |

### 2.3 ESP32 Classic Purge
- Completely remove `#ifdef CONFIG_IDF_TARGET_ESP32`, `#ifndef CONFIG_IDF_TARGET_ESP32S3`, and any Classic ESP32 GPIO tables from `components/hardware_map/hardware_map.c`.
- Discard `partitions_esp32.csv` and `sdkconfig.defaults.esp32`.
- Remove legacy build target flags (`-Target esp32`) from scripts.

---

## 3. Flash Memory & Partition Map (`partitions.csv`)

The partition table is designed for 16 MB flash with symmetrical dual 3.5 MB OTA slots:

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

- **Bootloader & Partition Table:** Offset `0x10000`.
- **OTA Slots (ota_0, ota_1):** 3.5 MB (0x380000) each, providing ample headroom for firmware growth.
- **NVS Keys & Coredump:** Dedicated partitions for NVS encryption key storage and hardware crash dump analysis.

---

## 4. Performance Tuning & Dual-Core Concurrency

### 4.1 Clock & Memory Strategy
1. **CPU Frequency:** 240 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`).
2. **Octal PSRAM (8 MB OPI):**
   - Configured with `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`.
   - `CONFIG_SPIRAM_USE_CAPS_ALLOC=y`: Internal SRAM is reserved for FreeRTOS task stacks, interrupt handlers, and critical buffers. Large dynamic allocations (TFT framebuffers, compressed web buffers, future TLS/MQTT payloads) use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
3. **Compiler Optimization:** `-O2` for release performance (`CONFIG_COMPILER_OPTIMIZATION_PERF=y`).
4. **TFT SPI DMA:** `SPI2_HOST` utilizing DMA channel allocation to ensure non-blocking display transfers.

### 4.2 Task Affinity & Core Pinning
- **Core 0 (Network, Protocol & System I/O):**
  - Wi-Fi Driver & LwIP stack (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`)
  - Local HTTP Web Server & SSE Streaming task
  - Captive Portal DNS task
  - NVS storage operations & logging
- **Core 1 (Real-Time Control Loop & Display Engine):**
  - **Pump Control Loop:** Evaluates float switch debounce and executes Timer 1 / Timer 2 state transitions.
  - **1-Wire Temperature Reading:** DS18B20 microsecond-sensitive bitbanging / RMT timing.
  - **Cooling Control Loop:** Hysteresis evaluation and compressor lockout countdowns.
  - **TFT Display Task:** Rendering widgets and flushing display buffers via DMA.

---

## 5. 24/7 Reliability & Fault Protection Systems

The system must run continuously 24/7 without manual intervention. All safety features from the baseline firmware are preserved:

### 5.1 Heap Memory Guard & Consecutive OOM Strikes
- **Monitoring Loop:** Periodically checks both `free_heap` and `largest_free_block` to detect memory leaks and heap fragmentation.
- **Critical Thresholds:**
  - Alert condition: `free_heap < 10,240 bytes` (10 KB) OR `largest_free_block < 5,120 bytes` (5 KB).
  - Recovery condition: `free_heap > 15,360 bytes` (15 KB) AND `largest_free_block > 8,192 bytes` (8 KB).
- **OOM Strike Action:**
  - Consecutive failures increment `oom_strike_count`.
  - Upon reaching `APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART = 3`:
    1. Records reason in NVS: `nvs_store_set_restart_detail("OOM")`.
    2. Executes clean software restart (`esp_restart()`) to recover memory rather than allowing unpredictable task failures.

### 5.2 Hardware Brownout Detector
- Configured at Level 3 (~2.80V) via `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_3=y`.
- Prevents flash/SRAM corruption if relay switching creates momentary DC rail dips.

### 5.3 Task Watchdog Timer (TWDT)
- Protects Idle tasks on both Core 0 and Core 1 (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`, `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y`).
- 10-second timeout with panic handler enabled to trigger immediate coredump logging upon task lockup.

### 5.4 Relay Safe Boot Initialization (`relay_safe_init`)
- Drives relay pins to safe inactive states before peripheral drivers register, preventing relay chatter during reboot or power transitions.

### 5.5 Socket Headroom & Rollback
- LwIP configured with 32 sockets and 32 active TCP connections (`CONFIG_LWIP_MAX_SOCKETS=32`, `CONFIG_LWIP_MAX_ACTIVE_TCP=32`).
- Bootloader OTA rollback protection enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

---

## 6. Extensible Telemetry Observer (`telemetry_hub`)

To cleanly support future ThingsBoard / VPS MQTT integration without modifying core pump logic or web handlers, an internal publish-subscribe event hub is established.

### 6.1 Telemetry Data Structures & Event Types
```c
typedef enum {
    TELEMETRY_EVT_PUMP_STATE,
    TELEMETRY_EVT_FLOAT_STATE,
    TELEMETRY_EVT_COOLING_STATE,
    TELEMETRY_EVT_SYSTEM_HEALTH
} telemetry_event_type_t;

typedef struct {
    uint8_t active_pump;        // 1 or 2
    bool is_running;            // true = RUN phase, false = WAIT phase
    uint32_t elapsed_sec;
    uint32_t remaining_sec;
    bool relay_1_active;
    bool relay_2_active;
} telemetry_pump_data_t;

typedef struct {
    float current_temp_c;
    bool is_sensor_valid;
    bool cooling_relay_active;
    uint32_t lockout_remaining_sec;
} telemetry_cooling_data_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t free_psram_bytes;
    int8_t wifi_rssi;
    uint32_t uptime_sec;
    uint8_t oom_strikes;
} telemetry_health_data_t;
```

### 6.2 Observer Pattern & Subscription
- Modules (`pump_control`, `cooling_control`, `app_main`) publish updates to `telemetry_hub`.
- **Registered Subscribers:**
  1. `web_server`: Streams updates via Server-Sent Events (SSE) to the local web UI.
  2. `tft_display`: Triggers immediate screen refresh upon value change.
  3. *(Future Extension)* `thingsboard_client`: Formats JSON payload and publishes to `v1/devices/me/telemetry` over MQTT.

---

## 7. Project Structure in `firmware/`

```text
firmware/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── version.txt
├── components/
│   ├── app_config/
│   ├── cooling_control/
│   ├── dns_server/
│   ├── hardware_map/
│   ├── nvs_store/
│   ├── pump_control/
│   ├── relay_safe_init/
│   ├── session/
│   ├── telemetry_hub/
│   ├── tft_display/
│   └── wifi_manager/
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c
│   ├── app_main.h
│   ├── web_server.c
│   ├── web_server.h
│   └── static/
└── scripts/
    ├── optimize_assets.py
    └── build.ps1
```

---

## 8. Verification & Validation Criteria

1. **Clean ESP-IDF Compilation:**
   - Execute `idf.py -C firmware build`.
   - Must produce `fishpump_firmware.bin` targeted strictly for `esp32s3` with zero compiler warnings in application components.
2. **Memory Map Verification:**
   - Validate partition table matches 16 MB allocation with 3.5 MB OTA slots.
   - Verify Octal PSRAM initialization logs indicate 8 MB enabled.
3. **Dual-Core Pinning Check:**
   - Verify Wi-Fi and Web server run on Core 0, while pump/cooling control tasks are pinned to Core 1.
4. **24/7 Reliability Tests:**
   - Verify TWDT triggers on simulated task hang.
   - Verify brownout detector registers level 3 in startup banner.
   - Verify OOM strike logic triggers restart if heap falls below threshold.
