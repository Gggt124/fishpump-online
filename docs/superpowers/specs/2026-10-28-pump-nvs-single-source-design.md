# Design Specification: Single Source of Truth for Pump NVS Persistence

**Document ID:** `2026-10-28-pump-nvs-single-source-design`  
**Date:** 2026-10-28  
**Status:** DRAFT / UNDER REVIEW  
**Author:** AI Assistant & Pair Programmer  

---

## 1. Context & Problem Statement

### 1.1 Current Architecture
The firmware manages water pumping and washing operations through two main components:
- `components/pump_control`: Controls Water Relays (Relay 1 / Relay 2) based on float switch inputs, runtime timers, and minimum dwell periods. It also controls the Wash Relay (Relay 2).
- `components/nvs_store`: Central NVS persistence layer responsible for managing Wi-Fi profiles, JWT session keys, display settings, cooling controller configurations, hardware pin maps, and pump settings.

### 1.2 The Double-Write Issue (Flash Wear & Redundancy)
In `firmware/main/web_server.c` (`handle_api_pump_config_post`, lines 3861–3863):
1. `nvs_store_save_pump_settings(&settings)` is called, serializing pump configuration into an atomic binary blob (`"pump_blob"`, version 2) within NVS namespace `"pump_cfg"`.
2. `pump_control_update_config(&update)` is called immediately after, which internally invokes `save_nvs_config()` in `pump_control.c` (lines 461–497).
3. `save_nvs_config()` writes **16 individual keys** into the same `"pump_cfg"` namespace:
   - `w_set1_on`, `w_set1_off`, `w_set1_start`
   - `w_set2_on`, `w_set2_off`, `w_set2_start`
   - `w_autostart`
   - `wash_on`, `wash_off`, `wash_start`, `wash_autostart`
   - `rel1_pol`, `rel2_pol`
   - `flt_gpio`, `rel1_gpio`, `rel2_gpio` (redundant with `hardware_map` in `"hw_cfg"`)

This produces:
- **Double Flash write wear:** Flash sectors undergo twice the wear cycles on every settings update.
- **I/O overhead & latency:** Web server POST responses are delayed by two independent Flash commit cycles.
- **Architectural divergence:** While `cooling_control` operates cleanly as a pure in-memory controller fed by `nvs_store`, `pump_control` maintains its own direct NVS coupling.

### 1.3 Hidden Coupling & Risks of a Naive Fix
Simply deleting `save_nvs_config()` from `pump_control.c` would cause severe boot regressions:
1. **Boot Initialization Coupling:** At startup, `app_main.c` loads `pump_settings` from `nvs_store_load_pump_settings()`, but `pump_control_init_context_t` only passes GPIOs, polarities, and dwell time. `pump_control_init()` calls `load_nvs_config()` to read the individual keys from `"pump_cfg"`. If individual keys are not written, `pump_control` would load stale or default timer values after reboot.
2. **Wash System Absence in `nvs_store`:** `nvs_store_pump_settings_t` does not include `wash_on`, `wash_off`, `wash_start_phase`, or `wash_auto_start`. These parameters only persist in Flash via `pump_control`'s `save_nvs_config()`.
3. **Ad-hoc NVS reads in Web Server:** `web_server.c` opens `"pump_cfg"` directly with `nvs_open` (lines 1020–1028 and 3790–3798) to read wash settings.

---

## 2. Design Goals & Principles

1. **Single Source of Truth:** `nvs_store` is the sole manager of Flash persistence for all pump and wash settings under namespace `"pump_cfg"`.
2. **Stateless Controller Pattern:** `pump_control` becomes a pure in-memory hardware controller (stateful in RAM, zero NVS dependency), identical to `cooling_control`.
3. **Atomic Blob Storage:** Store all pump (Water + Wash) settings in a single versioned blob (`"pump_blob"`, version 3).
4. **Backward Compatibility & Seamless Migration:**
   - Existing devices with Blob v1 or v2 must upgrade smoothly without losing settings; missing wash fields will populate with standard defaults (10s on, 60s off, start ON, auto-start false).
   - Devices upgrading from legacy individual keys will migrate all fields (including legacy wash keys) into Blob v3.
5. **Clean API Abstraction:** `web_server.c` interacts exclusively with `nvs_store` for persistence and `pump_control` for runtime execution.

---

## 3. Detailed Architecture & Interface Changes

### 3.1 `components/nvs_store`

#### Data Structure Expansion (`include/nvs_store.h`)
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

  /* Wash System Settings (added in Blob v3) */
  uint32_t wash_on_sec;
  uint32_t wash_off_sec;
  hardware_timer_start_phase_t wash_start_phase;
  bool wash_auto_start;
} nvs_store_pump_settings_t;
```

#### Blob Versioning & Migration (`nvs_store.c`)
- Update `#define NVS_PUMP_BLOB_VERSION 3`
- In `nvs_store_pump_settings_defaults()`:
  ```c
  out->wash_on_sec = 10;
  out->wash_off_sec = 60;
  out->wash_start_phase = HARDWARE_TIMER_START_PHASE_ON;
  out->wash_auto_start = false;
  ```
- In `nvs_store_load_pump_settings()`:
  - Accept blob versions `1`, `2`, and `3`.
  - For v1/v2, payload length is smaller than `sizeof(nvs_store_pump_settings_t)`. `memcpy(&loaded, blob + 1, copy_len)` copies existing fields; new wash fields retain default values.
  - In legacy key fallback, also load `"wash_on"`, `"wash_off"`, `"wash_start"`, `"wash_autostart"` if present.
- In `pump_settings_valid()`:
  - Validate duration for `wash_on_sec` and `wash_off_sec`.
  - Validate `wash_start_phase` and `wash_auto_start`.
- In `nvs_store_clear_pump_settings()`:
  - Erase `"pump_blob"`.
  - Also erase legacy individual keys (`wash_on`, `wash_off`, `w_set1_on`, etc.) to keep NVS clean.

---

### 3.2 `components/pump_control`

#### Header Changes (`include/pump_control.h`)
Extend `pump_control_init_context_t`:
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

  /* Complete runtime configuration passed by caller */
  pump_control_timer_set_config_t water_set1;
  pump_control_timer_set_config_t water_set2;
  pump_control_timer_set_config_t wash_timer;
  bool water_auto_start;
  bool wash_auto_start;
} pump_control_init_context_t;
```

#### Implementation Changes (`pump_control.c`)
- Remove `#include "nvs.h"` and `#include "nvs_flash.h"`.
- Remove `#define NVS_PUMP_NAMESPACE` and all `NVS_KEY_*` macros.
- Delete `load_nvs_config()` and `save_nvs_config()`.
- Update `init_context_valid()` to validate `water_set1`, `water_set2`, and `wash_timer`.
- In `pump_control_init()`:
  - Initialize `s_config` directly from `context`.
  - Auto-start logic respects `context->allow_auto_start && context->water_auto_start` (and `wash_auto_start`).
- In `pump_control_update_config()`:
  - Update `s_config` fields under mutex lock.
  - Return `true` without touching NVS.

---

### 3.3 `firmware/main/app_main.c`

In `app_main()`:
- Load `pump_settings` via `nvs_store_load_pump_settings(&pump_settings)`.
- Populate `pump_control_init_context_t` with:
  - Water Set 1: `timer1_on_sec`, `timer1_off_sec`, `timer1_start_phase`
  - Water Set 2: `timer2_on_sec`, `timer2_off_sec`, `timer2_start_phase`
  - Wash Timer: `wash_on_sec`, `wash_off_sec`, `wash_start_phase`
  - Auto starts: `auto_start`, `wash_auto_start`
- Invoke `pump_control_start_task(&pump_init_context)`.

---

### 3.4 `firmware/main/web_server.c`

1. **Remove Ad-Hoc NVS Reads:**
   - Remove `nvs_open("pump_cfg", NVS_READONLY, &nvs_h)` in `api_pump_add_config_fields()` (lines 1020–1029).
   - Read wash configuration directly from `settings->wash_on_sec`, `settings->wash_off_sec`, etc.
2. **Update Request Body Parsing:**
   - In `api_pump_parse_settings_payload()`: Parse `wash_on` / `wash_on_sec`, `wash_off` / `wash_off_sec`, `wash_start_phase`, and `wash_auto_start` directly into `settings`.
3. **Handle Config Post:**
   - In `handle_api_pump_config_post()`:
     - Remove ad-hoc NVS reads (lines 3790–3799).
     - Save complete configuration via `nvs_store_save_pump_settings(&settings)`.
     - Update runtime state via `pump_control_update_config(&update)`.
     - Respond with current config and status.

---

## 4. Flash Wear & Reliability Analysis

| Metric | Before Refactoring | After Refactoring | Improvement |
| :--- | :--- | :--- | :--- |
| **NVS Writes per POST** | 2 writes (1 blob + 16 keys) | 1 write (1 atomic blob) | **~50% reduction in write cycles** |
| **Power-Loss Atomicity** | Partial write risk on 16 keys | 100% atomic (ESP-IDF blob commit) | **Eliminates state corruption** |
| **Boot Read Operations** | 2 reads (`nvs_store` + `pump_control`) | 1 read (`nvs_store` in `app_main`) | **Faster boot, single read** |
| **Coupling / Ownership** | Dual ownership of `"pump_cfg"` | Single owner (`nvs_store`) | **Clear module boundaries** |
