# Archiving and Decoupling TFT Display System Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create a permanent Git snapshot of the TFT display subsystem, completely decouple and remove all TFT code and configurations from firmware and web UI, and verify clean compilation.

**Architecture:** Use Git tag and branch for permanent archival snapshot before removing files. Decouple components systematically across build configuration, application runtime, web server routes, persistent NVS storage, and web UI dashboard. Rebuild and verify zero residual references.

**Tech Stack:** ESP-IDF (C / FreeRTOS / ESP32-S3), CMake, HTML/CSS/Vanilla JavaScript, Python (asset optimization script).

## Global Constraints

- Never commit broken intermediate states that fail compilation.
- Ensure Git snapshot tag and branch are created before any removal.
- Strictly eliminate all residual `#include "tft_display.h"` and references to `tft` across `firmware/`.
- Maintain all existing pump control, cooling control, telemetry hub, and Wi-Fi features intact.
- Follow conventional commits in English.

---

### Task 1: Create Git Archival Snapshot

**Files:**
- Repository Git state

**Interfaces:**
- Consumes: Current clean HEAD commit on `firmware/esp32s3-dev`
- Produces: Tag `archive/tft-system-snapshot` and Branch `archive/tft-system`

- [ ] **Step 1: Create annotated Git tag**

```bash
git tag -a archive/tft-system-snapshot -m "Archive TFT ST7789 display system before removal"
```

- [ ] **Step 2: Create dedicated archive branch**

```bash
git branch archive/tft-system
```

- [ ] **Step 3: Verify tag and branch exist**

Run: `git tag -l "archive/*"` and `git branch -l "archive/*"`
Expected: Tag `archive/tft-system-snapshot` and branch `archive/tft-system` are displayed.

---

### Task 2: Component & Build System Decoupling

**Files:**
- Delete: `firmware/components/tft_display/`
- Modify: `firmware/main/CMakeLists.txt:61`

**Interfaces:**
- Consumes: None
- Produces: Build system without `tft_display` component dependency

- [ ] **Step 1: Remove `firmware/components/tft_display` directory**

Delete directory `firmware/components/tft_display/` and all its files (`tft_display.c`, `tft_display.h`, `include/tft_display.h`, `font8x16.h`, `CMakeLists.txt`).

- [ ] **Step 2: Remove `tft_display` from `firmware/main/CMakeLists.txt`**

Remove line 61 (`tft_display`) from the `REQUIRES` block of `idf_component_register`.

- [ ] **Step 3: Verify build directory CMake cache update**

Run: `cmake -B build` or check CMake status.

---

### Task 3: Hardware Configuration & Pin Pool Cleanup

**Files:**
- Modify: `firmware/components/app_config/include/app_config.h:97-129`
- Modify: `firmware/components/hardware_map/hardware_map.c:21`

**Interfaces:**
- Consumes: None
- Produces: Clean configuration header without TFT pin definitions

- [ ] **Step 1: Remove TFT configuration block in `app_config.h`**

In `firmware/components/app_config/include/app_config.h`, delete lines 97–100 (`APP_CONFIG_TFT_ENABLED`) and lines 107–129 (all `APP_TEMPLATE_TFT_*` definitions).

- [ ] **Step 2: Update hardware map documentation in `hardware_map.c`**

In `firmware/components/hardware_map/hardware_map.c`, update line 21 from:
```c
 * - GPIO 9-14: TFT wiring
```
to:
```c
 * - GPIO 9-14: Unassigned (formerly TFT wiring, archived)
```

- [ ] **Step 3: Verify header consistency**

Ensure no other header files depend on `APP_TEMPLATE_TFT_*` macros.

---

### Task 4: Core Firmware Application Decoupling

**Files:**
- Modify: `firmware/main/app_main.c`

**Interfaces:**
- Consumes: FreeRTOS and driver headers
- Produces: `app_main.c` without display initialization or background task

- [ ] **Step 1: Remove header inclusion and log level**

In `firmware/main/app_main.c`:
- Remove `#include "tft_display.h"` (line 24)
- Remove `esp_log_level_set("TFT_DISPLAY", ESP_LOG_INFO);` (line 745)

- [ ] **Step 2: Remove button wake call**

In `firmware/main/app_main.c`, remove `tft_display_reset_idle_timer();` (lines 655–656) from the button loop task.

- [ ] **Step 3: Remove TFT initialization and boot splash**

In `firmware/main/app_main.c`, remove the entire TFT display initialization block (lines 820–833):
```c
  /* Initialize TFT display */
  if (tft_display_init() == ESP_OK) {
    ...
  }
```

- [ ] **Step 4: Remove background task startup**

In `firmware/main/app_main.c`, remove lines 1134–1135:
```c
  /* Start background TFT status update task */
  tft_display_start_task();
```

---

### Task 5: Web Server API & Route Decoupling

**Files:**
- Modify: `firmware/main/web_server.c`

**Interfaces:**
- Consumes: `httpd_req_t`
- Produces: Cleaned web server without display wake/config routes

- [ ] **Step 1: Remove header inclusion**

In `firmware/main/web_server.c`, remove line 29: `#include "tft_display.h"`.

- [ ] **Step 2: Remove TFT locked pin reporting in `get_locked_pins`**

In `firmware/main/web_server.c`, remove lines 870–877:
```c
  cJSON_AddNumberToObject(locked, "tft_cs", (double)APP_TEMPLATE_TFT_CS_GPIO);
  cJSON_AddNumberToObject(locked, "tft_rst",
                          (double)APP_TEMPLATE_TFT_RESET_GPIO);
  cJSON_AddNumberToObject(locked, "tft_dc", (double)APP_TEMPLATE_TFT_DC_GPIO);
  cJSON_AddNumberToObject(locked, "tft_mosi",
                          (double)APP_TEMPLATE_TFT_MOSI_GPIO);
  cJSON_AddNumberToObject(locked, "tft_sck", (double)APP_TEMPLATE_TFT_SCK_GPIO);
  cJSON_AddNumberToObject(locked, "tft_led", (double)APP_TEMPLATE_TFT_LED_GPIO);
```

- [ ] **Step 3: Remove display handler functions**

In `firmware/main/web_server.c`, remove lines 4691–4780 (`handle_display_wake`, `handle_api_display_config_get`, and `handle_api_display_config_post`).

- [ ] **Step 4: Remove route entries from route table**

In `firmware/main/web_server.c`, remove routes for `/api/display/wake` and `/api/display/config` (lines 5054–5062).

- [ ] **Step 5: Remove idle timer reset from request hook**

In `firmware/main/web_server.c` (`handle_instrumented_route`), remove lines 5090–5095:
```c
  if (route->method == HTTP_POST) {
    /* Do not wake screen for config/preview adjustments */
    if (strcmp(route->uri, "/api/display/config") != 0) {
      tft_display_reset_idle_timer();
    }
  }
```

---

### Task 6: NVS Store Cleanup

**Files:**
- Modify: `firmware/components/nvs_store/include/nvs_store.h:197-198`
- Modify: `firmware/components/nvs_store/nvs_store.c:1660-1685`

**Interfaces:**
- Consumes: None
- Produces: `nvs_store` without unused display settings functions

- [ ] **Step 1: Remove function prototypes from `nvs_store.h`**

Remove declarations:
```c
bool nvs_store_load_display_settings(uint8_t *dim_percent_out);
bool nvs_store_save_display_settings(uint8_t dim_percent);
```

- [ ] **Step 2: Remove function implementations from `nvs_store.c`**

Remove definitions of `nvs_store_load_display_settings` and `nvs_store_save_display_settings`.

---

### Task 7: Web Dashboard Frontend Decoupling & Asset Compression

**Files:**
- Modify: `firmware/main/static/index.html`
- Modify: `firmware/main/static/app.js`
- Modify: `firmware/main/static/style.css`
- Generate: `firmware/main/static_compressed/`

**Interfaces:**
- Consumes: Web static assets
- Produces: Minified/compressed assets without TFT UI

- [ ] **Step 1: Remove TFT sections from `index.html`**

In `firmware/main/static/index.html`, remove:
- Section `<section class="section-card hardware-install" id="tft-wiring">`
- Section Screen Timeout `<section class="section-card" id="screen-timeout">` (or `#tft-dim`)

- [ ] **Step 2: Remove TFT bindings and handlers from `app.js`**

In `firmware/main/static/app.js`, remove:
- Bindings for `locked-tft-*`
- Display dimming / screen timeout load & save functions and event handlers
- Any calls to `/api/display/config` or `/api/display/wake`

- [ ] **Step 3: Remove TFT styles from `style.css`**

In `firmware/main/static/style.css`, remove `.tft-wiring-grid` and associated styling rules.

- [ ] **Step 4: Regenerate compressed web assets**

Run: `python scripts/optimize_assets.py` from `firmware/`.
Expected: `firmware/main/static_compressed/index.html.gz`, `style.css.gz`, and `app.js.gz` updated.

---

### Task 8: Full Build & Cleanliness Verification

**Files:**
- Verification only

**Interfaces:**
- Consumes: Cleaned workspace
- Produces: Passing build and clean git working tree

- [ ] **Step 1: Run firmware build**

Run: `idf.py build`
Expected: Build succeeds with `Project build complete.` and 0 errors.

- [ ] **Step 2: Audit codebase for any remaining TFT references**

Run: `git grep -i "tft" firmware/`
Expected: Zero results returned.

- [ ] **Step 3: Audit codebase for any remaining display handler references**

Run: `git grep -i "handle_display" firmware/` and `git grep -i "tft_display" firmware/`
Expected: Zero results returned.

- [ ] **Step 4: Commit changes**

```bash
git add firmware/
git commit -m "feat(display): archive and decouple TFT ST7789 display subsystem"
```
