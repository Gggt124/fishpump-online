# Single Source of Truth for Pump NVS Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate redundant double-layer NVS persistence in pump configuration by making `nvs_store` the single source of truth for Flash storage and converting `pump_control` into a pure in-memory hardware controller.

**Architecture:** Extend `nvs_store_pump_settings_t` to include Wash timer fields under atomic Blob v3 with full backward compatibility. Expand `pump_control_init_context_t` to receive complete timer configurations at boot, strip all direct NVS dependencies from `pump_control.c`, and update `web_server.c` to persist solely through `nvs_store`.

**Tech Stack:** ESP-IDF v6.0 (C99), FreeRTOS, ESP32-S3, NVS Flash, cJSON.

## Global Constraints

- Must compile cleanly with ESP-IDF v6.0 using `$env:IDF_TOOLS_PATH = "C:\Espressif"; & "C:\esp\v6.0.1\esp-idf\export.ps1"; idf.py build`.
- 100% backward compatibility for existing NVS data: devices booting with Blob v1, Blob v2, or legacy individual keys must upgrade cleanly without losing water or wash configurations.
- REST API compatibility: `/api/pump/config` JSON contract (both GET and POST) must preserve all existing field names (`water_set1_on`, `water_set2_on`, `wash_on`, `wash_off`, `wash_start_phase`, `wash_auto_start`, and legacy aliases).
- Maintain documentation integrity and English comments/docstrings in source code.

---

### Task 1: Extend `nvs_store` with Wash Timer Settings & Blob v3 Migration

**Files:**
- Modify: `components/nvs_store/include/nvs_store.h:22-44`
- Modify: `components/nvs_store/nvs_store.c:40-120, 795-1035`

**Interfaces:**
- Consumes: `hardware_map_timer_start_phase_t` from `hardware_map.h`, duration validators from `app_config.h`.
- Produces:
  - Extended `nvs_store_pump_settings_t` containing `wash_on_sec`, `wash_off_sec`, `wash_start_phase`, `wash_auto_start`.
  - Updated `nvs_store_pump_settings_defaults()` populating default wash parameters.
  - Blob v3 serialization and deserialization with backward compatibility for v1/v2 and legacy keys.

- [ ] **Step 1: Update `nvs_store.h` struct definition**

In `components/nvs_store/include/nvs_store.h`, add wash timer fields to `nvs_store_pump_settings_t`:

```c
typedef struct {
  uint32_t timer1_on_sec;
  uint32_t timer1_off_sec;
  uint32_t timer2_on_sec;
  uint32_t timer2_off_sec;
  bool relay_active_low;
  bool relay1_active_low;
  bool relay2_active_low;
  hardware_timer_start_phase_t timer1_start_phase;
  hardware_timer_start_phase_t timer2_start_phase;
  bool auto_start;
  uint32_t min_dwell_sec;

  /* Wash system configuration (Blob v3) */
  uint32_t wash_on_sec;
  uint32_t wash_off_sec;
  hardware_timer_start_phase_t wash_start_phase;
  bool wash_auto_start;
} nvs_store_pump_settings_t;
```

- [ ] **Step 2: Update blob version and validation in `nvs_store.c`**

1. Change `#define NVS_PUMP_BLOB_VERSION 2` to `#define NVS_PUMP_BLOB_VERSION 3`.
2. Define legacy wash keys for migration fallback:
   ```c
   #define NVS_PUMP_KEY_WASH_ON "wash_on"
   #define NVS_PUMP_KEY_WASH_OFF "wash_off"
   #define NVS_PUMP_KEY_WASH_START "wash_start"
   #define NVS_PUMP_KEY_WASH_AUTOSTART "wash_autostart"
   ```
3. Update `pump_blob_boolean_fields_valid()` to include `wash_auto_start`:
   ```c
   static bool pump_blob_boolean_fields_valid(const uint8_t *payload,
                                              size_t payload_len) {
     return pump_blob_bool_field_valid(
                payload, payload_len,
                offsetof(nvs_store_pump_settings_t, relay_active_low)) &&
            pump_blob_bool_field_valid(
                payload, payload_len,
                offsetof(nvs_store_pump_settings_t, relay1_active_low)) &&
            pump_blob_bool_field_valid(
                payload, payload_len,
                offsetof(nvs_store_pump_settings_t, relay2_active_low)) &&
            pump_blob_bool_field_valid(
                payload, payload_len,
                offsetof(nvs_store_pump_settings_t, auto_start)) &&
            pump_blob_bool_field_valid(
                payload, payload_len,
                offsetof(nvs_store_pump_settings_t, wash_auto_start));
   }
   ```
4. Update `pump_settings_valid()`:
   ```c
   static bool pump_settings_valid(const nvs_store_pump_settings_t *settings) {
     return settings && pump_bool_raw_valid((uint8_t)settings->relay_active_low) &&
            pump_bool_raw_valid((uint8_t)settings->relay1_active_low) &&
            pump_bool_raw_valid((uint8_t)settings->relay2_active_low) &&
            pump_bool_raw_valid((uint8_t)settings->auto_start) &&
            pump_bool_raw_valid((uint8_t)settings->wash_auto_start) &&
            pump_duration_valid(settings->timer1_on_sec) &&
            pump_duration_valid(settings->timer1_off_sec) &&
            pump_duration_valid(settings->timer2_on_sec) &&
            pump_duration_valid(settings->timer2_off_sec) &&
            pump_duration_valid(settings->wash_on_sec) &&
            pump_duration_valid(settings->wash_off_sec) &&
            hardware_map_timer_start_phase_valid(settings->timer1_start_phase) &&
            hardware_map_timer_start_phase_valid(settings->timer2_start_phase) &&
            hardware_map_timer_start_phase_valid(settings->wash_start_phase) &&
            settings->min_dwell_sec <= APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC;
   }
   ```

- [ ] **Step 3: Update defaults, loading, and cleanup in `nvs_store.c`**

1. In `nvs_store_pump_settings_defaults()`:
   ```c
   out->wash_on_sec = 10;
   out->wash_off_sec = 60;
   out->wash_start_phase = HARDWARE_TIMER_START_PHASE_ON;
   out->wash_auto_start = false;
   ```
2. In `nvs_store_load_pump_settings()`:
   - Accept blob versions: `blob[0] == NVS_PUMP_BLOB_VERSION || blob[0] == 2 || blob[0] == 1`.
   - In legacy key fallback (`blob_err == ESP_ERR_NVS_NOT_FOUND`):
     Attempt to read legacy wash keys if they exist:
     ```c
     uint32_t wash_on = loaded.wash_on_sec;
     uint32_t wash_off = loaded.wash_off_sec;
     uint8_t wash_start = (uint8_t)loaded.wash_start_phase;
     bool wash_autostart = loaded.wash_auto_start;

     if (load_u32_key(handle, NVS_PUMP_KEY_WASH_ON, &wash_on) == ESP_OK && pump_duration_valid(wash_on)) {
       loaded.wash_on_sec = wash_on;
     }
     if (load_u32_key(handle, NVS_PUMP_KEY_WASH_OFF, &wash_off) == ESP_OK && pump_duration_valid(wash_off)) {
       loaded.wash_off_sec = wash_off;
     }
     if (load_u8_key(handle, NVS_PUMP_KEY_WASH_START, &wash_start) == ESP_OK && hardware_map_timer_start_phase_valid((hardware_timer_start_phase_t)wash_start)) {
       loaded.wash_start_phase = (hardware_timer_start_phase_t)wash_start;
     }
     if (load_bool_key(handle, NVS_PUMP_KEY_WASH_AUTOSTART, &wash_autostart) == ESP_OK) {
       loaded.wash_auto_start = wash_autostart;
     }
     ```
3. In `nvs_store_clear_pump_settings()`:
   Also erase legacy individual keys (`NVS_PUMP_KEY_WASH_ON`, `NVS_PUMP_KEY_WASH_OFF`, `NVS_PUMP_KEY_WASH_START`, `NVS_PUMP_KEY_WASH_AUTOSTART`, `"w_set1_on"`, `"w_set1_off"`, `"w_set1_start"`, `"w_set2_on"`, `"w_set2_off"`, `"w_set2_start"`, `"w_autostart"`, `"rel1_pol"`, `"rel2_pol"`, `"flt_gpio"`, `"rel1_gpio"`, `"rel2_gpio"`).

- [ ] **Step 4: Verify syntax and compile check**

Run: `$env:IDF_TOOLS_PATH = "C:\Espressif"; & "C:\esp\v6.0.1\esp-idf\export.ps1"; idf.py reconfigure`
Expected: CMake reconfigure succeeds.

---

### Task 2: Refactor `pump_control` to be a Pure In-Memory Controller

**Files:**
- Modify: `components/pump_control/include/pump_control.h:80-95`
- Modify: `components/pump_control/pump_control.c:1-50, 360-500, 620-685, 1020-1045`

**Interfaces:**
- Consumes: Full initialization context via `pump_control_init_context_t`, runtime updates via `pump_control_config_update_t`.
- Produces: `pump_control_init()`, `pump_control_update_config()`, `pump_control_get_status()` completely decoupled from NVS.

- [ ] **Step 1: Extend `pump_control_init_context_t` in `pump_control.h`**

In `components/pump_control/include/pump_control.h`:
```c
typedef struct {
  gpio_num_t float_gpio;
  gpio_num_t relay1_gpio;
  gpio_num_t relay2_gpio;
  bool relay1_active_low;
  bool relay2_active_low;
  bool allow_auto_start;
  bool allow_runtime_control;
  uint32_t min_dwell_sec;
  pump_control_timer_set_config_t water_set1;
  pump_control_timer_set_config_t water_set2;
  pump_control_timer_set_config_t wash_timer;
  bool water_auto_start;
  bool wash_auto_start;
} pump_control_init_context_t;
```

- [ ] **Step 2: Remove NVS headers and definitions from `pump_control.c`**

1. Remove `#include "nvs.h"` and `#include "nvs_flash.h"`.
2. Remove `#define NVS_PUMP_NAMESPACE "pump_cfg"` and lines 21–36 (`NVS_KEY_*` defines).
3. Delete `load_nvs_config()` (lines 365–459) and `save_nvs_config()` (lines 461–497).

- [ ] **Step 3: Update `init_context_valid()` and `pump_control_init()` in `pump_control.c`**

1. In `init_context_valid()`:
   Add validation for timer configs:
   ```c
   if (!timer_set_config_valid(&context->water_set1) ||
       !timer_set_config_valid(&context->water_set2) ||
       !timer_set_config_valid(&context->wash_timer)) {
     return false;
   }
   ```
2. In `pump_control_init()`:
   Remove `load_nvs_config()`.
   Initialize `s_config` directly from `context`:
   ```c
   s_config.water_set1 = context->water_set1;
   s_config.water_set2 = context->water_set2;
   s_config.wash_timer = context->wash_timer;
   s_config.float_gpio = context->float_gpio;
   s_config.relay1_gpio = context->relay1_gpio;
   s_config.relay2_gpio = context->relay2_gpio;
   s_config.relay1_active_low = context->relay1_active_low;
   s_config.relay2_active_low = context->relay2_active_low;
   s_config.min_dwell_sec = context->min_dwell_sec;
   s_runtime_enabled = context->allow_runtime_control;

   bool auto_start_permitted = context->allow_auto_start && s_runtime_enabled;
   s_config.water_auto_start = auto_start_permitted && context->water_auto_start;
   s_config.wash_auto_start = auto_start_permitted && context->wash_auto_start;
   ```

- [ ] **Step 4: Update `pump_control_update_config()` in `pump_control.c`**

Update `pump_control_update_config()` to only update RAM without NVS:
```c
bool pump_control_update_config(const pump_control_config_update_t *update) {
  if (!config_update_valid(update)) {
    ESP_LOGE(TAG, "Invalid config update parameters");
    return false;
  }
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  s_config.water_set1 = update->water_set1;
  s_config.water_set2 = update->water_set2;
  s_config.wash_timer = update->wash_timer;
  s_config.water_auto_start = update->water_auto_start;
  s_config.wash_auto_start = update->wash_auto_start;
  s_config.min_dwell_sec = update->min_dwell_sec;

  xSemaphoreGive(s_pump_mutex);
  return true;
}
```

---

### Task 3: Update `app_main.c` Initialization Wiring

**Files:**
- Modify: `firmware/main/app_main.c:1000-1020`

**Interfaces:**
- Consumes: `pump_settings` loaded from `nvs_store_load_pump_settings()`.
- Produces: Complete `pump_control_init_context_t` populated with timer and auto-start configurations passed to `pump_control_start_task()`.

- [ ] **Step 1: Wire full pump configuration in `app_main()`**

In `firmware/main/app_main.c`, populate the new timer fields in `pump_init_context`:
```c
  pump_control_init_context_t pump_init_context = {
      .float_gpio = active_hardware_map.float_input_gpio,
      .relay1_gpio = active_hardware_map.pump_relay1_gpio,
      .relay2_gpio = active_hardware_map.pump_relay2_gpio,
      .relay1_active_low = pump_settings.relay1_active_low,
      .relay2_active_low = pump_settings.relay2_active_low,
      .allow_auto_start = allow_auto_start,
      .allow_runtime_control =
          hardware_map_safe && hardware_map_promotion_ok,
      .min_dwell_sec = pump_settings.min_dwell_sec,
      .water_set1 =
          {
              .on_sec = pump_settings.timer1_on_sec,
              .off_sec = pump_settings.timer1_off_sec,
              .start_phase = (pump_settings.timer1_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_set2 =
          {
              .on_sec = pump_settings.timer2_on_sec,
              .off_sec = pump_settings.timer2_off_sec,
              .start_phase = (pump_settings.timer2_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .wash_timer =
          {
              .on_sec = pump_settings.wash_on_sec,
              .off_sec = pump_settings.wash_off_sec,
              .start_phase = (pump_settings.wash_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_auto_start = pump_settings.auto_start,
      .wash_auto_start = pump_settings.wash_auto_start,
  };
```

---

### Task 4: Clean up `web_server.c` Direct NVS Reads & Route via `nvs_store`

**Files:**
- Modify: `firmware/main/web_server.c:1015-1055, 2050-2145, 3770-3875`

**Interfaces:**
- Consumes: Extended `nvs_store_pump_settings_t` for both reading and writing pump configuration.
- Produces: Clean REST handlers for `/api/pump/config` (GET and POST) with zero direct NVS operations.

- [ ] **Step 1: Clean up `api_pump_add_config_fields()` in `web_server.c`**

Delete lines 1015–1029 (the `nvs_open("pump_cfg", ...)` block).
Use `settings->wash_on_sec`, `settings->wash_off_sec`, `settings->wash_start_phase`, `settings->wash_auto_start` directly:
```c
  cJSON_AddNumberToObject(root, "water_set1_on", settings->timer1_on_sec);
  cJSON_AddNumberToObject(root, "water_set1_off", settings->timer1_off_sec);
  cJSON_AddStringToObject(
      root, "water_set1_start_phase",
      hardware_map_timer_start_phase_name(settings->timer1_start_phase));
  cJSON_AddNumberToObject(root, "water_set2_on", settings->timer2_on_sec);
  cJSON_AddNumberToObject(root, "water_set2_off", settings->timer2_off_sec);
  cJSON_AddStringToObject(
      root, "water_set2_start_phase",
      hardware_map_timer_start_phase_name(settings->timer2_start_phase));
  cJSON_AddNumberToObject(root, "wash_on", settings->wash_on_sec);
  cJSON_AddNumberToObject(root, "wash_off", settings->wash_off_sec);
  cJSON_AddStringToObject(
      root, "wash_start_phase",
      settings->wash_start_phase == HARDWARE_TIMER_START_PHASE_ON ? "ON" : "OFF");
  cJSON_AddBoolToObject(root, "water_auto_start", settings->auto_start);
  cJSON_AddBoolToObject(root, "wash_auto_start", settings->wash_auto_start);
```

- [ ] **Step 2: Update payload parsing in `api_pump_parse_settings_payload()`**

Parse wash parameters directly into `settings`:
```c
  const cJSON *w_on = api_pump_get_item(root, "wash_on", "wash_on_sec");
  if (w_on && cJSON_IsNumber(w_on) && w_on->valuedouble >= APP_TEMPLATE_PUMP_TIMER_MIN_SEC && w_on->valuedouble <= APP_TEMPLATE_PUMP_TIMER_MAX_SEC) {
    settings->wash_on_sec = (uint32_t)w_on->valuedouble;
  }

  const cJSON *w_off = api_pump_get_item(root, "wash_off", "wash_off_sec");
  if (w_off && cJSON_IsNumber(w_off) && w_off->valuedouble >= APP_TEMPLATE_PUMP_TIMER_MIN_SEC && w_off->valuedouble <= APP_TEMPLATE_PUMP_TIMER_MAX_SEC) {
    settings->wash_off_sec = (uint32_t)w_off->valuedouble;
  }

  const cJSON *w_start = cJSON_GetObjectItem(root, "wash_start_phase");
  if (w_start) {
    hardware_timer_start_phase_t sp;
    if (api_pump_parse_start_phase(w_start, &sp) && hardware_map_timer_start_phase_valid(sp)) {
      settings->wash_start_phase = sp;
    }
  }

  const cJSON *w_auto = cJSON_GetObjectItem(root, "wash_auto_start");
  if (w_auto && cJSON_IsBool(w_auto)) {
    settings->wash_auto_start = cJSON_IsTrue(w_auto);
  }
```

- [ ] **Step 3: Update `handle_api_pump_config_post()` in `web_server.c`**

Delete lines 3780–3799 (manual NVS read block).
Construct `pump_control_config_update_t update` directly from `settings`:
```c
  pump_control_config_update_t update = {
      .water_set1 =
          {
              .on_sec = settings.timer1_on_sec,
              .off_sec = settings.timer1_off_sec,
              .start_phase = (settings.timer1_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_set2 =
          {
              .on_sec = settings.timer2_on_sec,
              .off_sec = settings.timer2_off_sec,
              .start_phase = (settings.timer2_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .wash_timer =
          {
              .on_sec = settings.wash_on_sec,
              .off_sec = settings.wash_off_sec,
              .start_phase = (settings.wash_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_auto_start = settings.auto_start,
      .wash_auto_start = settings.wash_auto_start,
      .min_dwell_sec = settings.min_dwell_sec,
  };

  if (!nvs_store_save_pump_settings(&settings)) {
    return send_api_error(req, "settings_save_failed",
                          "Could not save pump settings to NVS",
                          "500 Internal Server Error");
  }

  if (!pump_control_update_config(&update)) {
    return send_api_error(req, "settings_apply_failed",
                          "Could not apply pump settings to runtime",
                          "500 Internal Server Error");
  }

  return api_pump_send_config(req, &settings, NVS_STORE_PUMP_SETTINGS_LOADED,
                              true);
```

---

### Task 5: Compile Verification & Regression Testing

**Files:** None (Build & Verification)

- [ ] **Step 1: Execute clean build**

Run:
```powershell
$env:IDF_TOOLS_PATH = "C:\Espressif"; & "C:\esp\v6.0.1\esp-idf\export.ps1"; idf.py build
```
Expected: Build succeeds with `Project build complete.` and binary artifacts generated in `build/`.

- [ ] **Step 2: Verify binary size & linking**

Confirm firmware binary was generated:
```powershell
Get-Item build\fishpump_firmware.bin | Select-Object Name, Length, LastWriteTime
```

- [ ] **Step 3: Check for remaining unexpected NVS calls**

Verify that `pump_control.c` contains zero calls to `nvs_`:
```powershell
Select-String -Path components\pump_control\pump_control.c -Pattern "nvs_"
```
Expected: No matching lines.
