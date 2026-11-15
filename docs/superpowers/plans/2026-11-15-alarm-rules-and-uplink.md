# Flow Switch + Generic Alarm Rules + ThingsBoard-Ready Uplink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a flow-switch input, a generic user-configurable alarm system evaluated on the board, and a transport-agnostic layer that formats and queues everything the board will later send to ThingsBoard.

**Architecture:** `telemetry_hub` keeps its shape and gains per-group timestamps plus FLOW and ALARM events. A pure-C `signal_registry` names every value in the hub snapshot. Pure-C `alarm_rules` holds the rule model, validation, JSON and the per-tick state machine; `alarm_engine` is the thin shell (1 s task, NVS, history, hub publish). Pure-C `uplink_format` builds ThingsBoard payloads; `uplink` is the shell (cadence, store-and-forward, transport plug, preview). `web_server.c` and the SPA expose it all.

**Tech Stack:** ESP-IDF v6.0.1 (C), FreeRTOS, esp_http_server + `espressif/cjson`, vanilla JS SPA, Python 3 contract script, QEMU (xtensa) self-test app.

**Spec:** `docs/superpowers/specs/2026-11-15-alarm-rules-and-uplink-design.md`. Read it first. Section numbers (§) below refer to it.

## Global Constraints

- Branch: `feature/alarm-rules`, cut from `firmware/esp32s3-dev` (not from `main`).
- Build (PowerShell): `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` once per shell, then `idf.py -C firmware build`. Every task ends with this build passing with no new warnings in touched files (`-Werror` is on).
- Self-test: `powershell -File firmware/test/sensor_selftest/run_selftest.ps1` → last line `SELFTEST PASS (N checks)`, exit code 0. Every task that adds pure logic adds a `test_<name>.c` there, declares it in `selftest.h`, calls it from `selftest_main.c`, and adds its component to `EXTRA_COMPONENT_DIRS` and `REQUIRES`.
- Pure components (`contact_debounce`, `signal_registry`, `alarm_rules`, `uplink_format`) may depend only on `telemetry_hub` (types), each other, `cjson` and libc. No NVS, GPIO, Wi-Fi, HTTP or FreeRTOS calls in them.
- No alarm logic inside `power_monitor`, `water_quality`, `pump_control` or `cooling_control`. No behaviour change to pump or cooling. Alarms never command a relay or the cooling output.
- New tasks (`flow_sw`, `alarm_eng`, `uplink`): pinned to core 1, priority 2 (sensor tasks are 3, cooling is 4), **not** subscribed to TWDT.
- `telemetry_hub` callbacks must not block. Engine and uplink read the snapshot with `telemetry_hub_get_latest()`; they do not subscribe.
- Time is uptime in ms as `uint32_t`. All elapsed-time math is unsigned subtraction (`now - then`), never comparison of absolute values, so it survives the 49.7-day wrap.
- Validity contract (sensor spec §5.1a): never compare a threshold against a value whose signal is not `trusted`.
- NVS is written only when the user creates, edits or deletes a rule. Nothing periodic.
- Every `/api/alarms/*` and `/api/uplink/*` endpoint: `require_auth` → 401 JSON. POSTs then `is_same_origin(req,false)` → 403, then a JSON object body. Errors use `send_api_error(req, code, field, status)`.
- Signal keys are `snake_case` and identical in the REST API, rule JSON and uplink payloads. PZEM channel 0 = water pump (`pump_*`), channel 1 = chiller (`chiller_*`).
- Flow switch: GPIO 9, internal pull-up, contact closed (pin low) = water flowing, 100 ms sampling, 1000 ms stable to accept a change.
- Limits: 24 rules, rule name ≤ 96 bytes UTF-8, history 32 events, SSE JSON ≤ 256 bytes.
- Counts (§5.5) have one definition, computed only by `alarm_counts()`.
- UI copy is Thai, in the style of the Sensors page. Buffers holding Thai text are sized in bytes (3 per character).
- Commit after each task. Commit messages end with `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`.

## Review Focus

1. **Uptime wrap.** A board up for 49.7 days wraps the ms counter; delays, stale ages and "since" times must keep working and must not raise or clear alarms at the wrap. Test in Task 6 (`test_alarm_eval_wrap`).
2. **Foreign or damaged rule storage.** A blob with a wrong version, a truncated blob, or a rule that names a signal id this firmware does not have must not crash, must not block boot, and must not delete the user's other rules. Tests in Task 7 (`test_alarm_blob`) and Task 6 (unknown signal id is skipped).
3. **Hostile rule input.** Names with `"`/`\`/control bytes/invalid UTF-8, names of exactly 96 and 97 bytes, NaN/Infinity/huge numbers, strings where numbers belong, a gate pointing at the rule's own signal. Each returns 400 with the field name. Tests in Task 5.
4. **Non-finite sensor values.** A NaN or Infinity reaching the snapshot must never be printed into JSON (it is not valid JSON and would break the UI and ThingsBoard). Such a value is treated as untrusted. Tests in Task 4 (registry) and Task 10 (payload).
5. **All SSE slots taken.** With three other tabs holding the SSE stream, the Alarms page gets no events. It must still show new alarms within 10 s through the summary poll. Source check in Task 9, bench item in Task 12.

---

## File Structure

| Path | Responsibility |
|---|---|
| `firmware/components/telemetry_hub/` | + group timestamps, `TELEMETRY_EVT_FLOW`, `TELEMETRY_EVT_ALARM` |
| `firmware/components/contact_debounce/` (new, pure) | stable-for-N-ms debounce of a contact |
| `firmware/components/flow_switch/` (new) | GPIO 9 input, task, status, hub publish |
| `firmware/components/hardware_map/` | fixed-input table (flow switch), PZEM bus rename, comments |
| `firmware/components/signal_registry/` (new, pure) | signal table + `signal_registry_read()` |
| `firmware/components/alarm_rules/` (new, pure) | `alarm_rules.h`; `alarm_validate.c`, `alarm_json.c`, `alarm_eval.c`, `alarm_ring.c`, `alarm_presets.c`, `alarm_blob.c` |
| `firmware/components/alarm_engine/` (new) | task, rule set, runtime state, ack, history, hub publish |
| `firmware/components/nvs_store/` | load/save of the alarm blob |
| `firmware/components/uplink_format/` (new, pure) | ThingsBoard payload builders, timestamp math, forward accounting |
| `firmware/components/uplink/` (new) | cadence task, sink, wall clock hook, preview |
| `firmware/main/app_main.{c,h}`, `main/CMakeLists.txt` | init order, `LED_STATE_ALARM`, REQUIRES |
| `firmware/main/web_server.c` | flow in sensors status + SSE, `/alarms`, `/api/alarms/*`, `/api/uplink/preview`, SSE `alarm` |
| `firmware/main/static/{index.html,app.js,style.css}` | flow card, renames, Alarms view, banner, badge, preview card |
| `firmware/test/sensor_selftest/` | new `test_*.c` files, CMake, `idf_component.yml` for cjson |
| `firmware/scripts/verify_alarm_system.py` (new) | source and asset contract checks for this project |
| `firmware/scripts/verify_sensor_integration.py` | expectations updated for the pump/chiller rename |
| `docs/bench-test-alarms.md` (new) | Thai bench checklist for the user |

---

### Task 1: `telemetry_hub` timestamps, FLOW and ALARM events

**Files:**
- Modify: `firmware/components/telemetry_hub/include/telemetry_hub.h`, `telemetry_hub.c`, `CMakeLists.txt` (add `esp_timer` to REQUIRES)
- Test: `firmware/test/sensor_selftest/main/test_telemetry.c`

**Interfaces — Produces:**
```c
/* appended to telemetry_event_type_t, in this order */
TELEMETRY_EVT_FLOW, TELEMETRY_EVT_ALARM

typedef struct { bool flow_on; } telemetry_flow_payload_t;

typedef enum { TELEMETRY_ALARM_RAISED = 0, TELEMETRY_ALARM_CLEARED, TELEMETRY_ALARM_ACKED } telemetry_alarm_type_t;
typedef struct {
    uint32_t seq; uint16_t rule_id; uint8_t type; uint8_t severity; uint8_t signal_id;
    float value; uint8_t active_count; uint8_t critical_count; uint8_t unacked_count;
} telemetry_alarm_payload_t;

/* added to the event union: .flow, .alarm */
/* added to telemetry_snapshot_t */
telemetry_flow_payload_t flow;   bool has_flow;
telemetry_alarm_payload_t alarm; bool has_alarm;
uint32_t pump_updated_ms, float_updated_ms, cooling_updated_ms, health_updated_ms,
         power_updated_ms[TELEMETRY_POWER_CHANNELS], water_updated_ms, flow_updated_ms, alarm_updated_ms;
```
`*_updated_ms` is uptime ms at the last publish of that group, set inside `telemetry_hub_publish()` under the mutex; 0 means never, so a real time of 0 is stored as 1.

- [ ] **Step 1: Add failing checks to `test_telemetry()`**
```c
telemetry_event_t f = {.type = TELEMETRY_EVT_FLOW};
f.data.flow.flow_on = true;
telemetry_hub_publish(&f);
telemetry_hub_get_latest(&snap);
CHECK(snap.has_flow && snap.flow.flow_on && snap.flow_updated_ms != 0);
CHECK(snap.power_updated_ms[1] != 0 && snap.power_updated_ms[0] == 0); /* ch1 published earlier, ch0 never */
CHECK(snap.water_updated_ms != 0 && snap.pump_updated_ms == 0);

telemetry_event_t a = {.type = TELEMETRY_EVT_ALARM};
a.data.alarm.seq = 7; a.data.alarm.rule_id = 3; a.data.alarm.type = TELEMETRY_ALARM_RAISED;
telemetry_hub_publish(&a);
telemetry_hub_get_latest(&snap);
CHECK(snap.has_alarm && snap.alarm.seq == 7 && snap.alarm.rule_id == 3 && snap.alarm_updated_ms != 0);
```
Update the existing `CHECK(s_events_seen == 2)` to `== 4`.
- [ ] **Step 2: Run the self-test.** Expected: build error (unknown `TELEMETRY_EVT_FLOW`).
- [ ] **Step 3: Implement** the types above and the two new `case`s plus timestamp assignment in every `case` of `telemetry_hub_publish()`.
- [ ] **Step 4: Run the self-test.** Expected: `SELFTEST PASS`.
- [ ] **Step 5: Run `idf.py -C firmware build`.** Expected: success (the `default: return;` in `web_server_telemetry_cb` ignores the new events for now).
- [ ] **Step 6: Commit** `feat(telemetry): add group timestamps, flow and alarm events`.

---

### Task 2: `contact_debounce` + `flow_switch` + boot wiring

**Files:**
- Create: `firmware/components/contact_debounce/{CMakeLists.txt,include/contact_debounce.h,contact_debounce.c}`
- Create: `firmware/components/flow_switch/{CMakeLists.txt,include/flow_switch.h,flow_switch.c}`
- Modify: `firmware/components/hardware_map/include/hardware_map.h`, `hardware_map.c` (comment block ~line 26: GPIO 9 is now the flow switch)
- Modify: `firmware/main/app_main.c` (init after `water_quality_init()`; `esp_log_level_set("flow_switch", ESP_LOG_INFO)`), `firmware/main/CMakeLists.txt`
- Test: `firmware/test/sensor_selftest/main/test_contact_debounce.c` (+ CMake, `selftest.h`, `selftest_main.c`)

**Interfaces — Consumes:** `TELEMETRY_EVT_FLOW` (Task 1).
**Interfaces — Produces:**
```c
/* contact_debounce.h (pure) */
typedef struct { bool state; bool candidate; uint32_t candidate_since_ms; } contact_debounce_t;
void contact_debounce_init(contact_debounce_t *d, bool initial_state, uint32_t now_ms);
/* Feed one raw sample. Returns true exactly when d->state changes. */
bool contact_debounce_update(contact_debounce_t *d, bool raw, uint32_t now_ms, uint32_t stable_ms);

/* hardware_map.h */
#define HARDWARE_FLOW_SWITCH_GPIO GPIO_NUM_9
typedef struct { gpio_num_t gpio; const char *id; const char *label; const char *wiring; } hardware_fixed_input_t;
const hardware_fixed_input_t *hardware_map_fixed_inputs(size_t *count);
/* one row: {GPIO_NUM_9, "flow_switch", "Flow switch (น้ำไหล)", "GPIO9 → สวิตช์ (NO) → GND"} */

/* flow_switch.h */
#define FLOW_SWITCH_SAMPLE_MS 100
#define FLOW_SWITCH_STABLE_MS 1000
#define FLOW_SWITCH_HEARTBEAT_MS 5000
typedef struct { bool flow_on; bool raw_closed; uint32_t last_change_ms; } flow_switch_status_t;
esp_err_t flow_switch_init(void);                       /* GPIO + task; never blocks */
bool      flow_switch_get_status(flow_switch_status_t *out); /* false before init */
```

- [ ] **Step 1: Write `test_contact_debounce()`**
```c
contact_debounce_t d;
contact_debounce_init(&d, false, 0);
CHECK(!contact_debounce_update(&d, true, 100, 1000) && !d.state);   /* candidate starts */
CHECK(!contact_debounce_update(&d, true, 1099, 1000) && !d.state);  /* 999 ms: not yet */
CHECK(contact_debounce_update(&d, true, 1100, 1000) && d.state);    /* 1000 ms: accepted once */
CHECK(!contact_debounce_update(&d, true, 1200, 1000));              /* no repeat */
CHECK(!contact_debounce_update(&d, false, 1300, 1000));             /* chatter ... */
CHECK(!contact_debounce_update(&d, true, 1700, 1000) && d.state);   /* ... shorter than 1 s is ignored */
CHECK(!contact_debounce_update(&d, false, 2000, 1000));
CHECK(contact_debounce_update(&d, false, 3000, 1000) && !d.state);  /* other direction */
contact_debounce_init(&d, false, 0xFFFFFF00u);                      /* across the uint32 wrap */
contact_debounce_update(&d, true, 0xFFFFFF80u, 1000);
CHECK(contact_debounce_update(&d, true, 0x00000380u, 1000) && d.state);
```
- [ ] **Step 2: Run the self-test.** Expected: build error (no `contact_debounce.h`).
- [ ] **Step 3: Implement `contact_debounce`.**
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Implement `flow_switch`.** Model the structure on `components/water_quality/water_quality.c` (mutex-guarded status, task created in init). The task samples `gpio_get_level(HARDWARE_FLOW_SWITCH_GPIO) == 0` every `FLOW_SWITCH_SAMPLE_MS`, feeds the debounce with `FLOW_SWITCH_STABLE_MS`, and publishes `TELEMETRY_EVT_FLOW` on every accepted change and every `FLOW_SWITCH_HEARTBEAT_MS`. The first publish happens right after init with the initial raw level as the state. REQUIRES: `contact_debounce telemetry_hub hardware_map esp_driver_gpio freertos esp_timer log`.
- [ ] **Step 6: Wire into `app_main.c`** next to the other sensor inits; on error log and continue.
- [ ] **Step 7: Build.** Expected: success.
- [ ] **Step 8: Commit** `feat(flow): add flow switch input with debounce`.

---

### Task 3: Flow on the web + pump/chiller rename

**Files:**
- Modify: `firmware/components/hardware_map/{include/hardware_map.h,hardware_map.c}` — rename `HARDWARE_BUS_PZEM_PUMP1/2` → `HARDWARE_BUS_PZEM_PUMP` / `HARDWARE_BUS_PZEM_CHILLER`; ids `pzem_pump` / `pzem_chiller`; labels `"PZEM-004T ปั๊มน้ำ"` / `"PZEM-004T Chiller"`; comment lines for GPIO 4/42 and 40/41
- Modify: `firmware/components/power_monitor/power_monitor.c:37` (macro names only)
- Modify: `firmware/main/web_server.c` — `handle_api_sensors_status` (~4237), `web_server_telemetry_cb` (~5334), `api_hardware_add_fixed_bus_pins` (~877); delete the stale `GET /api/cooling/status` comment above the sensors block (~4210)
- Modify: `firmware/main/static/index.html` (power card titles ~1048/1063; new flow card in `.sensors-grid`; fixed-input list in `#fixed-bus-info`), `app.js` (`initTelemetrySSE`, `renderSensorsStatus`, `renderFixedBusPins`), `style.css` if needed
- Modify: `firmware/scripts/verify_sensor_integration.py` (expected strings)
- Create: `firmware/scripts/verify_alarm_system.py`

**Interfaces — Consumes:** `flow_switch_get_status()`, `hardware_map_fixed_inputs()`, snapshot `flow`, `flow_updated_ms`.
**Interfaces — Produces (wire contract):**
- `/api/sensors/status`: `power[].name` is `"pump"` (ch 0) / `"chiller"` (ch 1); new `"flow":{"on":bool,"age_ms":number|null}` where `age_ms` = now − `flow_updated_ms`.
- SSE: `event: flow`, data `{"on":true}`.
- `/api/hardware/map`: new array `fixed_inputs` of `{"id","label","gpio","wiring"}`.
- DOM ids: `flow-card`, `flow-state` (text `ไหล` / `ไม่ไหล`), `hw-fixed-inputs`.
- Card titles: `ไฟฟ้า ปั๊มน้ำ`, `ไฟฟ้า Chiller`, `การไหลของน้ำ`.

- [ ] **Step 1: Create `verify_alarm_system.py`** in the style of `verify_sensor_integration.py` (same `assert_test` helper, exit 1 on any failure) with these checks:
  - `web_server.c` contains `"\"flow\""` handling in `handle_api_sensors_status` and a `case TELEMETRY_EVT_FLOW:` producing `evt_name = "flow"`
  - `web_server.c` contains neither `"pump1"` nor `"pump2"`; `hardware_map.c` contains neither `pzem_pump1` nor `ปั๊ม 1`
  - `index.html` contains `id="flow-card"`, `id="flow-state"`, `id="hw-fixed-inputs"`, `ไฟฟ้า ปั๊มน้ำ`, `ไฟฟ้า Chiller`, and not `ไฟฟ้า ปั๊ม 1`
  - `app.js` registers `addEventListener('flow'`
- [ ] **Step 2: Run** `python firmware/scripts/verify_alarm_system.py`. Expected: FAIL on every check.
- [ ] **Step 3: Implement** the C, HTML and JS changes above. The flow card has no fault banner (spec §5.2a: no fault state).
- [ ] **Step 4: Update `verify_sensor_integration.py`** expectations that mention `pump1`/`pump2`/`ปั๊ม 1`.
- [ ] **Step 5: Run both scripts and the build.** Expected: both scripts print all-pass and exit 0; build succeeds (it regenerates `static_compressed/`).
- [ ] **Step 6: Commit** `feat(web): show flow switch and rename power channels to pump/chiller`.

---

### Task 4: `signal_registry`

**Files:**
- Create: `firmware/components/signal_registry/{CMakeLists.txt,include/signal_registry.h,signal_registry.c}` (REQUIRES `telemetry_hub`)
- Test: `firmware/test/sensor_selftest/main/test_signal_registry.c`

**Interfaces — Produces:**
```c
typedef enum { SIGNAL_KIND_NUMBER = 0, SIGNAL_KIND_BOOL } signal_kind_t;
typedef enum { SIGNAL_SRC_POWER_PUMP = 0, SIGNAL_SRC_POWER_CHILLER, SIGNAL_SRC_WATER, SIGNAL_SRC_COOLING,
               SIGNAL_SRC_PUMP, SIGNAL_SRC_FLOAT, SIGNAL_SRC_FLOW, SIGNAL_SRC_HEALTH, SIGNAL_SRC_COUNT } signal_source_t;
typedef struct {
    uint8_t id; const char *key; const char *label_th; const char *unit; const char *group;
    signal_kind_t kind; signal_source_t source; uint8_t decimals; uint8_t period_sec; /* 0 = on change */
} signal_def_t;
typedef struct {
    bool present;       /* source published at least once */
    bool trusted;       /* safe to compare / send */
    bool source_fault;  /* debounced "sensor is gone" for this source */
    double value;       /* bools are 0.0 / 1.0 */
    uint32_t age_ms;    /* now - source updated_ms; now_ms itself if never published */
} signal_value_t;

const signal_def_t *signal_registry_all(size_t *count);
const signal_def_t *signal_registry_by_id(uint8_t id);      /* NULL if unknown */
const signal_def_t *signal_registry_by_key(const char *key); /* NULL if unknown */
void signal_registry_read(const signal_def_t *def, const telemetry_snapshot_t *snap,
                          uint32_t now_ms, signal_value_t *out);
```

**The table** (ids are permanent; never renumber or reuse):

| id | key | label_th | unit | dec | kind | source | group |
|---|---|---|---|---|---|---|---|
| 1 | `pump_voltage_v` | แรงดันปั๊มน้ำ | V | 1 | num | POWER_PUMP | pump_power |
| 2 | `pump_current_a` | กระแสปั๊มน้ำ | A | 3 | num | | |
| 3 | `pump_power_w` | กำลังไฟปั๊มน้ำ | W | 1 | num | | |
| 4 | `pump_energy_wh` | พลังงานสะสมปั๊มน้ำ | Wh | 0 | num | | |
| 5 | `pump_frequency_hz` | ความถี่ไฟปั๊มน้ำ | Hz | 1 | num | | |
| 6 | `pump_pf` | PF ปั๊มน้ำ | | 2 | num | | |
| 7 | `pump_meter_fault` | มิเตอร์ไฟปั๊มน้ำไม่ตอบ | | 0 | bool | | |
| 11–17 | same seven with `chiller_` / "Chiller" | | | | | POWER_CHILLER | chiller_power |
| 21 | `water_ph` | pH | pH | 2 | num | WATER | water |
| 22 | `water_temp_c` | อุณหภูมิน้ำ (หัว pH) | °C | 1 | num | | |
| 23 | `water_ph_stable` | ค่า pH นิ่ง | | 0 | bool | | |
| 24 | `water_sensor_fault` | เซนเซอร์ pH ไม่ตอบ | | 0 | bool | | |
| 31 | `pt100_temp_c` | อุณหภูมิน้ำ (PT100) | °C | 2 | num | COOLING | cooling |
| 32 | `cooling_duty_pct` | ระดับสั่งงาน cooling | % | 1 | num | | |
| 33 | `cooling_output_mv` | แรงดันสั่ง inverter | mV | 0 | num | | |
| 34 | `cooling_mode` | โหมด cooling | | 0 | num | | |
| 35 | `cooling_lockout_sec` | เวลาหน่วงคอมเพรสเซอร์ | s | 0 | num | | |
| 36 | `pt100_fault` | เซนเซอร์ PT100 ไม่ตอบ | | 0 | bool | | |
| 37 | `cooling_on` | cooling กำลังสั่งงาน | | 0 | bool | | |
| 41 | `relay1_on` | รีเลย์ 1 (ปั๊มน้ำ) เปิด | | 0 | bool | PUMP | pump |
| 42 | `relay2_on` | รีเลย์ 2 (ล้าง) เปิด | | 0 | bool | | |
| 43 | `pump_is_running` | ระบบปั๊มทำงาน | | 0 | bool | | |
| 44 | `pump_active_set` | ชุดเวลาที่ใช้ | | 0 | num | | |
| 51 | `float_high` | ลูกลอยลอย | | 0 | bool | FLOAT | float |
| 52 | `flow_on` | น้ำไหล | | 0 | bool | FLOW | pump |
| 61 | `free_heap_bytes` | หน่วยความจำว่าง | B | 0 | num | HEALTH | system |
| 62 | `free_psram_bytes` | PSRAM ว่าง | B | 0 | num | | |
| 63 | `wifi_rssi_dbm` | สัญญาณ Wi-Fi | dBm | 0 | num | | |
| 64 | `uptime_sec` | เวลาทำงาน | s | 0 | num | | |
| 65 | `oom_strikes` | จำนวนครั้งหน่วยความจำต่ำ | | 0 | num | | |

That is 36 rows (7 + 7 + 4 + 7 + 4 + 1 + 1 + 5). `period_sec` per source: POWER_* 2, WATER 2, COOLING 1, HEALTH 5, FLOW 5, PUMP 0, FLOAT 0.

**Trust rules** (§5.3):
- POWER_*, WATER numeric and `water_ph_stable`: `trusted = valid && !fault`; `source_fault = fault`.
- `*_meter_fault`, `water_sensor_fault`: `value = fault`, `trusted = present`.
- `pt100_temp_c`: `trusted = is_sensor_valid`, `source_fault = !is_sensor_valid`. `pt100_fault = !is_sensor_valid`. `cooling_on = output_mv > 0`. Other cooling signals: `trusted = present`.
- `wifi_rssi_dbm`: `trusted = present && value != 0`.
- Any numeric value that is NaN or infinite: `trusted = false`.
- Everything else: `trusted = present`.

- [ ] **Step 1: Write `test_signal_registry()`**
```c
size_t n; const signal_def_t *all = signal_registry_all(&n);
CHECK(n == 36);
for (size_t i = 0; i < n; i++) for (size_t j = i + 1; j < n; j++)
    CHECK(all[i].id != all[j].id && strcmp(all[i].key, all[j].key) != 0);
CHECK(signal_registry_by_key("pump_current_a")->id == 2 && signal_registry_by_id(52)->kind == SIGNAL_KIND_BOOL);
CHECK(signal_registry_by_key("pump1_current_a") == NULL && signal_registry_by_id(200) == NULL);

telemetry_snapshot_t s = {0}; signal_value_t v;
signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
CHECK(!v.present && !v.trusted && v.age_ms == 5000);

s.has_power[0] = true; s.power_updated_ms[0] = 4000;
s.power[0] = (telemetry_power_payload_t){.channel = 0, .valid = true, .current_a = 1.5f};
signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
CHECK(v.present && v.trusted && v.value == 1.5 && v.age_ms == 1000);
signal_registry_read(signal_registry_by_key("chiller_current_a"), &s, 5000, &v);
CHECK(!v.present);                                   /* channels are independent */

s.power[0].valid = false;                            /* transient miss */
signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
CHECK(v.present && !v.trusted && !v.source_fault);
s.power[0].fault = true;                             /* debounced fault */
signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
CHECK(!v.trusted && v.source_fault);
signal_registry_read(signal_registry_by_key("pump_meter_fault"), &s, 5000, &v);
CHECK(v.trusted && v.value == 1.0);                  /* the fault signal itself is trusted */

s.power[0] = (telemetry_power_payload_t){.valid = true, .current_a = NAN};
signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
CHECK(!v.trusted);                                   /* Review Focus 4 */

s.has_cooling = true; s.cooling = (telemetry_cooling_payload_t){.is_sensor_valid = false, .output_mv = 3000};
signal_registry_read(signal_registry_by_key("pt100_temp_c"), &s, 5000, &v);  CHECK(!v.trusted && v.source_fault);
signal_registry_read(signal_registry_by_key("pt100_fault"), &s, 5000, &v);   CHECK(v.trusted && v.value == 1.0);
signal_registry_read(signal_registry_by_key("cooling_on"), &s, 5000, &v);    CHECK(v.trusted && v.value == 1.0);

s.has_health = true; s.health.wifi_rssi = 0;
signal_registry_read(signal_registry_by_key("wifi_rssi_dbm"), &s, 5000, &v); CHECK(v.present && !v.trusted);
s.health.wifi_rssi = -61;
signal_registry_read(signal_registry_by_key("wifi_rssi_dbm"), &s, 5000, &v); CHECK(v.trusted && v.value == -61.0);

s.has_flow = true; s.flow.flow_on = true; s.flow_updated_ms = 0xFFFFFFF0u;   /* age across the wrap */
signal_registry_read(signal_registry_by_key("flow_on"), &s, 0x00000010u, &v);
CHECK(v.trusted && v.value == 1.0 && v.age_ms == 0x20);
```
- [ ] **Step 2: Run the self-test.** Expected: build error.
- [ ] **Step 3: Implement** the table and `signal_registry_read()`.
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(signals): add signal registry over the telemetry snapshot`.

---

### Task 5: `alarm_rules` — model, validation, JSON

**Files:**
- Create: `firmware/components/alarm_rules/{CMakeLists.txt,include/alarm_rules.h,alarm_validate.c,alarm_json.c}` (REQUIRES `signal_registry telemetry_hub cjson`)
- Modify: `firmware/test/sensor_selftest/main/idf_component.yml` (create; `espressif/cjson: ^1.7.19`, same as `firmware/main/idf_component.yml`)
- Test: `firmware/test/sensor_selftest/main/test_alarm_rules.c`

**Interfaces — Produces:**
```c
#define ALARM_MAX_RULES  24
#define ALARM_NAME_BYTES 97   /* 96 bytes UTF-8 + NUL */

typedef enum { ALARM_COND_ABOVE = 0, ALARM_COND_BELOW, ALARM_COND_OUTSIDE, ALARM_COND_INSIDE,
               ALARM_COND_IS_TRUE, ALARM_COND_IS_FALSE, ALARM_COND_STALE, ALARM_COND_COUNT } alarm_cond_t;
typedef enum { ALARM_SEV_INFO = 1, ALARM_SEV_WARNING = 2, ALARM_SEV_CRITICAL = 3 } alarm_severity_t;
typedef enum { ALARM_GATE_NONE = 0, ALARM_GATE_WHEN_TRUE, ALARM_GATE_WHEN_FALSE } alarm_gate_mode_t;

typedef struct {
    uint16_t id; bool enabled; uint8_t signal_id; uint8_t condition; uint8_t severity; bool latch;
    uint8_t gate_mode; uint8_t gate_signal_id;
    float threshold; float threshold_hi; float hysteresis;
    uint16_t delay_sec; uint16_t stale_sec;
    char name[ALARM_NAME_BYTES];
} alarm_rule_t;

typedef struct { uint16_t next_id; uint8_t count; alarm_rule_t rules[ALARM_MAX_RULES]; } alarm_rule_set_t;

/* true if valid; otherwise *bad_field names the JSON field ("name","signal","condition","threshold",
 * "lo","hi","hysteresis","delay_sec","stale_sec","severity","gate"). */
bool alarm_rule_validate(const alarm_rule_t *rule, const char **bad_field);

typedef enum { ALARM_JSON_OK = 0, ALARM_JSON_MISSING_FIELD, ALARM_JSON_INVALID_TYPE, ALARM_JSON_INVALID_VALUE } alarm_json_err_t;
/* Parses and validates. *has_id is true when the object carried "id". */
alarm_json_err_t alarm_rule_from_json(const cJSON *obj, alarm_rule_t *out, bool *has_id, const char **bad_field);
cJSON *alarm_rule_to_json(const alarm_rule_t *rule);   /* caller deletes; NULL on OOM */
const char *alarm_cond_name(uint8_t cond);       /* "above","below","outside","inside","is_true","is_false","stale" */
const char *alarm_severity_name(uint8_t sev);    /* "info","warning","critical" */
```
JSON shape and field names: spec §5.4. Defaults when a field is absent: `enabled` true, `latch` false, `hysteresis` 0, `delay_sec` 0, no gate. `name`, `signal`, `condition`, `severity` and the condition's value field(s) are required.

Validation rules (§5.4): signal exists; condition fits kind (`above/below/outside/inside` need NUMBER, `is_true/is_false` need BOOL, `stale` needs `period_sec > 0`); numbers finite; `lo < hi`; `0 <= hysteresis`, and for ranges `hysteresis < (hi - lo) / 2`; `delay_sec <= 3600`; `stale_sec` in `max(5, 3 × period_sec)..3600`; severity 1..3; gate signal exists, is BOOL, differs from `signal_id`; name non-empty, ≤ 96 bytes, valid UTF-8, no byte < 0x20, no `"`, no `\`.

- [ ] **Step 1: Write `test_alarm_rules()`.** Use a helper `static alarm_json_err_t parse(const char *json, alarm_rule_t *r, const char **f)` that runs `cJSON_Parse` + `alarm_rule_from_json`. Assertions:
```c
/* round trip, Thai name, range + gate */
CHECK(parse("{\"name\":\"pH ผิดช่วง\",\"signal\":\"water_ph\",\"condition\":\"outside\",\"lo\":6.5,\"hi\":8.5,"
            "\"hysteresis\":0.1,\"delay_sec\":60,\"severity\":\"warning\","
            "\"gate\":{\"signal\":\"relay1_on\",\"when\":true}}", &r, &f) == ALARM_JSON_OK);
CHECK(r.signal_id == 21 && r.condition == ALARM_COND_OUTSIDE && r.threshold == 6.5f && r.threshold_hi == 8.5f
      && r.delay_sec == 60 && r.severity == ALARM_SEV_WARNING && r.enabled && !r.latch
      && r.gate_mode == ALARM_GATE_WHEN_TRUE && r.gate_signal_id == 41);
/* to_json then from_json gives the same rule (memcmp of the struct after zero-init) */
/* each condition parses: above(threshold), below, inside, is_true on flow_on, is_false, stale(stale_sec) */
CHECK(parse(STALE_ON_HEALTH_10S, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "stale_sec")); /* min 15 */
CHECK(parse(STALE_ON_HEALTH_15S, &r, &f) == ALARM_JSON_OK);
CHECK(parse(STALE_ON_RELAY1, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition"));     /* period 0 */
CHECK(parse(ABOVE_ON_FLOW_ON, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition"));
CHECK(parse(IS_TRUE_ON_WATER_PH, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition"));
CHECK(parse(UNKNOWN_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "signal"));
CHECK(parse(LO_NOT_BELOW_HI, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hi"));
CHECK(parse(HYST_HALF_SPAN, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hysteresis"));
CHECK(parse(NEGATIVE_HYST, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hysteresis"));
CHECK(parse(DELAY_3601, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "delay_sec"));
CHECK(parse(THRESHOLD_1E40, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "threshold"));      /* inf as float */
CHECK(parse(THRESHOLD_AS_STRING, &r, &f) == ALARM_JSON_INVALID_TYPE && !strcmp(f, "threshold"));
CHECK(parse(NAME_AS_NUMBER, &r, &f) == ALARM_JSON_INVALID_TYPE && !strcmp(f, "name"));
CHECK(parse(NO_SEVERITY, &r, &f) == ALARM_JSON_MISSING_FIELD && !strcmp(f, "severity"));
CHECK(parse(ABOVE_WITHOUT_THRESHOLD, &r, &f) == ALARM_JSON_MISSING_FIELD && !strcmp(f, "threshold"));
CHECK(parse(GATE_ON_OWN_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "gate"));
CHECK(parse(GATE_ON_NUMBER_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "gate"));
/* names: Review Focus 3 */
CHECK(parse(NAME_EMPTY, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
CHECK(parse(NAME_96_BYTES, &r, &f) == ALARM_JSON_OK);        /* 32 × "ก" */
CHECK(parse(NAME_97_BYTES, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
CHECK(parse(NAME_WITH_QUOTE, &r, &f) == ALARM_JSON_INVALID_VALUE);      /* JSON "a\"b" */
CHECK(parse(NAME_WITH_BACKSLASH, &r, &f) == ALARM_JSON_INVALID_VALUE);  /* JSON "a\\b" */
CHECK(parse(NAME_WITH_NEWLINE, &r, &f) == ALARM_JSON_INVALID_VALUE);    /* JSON "a\nb" */
/* invalid UTF-8 cannot come through cJSON; call alarm_rule_validate directly */
alarm_rule_t bad = r; memcpy(bad.name, "\xE0\xB8", 3);
CHECK(!alarm_rule_validate(&bad, &f) && !strcmp(f, "name"));
CHECK(!strcmp(alarm_cond_name(ALARM_COND_STALE), "stale") && !strcmp(alarm_severity_name(3), "critical"));
```
Define each upper-case name as a JSON string literal at the top of the test file.
- [ ] **Step 2: Run the self-test.** Expected: build error.
- [ ] **Step 3: Implement** `alarm_validate.c` and `alarm_json.c`.
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(alarm): add rule model, validation and JSON`.

---

### Task 6: `alarm_rules` — evaluation state machine and counts

**Files:**
- Create: `firmware/components/alarm_rules/alarm_eval.c`; extend `include/alarm_rules.h`, `CMakeLists.txt`
- Test: `firmware/test/sensor_selftest/main/test_alarm_eval.c`

**Interfaces — Consumes:** `alarm_rule_t`, `signal_registry_read()`.
**Interfaces — Produces:**
```c
typedef enum { ALARM_STATE_NORMAL = 0, ALARM_STATE_PENDING, ALARM_STATE_ACTIVE, ALARM_STATE_LATCHED } alarm_state_t;
typedef struct { uint8_t state; bool acked; uint32_t pending_since_ms; uint32_t active_since_ms; float last_value; } alarm_runtime_t;

typedef enum { ALARM_EVT_RAISED = 0, ALARM_EVT_CLEARED, ALARM_EVT_ACKED } alarm_event_type_t; /* == telemetry_alarm_type_t */
typedef enum { ALARM_REASON_NONE = 0, ALARM_REASON_CONDITION, ALARM_REASON_ACKED, ALARM_REASON_RULE_CHANGED } alarm_reason_t;
typedef struct { uint8_t type; uint8_t reason; float value; } alarm_transition_t;

/* One tick for one rule. Returns true and fills *out when an event is produced (at most one per call).
 * A disabled rule, or one whose signal id is unknown, is treated as not violating. */
bool alarm_rule_step(const alarm_rule_t *rule, alarm_runtime_t *rt, const telemetry_snapshot_t *snap,
                     uint32_t now_ms, alarm_transition_t *out);
/* Acknowledge. Returns false when the rule is NORMAL/PENDING or already acked. */
bool alarm_rule_ack(alarm_runtime_t *rt, alarm_transition_t *out);
/* Rule edited or deleted: state -> NORMAL; emits cleared/rule_changed if it was ACTIVE or LATCHED. */
bool alarm_rule_reset(alarm_runtime_t *rt, alarm_transition_t *out);

typedef struct { uint8_t active, critical, unacked, critical_unacked, max_severity; } alarm_counts_t;
void alarm_counts(const alarm_rule_set_t *set, const alarm_runtime_t rt[ALARM_MAX_RULES], alarm_counts_t *out);
```
Behaviour: spec §5.5 exactly (gate, trust, hysteresis per condition, event contract table, counts). `out->value` is the signal value at the transition (0 for stale).

- [ ] **Step 1: Write `test_alarm_eval()`** with helpers `snap_ph(float ph, uint32_t t)` (valid water reading, `water_updated_ms = t`), `snap_relay1(bool)`, and `step(...)` returning the event type or -1. One block per behaviour, each asserting state and event:
  - **delay:** rule `water_ph above 8.5, delay 60`. ph 9.0 at t=0 → PENDING, no event; t=59 999 → still PENDING; t=60 000 → `RAISED`, ACTIVE, `value == 9.0f`.
  - **pending drops:** ph back to 8.0 at t=30 000 → NORMAL, no event.
  - **hysteresis above:** `hysteresis 0.2`. ACTIVE at 9.0; 8.4 → still ACTIVE (needs ≤ 8.3); 8.3 → `CLEARED` reason `CONDITION`.
  - **flicker raises once:** delay 0; values 8.6, 8.45, 8.6, 8.45 → exactly one `RAISED`, no `CLEARED`.
  - **below / outside / inside:** one raise-and-clear each with `lo 6.5, hi 8.5, hysteresis 0.1` (outside clears only within 6.6..8.4; inside clears only below 6.4 or above 8.6).
  - **bool:** `flow_on is_false` raises when flow false, clears when true.
  - **gate:** `flow_on is_false, gate relay1_on true, delay 15`. Relay off + no flow → NORMAL forever. Relay on at t=0 → PENDING; relay off at t=10 000 → NORMAL; relay on at t=20 000 → `RAISED` at 35 000 (delay restarted). While ACTIVE, relay off → `CLEARED`/`CONDITION`. Gate signal not present → treated as gate closed.
  - **transient miss:** ACTIVE; next tick `valid=false, fault=false` → no change. PENDING with a transient miss → stays PENDING, timer keeps running.
  - **fault:** PENDING + `fault=true` → NORMAL. ACTIVE + `fault=true` → stays ACTIVE, no event.
  - **signal never present:** value rule → NORMAL, no event.
  - **stale:** `water_ph stale 30`. `water_updated_ms = 1000`, now 31 000 → not yet (age 30 000 is not > 30 000); now 31 001 → `RAISED`; update at 40 000 → `CLEARED`. Never published: now 30 000 → no; now 30 001 → `RAISED`.
  - **latch:** `latch=true`. ACTIVE → condition gone → LATCHED, **no event**; `alarm_rule_ack` → `CLEARED`/`ACKED`, NORMAL.
  - **ack while active:** `alarm_rule_ack` → `ACKED`, state ACTIVE, `acked`; second ack returns false; condition gone → `CLEARED`/`CONDITION` and NORMAL even with `latch=true`.
  - **reset:** ACTIVE → `alarm_rule_reset` → `CLEARED`/`RULE_CHANGED`; NORMAL → returns false.
  - **disabled / unknown signal:** `enabled=false` with a violating value → no event; `signal_id = 200` → no event, no crash (Review Focus 2).
  - **one raised, one cleared:** drive a rule through raise → latch → ack → raise → clear and count: `RAISED` == `CLEARED` == 2.
  - **counts:** set of four rules in states ACTIVE(critical, unacked), ACTIVE(warning, acked), LATCHED(critical), NORMAL → `active 3, critical 2, unacked 2, critical_unacked 2, max_severity 3`; all NORMAL → all zero.
- [ ] **Step 2: Add `test_alarm_eval_wrap()`** (Review Focus 1): `water_ph above 8.5, delay 60`; first violating tick at `now = 0xFFFFFF00`, then `now = 0xFFFFFF00 + 59 999` (wrapped) → still PENDING; `+60 000` → `RAISED`. Stale rule with `water_updated_ms = 0xFFFFFFF0`, `now = 0x00000100` → age 272 ms, not stale.
- [ ] **Step 3: Run the self-test.** Expected: build error.
- [ ] **Step 4: Implement `alarm_eval.c`.**
- [ ] **Step 5: Run the self-test.** Expected: PASS.
- [ ] **Step 6: Commit** `feat(alarm): add rule evaluation state machine and counts`.

---

### Task 7: History ring, presets, storage, `alarm_engine`, LED

**Files:**
- Create: `firmware/components/alarm_rules/{alarm_ring.c,alarm_presets.c,alarm_blob.c}`; extend `alarm_rules.h`
- Modify: `firmware/components/nvs_store/{include/nvs_store.h,nvs_store.c,CMakeLists.txt}` (REQUIRES + `alarm_rules`; update the factory-reset comment's namespace list)
- Create: `firmware/components/alarm_engine/{CMakeLists.txt,include/alarm_engine.h,alarm_engine.c}` (REQUIRES `alarm_rules signal_registry telemetry_hub nvs_store freertos esp_timer esp_hw_support log`)
- Modify: `firmware/main/app_main.h` (`LED_STATE_ALARM`), `app_main.c` (name `"alarm"`, `update_status_leds`, `hardware_ui_task`, init, log level), `main/CMakeLists.txt`
- Test: `firmware/test/sensor_selftest/main/test_alarm_store.c`

**Interfaces — Produces:**
```c
/* alarm_rules.h */
#define ALARM_HISTORY_LEN 32
typedef struct {
    uint32_t seq; uint8_t type; uint8_t reason; uint16_t rule_id; uint8_t signal_id; uint8_t severity;
    float value; uint32_t uptime_ms; char name[ALARM_NAME_BYTES];
} alarm_event_t;
typedef struct { alarm_event_t items[ALARM_HISTORY_LEN]; uint8_t head; uint8_t count; uint32_t last_seq; } alarm_ring_t;
void   alarm_ring_init(alarm_ring_t *ring);
uint32_t alarm_ring_push(alarm_ring_t *ring, const alarm_event_t *evt);  /* assigns and returns seq = ++last_seq */
/* Copies events with seq > after_seq, oldest first, up to max. *oldest_seq = lowest seq still held (0 if empty). */
size_t alarm_ring_since(const alarm_ring_t *ring, uint32_t after_seq, alarm_event_t *out, size_t max, uint32_t *oldest_seq);

void alarm_presets_fill(alarm_rule_set_t *out);   /* 13 rules, ids 1..13, all enabled=false, next_id=14 */

#define ALARM_BLOB_VERSION 1
#define ALARM_BLOB_MAX_BYTES (4 + ALARM_MAX_RULES * sizeof(alarm_rule_t))
/* Layout: [version u8][count u8][next_id u16 LE][count × alarm_rule_t]. */
size_t alarm_blob_encode(const alarm_rule_set_t *set, uint8_t *buf, size_t size);   /* 0 if it does not fit */
/* false = blob unusable (wrong version, bad length, count > max). A rule that fails alarm_rule_validate()
 * is dropped and counted in *dropped; the rest are kept. */
bool alarm_blob_decode(const uint8_t *buf, size_t len, alarm_rule_set_t *out, uint8_t *dropped);

/* nvs_store.h — namespace "alarm_cfg", key "rules_blob" */
typedef enum { NVS_STORE_ALARM_RULES_LOADED = 0, NVS_STORE_ALARM_RULES_DEFAULTS_MISSING,
               NVS_STORE_ALARM_RULES_DEFAULTS_INVALID, NVS_STORE_ALARM_RULES_DEFAULTS_ERROR } nvs_store_alarm_rules_load_status_t;
/* On any status other than LOADED, *out holds the presets. */
nvs_store_alarm_rules_load_status_t nvs_store_load_alarm_rules(alarm_rule_set_t *out);
bool nvs_store_save_alarm_rules(const alarm_rule_set_t *set);

/* alarm_engine.h */
typedef struct { alarm_counts_t counts; uint32_t last_seq; uint32_t boot_id; uint32_t last_tick_ms; bool running; } alarm_engine_summary_t;
typedef struct { uint16_t rule_id; uint8_t signal_id; uint8_t severity; uint8_t state; bool acked;
                 float value; uint32_t since_ms; char name[ALARM_NAME_BYTES]; } alarm_active_t;
#define ALARM_ENGINE_STALL_MS 5000
esp_err_t alarm_engine_init(void);
bool      alarm_engine_get_summary(alarm_engine_summary_t *out);            /* false before init */
bool      alarm_engine_get_rules(alarm_rule_set_t *out, nvs_store_alarm_rules_load_status_t *storage);
/* ESP_OK | ESP_ERR_INVALID_ARG (validation; *bad_field set) | ESP_ERR_NOT_FOUND (has_id, unknown id)
 * | ESP_ERR_NO_MEM (24 rules already) | ESP_FAIL (NVS write failed; RAM state rolled back) */
esp_err_t alarm_engine_upsert_rule(const alarm_rule_t *rule, bool has_id, alarm_rule_t *stored, const char **bad_field);
esp_err_t alarm_engine_delete_rule(uint16_t id);                            /* ESP_OK | ESP_ERR_NOT_FOUND | ESP_FAIL */
esp_err_t alarm_engine_ack(uint16_t rule_id);                               /* ESP_OK | ESP_ERR_NOT_FOUND */
size_t    alarm_engine_ack_all(void);                                       /* number acknowledged */
size_t    alarm_engine_get_active(alarm_active_t *out, size_t max);         /* ACTIVE and LATCHED rules */
size_t    alarm_engine_get_events_since(uint32_t after_seq, alarm_event_t *out, size_t max, uint32_t *oldest_seq);
uint32_t  alarm_engine_get_rules_revision(void);   /* +1 on every successful upsert or delete; 0 before init */

/* app_main.h */
LED_STATE_ALARM   /* appended to app_led_state_t; app_led_state_name() returns "alarm" */
```

**Preset table** for `alarm_presets_fill()` — spec §5.6, in this order, ids 1–13, all `enabled=false`:

| id | name | signal | condition | hyst | delay | sev | latch | gate |
|---|---|---|---|---|---|---|---|---|
| 1 | pH ผิดช่วง | water_ph | outside 6.5–8.5 | 0.1 | 60 | warning | no | — |
| 2 | น้ำร้อนเกิน | pt100_temp_c | above 32 | 0.5 | 60 | warning | no | — |
| 3 | ไฟตก/ไฟเกิน | pump_voltage_v | outside 200–245 | 2 | 5 | warning | no | — |
| 4 | รีเลย์ปั๊มน้ำค้าง | pump_current_a | above 0.1 | 0 | 10 | critical | yes | relay1_on false |
| 5 | ปั๊มน้ำเปิดแต่น้ำไม่ไหล | flow_on | is_false | — | 15 | critical | no | relay1_on true |
| 6 | น้ำไหลทั้งที่ปั๊มปิด | flow_on | is_true | — | 30 | warning | no | relay1_on false |
| 7 | ปั๊มน้ำไม่ทำงานตามคำสั่ง | pump_current_a | below 0.3 | 0 | 10 | critical | no | relay1_on true |
| 8 | Chiller ไม่ทำงานตามคำสั่ง | chiller_power_w | below 50 | 0 | 180 | critical | no | cooling_on true |
| 9 | Chiller ทำงานทั้งที่สั่งหยุด | chiller_power_w | above 200 | 0 | 120 | warning | no | cooling_on false |
| 10 | เซนเซอร์ PT100 ไม่ตอบ | pt100_fault | is_true | — | 0 | critical | no | — |
| 11 | เซนเซอร์ pH ไม่ตอบ | water_sensor_fault | is_true | — | 0 | warning | no | — |
| 12 | มิเตอร์ไฟปั๊มน้ำไม่ตอบ | pump_meter_fault | is_true | — | 0 | warning | no | — |
| 13 | มิเตอร์ไฟ Chiller ไม่ตอบ | chiller_meter_fault | is_true | — | 0 | warning | no | — |

**Engine behaviour** (§5.6):
- One mutex guards the rule set, the parallel `alarm_runtime_t[ALARM_MAX_RULES]`, the ring and the cached summary. Deleting a rule compacts both arrays together.
- Task `alarm_eng`: every 1000 ms (`vTaskDelayUntil`), take the mutex, read the snapshot, call `alarm_rule_step` for each rule, push each transition into the ring with the rule's name copied, recompute counts, release the mutex, **then** publish one `TELEMETRY_EVT_ALARM` per transition (never publish under the mutex — same rule as `pump_control`).
- `boot_id = esp_random()` at init (non-zero). `seq` comes from the ring.
- Upsert: validate → for a create assign `id = next_id++` → for an update call `alarm_rule_reset` on that rule's runtime (push the event if any) → save through `nvs_store_save_alarm_rules`; on save failure restore the previous set and return `ESP_FAIL`. Updating keeps the rule's position.
- Rule blobs are built in a heap buffer of `ALARM_BLOB_MAX_BYTES`, not on the stack.

**LED** (§5.7): in `hardware_ui_task`'s released-button branch, after the staging check and before the AP check: when the cached `critical_unacked > 0`, `s_led_state = LED_STATE_ALARM` and both LEDs blink at 1 Hz (`(release_ticks / 10) % 2`, the loop is 50 ms). Red at `STATUS_RGB_BRIGHTNESS_DEFAULT`. The summary is re-read at most once per second, like `cached_stg_type`.

- [ ] **Step 1: Write `test_alarm_store()`**
```c
/* ring */
alarm_ring_t ring; alarm_ring_init(&ring); alarm_event_t e = {0}, out[40]; uint32_t oldest;
CHECK(alarm_ring_since(&ring, 0, out, 40, &oldest) == 0 && oldest == 0);
for (int i = 0; i < 5; i++) CHECK(alarm_ring_push(&ring, &e) == (uint32_t)(i + 1));
CHECK(alarm_ring_since(&ring, 2, out, 40, &oldest) == 3 && out[0].seq == 3 && out[2].seq == 5 && oldest == 1);
CHECK(alarm_ring_since(&ring, 2, out, 2, &oldest) == 2 && out[1].seq == 4);     /* max respected, oldest first */
for (int i = 0; i < 40; i++) alarm_ring_push(&ring, &e);                         /* overrun: 45 pushed */
CHECK(alarm_ring_since(&ring, 0, out, 40, &oldest) == 32 && oldest == 14 && out[0].seq == 14 && out[31].seq == 45);

/* presets */
alarm_rule_set_t set; const char *f; alarm_presets_fill(&set);
CHECK(set.count == 13 && set.next_id == 14);
for (int i = 0; i < set.count; i++)
    CHECK(set.rules[i].id == i + 1 && !set.rules[i].enabled && alarm_rule_validate(&set.rules[i], &f));
CHECK(set.rules[4].signal_id == 52 && set.rules[4].condition == ALARM_COND_IS_FALSE
      && set.rules[4].gate_mode == ALARM_GATE_WHEN_TRUE && set.rules[4].gate_signal_id == 41 && set.rules[4].delay_sec == 15);
CHECK(set.rules[3].latch && set.rules[3].severity == ALARM_SEV_CRITICAL);

/* blob: Review Focus 2 */
static uint8_t buf[ALARM_BLOB_MAX_BYTES]; alarm_rule_set_t back; uint8_t dropped;
size_t len = alarm_blob_encode(&set, buf, sizeof(buf));
CHECK(len == 4 + 13 * sizeof(alarm_rule_t) && buf[0] == ALARM_BLOB_VERSION);
CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 0 && back.count == 13 && back.next_id == 14
      && memcmp(&back.rules[0], &set.rules[0], sizeof(alarm_rule_t)) == 0);
CHECK(!alarm_blob_decode(buf, len - 1, &back, &dropped));            /* truncated */
CHECK(!alarm_blob_decode(buf, 3, &back, &dropped));                  /* shorter than the header */
buf[0] = 9; CHECK(!alarm_blob_decode(buf, len, &back, &dropped));    /* unknown version */
buf[0] = ALARM_BLOB_VERSION; buf[1] = 25; CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* count > max */
buf[1] = 13; set.rules[2].signal_id = 200;                           /* a signal this firmware lacks */
len = alarm_blob_encode(&set, buf, sizeof(buf));
CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 1 && back.count == 12 && back.next_id == 14);
CHECK(alarm_blob_encode(&set, buf, 10) == 0);
```
- [ ] **Step 2: Run the self-test.** Expected: build error.
- [ ] **Step 3: Implement** `alarm_ring.c`, `alarm_presets.c`, `alarm_blob.c`.
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Implement `nvs_store_load_alarm_rules` / `nvs_store_save_alarm_rules`** following `nvs_store_save_pump_settings` (open, set_blob, commit, close). Key absent → `DEFAULTS_MISSING`; `alarm_blob_decode` false → `DEFAULTS_INVALID`; NVS error → `DEFAULTS_ERROR`. Log a warning with the count when rules were dropped.
- [ ] **Step 6: Implement `alarm_engine`** per the behaviour list.
- [ ] **Step 7: Wire `app_main`:** `alarm_engine_init()` after `pump_control_start_task(...)` and before session/Wi-Fi init; `esp_log_level_set("alarm_engine", ESP_LOG_INFO)`; the LED state.
- [ ] **Step 8: Build.** Expected: success.
- [ ] **Step 9: Flash and check the serial log** (USB port, COM12): boot completes, one INFO line from `alarm_engine` reporting 13 rules and storage `defaults-missing`, no repeating errors, pump auto-start behaviour unchanged, heap figures in `[BOARD_DIAG]` steady across three reports.
- [ ] **Step 10: Commit** `feat(alarm): add alarm engine, rule storage, presets and alarm LED`.

---

### Task 8: Web API for alarms + SSE

**Files:**
- Modify: `firmware/main/web_server.c` (new block after the sensors block; `s_routes[]`; `web_server_telemetry_cb`), `firmware/main/CMakeLists.txt` (REQUIRES `alarm_engine alarm_rules signal_registry`)
- Modify: `firmware/scripts/verify_alarm_system.py`

**Interfaces — Consumes:** all of `alarm_engine.h`, `alarm_rule_from_json`, `alarm_rule_to_json`, `signal_registry_all`, `signal_registry_read`.
**Interfaces — Produces (wire contract, spec §5.7):**

| Method | Path | Handler | Success | Errors |
|---|---|---|---|---|
| GET | `/alarms` | `handle_get_alarms` | SPA shell (same as `handle_get_sensors`) | — |
| GET | `/api/alarms/signals` | `handle_api_alarms_signals` | `{"ok":true,"signals":[{"key","label","unit","kind":"number"\|"bool","group","period_sec","decimals"}]}` | 401 |
| GET | `/api/alarms/values` | `handle_api_alarms_values` | `{"ok":true,"values":{"<key>":{"v":<number\|bool\|null>,"trusted":bool}}}` (`v` null when not present or not finite) | 401 |
| GET | `/api/alarms/rules` | `handle_api_alarms_rules_get` | `{"ok":true,"rules":[…],"max":24,"storage":"saved"\|"defaults"\|"invalid"}` | 401 |
| POST | `/api/alarms/rules` | `handle_api_alarms_rules_post` | `200 {"ok":true,"rule":{…}}` | 400 `missing_field` / `invalid_type` / `value_out_of_range` with the field name as message; 404 `not_found`; 409 `rules_full`; 500 `storage_failed` |
| POST | `/api/alarms/rules/delete` | `handle_api_alarms_rules_delete` | `200 {"ok":true}` | 400, 404 `not_found`, 500 |
| GET | `/api/alarms/status` | `handle_api_alarms_status` | spec §5.7 JSON; `engine_stalled` = `now − last_tick_ms > ALARM_ENGINE_STALL_MS` or engine not running | 401 |
| GET | `/api/alarms/summary` | `handle_api_alarms_summary` | `{"ok":true,"boot_id","last_seq","engine_stalled","summary":{"active","critical","unacked","critical_unacked","max_severity"}}` | 401 |
| POST | `/api/alarms/ack` | `handle_api_alarms_ack` | `200 {"ok":true,"acked":<n>}` for `{"id":n}` or `{"all":true}` | 400, 404 |

- `storage`: `LOADED` → `"saved"`, `DEFAULTS_MISSING` → `"defaults"`, the other two → `"invalid"`.
- In `active[]` and `history[]`, `state` is `"active"`/`"latched"`, `type` is `"raised"`/`"cleared"`/`"acked"`, `reason` is `null`/`"condition"`/`"acked"`/`"rule_changed"`, `severity` uses `alarm_severity_name`, `signal` is the registry key.
- Generalise the guard: `static cJSON *api_begin_json_post(httpd_req_t *req, char *body, size_t body_size, esp_err_t *sent)`; `sensors_begin_post` becomes a wrapper that passes its own 256-byte buffer. Alarm POSTs use a 768-byte buffer.
- SSE: `case TELEMETRY_EVT_ALARM:` → `evt_name = "alarm"`, format
  `{"seq":%lu,"type":"%s","rule_id":%u,"sev":%u,"active":%u,"critical":%u,"unacked":%u}`.
- Large responses (`status`, `rules`, `signals`) are built with cJSON on the heap, like `handle_api_sensors_status`; the `alarm_rule_set_t` and event arrays are heap-allocated, never on the httpd stack.

- [ ] **Step 1: Extend `verify_alarm_system.py`** with:
  - each path in the table is present in `s_routes[]` with its handler and method
  - every `handle_api_alarms_*` body contains `require_auth(`; every POST handler body contains `api_begin_json_post(`; `api_begin_json_post` contains `is_same_origin(`
  - worst-case length of the SSE alarm format (seq `4294967295`, type `cleared`, rule_id `65535`, sev `3`, counts `255`) is < 256, computed from the format string copied into the script, and the same format string appears in `web_server.c`
  - `web_server.c` contains `case TELEMETRY_EVT_ALARM:`
- [ ] **Step 2: Run the script.** Expected: the new checks FAIL.
- [ ] **Step 3: Implement** the handlers, routes and SSE case.
- [ ] **Step 4: Run the script and the build.** Expected: all checks pass; build succeeds.
- [ ] **Step 5: Commit** `feat(web): add alarm REST API and SSE event`.

---

### Task 9: Alarms page, banner and badge

**Files:**
- Modify: `firmware/main/static/index.html`, `app.js`, `style.css`
- Modify: `firmware/scripts/verify_alarm_system.py`

**Interfaces — Consumes:** the wire contract of Task 8.
**Interfaces — Produces (DOM ids and JS names the script checks):**
- View `view-alarms` at path `/alarms`; nav `nav-alarms` (desktop, after `nav-sensors`), `m-nav-alarms` (mobile "more" menu, after `m-nav-sensors`); label `แจ้งเตือน`.
- `alarm-banner` (outside the views, above them; hidden when `active == 0`; class `is-critical` when `critical > 0`), `alarm-badge` on both nav items.
- In the view: `alarm-active-list`, `alarm-rule-list`, `alarm-rule-count` (text like `5/24`), `alarm-history-list`, `alarm-add-rule`, `alarm-engine-stalled` (banner shown when `engine_stalled`), dialog `alarm-rule-dialog` with fields `alarm-f-name`, `alarm-f-signal`, `alarm-f-live` (live value), `alarm-f-condition`, `alarm-f-threshold`, `alarm-f-lo`, `alarm-f-hi`, `alarm-f-stale`, `alarm-f-hysteresis`, `alarm-f-delay`, `alarm-f-severity`, `alarm-f-latch`, `alarm-f-gate-signal`, `alarm-f-gate-when`, `alarm-f-save`.
- JS: `var ALARM_SUMMARY_POLL_MS = 10000;`, `var ALARM_VALUES_POLL_MS = 2000;`, `function initAlarmsPage()`, `function syncAlarmSummary()`, `function syncAlarmStatus()`, `localStorage` key `fishpump_alarm_seen` holding `{"boot_id":n,"seq":n}`.

**Behaviour** (§5.7):
- `syncAlarmSummary()` runs on every authenticated view every `ALARM_SUMMARY_POLL_MS`, skipped while `document.hidden`. It is started once after login and is **not** stopped by `cleanupCurrentView()`. It updates the banner and badge. On `/alarms`, when the summary's `last_seq` or `boot_id` differs from the last one seen, it calls `syncAlarmStatus()` — this is what keeps the page current when no SSE slot is free (Review Focus 5).
- `/alarms` opens SSE through the existing `initTelemetrySSE()`; add an `alarm` listener there that calls `syncAlarmSummary()` and, on `/alarms`, `syncAlarmStatus()`. Add `/alarms` to the reconnect path check (currently `'/cooling' || '/sensors'`).
- Badge number = `summary.active` + history rows with `seq` above the stored seen seq (same `boot_id`). Opening `/alarms` stores the current `boot_id` and `last_seq`.
- Times: `new Date(Date.now() - (status.uptime_ms - row.uptime_ms))`, shown as `HH:MM` (with the date when not today).
- Rule dialog: the condition list is filtered by the selected signal's `kind` and `period_sec`; value fields shown depend on the condition; the unit comes from the signal. While the dialog is open, `/api/alarms/values` is polled every `ALARM_VALUES_POLL_MS` to fill `alarm-f-live`, and the poll stops when the dialog closes. Save is disabled while its POST is in flight. A 400 marks the field named in the error. Delete and "acknowledge all" ask for confirmation.
- Rule rows show a one-line Thai summary built from the rule (for example `pH นอกช่วง 6.5–8.5 นาน 60 วิ`), an enable toggle (a POST of the whole rule with `enabled` flipped), edit and delete.

- [ ] **Step 1: Extend `verify_alarm_system.py`** with: every id above present in `index.html`; `app.js` contains `path === '/alarms'` in both `handleRoute` places, `addEventListener('alarm'`, `ALARM_SUMMARY_POLL_MS = 10000`, `fishpump_alarm_seen`, and `/alarms` in the SSE reconnect condition; inside `syncAlarmSummary` the text `syncAlarmStatus(` appears (the fallback for Review Focus 5); `static_compressed/index.html.gz` decompresses and contains `view-alarms`.
- [ ] **Step 2: Run the script.** Expected: the new checks FAIL.
- [ ] **Step 3: Implement** markup, styles and script.
- [ ] **Step 4: Run the script and the build.** Expected: all pass; build succeeds.
- [ ] **Step 5: Commit** `feat(ui): add alarms page, banner and badge`.

---

### Task 10: `uplink_format`

**Files:**
- Create: `firmware/components/uplink_format/{CMakeLists.txt,include/uplink_format.h,uplink_format.c}` (REQUIRES `signal_registry alarm_rules telemetry_hub cjson`)
- Test: `firmware/test/sensor_selftest/main/test_uplink_format.c`

**Interfaces — Produces:**
```c
#define UPLINK_FW_TYPE "fishpump"
#define UPLINK_CONTRACT_VERSION 1
#define UPLINK_TELEMETRY_BUF 2048
#define UPLINK_EVENT_BUF     768
#define UPLINK_IDENTITY_BUF  8192
#define UPLINK_RULES_BUF     12288

/* Each returns the JSON length written (NUL-terminated), or -1 if it does not fit. */
int uplink_format_telemetry(const telemetry_snapshot_t *snap, uint32_t now_ms, const alarm_counts_t *counts,
                            char *buf, size_t size);
/* ts_unix_ms == 0 -> plain {"alarm_event":{…}}; otherwise {"ts":…,"values":{"alarm_event":{…}}} */
int uplink_format_event(const alarm_event_t *evt, uint32_t boot_id, int64_t ts_unix_ms, char *buf, size_t size);
int uplink_format_identity(const char *fw_version, char *buf, size_t size);
int uplink_format_rules(const alarm_rule_set_t *set, char *buf, size_t size);

/* Wall-clock time of an event recorded at event_uptime_ms. Returns 0 when now_unix_ms == 0. */
int64_t uplink_event_ts(int64_t now_unix_ms, uint32_t now_uptime_ms, uint32_t event_uptime_ms);

typedef struct { uint32_t last_forwarded_seq; uint32_t dropped; } uplink_forward_t;
/* Call before fetching events: if the oldest event still held is newer than last_forwarded_seq + 1,
 * the gap is added to dropped and last_forwarded_seq jumps to oldest_seq - 1. Returns the gap. */
uint32_t uplink_forward_account_gap(uplink_forward_t *f, uint32_t oldest_seq);
```
Payload shapes: spec §5.8. Telemetry is one flat object: every registry signal that is `trusted` (numbers as numbers rounded to the signal's `decimals`, bools as `true`/`false`), plus `alarm_active`, `alarm_critical`, `alarm_unacked`, `alarm_max_severity`. Untrusted signals are omitted. Identity is `{"fw_type","fw_version","contract_version","signals":[{"key","label","unit","kind","group","decimals","period_sec"}]}`. Event object fields: `seq, boot_id, type, rule_id, name, signal, severity, value, reason` (`reason` null when none).

- [ ] **Step 1: Write `test_uplink_format()`**. Parse every produced string back with `cJSON_Parse` and assert on the tree:
  - **telemetry:** snapshot with pump power valid (`current_a 1.2344`), water faulted, flow on, health with `wifi_rssi 0` → has `pump_current_a == 1.234`, `flow_on` is true, `water_sensor_fault` is true, **no** `water_ph`, **no** `wifi_rssi_dbm`, **no** `chiller_current_a`; `alarm_active/critical/unacked/max_severity` equal the counts passed.
  - **non-finite (Review Focus 4):** `current_a = NAN` → output parses, has no `pump_current_a`, and the raw string contains neither `nan` nor `inf`.
  - **too small:** `uplink_format_telemetry(..., buf, 16)` returns -1.
  - **event:** `ts 0` → root has `alarm_event` with `seq 12`, `type "raised"`, `name "pH ผิดช่วง"`, `signal "water_ph"`, `severity "warning"`, `reason` null. `ts 1745000000000` → root has `ts == 1745000000000` and `values.alarm_event.seq == 12`. A `cleared`/`ACKED` event has `reason "acked"`.
  - **identity:** `fw_type "fishpump"`, `contract_version 1`, `fw_version` as passed, `signals` has 36 entries, entry with `key "pump_current_a"` has `unit "A"`, `kind "number"`, `decimals 3`, `period_sec 2`; the length is < `UPLINK_IDENTITY_BUF`.
  - **rules:** a set of 24 rules each with a 96-byte name and a gate formats to a length > 0 and < `UPLINK_RULES_BUF`, and parses to `alarm_rules` with 24 entries.
  - **ts math:** `uplink_event_ts(1745000060000, 100000, 40000) == 1745000000000`; across the wrap `uplink_event_ts(1745000000000, 0x00000100, 0xFFFFFF00) == 1745000000000 - 512`; `uplink_event_ts(0, 5, 1) == 0`.
  - **forward accounting:** `{last_forwarded_seq 3}` with `oldest_seq 2` → gap 0, unchanged; with `oldest_seq 14` → gap 10, `last_forwarded_seq 13`, `dropped 10`; `oldest_seq 0` (empty ring) → gap 0.
- [ ] **Step 2: Run the self-test.** Expected: build error.
- [ ] **Step 3: Implement `uplink_format.c`.**
- [ ] **Step 4: Run the self-test.** Expected: PASS.
- [ ] **Step 5: Commit** `feat(uplink): add ThingsBoard payload formatting`.

---

### Task 11: `uplink` shell, preview endpoint and card

**Files:**
- Create: `firmware/components/uplink/{CMakeLists.txt,include/uplink.h,uplink.c}` (REQUIRES `uplink_format alarm_engine alarm_rules signal_registry telemetry_hub app_config freertos esp_timer heap log`)
- Modify: `firmware/main/app_main.c` (init after `alarm_engine_init()`, log level), `main/CMakeLists.txt`, `web_server.c`, `static/index.html`, `static/app.js`
- Modify: `firmware/scripts/verify_alarm_system.py`

**Interfaces — Consumes:** `uplink_format.h`, `alarm_engine_get_events_since`, `alarm_engine_get_summary`, `alarm_engine_get_rules`, `alarm_engine_get_rules_revision`.
**Interfaces — Produces:**
```c
typedef enum { UPLINK_MSG_TELEMETRY = 0, UPLINK_MSG_ATTRIBUTES } uplink_msg_kind_t;
typedef struct {
    bool      (*is_connected)(void *ctx);
    esp_err_t (*send)(void *ctx, uplink_msg_kind_t kind, const char *json, size_t len); /* ESP_OK = accepted */
    void       *ctx;
} uplink_sink_t;

#define UPLINK_INTERVAL_SEC 30
#define UPLINK_MIN_GAP_MS   2000
#define UPLINK_TOPIC_TELEMETRY  "v1/devices/me/telemetry"   /* for the MQTT project; unused here */
#define UPLINK_TOPIC_ATTRIBUTES "v1/devices/me/attributes"

esp_err_t uplink_init(void);
esp_err_t uplink_set_sink(const uplink_sink_t *sink);             /* NULL = none; copied */
void      uplink_set_wall_clock(int64_t (*now_unix_ms)(void));    /* NULL = none */
/* Whole preview as a malloc'd JSON string (caller frees), NULL on OOM or before init. */
char     *uplink_preview_json(void);
```
- HTTP: `GET /api/uplink/preview` → `handle_api_uplink_preview`, login required, body is `uplink_preview_json()`:
  `{"ok":true,"sink":"none"|"set","connected":bool,"interval_sec":30,"pending_events":n,"dropped":n,"last_telemetry":{…}|null,"last_event":{…}|null,"identity":{…},"rules":{…}}`.
- DOM: card `uplink-preview-card` at the end of `view-status`, a `<details>` titled `ข้อมูลที่จะส่งขึ้น ThingsBoard (ตัวอย่าง)`, summary line `uplink-preview-summary`, `<pre id="uplink-preview-json">`, button `uplink-preview-refresh`. It loads only when the details element is opened or refresh is pressed.

**Behaviour** (§5.8). Task `uplink`, 1 s tick:
1. Read the snapshot and the engine summary.
2. Telemetry is due when `UPLINK_INTERVAL_SEC` has elapsed since the last one, or when `relay1_on`, `relay2_on`, `float_high` or `flow_on` differs from the last formatted values or the engine's `last_seq` changed — but never sooner than `UPLINK_MIN_GAP_MS` after the previous one. When due, format into the persistent telemetry buffer (kept for the preview); if a sink is set and connected, send it. A failed or skipped send is not retried.
3. Events: `uplink_forward_account_gap()`, then fetch events after `last_forwarded_seq`. For each, in order: format (with `uplink_event_ts` when a wall clock is set) into the persistent event buffer; if a sink is set and connected, send it and advance `last_forwarded_seq` only on `ESP_OK`; otherwise stop for this tick. `pending_events = summary.last_seq − last_forwarded_seq`.
4. Attributes: when the sink changes from not-connected to connected, send identity then rules. When `alarm_engine_get_rules_revision()` changes while connected, send rules.
- The telemetry and event buffers are allocated once at init with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Identity and rules buffers are allocated for the build and freed after the send or after the preview is built.
- `fw_version` is `APP_TEMPLATE_FIRMWARE_VERSION`.
- With no sink (this project's normal state) nothing is sent, events stay pending in the engine ring, and `dropped` grows once more than 32 events are unsent.

- [ ] **Step 1: Extend `verify_alarm_system.py`** with: route `/api/uplink/preview` → `handle_api_uplink_preview`, whose body contains `require_auth(`; `index.html` has `uplink-preview-card`, `uplink-preview-json`, `uplink-preview-refresh`; `uplink.h` defines `UPLINK_INTERVAL_SEC 30`; `uplink.c` does not contain `telemetry_hub_subscribe` (it must poll).
- [ ] **Step 2: Run the script.** Expected: the new checks FAIL.
- [ ] **Step 3: Implement** `uplink`, the handler and the card.
- [ ] **Step 4: Run the script, the self-test and the build.** Expected: all pass.
- [ ] **Step 5: Flash and check the serial log** for 2 minutes: no repeating errors; `[BOARD_DIAG]` heap and PSRAM figures steady; task count grew by exactly three compared with Task 3's firmware (`flow_sw`, `alarm_eng`, `uplink`).
- [ ] **Step 6: Commit** `feat(uplink): add uplink task, transport plug and preview`.

---

### Task 12: Bench checklist and bench run

**Files:**
- Create: `docs/bench-test-alarms.md`
- Modify: `docs/backlog/alarm-rules.md` (status line at the top: implemented, pointing at the spec, plan and bench doc)

The user runs web checks himself with the board in AP mode (joining the board's AP from the development PC drops its internet). The implementer flashes, watches the serial log, and writes the checklist.

- [ ] **Step 1: Write `docs/bench-test-alarms.md`** in Thai, in the format of `docs/bench-test-sensors.md` (numbered items, each with "ทำ" and "ต้องเห็น", a tick box, and a line for notes). It contains items 0–10 of spec §6 "Bench", expanded into concrete steps, plus:
  - an item for Review Focus 5: open the dashboard in three other tabs on `/cooling` or `/sensors`, open `/alarms` in a fourth, trigger an alarm → it appears on the Alarms page within 10 s;
  - an item checking the rule dialog shows a live value and marks the bad field on invalid input (empty name, low bound above high bound);
  - the curl commands for the 401/403 checks, each with `-H "Origin: http://192.168.4.1"` where a 403 is **not** expected.
- [ ] **Step 2: Run the three automated gates and record their output in the commit message body:**
  `powershell -File firmware/test/sensor_selftest/run_selftest.ps1` → `SELFTEST PASS`;
  `python firmware/scripts/verify_alarm_system.py` and `python firmware/scripts/verify_sensor_integration.py` → all pass;
  `idf.py -C firmware build` → success.
- [ ] **Step 3: Flash, then watch the serial log for 10 minutes** (USB port, COM12) with the default rule set: boot normal, no new repeating errors, heap steady across the `[BOARD_DIAG]` reports.
- [ ] **Step 4: Commit** `docs: add alarm bench-test checklist`.
- [ ] **Step 5: Hand the checklist to the user** and wait for his report. Fix only the items he reports as failed, each as its own commit with a self-test or script check that would have caught it where one is possible.

Real-sensor checks (pH out of range, real pump and chiller currents, the three placeholder thresholds, which PZEM answers when mains is off) wait for sensor bench Part B and are listed at the end of the checklist as "รอทดสอบกับอุปกรณ์จริง".
