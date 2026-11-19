# ThingsBoard Link — Milestone 3 (settings and state attributes + status telemetry) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ThingsBoard always shows the board's real settings and state, whichever path changed them. The board publishes its current settings as client attributes on connect and within 2 s of any change. It also sends a status telemetry message with what the web app's Status, Pump, Cooling and Protection cards show that is not yet a registry signal.

**Architecture:**
- The pure helpers go in `uplink_format`, which is QEMU-tested. They are:
  - a hash and a per-key "last sent" cache;
  - the due rules for rebuilding and sending;
  - a copy-without-keys helper;
  - the status-telemetry field table and the flattener.
- `device_api` gains a write hook. `uplink` gains a pluggable "state source" with two builders, attributes and status, plus a "settings changed" flag. The uplink task calls the builders with no uplink lock held. It sends each attribute key whose hash changed, and sends the status message when it is due.
- A new `firmware/main/tb_state.c` implements the two builders. It reads the `*.get` methods through `device_api_call(..., DAPI_SRC_TB, ...)`, so the values are the web API's values. It also wires the write hook to `uplink_note_settings_changed()`.
- HTTP-only paths that change state (`/api/confirm`, credentials, change-password, OTA upload) raise the flag directly.
- On the ThingsBoard side:
  - the parity script checks that each client attribute matches the board's HTTP reply and follows a write and its restore;
  - the dashboard pages show the attributes and the status keys;
  - the pump "load current values" button keeps only the writable keys (`load_keys`, from the M2 backlog).

**Tech Stack:** ESP-IDF v6.0.1, cJSON, FreeRTOS, managed `espressif/mqtt ^1.1.0`, ThingsBoard CE 4.3.1 REST. The PC scripts use the Python 3 standard library only.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`, sections 6.3, 7.2 ("live telemetry and client attributes"), 7.3 step 3 ("the ThingsBoard client attribute followed both changes"), 8 and 8a item 3. Milestones 1 and 2 are done (`docs/bench-test-thingsboard.md`; HEAD `e1b3ec0`).

Out of scope:
- remote `wifi.connect` through staging, and the empty-password `wifi.profiles.save` guard (M4);
- OTA (M5);
- sending the active alarm list as an attribute (backlog option for the M1 outbox limit; it needs a rule-chain change, so it is left for later);
- the soak (M6).

## Global Constraints

- **Gates.** These stay green after every task:
  - The build in PowerShell: `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM; idf.py -C firmware build`. It must have **0 warnings** in project code.
  - `python firmware/scripts/verify_alarm_system.py`, `verify_dry_run.py`, `verify_sensor_integration.py`, `verify_pre_mqtt.py`, `verify_pt100_inverter_system.py`, `verify_device_api.py` and `verify_tb_link.py`.
  - The QEMU selftest `firmware/test/sensor_selftest/run_selftest.ps1` prints `SELFTEST PASS (<n> checks)`. The baseline is 1396; it only grows.
  - `python thingsboard/parity_test.py --selftest` and `python thingsboard/tb_dashboard.py --check`.
- **The web app does not change.**
  - No file under `firmware/main/static/` changes.
  - Every HTTP endpoint keeps its status line and body bytes, except `/api/uplink/preview`, which gains one key, `last_status`.
- **Contract:** `fw_type:"fishpump"`, `contract_version:1`. The new keys are additions, so the version stays 1 (spec 4). Existing telemetry, event, `identity` and `alarm_rules` messages do not change.
- **Client attribute keys**, exactly: `cfg_pump`, `cfg_cooling`, `cfg_protection`, `wifi`, `hardware_map`, `tb_link`. Each one is sent as its own message, `{"<key>":<value>}`, on `v1/devices/me/attributes`.
- **Never in any attribute or telemetry:** a Wi-Fi password, the web password or user name, the ThingsBoard token, or the provision key or secret. Wi-Fi data is copied from an allow-list of fields, never by dropping keys.
- **Timing** (spec 6.3):
  - Attributes are sent on every connect, and within 2 s of a change.
  - Status telemetry is sent every 30 s, and at once (at most every 2 s) when a watched value changes.
  - These constants go in `uplink_format.h`: `UPLINK_STATE_RECHECK_MS 60000u`, `UPLINK_STATUS_INTERVAL_MS 30000u`, `UPLINK_STATUS_MIN_GAP_MS 2000u`, `UPLINK_STATUS_POLL_MS 2000u`, `UPLINK_STATUS_OFFLINE_POLL_MS 30000u`.
- **Locks.**
  - The uplink task calls the state builders with neither `s.lock` nor `s.send_lock` held. The `uplink.preview` method takes `s.lock` while it holds the `device_api` lock, so holding `s.lock` across a builder call would deadlock.
  - The write hook runs after the `device_api` lock is released and never blocks: it is an atomic store.
- **Stack:**
  - The uplink task now runs `device_api` methods, which were written for the 6144 B `tb_rpc` stack. `UPLINK_TASK_STACK` goes from 4096 to **6144**.
  - Buffers over 256 B go on the heap; buffers that live as long as the task go in PSRAM.
- **Logging:**
  - A new tag `tb_state` is raised to INFO in `app_main.c`.
  - The uplink task logs each attribute it sends: `attribute <key> sent (<n> B)`.
  - Builders never log reply bodies.
- **Never stage** `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`. No merge or push.

## Review Focus

1. **Board online but ThingsBoard down (or no host set).** The status builder must not take the `device_api` lock every 2 s for nothing. While the sink is not connected, status is polled every 30 s (for the preview only), and attributes are not built. → Task 1 test `status_poll_due` offline case; Task 3 checks it in `do_status`.
2. **Reconnect after a drop.** Every attribute is sent again even if unchanged, because the old outbox may have lost it. → Task 1 test: after `uplink_attr_cache_reset` every key reads as changed. Task 3 resets the cache on the connect edge.
3. **A `*.get` call fails** (busy after 5 s, or out of memory). That key is left out of this round. It is never sent as an error object, its cached hash stays, and it is retried on the next rebuild. → Task 4 builder rule; a Task 4 verify check that each builder tests `== DAPI_OK`.
4. **A change that does not go through a write.** For example, the cooling `test` mode ends on its timeout, or the hardware map's pending state changes after a reboot. These show up within the 60 s recheck. The status telemetry carries `cooling_mode_name` at once. → Task 1 test `state_rebuild_due` at 60000 ms; Task 7 bench step I5.
5. **Wi-Fi data leaking a password.** This would happen if a new key appears in `wifi.profiles.get` or `status.get`. → The allow-list copy in Task 4, plus a Task 4 verify check that `tb_state.c` names no `password`, `token` or `secret`.

## Calls in this plan (the user can overturn any)

1. **The `wifi` attribute has no RSSI**, although spec 6.3 lists it.
   - RSSI already goes out every 30 s as the registry signal `wifi_rssi_dbm`.
   - As an attribute it would either go stale, or be resent on every recheck because it moves by 1 dBm.
2. **Live parts are left out of the setting attributes.**
   - `cfg_cooling` drops `status`, and `cfg_protection` drops `pump_status`. Those are live state, and they belong in the status telemetry.
   - `hardware_map` drops `options`, the 6 KB static pin list.
   - Otherwise each value is the `*.get` reply as it is.
3. **A 60 s recheck**, which the spec does not ask for. It catches changes that no write raises, such as a cooling test mode ending on its timeout. Only keys whose hash changed are sent.
4. **The write hook fires after every write method that ran, even one that failed.** The rebuild then sends nothing unless something really changed. This is simpler than deciding which failures changed state.
5. **Status telemetry is its own message**, not merged into the existing telemetry. The Task 1 size test shows it fits in one message, so no second message is needed.
6. **The uplink task stack goes from 4096 to 6144 B**, because it now runs `device_api` methods.
7. **The parity follow check passes at 3 s**, against the spec's 2 s. The board sends on the next 1 s tick, but the script also pays ThingsBoard's REST latency and a 250 ms poll step. The measured delay is printed so it can be judged.
8. **Two M2 backlog minors are folded in:** `load_keys` for the dashboard load button, and the wrong error name in `device-api.md` section 4. The empty-password Wi-Fi guard waits for M4.

---

### Task 1: Pure state helpers and the status field table (`uplink_format`)

**Files:**
- Modify: `firmware/components/uplink_format/include/uplink_format.h`, `firmware/components/uplink_format/uplink_format.c`
- Test: create `firmware/test/sensor_selftest/main/test_uplink_state.c`. Register `test_uplink_state()` in `selftest.h`, `selftest_main.c` and `main/CMakeLists.txt` (SRCS).

**Interfaces:**
- Produces (in `uplink_format.h`):
  - `#define UPLINK_STATE_ATTR_MAX 8`, `UPLINK_STATE_KEY_MAX 24`, `UPLINK_STATE_ATTR_MAX_BYTES 4096`, plus the five timing constants from Global Constraints.
  - `uint32_t uplink_hash32(const char *data, size_t len);` FNV-1a 32-bit: offset basis 2166136261, prime 16777619.
  - The per-key "last sent" cache:
    - `typedef struct { char key[UPLINK_STATE_KEY_MAX]; uint32_t hash; } uplink_attr_slot_t;`
    - `typedef struct { uplink_attr_slot_t slot[UPLINK_STATE_ATTR_MAX]; size_t n; } uplink_attr_cache_t;`
    - `void uplink_attr_cache_reset(uplink_attr_cache_t *c);`
    - `bool uplink_attr_cache_changed(const uplink_attr_cache_t *c, const char *key, uint32_t hash);` Returns true for an unknown key or a different hash.
    - `bool uplink_attr_cache_commit(uplink_attr_cache_t *c, const char *key, uint32_t hash);` Returns false when the key is too long (≥ `UPLINK_STATE_KEY_MAX`) or the table is full.
  - The due rules:
    - `bool uplink_state_rebuild_due(bool dirty, bool ever, uint32_t since_build_ms);` = `dirty || !ever || since >= UPLINK_STATE_RECHECK_MS`.
    - `bool uplink_status_poll_due(bool connected, bool ever, uint32_t since_poll_ms);` The limit is `UPLINK_STATUS_POLL_MS` when connected, `UPLINK_STATUS_OFFLINE_POLL_MS` when not, and it is always due when `!ever`.
    - `bool uplink_status_send_due(bool ever, uint32_t since_sent_ms, bool watched_changed);` = `!ever || since >= UPLINK_STATUS_INTERVAL_MS || (watched_changed && since >= UPLINK_STATUS_MIN_GAP_MS)`.
  - `cJSON *uplink_json_without(const cJSON *obj, const char *const *drop, size_t ndrop);` Returns a deep copy minus the listed top-level keys, or NULL when `obj` is not an object or memory runs out. The source is not changed.
  - The status table and flattener:
    - `typedef enum { UPLINK_SRC_PUMP = 0, UPLINK_SRC_COOLING, UPLINK_SRC_STATUS, UPLINK_SRC_COUNT } uplink_status_src_t;`
    - `typedef struct { uint8_t src; const char *path; const char *key; bool watched; } uplink_status_field_t;`
    - `extern const uplink_status_field_t uplink_status_fields[]; extern const size_t uplink_status_field_count;`
    - `int uplink_status_flatten(const cJSON *const srcs[UPLINK_SRC_COUNT], cJSON *out, uint32_t *watched_hash);` It adds each field found (number, bool or string; `path` is dot-separated) to `out` under its `key`. A NULL source or a missing path is skipped. It returns the number of keys added.
    - `*watched_hash` is FNV-1a over, for each watched field in table order, `key=` + the unformatted value + `;`. A watched field that is missing contributes `key=?;`.

- [ ] **Step 1: Write the failing tests** in `test_uplink_state.c`:
  - `uplink_hash32("", 0) == 2166136261u` and `uplink_hash32("a", 1) == 0xe40c292cu`.
  - The cache:
    - After reset, `changed("cfg_pump", 1)` is true.
    - After `commit("cfg_pump", 1)`, `changed("cfg_pump", 1)` is false and `changed("cfg_pump", 2)` is true.
    - Committing 8 distinct keys works; a 9th distinct key returns false.
    - A 24-character key returns false.
    - After reset, every committed key is changed again.
  - The due rules:
    - `rebuild_due(false, true, 59999)` is false; `(false, true, 60000)`, `(true, true, 0)` and `(false, false, 0)` are true.
    - `poll_due(true, true, 1999)` is false and `(true, true, 2000)` is true. `poll_due(false, true, 29999)` is false and `(false, true, 30000)` is true. `poll_due(false, false, 0)` is true.
    - `send_due(true, 29999, false)` and `(true, 1999, true)` are false. `send_due(true, 30000, false)`, `(true, 2000, true)` and `(false, 0, false)` are true.
  - `uplink_json_without`:
    - `{"ok":true,"a":1,"status":{"x":1}}` without `ok` and `status` gives `{"a":1}`, and the source still has 3 keys.
    - A JSON array source gives NULL.
  - The flattener, on canned replies parsed from strings taken from the 2026-11-20 golden capture: `pump.status` (with `system_water`, `system_wash` and `dry_run`), `cooling.status` and `status.get`.
    - The output has `pump_phase:"OFF"`, `dry_run_state:"disabled"`, `cooling_mode_name:"auto"`, `cooling_blocked_reason:"sensor_fault"`, `heap_min_free_bytes:120372` and `wifi_connected:true`.
    - With `srcs[UPLINK_SRC_PUMP] = NULL`, no output key starts with `pump_`, `wash_` or `dry_run_`, and the count drops by the number of pump fields.
    - Changing `system_water.countdown_sec` leaves the watched hash the same. Changing `system_water.phase` changes it. Removing `cooling.status.fault_code` changes it.
  - Size:
    - Every string field is 32 `x` characters and every number is `-2147483648`.
    - Then the unformatted print of the output, plus `7 * 40` bytes for the keys `tb_state` adds (Task 4), must be `< UPLINK_TELEMETRY_BUF`.
  - The table:
    - Keys are unique and match `[a-z0-9_]+`.
    - No key starts with `alarm_`, and `signal_registry_by_key(key) == NULL` for every key.
    - Every `src < UPLINK_SRC_COUNT`.

- [ ] **Step 2: Run the selftest and see it fail** (it does not link yet).

  Run: `firmware/test/sensor_selftest/run_selftest.ps1`. Expected: a build error on the missing `uplink_hash32` and the other new names.

- [ ] **Step 3: Implement the helpers and the table in `uplink_format.c`**

  The table, exactly. `*` marks a watched field. Paths are into the method replies:

  | Source | Path → key |
  |---|---|
  | `UPLINK_SRC_PUMP` (`pump.status`) | `system_water.phase`→`pump_phase`*, `system_water.countdown_sec`→`pump_countdown_sec`, `system_water.total_duration_sec`→`pump_cycle_sec`, `system_water.cooldown_remaining_sec`→`pump_cooldown_sec`, `fault`→`pump_fault`*, `initial_stabilizing`→`pump_stabilizing`*, `config_valid`→`pump_config_valid`*, `system_wash.running`→`wash_running`*, `system_wash.phase`→`wash_phase`*, `system_wash.countdown_sec`→`wash_countdown_sec`, `dry_run.enabled`→`dry_run_enabled`*, `dry_run.state`→`dry_run_state`*, `dry_run.trips`→`dry_run_trips`*, `dry_run.max_retries`→`dry_run_max_retries`, `dry_run.wait_remaining_sec`→`dry_run_wait_sec`, `dry_run.dry_accum_sec`→`dry_run_accum_sec` |
  | `UPLINK_SRC_COOLING` (`cooling.status`) | `mode`→`cooling_mode_name`*, `auto_enable`→`cooling_auto_enable`*, `sensor_state`→`cooling_sensor_state`*, `fault_code`→`cooling_fault_code`*, `cooling_demand`→`cooling_demand`*, `blocked_reason`→`cooling_blocked_reason`*, `temperature_valid`→`cooling_temp_valid`*, `test_remaining_sec`→`cooling_test_sec` |
  | `UPLINK_SRC_STATUS` (`status.get`) | `sta_connected`→`wifi_connected`*, `sta_channel`→`wifi_channel`, `min_free_heap`→`heap_min_free_bytes`, `largest_free_block`→`heap_largest_block_bytes` |

- [ ] **Step 4: Run the selftest and see it pass**

  Run: `firmware/test/sensor_selftest/run_selftest.ps1`. Expected: `SELFTEST PASS (<n> checks)` with n > 1396, and no `FAIL` lines. Also run the firmware build: 0 warnings.

- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/uplink_format firmware/test/sensor_selftest/main
  git commit -m "feat: uplink state helpers and the status telemetry table"
  ```

### Task 2: `device_api` write hook

**Files:**
- Modify: `firmware/components/device_api/include/device_api.h`, `firmware/components/device_api/device_api.c`
- Test: `firmware/test/sensor_selftest/main/test_device_api.c`

**Interfaces:**
- Produces:
  - `typedef void (*dapi_write_hook_t)(const char *method, dapi_status_t st);`
  - `void device_api_set_write_hook(dapi_write_hook_t hook);` NULL means none. It is an atomic pointer store, set once at boot.
- When the hook runs:
  - It runs for every `DAPI_F_WRITE` method whose function ran, whatever status it returned.
  - It runs after the lock is released.
  - It does not run for calls refused before the function: `unknown_method`, `local_only`, TB `invalid_json`, TB `invalid_body` and `busy`.

- [ ] **Step 1: Write the failing test.**
  - Register new test methods: `"hook.read"` (READ, returns OK), `"hook.write"` (WRITE, returns `DAPI_BAD_REQUEST` with a `dapi_error` body) and `"hook.local"` (WRITE | LOCAL_ONLY, returns OK).
  - The hook records its call count and the last method and status.
  - Check, in this order:
    - `hook.read` → count 0;
    - `hook.write` → count 1, method `"hook.write"`, status `DAPI_BAD_REQUEST`;
    - `hook.local` from `DAPI_SRC_TB` → count still 1;
    - `hook.local` from `DAPI_SRC_HTTP` → count 2;
    - `hook.write` with TB params of 600 B when `max_params_bytes` is 512 → count still 2;
    - `device_api_set_write_hook(NULL)`, then `hook.write` → count still 2.
- [ ] **Step 2: Run the selftest and see it fail.** Expected: a build error on `device_api_set_write_hook`.
- [ ] **Step 3: Implement it.** Use `_Atomic dapi_write_hook_t` (`stdatomic.h`, as `s_count` already does), and call the hook after `xSemaphoreGive`.
- [ ] **Step 4: Run the selftest and see it pass.** Also run the build (0 warnings) and `python firmware/scripts/verify_device_api.py` (all PASS).
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/device_api firmware/test/sensor_selftest/main/test_device_api.c
  git commit -m "feat: device_api write hook"
  ```

### Task 3: `uplink` sends state attributes and status telemetry

**Files:**
- Modify: `firmware/components/uplink/include/uplink.h`, `firmware/components/uplink/uplink.c` and `firmware/components/uplink/CMakeLists.txt` (add `cjson` to REQUIRES).
- Test: `firmware/scripts/verify_tb_link.py` (new function `test_uplink_state()`, called from `main`).

**Interfaces:**
- Consumes: Task 1's cache, due rules and constants, `uplink_hash32`, and `UPLINK_STATE_ATTR_MAX_BYTES`.
- Produces (in `uplink.h`):
  - The state source:
    ```c
    typedef struct {
        /* One member per client attribute (e.g. "cfg_pump"); NULL = nothing this round. Called on the uplink task
         * with no uplink lock held; may take the device_api lock. The caller deletes the object. */
        cJSON *(*build_attributes)(void);
        /* Flat status telemetry; *watched_hash covers the watched keys. NULL = skip. Same calling rules. */
        cJSON *(*build_status)(uint32_t *watched_hash);
    } uplink_state_source_t;
    ```
  - `esp_err_t uplink_set_state_source(const uplink_state_source_t *src);` The source is copied under `s.lock`. NULL means none. It returns `ESP_ERR_INVALID_STATE` before `uplink_init`.
  - `void uplink_note_settings_changed(void);` An atomic store. It never blocks and is safe from any task, including before `uplink_init`.
- Behaviour on each 1 s tick, after `do_attributes`:
  - **`do_state`:**
    - On the sink's connect edge, call `uplink_attr_cache_reset` and set `ever = false`.
    - When connected and `uplink_state_rebuild_due(atomic_exchange(&dirty, false), ever, since_build)`:
      - Build the attributes, then go through each member.
      - Print `{"<key>":<value>}` unformatted.
      - If the print is larger than `UPLINK_STATE_ATTR_MAX_BYTES`, log a warning and skip that member.
      - If the cache says it changed, send it as `UPLINK_MSG_ATTRIBUTES` under `send_lock`. On `ESP_OK`, commit its hash. Otherwise set `dirty` back to true.
      - Then set `ever = true` and record the build time.
    - When not connected, build nothing.
  - **`do_status`:**
    - When `uplink_status_poll_due(connected, polled_ever, since_poll)`, call `build_status`, print it into a PSRAM buffer of `UPLINK_TELEMETRY_BUF` (`cJSON_PrintPreallocated`), and keep it under `s.lock` for the preview.
    - If connected and `uplink_status_send_due(sent_ever, since_sent, hash != last_hash)`, send it as `UPLINK_MSG_TELEMETRY`.
    - A disconnect sets `sent_ever = false`, so the first message after a connect goes out at once.
    - If the message does not fit, log a warning and send nothing.
  - **The preview:**
    - `uplink_preview_json` adds `"last_status":<object or null>` after `"last_telemetry"`.
    - The preview's buffer size grows by `UPLINK_TELEMETRY_BUF`.
  - `UPLINK_TASK_STACK` becomes 6144.

- [ ] **Step 1: Write the failing verify checks** in `test_uplink_state()`:
  - `uplink.h` declares `uplink_set_state_source` and `uplink_note_settings_changed`.
  - `#define UPLINK_TASK_STACK 6144`.
  - `uplink.c` uses `uplink_attr_cache_reset`, `uplink_state_rebuild_due`, `uplink_status_poll_due`, `uplink_status_send_due` and `UPLINK_STATE_ATTR_MAX_BYTES`.
  - In the bodies of `do_state` and `do_status` (use `function_body`), the text between each `xSemaphoreTake` and its next `xSemaphoreGive` does not contain `build_attributes(` or `build_status(`.
  - The preview format string contains `\"last_status\":`.
  - The CMake REQUIRES of `uplink` contain `cjson`.
- [ ] **Step 2: Run the checks and see them fail.** Run `python firmware/scripts/verify_tb_link.py`. Expected: the new checks FAIL and the old ones pass.
- [ ] **Step 3: Implement `do_state`, `do_status`, the setters and the preview key** as specified above.
- [ ] **Step 4: Run the checks and see them pass.**
  - `python firmware/scripts/verify_tb_link.py` passes.
  - The build has 0 warnings.
  - The QEMU selftest passes with the same count as after Task 2.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/uplink firmware/scripts/verify_tb_link.py
  git commit -m "feat: uplink publishes state attributes and status telemetry"
  ```

### Task 4: `tb_state` builders, wiring and the HTTP-only triggers

**Files:**
- Create: `firmware/main/tb_state.c`, `firmware/main/tb_state.h`.
- Modify:
  - `firmware/main/CMakeLists.txt` (SRCS)
  - `firmware/main/app_main.c` (init and log tag)
  - `firmware/main/web_server.c` (the triggers)
  - `docs/firmware/device-api.md` (a new section)
- Test: `firmware/scripts/verify_tb_link.py` (new function `test_tb_state()`).

**Interfaces:**
- Consumes:
  - `uplink_set_state_source` and `uplink_note_settings_changed` (Task 3);
  - `device_api_set_write_hook` (Task 2);
  - `uplink_json_without`, `uplink_status_flatten` and `UPLINK_SRC_*` (Task 1);
  - `tb_link_get_config(...).device_name`, `tb_link_get_status`, `APP_TEMPLATE_FIRMWARE_VERSION`;
  - `alarm_engine_stack_hwm`, `uplink_stack_hwm`, `dry_run_guard_stack_hwm`, `tb_link_stack_hwm`, `tb_link_rpc_stack_hwm`.
- Produces: `esp_err_t tb_state_init(void);`. It registers the state source and the write hook (the hook calls `uplink_note_settings_changed()`). Call it in `app_main.c` right after `device_api_init()`.
- Attribute values. Each read is `device_api_call(method, NULL, DAPI_SRC_TB, &reply)`. A status other than `DAPI_OK` means the key is left out this round (Review Focus 3).

  | Key | Value |
  |---|---|
  | `cfg_pump` | `pump.config.get` without `ok` |
  | `cfg_cooling` | `cooling.config.get` without `ok`, `status` |
  | `cfg_protection` | `protection.get` without `ok`, `pump_status` |
  | `hardware_map` | `hardware.map.get` without `ok`, `options` |
  | `wifi` | allow-list only: from `status.get`, `ssid`←`sta_ssid`, `ip`←`sta_ip`, `mode`←`wifi_mode`, `ap_enabled`; from `wifi.profiles.get`, `auto_index`, and `profiles` as a list of `{ssid, auto, connected}`. `auto` is the `auto` of the profile with `connected:true`, else false. |
  | `tb_link` | `{"device_name": <config device_name>, "fw_version": APP_TEMPLATE_FIRMWARE_VERSION}` |

- Status telemetry:
  - Call `uplink_status_flatten` on the `pump.status`, `cooling.status` and `status.get` replies. A failed read is passed as NULL.
  - Then add `stack_hwm_alarm`, `stack_hwm_uplink`, `stack_hwm_dry_run`, `stack_hwm_tb_link` and `stack_hwm_tb_rpc`, each only when ≥ 0. Add `tb_sent` and `tb_failed` from `tb_link_get_status`.
- The HTTP-only triggers: call `uplink_note_settings_changed()` once each, on success, in:
  - `POST /api/confirm`;
  - `DELETE /api/confirm`, before the reboot;
  - `handle_api_auth_credentials`;
  - `handle_api_change_password`;
  - the OTA upload handler, after `esp_ota_set_boot_partition` succeeds.

- [ ] **Step 1: Write the failing verify checks** in `test_tb_state()`:
  - `tb_state.c` is in the main SRCS.
  - `app_main.c` calls `tb_state_init()` after `device_api_init()` and has `esp_log_level_set("tb_state", ESP_LOG_INFO)`.
  - `tb_state.c` calls `uplink_set_state_source` and `device_api_set_write_hook`, and contains `== DAPI_OK`.
  - `tb_state.c` contains none of `password`, `token`, `secret` or `provision` (case-insensitive).
  - Each of the five web handlers' bodies contains `uplink_note_settings_changed()`.
  - `tb_state.c` names all six attribute keys as string literals.
- [ ] **Step 2: Run the checks and see them fail.** Run `python firmware/scripts/verify_tb_link.py`; the new checks FAIL.
- [ ] **Step 3: Implement `tb_state.c`, the wiring and the triggers.**
- [ ] **Step 4: Write the reference section** in `docs/firmware/device-api.md`: "Client attributes and status telemetry (milestone 3)".
  - The six attribute keys and how each is derived (the table above).
  - The full status key table, with the source path, the type and whether the key is watched.
  - The timing rules.
  - The rule that a failed read leaves the key out.
  - Write it in the doc's existing style.
- [ ] **Step 5: Run every gate.** Run `verify_tb_link.py`, the other verify scripts, the build (0 warnings) and the QEMU selftest. All must pass.
- [ ] **Step 6: Commit**
  ```bash
  git add firmware/main/tb_state.c firmware/main/tb_state.h firmware/main/CMakeLists.txt firmware/main/app_main.c firmware/main/web_server.c firmware/scripts/verify_tb_link.py docs/firmware/device-api.md
  git commit -m "feat: tb_state publishes settings attributes and status telemetry"
  ```

### Task 5: Parity script checks the attributes and the status keys

**Files:**
- Create: `thingsboard/state_cases.json`.
- Modify:
  - `thingsboard/tb_api.py`
  - `thingsboard/parity_test.py`
  - `thingsboard/parity_cases.json` (an `attr` field on the write cases)
  - `thingsboard/README.md`
  - `firmware/scripts/verify_tb_link.py` (the drift check)

**Interfaces:**
- Produces:
  - In `tb_api.py`:
    - `client_attributes(device_id, keys) -> dict` (key → value). It calls `GET /api/plugins/telemetry/DEVICE/{id}/values/attributes/CLIENT_SCOPE?keys=a,b`. A value that is a JSON string is decoded.
    - `latest_telemetry(device_id, keys) -> dict` (key → `(ts_ms, value)`). It calls `GET /api/plugins/telemetry/DEVICE/{id}/values/timeseries?keys=...`.
  - `state_cases.json`:
    ```json
    {"attributes": [
       {"key": "cfg_pump", "http": "/api/pump/config", "drop": ["ok"]},
       {"key": "cfg_cooling", "http": "/api/cooling/config", "drop": ["ok", "status"]},
       {"key": "cfg_protection", "http": "/api/protection", "drop": ["ok", "pump_status"]},
       {"key": "hardware_map", "http": "/api/hardware/map", "drop": ["ok", "options"]}],
     "attr_present": ["wifi", "tb_link"],
     "status_keys": ["<every key of the Task 1 table, in table order>", "stack_hwm_alarm", "stack_hwm_uplink", "stack_hwm_dry_run", "stack_hwm_tb_link", "stack_hwm_tb_rpc", "tb_sent", "tb_failed"],
     "status_max_age_s": 45, "follow_timeout_s": 5, "follow_pass_s": 3}
    ```
  - In `parity_cases.json`, the write cases gain `"attr": {"key": ..., "path": ...}`. The path is relative to the attribute value:
    - pump: `cfg_pump` / `water_set1_off`;
    - protection: `cfg_protection` / `pump.prime_sec`;
    - cooling `min_off`: `cfg_cooling` / `min_off_sec`.
- New report rows in `parity_test.py`:
  - `attr <key>`: the TB value equals the HTTP reply minus `drop`, using `compare_bodies`. The ignore list is that group's read ignore list.
  - `attr <key> present` for each `attr_present` key.
  - `status <key>`: present, with age ≤ `status_max_age_s`.
  - **The follow check, for a write case with `attr`.** After the TB write succeeds, poll `client_attributes` every 250 ms, for up to `follow_timeout_s`, until the path equals the new value. After the HTTP restore, do the same until it equals the old value.
    - The row is PASS when both are seen within `follow_pass_s`, and shows the two measured delays, e.g. `PASS (follow 1.1 s / 0.9 s)`.
    - Otherwise the row is FAIL with the last seen value.
  - The rows run with the reads. The follow rows run only with `--writes`.

- [ ] **Step 1: Write the failing selftest cases** in `parity_test.py --selftest`. Use the existing fake TB/board classes with a fake attribute store.
  - Matching attribute → PASS; a different value → FAIL naming the path.
  - A missing status key → FAIL. An old ts (now − 60 s) → FAIL.
  - Follow, with the fake updating after 2 polls → PASS. With the fake never updating → FAIL after the timeout. The test patches the sleep so it runs instantly.
  - A string-encoded attribute value is decoded.
- [ ] **Step 2: Add the drift check** in `verify_tb_link.py`. It parses the `{UPLINK_SRC_*, "<path>", "<key>", <bool>}` rows of `uplink_format.c`. The check: `state_cases.json` `status_keys` equals those keys in order, followed by the 7 `tb_state` keys.
- [ ] **Step 3: Run both and see them fail.** Run `python thingsboard/parity_test.py --selftest` and `python firmware/scripts/verify_tb_link.py`; the new cases FAIL.
- [ ] **Step 4: Implement** the `tb_api` helpers, `state_cases.json`, the `attr` fields and the new runners.
- [ ] **Step 5: Run both and see them pass.**
  - `parity_test.py --selftest` prints `selftest OK` (or its existing pass line).
  - `verify_tb_link.py` passes.
  - `tb_dashboard.py --check` still passes.
  - Add a README line: the follow rows need `--writes`.
- [ ] **Step 6: Commit**
  ```bash
  git add thingsboard/state_cases.json thingsboard/tb_api.py thingsboard/parity_test.py thingsboard/parity_cases.json thingsboard/README.md firmware/scripts/verify_tb_link.py
  git commit -m "feat: parity checks the client attributes and status telemetry"
  ```

### Task 6: Dashboard pages show attributes and status; `load_keys`

**Files:**
- Modify:
  - `thingsboard/dashboard_pages.json`
  - `thingsboard/tb_dashboard.py`
  - `thingsboard/widgets/fishpump_rpc_panel.json` (the controller script)
  - `docs/firmware/device-api.md` (section 4: the pump load button gives `invalid_body`, not `read_only_field`; describe `load_keys`)
  - `docs/backlog/after-frontend.md` (mark the `load_keys` minor done)

**Interfaces:**
- Consumes: `state_cases.json` (Task 5) for the known attribute and status keys.
- Each page may have `"attributes": [<client attribute keys>]`. When the list is non-empty, `build_dashboard` adds a third widget:
  - the same `LATEST_FQN` entity table;
  - keys of type `"attribute"`;
  - title `ค่าตั้งปัจจุบัน (client attributes)`;
  - placed at row 8, col 14, size 10×8.
  - Its uid is `_uid(page_id, "attributes")`.
- The page contents:

  | Page | Attributes | Telemetry to add |
  |---|---|---|
  | pump | `cfg_pump` | `pump_phase`, `pump_countdown_sec`, `pump_cycle_sec`, `pump_cooldown_sec`, `pump_fault`, `pump_stabilizing`, `wash_running`, `wash_phase`, `wash_countdown_sec` |
  | cooling | `cfg_cooling` | the `cooling_*` status keys |
  | protection | `cfg_protection` | the `dry_run_*` status keys |
  | wifi | `wifi` | `wifi_connected`, `wifi_channel` |
  | hardware | `hardware_map` | — |
  | status | `tb_link` | `heap_min_free_bytes`, `heap_largest_block_bytes`, the five `stack_hwm_*`, `tb_sent`, `tb_failed` |

  There is no firmware page yet; milestone 5 adds it.
- `load_keys` is an optional list on an action that has `load`. After the load (and after `load_path`), the widget keeps only those top-level keys.
  - On `pump.config.set` it is: `water_set1_on`, `water_set1_off`, `water_set1_start_phase`, `water_set2_on`, `water_set2_off`, `water_set2_start_phase`, `wash_on`, `wash_off`, `wash_start_phase`, `water_auto_start`, `wash_auto_start`, `min_dwell_sec`, `relay1_polarity`, `relay2_polarity`.
  - On `cooling.config.set` it is: `auto_enable`, `curve_type`, `points`, `min_off_sec`, `test_timeout_sec`, `mode`, `compressor_min_off_sec`, `cooling_relay_polarity`.

- [ ] **Step 1: Write the failing checks** in `check_pages`, with matching broken entries in `BROKEN_FIXTURE`:
  - `attributes` must be a list of strings, each in `state_cases.json` `attributes[].key ∪ attr_present`.
  - A telemetry key that looks like a status key (it starts with `pump_`, `wash_`, `dry_run_`, `cooling_`, `wifi_`, `heap_`, `stack_hwm_` or `tb_`) must be either in `status_keys` or one of the registry keys already on the pages today. Read the registry keys from the page file's current lists; do not hard-code them.
  - `load_keys` without `load` → problem. `load_keys` that is not a non-empty list of strings → problem.
  - The built dashboard has an attributes widget for every page with attributes.
- [ ] **Step 2: Run the check and see it fail.** Run `python thingsboard/tb_dashboard.py --check`. Expected: it reports problems (the new keys are not in the pages yet, or the checker rejects the fixture).
- [ ] **Step 3: Implement** the page entries, the widget builder, the controller script's `load_keys` filter (inside the existing load callback, after `dig`) and the doc fixes.
- [ ] **Step 4: Run the gates and see them pass.**
  - `python thingsboard/tb_dashboard.py --check` prints its OK line, and the widget script parses with `node`.
  - `python thingsboard/parity_test.py --selftest` passes.
- [ ] **Step 5: Commit**
  ```bash
  git add thingsboard/dashboard_pages.json thingsboard/tb_dashboard.py thingsboard/widgets/fishpump_rpc_panel.json docs/firmware/device-api.md docs/backlog/after-frontend.md
  git commit -m "feat: dashboard shows client attributes and status telemetry; load_keys"
  ```

### Task 7: Bench round I (the controller runs it with the user; not a subagent task)

**Files:** `docs/bench-test-thingsboard.md` (new section "Milestone 3: รอบ I"), `docs/backlog/after-frontend.md`.

How the work is split:
- The user flashes with `flash_monitor.ps1` in their own terminal and types every password.
- Claude runs the scripts from the user's tab "1", which has `TB_PASSWORD` and `BOARD_PASSWORD` in its env, and drives the dashboard in the browser.
- Golden and board data stay under `thingsboard/.local/`. The board's settings are changed only by the restore-guarded parity writes.

- [ ] **I0 (before flashing):** run `python thingsboard/api_golden.py capture --label m3pre` (with the usual board arguments).
- [ ] **I1 (flash, then compare):**
  - Run `api_golden.py compare --label m3post --base m3pre`.
  - Expected: the only difference is the new `last_status` key in `uplink_preview`.
  - Also expected: no reboot loop, and `[BOARD_DIAG] hwm_uplink` ≥ 512.
- [ ] **I2 (attributes after connect):**
  - Run `python thingsboard/parity_test.py` (reads only).
  - Expected: every `attr …` row and every `status …` row passes.
- [ ] **I3 (follow):**
  - Run `parity_test.py --writes --group pump --group protection --group cooling`.
  - Expected: the follow rows pass within 3 s.
  - Record the measured delays.
- [ ] **I4 (local web edit):**
  - The user changes one pump timer on the web app and saves it.
  - Claude polls `cfg_pump`. It must change within 3 s.
  - The user then restores the value.
- [ ] **I5 (status):**
  - Watch `pump_phase` on the dashboard pump page through a phase change. It must show within about 3 s.
  - Watch the 30 s cadence of `heap_min_free_bytes` in the latest values.
- [ ] **I6 (dashboard walk-through):**
  - Every page shows its attribute widget and its status keys.
  - The pump "load current values" button followed by `pump.config.set` gives `ok:true`, which shows `load_keys` works.
- [ ] **I7 (link off):**
  - Set the host blank on the ThingsBoard card.
  - Expected: the web app, alarms and pump keep working, and `last_status` in the preview still updates (about every 30 s).
  - Restore the host. All 6 attributes come back within 5 s of the reconnect.
- [ ] **I8 (budget):**
  - Leave the dashboard open for 10 minutes.
  - Expected: internal heap free ≥ 100 KB, and every task's HWM ≥ 512 B in `BOARD_DIAG`.
- [ ] **Write the results and commit:**
  - Write the results into the bench file. Move any failures into the backlog with a one-line fix idea.
  - Commit `docs: bench milestone 3`.
