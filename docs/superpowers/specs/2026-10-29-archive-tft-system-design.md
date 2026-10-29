# Design Specification: Archiving and Decoupling TFT Display System

**Document ID:** `2026-10-29-archive-tft-system-design`  
**Date:** 2026-10-29  
**Status:** APPROVED IN PRINCIPLE  
**Author:** AI Assistant & Pair Programmer  

---

## 1. Context & Objectives

### 1.1 Context
The Fish Pump controller currently includes support for a 320x240 ST7789 SPI TFT display (`components/tft_display`). The display system runs a dedicated FreeRTOS background task pinned to Core 1, subscribes to telemetry events, consumes 6 dedicated GPIO pins (GPIO 9–14), uses SPI2_HOST with DMA, and provides a web UI card and API endpoints for screen brightness and idle dimming.

### 1.2 Objectives
1. **Archive the entire TFT display subsystem:** Create a permanent Git snapshot (annotated tag and dedicated archive branch) so the subsystem can be restored or reused at any time.
2. **Complete Decoupling:** Completely remove the TFT display component, configurations, firmware task, web APIs, and web UI elements from the active development branch (`firmware/esp32s3-dev`).
3. **Resource Optimization:** Free hardware GPIOs (GPIO 9–14), reclaim Core 1 CPU cycles, reduce RAM consumption (FreeRTOS task stack and frame buffers), and minimize Flash binary size.
4. **Zero Residual Coupling:** Ensure no leftover references, dead configurations, or dangling symbols remain in `app_config.h`, `app_main.c`, `web_server.c`, `nvs_store`, or the frontend assets.

---

## 2. Archival & Restoration Strategy (Git)

### 2.1 Pre-Removal Snapshot
Before any files are deleted or modified on the active branch, create:
1. **Annotated Git Tag:**
   ```bash
   git tag -a archive/tft-system-snapshot -m "Archive TFT ST7789 display system before removal"
   ```
2. **Dedicated Archive Branch:**
   ```bash
   git branch archive/tft-system
   ```

### 2.2 Restoration Procedure
If the TFT display feature needs to be inspected or restored in the future:
- **Inspect code:** `git checkout archive/tft-system`
- **Selective restore to active branch:**
  ```bash
  git checkout archive/tft-system -- firmware/components/tft_display
  ```
- **Review historical diff:**
  ```bash
  git diff archive/tft-system-snapshot HEAD
  ```

---

## 3. Firmware Architecture Changes

### 3.1 Component Removal & Build System
- **Delete Directory:** Remove `firmware/components/tft_display/` in its entirety.
- **`firmware/main/CMakeLists.txt`:** Remove `tft_display` from the `REQUIRES` list.

### 3.2 Configuration Cleanup (`components/app_config/include/app_config.h`)
Remove the following macros and configurations:
- Master flag: `APP_CONFIG_TFT_ENABLED`
- Pin mapping definitions:
  - `APP_TEMPLATE_TFT_CS_GPIO` (10)
  - `APP_TEMPLATE_TFT_RESET_GPIO` (14)
  - `APP_TEMPLATE_TFT_DC_GPIO` (13)
  - `APP_TEMPLATE_TFT_MOSI_GPIO` (11)
  - `APP_TEMPLATE_TFT_SCK_GPIO` (12)
  - `APP_TEMPLATE_TFT_LED_GPIO` (9)
- Hardware parameters:
  - `APP_TEMPLATE_TFT_PANEL_ST7789`
  - `APP_TEMPLATE_TFT_RGB_ELE_ORDER`
  - `APP_TEMPLATE_TFT_INVERT_COLOR`
  - `APP_TEMPLATE_TFT_SPI_CLK_HZ`

### 3.3 Hardware Mapping Documentation (`components/hardware_map/hardware_map.c`)
- Update header comment at line 21 (`- GPIO 9-14: TFT wiring`) to reflect that TFT has been archived and GPIO 9-14 are freed from active driver reservation.

### 3.4 Application Lifecycle (`firmware/main/app_main.c`)
- Remove `#include "tft_display.h"`.
- Remove `tft_display_reset_idle_timer()` call from physical button loop (lines 655–656).
- Remove `esp_log_level_set("TFT_DISPLAY", ESP_LOG_INFO)` (line 745).
- Remove initialization block (lines 820–833):
  - `tft_display_init()`
  - `tft_display_get_idle_dim_percent()` / `tft_display_set_idle_dim_percent()`
  - Initial boot screen drawing (`tft_clear()`, `tft_draw_string()`, etc.)
- Remove background task invocation:
  - `tft_display_start_task()` (lines 1134–1135).

### 3.5 Web Server & API (`firmware/main/web_server.c`)
- Remove `#include "tft_display.h"`.
- Remove locked pin reporting for TFT (`tft_cs`, `tft_rst`, `tft_dc`, `tft_mosi`, `tft_sck`, `tft_led`) in `get_locked_pins()`.
- Remove handlers:
  - `handle_display_wake()`
  - `handle_api_display_config_get()`
  - `handle_api_display_config_post()`
- Remove route table registrations for:
  - `POST /api/display/wake`
  - `GET /api/display/config`
  - `POST /api/display/config`
- Remove `tft_display_reset_idle_timer()` call in HTTP request instrumentation (`handle_instrumented_route`, line 5093).

### 3.6 NVS Store Cleanup (`components/nvs_store`)
- Remove display settings API signatures from `components/nvs_store/include/nvs_store.h`:
  - `nvs_store_load_display_settings()`
  - `nvs_store_save_display_settings()`
- Remove their implementations from `components/nvs_store/nvs_store.c`.

---

## 4. Web Dashboard & Frontend Changes

### 4.1 HTML Templates (`firmware/main/static/index.html`)
- Remove TFT Hardware Wiring section (`<section class="section-card hardware-install" id="tft-wiring">`).
- Remove Screen Timeout / TFT Dimming setting section (`<section ... id="screen-timeout">` / `#tft-dim`).

### 4.2 Frontend Scripts (`firmware/main/static/app.js`)
- Remove references and DOM bindings for `locked-tft-*` (`locked-tft-sck`, `locked-tft-mosi`, `locked-tft-cs`, `locked-tft-dc`, `locked-tft-rst`, `locked-tft-led`).
- Remove screen timeout / display dimming config loaders, event handlers, and API calls to `/api/display/config` and `/api/display/wake`.

### 4.3 Stylesheets (`firmware/main/static/style.css`)
- Remove CSS definitions for `.tft-wiring-grid` and display dim controls.

### 4.4 Static Asset Compression
- Re-run `python scripts/optimize_assets.py` to regenerate `index.html.gz`, `style.css.gz`, and `app.js.gz` in `firmware/main/static_compressed/`.

---

## 5. Verification & Acceptance Criteria

1. **Git Verification:**
   - Tag `archive/tft-system-snapshot` exists and points to the commit immediately before removal.
   - Branch `archive/tft-system` exists and tracks the snapshot state.
2. **Build Cleanliness:**
   - `idf.py build` succeeds with zero errors and no unresolved symbols.
   - No warnings related to missing `tft_display` or `display` functions.
3. **Repository Tree Cleanliness:**
   - `git grep -i "tft" firmware/` returns zero hits in source code, headers, and build scripts.
   - `git grep -i "display" firmware/` returns zero hits for TFT display functions.
4. **Web UI Verification:**
   - Web application serves successfully without console JavaScript errors or 404s for removed display endpoints.
