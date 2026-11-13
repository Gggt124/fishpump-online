# PZEM-004T x2 + SEN0708 pH Sensor Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read two PZEM-004T power meters and one SEN0708 pH sensor on the three ESP32-S3 UARTs. Publish the readings on `telemetry_hub` under a uniform validity contract. Show them on a new Sensors page that supports pH calibration, offsets and kWh reset.

**Architecture:** The bench-tested drivers (`pzem004t`, `sen0708`) are copied in as components. Two owner components, `power_monitor` and `water_quality`, each run their own task, publish `TELEMETRY_EVT_POWER` / `TELEMETRY_EVT_WATER`, and take web commands through a FreeRTOS queue. A tiny pure-C `sensor_health` component implements the shared valid/fault rule. `web_server.c` adds SSE events, REST endpoints and a `/sensors` route. The SPA gets a new view.

**Tech Stack:** ESP-IDF v6.0.1 (C), FreeRTOS, esp_http_server + cJSON, vanilla JS SPA, Python 3 verification script, QEMU (xtensa) for pure-logic self-tests.

**Spec:** `docs/superpowers/specs/2026-11-13-pzem-ph-sensor-integration-design.md`. Read it first. Section numbers below (§) refer to it.

**Reference sources (read-only, copy from):**
- `C:\Users\Copter\OneDrive\Desktop\test\pzem004t\components\pzem004t\` (driver)
- `C:\Users\Copter\OneDrive\Desktop\test\sen0708_rs485\components\sen0708\` (driver, proto, ph_cal, Kconfig)
- `C:\Users\Copter\OneDrive\Desktop\test\sen0708_rs485\main\main.c` (sensor task, command handling to port)
- `C:\Users\Copter\OneDrive\Desktop\test\sen0708_rs485\main\selftest.c` (50 checks to port)
- `C:\Users\Copter\OneDrive\Desktop\test\sen0708_rs485\components\web_dashboard\index.html` (calibration wizard wording and flow)

## Global Constraints

- Build environment (PowerShell): `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` once per shell, then `idf.py -C firmware build`. Every task ends with this build passing with no new warnings in the touched files.
- Pins are fixed in code. UART0 TX4/RX42 → PZEM pump 1. UART1 TX40/RX41 → PZEM pump 2. UART2 TX21/RX14 → RS485 SEN0708. PZEM 9600 baud, SEN0708 4800 baud, all at address 0x01.
- Console: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`.
- Poll interval 2000 ms for both sensor tasks. Fault after 3 consecutive failures. One good read clears it.
- Validity contract §5.1a: one event per sensor/channel per poll; `valid` = this poll OK; `fault` = ≥3 consecutive failures or init failed; numeric fields hold the last good values (0 if never).
- pH calibration rules come unchanged from `ph_cal.h`: P1 3.0–5.0, P2 8.5–10.5, stability 5 samples < 0.05 pH, P2 timeout 10 min. Offset limits ±2.00 pH and ±10.0 °C.
- All write endpoints: `require_auth` → 401 JSON, then `is_same_origin(req,false)` → 403, then a JSON body via `read_request_body` + `cJSON_Parse`. Errors use `send_api_error(req, code, field, status)`.
- Sensor tasks: pinned to core 1, priority strictly below `COOLING_CONTROL_TASK_PRIORITY` and below the pump task's priority.
- No NVS storage for sensors. No changes to pump or cooling behaviour.
- UI copy is Thai, in the style of the cooling page.

## Review Focus

1. **Boot with no sensors wired.** Pumps, cooling and web must work. All three sensors must show fault after ~6 s, and boot must not stall on UART reads. Covered in Task 10 (bench) and by `power_monitor_init`/`water_quality_init` never reading the bus (Tasks 6 and 7).
2. **Malformed POST bodies** (missing field, string instead of number, NaN/huge numbers, channel 2, point 3): these must return 400 and never queue anything. Covered by curl checks in Task 10 and by handler range checks in Task 8.
3. **Button spam / queue full:** must return 503 with `queue_full` and must not block the httpd task. Covered by the curl loop in Task 10.
4. **SSE JSON larger than the 256-byte buffer:** this silently drops the event. Covered by a max-width format check in `verify_sensor_integration.py` (Task 8).
5. **SSE reconnect only fires on `/cooling` today.** After a drop the Sensors page would freeze. Covered by a source check in Task 9 and a bench check in Task 10.

---

## File Structure

| Path | Responsibility |
|---|---|
| `firmware/sdkconfig.defaults` | console → USB-Serial-JTAG |
| `firmware/components/hardware_map/{hardware_map.c,include/hardware_map.h}` | `hardware_bus_pin_t` table + `hardware_map_fixed_bus_pins()`; updated pin comments |
| `firmware/components/sensor_health/` (new) | pure-C valid/fault counter shared by both owners |
| `firmware/components/pzem004t/` (new, copied) | PZEM driver |
| `firmware/components/sen0708/` (new, copied) | SEN0708 driver + proto + ph_cal + Kconfig |
| `firmware/components/telemetry_hub/` | POWER/WATER events + snapshot fields |
| `firmware/components/power_monitor/` (new) | 2-channel PZEM task, energy-reset queue, status, notice |
| `firmware/components/water_quality/` (new) | SEN0708 task, stability, calibration, offsets, command queue, status |
| `firmware/main/app_main.c`, `main/CMakeLists.txt` | init calls + REQUIRES |
| `firmware/main/web_server.c` | SSE `power`/`water`, `/api/sensors/status`, 4 POST endpoints, `/sensors` page route, fixed pins in `/api/hardware/map` |
| `firmware/main/static/{index.html,app.js,style.css}` + `static_compressed/` | Sensors view, nav, hardware fixed-pin list |
| `firmware/test/sensor_selftest/` (new) | standalone QEMU test app for pure logic |
| `firmware/scripts/verify_sensor_integration.py` (new) | source and asset contract checks |

**Deviation from spec §5.8 (plan decision):** the self-test is a separate QEMU test app under `firmware/test/sensor_selftest/`. It is not a `CONFIG_APP_SENSOR_SELFTEST` boot option in the production firmware. The full firmware needs octal PSRAM and Wi-Fi, which QEMU does not run reliably. A separate app keeps test code out of the shipped binary.

---

### Task 1: Console move + fixed bus pin table

**Files:**
- Modify: `firmware/sdkconfig.defaults` (the "Console Output" block)
- Modify: `firmware/components/hardware_map/include/hardware_map.h`
- Modify: `firmware/components/hardware_map/hardware_map.c` (comment block ~lines 18–35, plus the new table)
- Modify: `docs/superpowers/specs/2026-11-10-pt100-max31865-inverter-cooling-design.md` (add one note line under §2.1 saying the planned UART pins there are superseded by the 2026-11-13 spec. Do not edit the table.)

**Interfaces:**
- Produces:
```c
typedef struct {
  uint8_t uart_port;     /* 0, 1, 2 */
  gpio_num_t tx_gpio;
  gpio_num_t rx_gpio;
  uint32_t baud;
  uint8_t modbus_addr;
  const char *id;        /* "pzem_pump1" | "pzem_pump2" | "rs485_ph" */
  const char *label;     /* "PZEM-004T ปั๊ม 1" | "PZEM-004T ปั๊ม 2" | "RS485 pH (SEN0708)" */
} hardware_bus_pin_t;

#define HARDWARE_BUS_PZEM_PUMP1 0
#define HARDWARE_BUS_PZEM_PUMP2 1
#define HARDWARE_BUS_RS485_PH   2
#define HARDWARE_BUS_COUNT      3

const hardware_bus_pin_t *hardware_map_fixed_bus_pins(size_t *count);
```

- [ ] **Step 1:** Replace `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` with `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` and `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`. Fix the comment to say "Console Output: USB-Serial-JTAG (UART0-2 are used by sensors)".
- [ ] **Step 2:** Add the type, macros and function to `hardware_map.h`. In `hardware_map.c`, add a `static const` table with exactly the rows (0,4,42,9600,0x01), (1,40,41,9600,0x01), (2,21,14,4800,0x01), indexed by the macros.
- [ ] **Step 3:** Rewrite the reserved-pin comment lines for GPIO 4, 14, 21 and 40–42 to say "fixed sensor UART (see hardware_map_fixed_bus_pins)". Leave GPIO 38/39 as ext button/LED. Do **not** add these pins to the allowlist.
- [ ] **Step 4:** Regenerate the config and build: `Remove-Item firmware\sdkconfig; idf.py -C firmware build`. Expected: build OK, and `firmware/sdkconfig` contains `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`.
- [ ] **Step 5:** Commit: `git commit -m "feat(hw): move console to USB-JTAG and add fixed sensor UART pin table"`

### Task 2: QEMU self-test app + `sensor_health`

**Files:**
- Create: `firmware/test/sensor_selftest/CMakeLists.txt`, `sdkconfig.defaults`, `main/CMakeLists.txt`, `main/selftest_main.c`, `main/test_sensor_health.c`, `main/selftest.h`
- Create: `firmware/components/sensor_health/{CMakeLists.txt, include/sensor_health.h, sensor_health.c}`
- Modify: `.gitignore` (add `firmware/test/sensor_selftest/build/` and `firmware/test/sensor_selftest/sdkconfig`)

**Interfaces:**
- Produces:
```c
#define SENSOR_HEALTH_FAULT_THRESHOLD 3
typedef struct { uint8_t consecutive_failures; bool valid; bool fault; bool ever_ok; } sensor_health_t;
void sensor_health_init(sensor_health_t *h);           /* valid=false, fault=false */
void sensor_health_mark_init_failed(sensor_health_t *h); /* fault=true permanently until a good read */
void sensor_health_record(sensor_health_t *h, bool read_ok);
```
- Test harness (`selftest.h`): `CHECK(cond)` macro identical to the reference `selftest.c`, global `int g_pass, g_fail`, and one `void test_<area>(void)` per file. `app_main` runs every test group and then logs exactly `SELFTEST PASS (%d checks)` or `SELFTEST FAIL (%d of %d)`.
- Test app config: `set(EXTRA_COMPONENT_DIRS ../../components/sensor_health ../../components/sen0708 ../../components/telemetry_hub)`, `idf_build_set_property(MINIMAL_BUILD ON)`. `sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`. The sen0708 and telemetry_hub dirs are added now even though they arrive in Tasks 3 and 5. Until then, comment them out.

- [ ] **Step 1: Write the failing tests** in `test_sensor_health.c`:
```c
void test_sensor_health(void) {
  sensor_health_t h; sensor_health_init(&h);
  CHECK(!h.valid && !h.fault && !h.ever_ok);
  sensor_health_record(&h, false); sensor_health_record(&h, false);
  CHECK(!h.valid && !h.fault);                 /* 2 misses: not yet fault */
  sensor_health_record(&h, false);
  CHECK(!h.valid && h.fault);                  /* 3rd miss: fault */
  sensor_health_record(&h, true);
  CHECK(h.valid && !h.fault && h.ever_ok && h.consecutive_failures == 0);
  sensor_health_record(&h, false);
  CHECK(!h.valid && !h.fault);                 /* single miss after good: transient */
  sensor_health_mark_init_failed(&h);
  CHECK(h.fault && !h.valid);
  for (int i = 0; i < 300; i++) sensor_health_record(&h, false);
  CHECK(h.fault && h.consecutive_failures == 255); /* saturates, no wrap */
}
```
- [ ] **Step 2:** Build the test app without `sensor_health.c` implemented: `idf.py -C firmware/test/sensor_selftest build`. Expected: link error for `sensor_health_record`.
- [ ] **Step 3:** Implement `sensor_health.c` (pure C, no ESP-IDF includes). Its CMake has no REQUIRES.
- [ ] **Step 4:** Build again, then run QEMU in the background: `idf.py -C firmware/test/sensor_selftest qemu`. Wait for the log line, then stop QEMU. Expected: `SELFTEST PASS (7 checks)`.
- [ ] **Step 5:** Commit: `git commit -m "test: add QEMU sensor self-test app and sensor_health component"`

### Task 3: Port `sen0708` driver + its 50 checks

**Files:**
- Create: `firmware/components/sen0708/` (copy `CMakeLists.txt`, `Kconfig`, `include/{sen0708.h,sen0708_proto.h,ph_cal.h}`, `sen0708.c`, `sen0708_proto.c`, `ph_cal.c` verbatim)
- Create: `firmware/test/sensor_selftest/main/test_sen0708.c` (the bodies of `test_build_frames`, `test_read_reply`, `test_write_replies`, `test_stability` and `test_calibration` from the reference `selftest.c`, exposed as `void test_sen0708(void)`)
- Modify: `firmware/test/sensor_selftest/CMakeLists.txt` (enable the sen0708 dir) and `main/selftest_main.c`

**Interfaces:**
- Produces: the unchanged API in `sen0708.h`, `sen0708_proto.h` and `ph_cal.h` (see the spec §5.1 and the reference headers).

- [ ] **Step 1:** Copy the test bodies into `test_sen0708.c` and register them in `selftest_main.c`. Build fails because the component is missing.
- [ ] **Step 2:** Copy the component files verbatim. The only edit allowed is the log TAG, so it reads `"sen0708"`.
- [ ] **Step 3:** Run QEMU as in Task 2 Step 4. Expected: `SELFTEST PASS (57 checks)`.
- [ ] **Step 4:** `idf.py -C firmware build` passes. The component is not referenced yet, so this proves it compiles alongside the rest.
- [ ] **Step 5:** Commit: `git commit -m "feat(sen0708): import SEN0708 RS485 driver with protocol and calibration self-tests"`

### Task 4: Port `pzem004t` driver

**Files:**
- Create: `firmware/components/pzem004t/` (copy `CMakeLists.txt`, `include/pzem004t.h`, `pzem004t.c` verbatim)

**Interfaces:**
- Produces: `pzem004t_new`, `pzem004t_delete`, `pzem004t_read`, `pzem004t_reset_energy`, `pzem004t_reading_t`, `pzem004t_config_t`, exactly as in the reference header.

- [ ] **Step 1:** Copy the files. In `CMakeLists.txt`, change `REQUIRES driver esp_driver_uart esp_timer` to `REQUIRES esp_driver_uart esp_timer freertos`, because ESP-IDF v6 has no monolithic `driver` component.
- [ ] **Step 2:** `idf.py -C firmware build` passes.
- [ ] **Step 3:** Commit: `git commit -m "feat(pzem004t): import PZEM-004T Modbus driver"`

No unit test: the driver has no pure parse function and is unchanged from the bench-tested version. It is exercised on hardware in Task 10.

### Task 5: `telemetry_hub` POWER/WATER events

**Files:**
- Modify: `firmware/components/telemetry_hub/include/telemetry_hub.h`, `telemetry_hub.c`
- Create: `firmware/test/sensor_selftest/main/test_telemetry.c`
- Modify: the test app CMake (enable the telemetry_hub dir) and `selftest_main.c`

**Interfaces:**
- Produces, appended to `telemetry_event_type_t` after `TELEMETRY_EVT_HEALTH`: `TELEMETRY_EVT_POWER`, `TELEMETRY_EVT_WATER`.
```c
#define TELEMETRY_POWER_CHANNELS 2
typedef struct {
  uint8_t channel; bool valid; bool fault;
  float voltage_v, current_a, power_w, energy_wh, frequency_hz, pf;
} telemetry_power_payload_t;
typedef struct { bool valid; bool fault; bool stable; float ph; float temperature_c; } telemetry_water_payload_t;
```
- The union gains `.power` and `.water`. The snapshot gains `telemetry_power_payload_t power[TELEMETRY_POWER_CHANNELS]; bool has_power[TELEMETRY_POWER_CHANNELS]; telemetry_water_payload_t water; bool has_water;`.
- `telemetry_hub_publish` returns `ESP_ERR_INVALID_ARG` for a POWER event with `channel >= TELEMETRY_POWER_CHANNELS` and does not cache it.

- [ ] **Step 1: Failing tests** in `test_telemetry.c`, with one `CHECK` per bullet (4 total):
  - After `telemetry_hub_init()`, publish POWER ch1 with current_a=1.5. The snapshot then has `has_power[1] && !has_power[0] && power[1].current_a == 1.5f`.
  - Publish POWER ch=2. It returns `ESP_ERR_INVALID_ARG`, and the snapshot is unchanged.
  - Publish WATER with ph=7.1, valid=true. The snapshot has `has_water && water.ph == 7.1f`.
  - A subscriber callback counting events sees 2 (the invalid publish is not delivered).
- [ ] **Step 2:** QEMU run fails (build error on unknown types).
- [ ] **Step 3:** Implement: the header types, the cache switch cases, and the channel guard.
- [ ] **Step 4:** QEMU: `SELFTEST PASS (61 checks)`. `idf.py -C firmware build` passes. Check that `web_server_telemetry_cb`'s `default: return;` still covers the new types until Task 8.
- [ ] **Step 5:** Commit: `git commit -m "feat(telemetry): add power and water telemetry events"`

### Task 6: `power_monitor` component + boot wiring

**Files:**
- Create: `firmware/components/power_monitor/{CMakeLists.txt, include/power_monitor.h, power_monitor.c}`
- Modify: `firmware/main/app_main.c` (after `telemetry_hub_init()` succeeds, before the web server starts) and `firmware/main/CMakeLists.txt` (REQUIRES `power_monitor`)

**Interfaces:**
- Consumes: `hardware_map_fixed_bus_pins` (Task 1), `sensor_health_*` (Task 2), `pzem004t_*` (Task 4), `TELEMETRY_EVT_POWER` (Task 5).
- Produces:
```c
#define POWER_MONITOR_CHANNELS 2
typedef struct {
  bool valid, fault;
  float voltage_v, current_a, power_w, energy_wh, frequency_hz, pf;
  uint16_t alarms;
  uint32_t last_ok_ms;            /* 0 = never */
} power_monitor_channel_t;
typedef struct {
  power_monitor_channel_t ch[POWER_MONITOR_CHANNELS];
  char notice[64]; bool notice_ok; /* last energy-reset result, "" if none */
} power_monitor_status_t;

esp_err_t power_monitor_init(void);
bool      power_monitor_get_status(power_monitor_status_t *out);
esp_err_t power_monitor_request_reset_energy(uint8_t channel); /* INVALID_ARG ch>=2; INVALID_STATE before init; NO_MEM queue full */
```
- CMake REQUIRES: `pzem004t sensor_health telemetry_hub hardware_map freertos esp_timer log`.

- [ ] **Step 1:** Implement `power_monitor_init`. For each channel, call `pzem004t_new` with the pins and port from `hardware_map_fixed_bus_pins()[HARDWARE_BUS_PZEM_PUMPn]`. On error, call `sensor_health_mark_init_failed` and continue. Create the queue (length 2, `uint8_t` channel), the status mutex, and the task (`"power_mon"`, 4096 stack, core 1, priority per Global Constraints). Return ESP_OK unless the queue, mutex or task cannot be created. It must not touch the bus.
- [ ] **Step 2:** Task loop. Every 2000 ms (`vTaskDelayUntil`), for ch 0 then ch 1: skip the read if the handle is NULL. Otherwise call `pzem004t_read`, then `sensor_health_record`, copy the values only on success, and publish one `TELEMETRY_EVT_POWER` per channel built from the stored status (§5.1a). Between polls, drain the reset queue with `xQueueReceive` and a timeout of the remaining period. For each item, call `pzem004t_reset_energy`. If the channel is faulted or has no handle, do not call it and set notice `"ปั๊ม N: เซนเซอร์ไม่ตอบ รีเซ็ต kWh ไม่ได้"` with ok=false. On ESP_OK the notice is `"ปั๊ม N: รีเซ็ต kWh แล้ว"` (ok=true); on any other error it is `"ปั๊ม N: รีเซ็ต kWh ล้มเหลว"` (ok=false). Logs: ESP_LOGW on fault transitions only. Use ESP_LOGD for each read.
- [ ] **Step 3:** In `app_main.c`, call `power_monitor_init()` after the telemetry hub init. On error, log at ESP_LOGE and continue.
- [ ] **Step 4:** `idf.py -C firmware build` passes.
- [ ] **Step 5:** Commit: `git commit -m "feat(power): add two-channel PZEM power monitor task"`

### Task 7: `water_quality` component + boot wiring

**Files:**
- Create: `firmware/components/water_quality/{CMakeLists.txt, include/water_quality.h, water_quality.c}`
- Modify: `firmware/main/app_main.c` (right after `power_monitor_init()`) and `firmware/main/CMakeLists.txt` (REQUIRES `water_quality`)

**Interfaces:**
- Consumes: `hardware_map_fixed_bus_pins()[HARDWARE_BUS_RS485_PH]`, `sensor_health_*`, `sen0708_*`, `ph_cal_*`, `ph_stability_*`, `TELEMETRY_EVT_WATER`.
- Produces: the spec §5.3 API exactly:
```c
typedef struct {
  bool valid, fault, stable;
  float ph, temperature_c;
  float ph_offset, temp_offset; bool offsets_known;
  const char *cal_state;   /* ph_cal_state_str() */
  const char *cal_error;   /* ph_cal_error_str(), NULL = none */
  char notice[64]; bool notice_ok;
  uint32_t last_ok_ms;
} water_quality_status_t;
esp_err_t water_quality_init(void);
bool      water_quality_get_status(water_quality_status_t *out);
esp_err_t water_quality_request_calibrate(uint8_t point, float buffer_ph);
esp_err_t water_quality_request_cancel(void);
esp_err_t water_quality_request_ph_offset(float offset);
esp_err_t water_quality_request_temp_offset(float offset);
```
  Each request returns INVALID_ARG on an obviously bad value (point not 1/2; non-finite; offset beyond `SEN0708_PH_OFFSET_LIMIT`/`SEN0708_TEMP_OFFSET_LIMIT`; buffer outside 0–14), INVALID_STATE before init, and NO_MEM when the queue (length 4) is full.
- CMake REQUIRES: `sen0708 sensor_health telemetry_hub hardware_map freertos esp_timer log`.

- [ ] **Step 1:** Port the reference `main.c` functions `poll_sensor`, `refresh_offsets`, `handle_cal_point`, `handle_offset`, `handle_cmd` and `sensor_task` into `water_quality.c`. Replace the `web_dashboard_*` calls with updates to a mutex-protected `water_quality_status_t`, and replace its fault counter with `sensor_health`. Notice strings are in Thai, following the reference meaning.
- [ ] **Step 2:** Publish one `TELEMETRY_EVT_WATER` after every poll (§5.1a) and after every handled command. Call `ph_cal_tick` each loop. Offsets are read once after the first successful poll (not in init, which must not touch the bus) and again after each offset write.
- [ ] **Step 3:** Task `"water_q"`: 4096 stack, core 1, priority equal to power_monitor's. Init failure handling is the same as Task 6.
- [ ] **Step 4:** Wire into `app_main.c`. `idf.py -C firmware build` passes.
- [ ] **Step 5:** Commit: `git commit -m "feat(water): add SEN0708 pH task with calibration and offset commands"`

### Task 8: Web API + SSE + contract script

**Files:**
- Modify: `firmware/main/web_server.c` (`web_server_telemetry_cb` switch, the new handlers, `s_routes[]`, `handle_api_hardware_map_get`)
- Create: `firmware/scripts/verify_sensor_integration.py` (same `assert_test` style and exit code as `verify_pt100_inverter_system.py`)

**Interfaces:**
- Consumes: `power_monitor_get_status`, `power_monitor_request_reset_energy`, `water_quality_*`, `hardware_map_fixed_bus_pins`.
- Produces (the UI relies on these exact keys):
  - SSE `event: power` data: `{"ch":%u,"valid":%s,"fault":%s,"v":%.1f,"a":%.3f,"w":%.1f,"wh":%.0f,"hz":%.1f,"pf":%.2f}`
  - SSE `event: water` data: `{"valid":%s,"fault":%s,"stable":%s,"ph":%.2f,"temp_c":%.1f}`
  - `GET /api/sensors/status` (auth) → the spec §5.5 JSON, plus a top-level `"power_notice":{"text":"","ok":true}`. Each power entry also has `"name":"pump1"|"pump2"`. `age_ms` is `now - last_ok_ms`, or `null` if never.
  - `POST /api/power/reset-energy` `{"channel":0|1}` → `202 {"ok":true,"queued":true}`
  - `POST /api/water/calibrate` `{"point":1|2,"buffer_ph":<0..14>}` → 202
  - `POST /api/water/calibrate/cancel` `{}` → 202
  - `POST /api/water/offset` with exactly one of `{"ph_offset":<±2>}` or `{"temp_offset":<±10>}` → 202
  - Error codes: `missing_field`, `value_out_of_range` (400), `queue_full` (503), `sensor_unavailable` (503 when the request function returns INVALID_STATE).
  - `GET /sensors`: a copy of `handle_get_hardware` (auth redirect to `/login`, serve index).
  - `GET /api/hardware/map` gains `"fixed_bus_pins":[{"id","label","uart","tx","rx","baud"}]`.

- [ ] **Step 1: Write the failing contract script** with these checks:
  - Every new URI string appears in `s_routes`.
  - Each new POST handler body contains `require_auth` and `is_same_origin`.
  - `"event: power"`/`"power"` and `"water"` cases exist in `web_server_telemetry_cb`.
  - **Width check (Review Focus 4):** format the two SSE format strings in Python with worst-case values (`ch=1`, all bools `false`, v=9999.9, a=99.999, w=99999.9, wh=9999999999, hz=99.9, pf=-1.00; ph=14.00, temp_c=-20.0). Each must be `< 256` bytes, and the full `event: power\ndata: …\n\n` must be `< 384`.
  - `index.html` contains `id="view-sensors"`, `id="nav-sensors"` and `id="m-nav-sensors"` (fails until Task 9).
  - `static_compressed/app.js.gz` decompresses to content equal to minified `app.js` containing `'/sensors'` (fails until Task 9).
- [ ] **Step 2:** Run `python firmware/scripts/verify_sensor_integration.py`. Expected: the route, SSE and UI checks FAIL.
- [ ] **Step 3:** Implement the SSE cases, the 6 handlers, the routes and the hardware-map field. Reuse `send_json`, `send_api_error` and `read_request_body`. Use a 256-byte body buffer. Validate the JSON types (`cJSON_IsNumber`) and finiteness before calling the request functions.
- [ ] **Step 4:** Run the script. Expected: all server-side checks pass, and only the two UI checks fail. `idf.py -C firmware build` passes.
- [ ] **Step 5:** Commit: `git commit -m "feat(web): add sensor status API, calibration endpoints and SSE events"`

### Task 9: Sensors page UI + Hardware fixed-pin list

**Files:**
- Modify: `firmware/main/static/index.html` (an icon symbol, a desktop nav item after `nav-cooling`, a mobile "more" entry, `<section id="view-sensors">`, and a fixed-pin block in `view-hardware`)
- Modify: `firmware/main/static/app.js` and `firmware/main/static/style.css`
- Regenerate: `firmware/main/static_compressed/*` via `python firmware/scripts/optimize_assets.py`

**Interfaces:**
- Consumes: the Task 8 endpoints and SSE keys. Use the existing `apiPost(url, body, cb)` and the confirm modal used by the cooling page.
- Produces (JS): `syncSensorsStatus(force)`, `handlePowerSseEvent(data)`, `handleWaterSseEvent(data)`, `renderSensorsStatus(json)`.

- [ ] **Step 1:** Markup, per spec §5.6:
  - Two power cards (ids `power-card-0`/`power-card-1`) with large A and W values, then V, Hz, PF and kWh, a fault banner, and a "รีเซ็ต kWh" button.
  - A water card with pH, temperature, a stable badge and a fault banner.
  - A calibration card with P1 (default 4.01) and P2 (default 9.18) inputs and buttons, a cancel button, and a status line.
  - An offset card with the current values, two inputs, save, and "รีเซ็ตเป็น 0".
  - Values stay visible and get a `.stale` class when `valid=false`. The banner shows only when `fault=true`.
- [ ] **Step 2:** In `app.js`, route `/sensors` the same way `/cooling` is routed. In `initTelemetrySSE`, add `power` and `water` listeners. **Change the reconnect condition** from `pathname === '/cooling'` to `/cooling` or `/sensors` (Review Focus 5). On open and after every POST, call `syncSensorsStatus(true)`. The calibrate buttons are disabled unless `stable && !fault`. Every write first opens a confirm dialog ("เขียนค่าคาลิเบรตจุดที่ 1 = 4.01 ลงเซนเซอร์?"). Add the `S` key force-sync for `/sensors`. Render `fixed_bus_pins` read-only in the hardware view.
- [ ] **Step 3:** Run `python firmware/scripts/optimize_assets.py`, then `python firmware/scripts/verify_sensor_integration.py`. Expected: all checks pass. Also add a source check that app.js contains `pathname === '/sensors'` inside the SSE reconnect block. `python firmware/scripts/verify_pt100_inverter_system.py` still passes.
- [ ] **Step 4:** `idf.py -C firmware build` passes.
- [ ] **Step 5:** Commit: `git commit -m "feat(ui): add sensors page with power, pH, calibration and offset cards"`

### Task 10: Bench verification on hardware

No code, unless a check fails. If one does, fix it in the owning task's files and re-run that task's checks.

- [ ] **Step 1:** Flash (`idf.py -C firmware -p COM8 flash`). Open the monitor on the **USB** port. Expected: boot log appears there.
- [ ] **Step 2 (Review Focus 1):** With all sensor connectors unplugged: the pump start/stop and cooling pages work. Within ~6 s the Sensors page shows three fault banners.
- [ ] **Step 3:** Plug in both PZEMs. Run pump 1 only: channel 0 current rises and channel 1 stays near 0. Swap pumps and see the reverse. Unplug PZEM 2: only card 2 faults. "รีเซ็ต kWh" on a healthy channel shows the success notice. On the faulted channel it shows the failure notice.
- [ ] **Step 4:** SEN0708: the reading becomes stable in water. Do a full two-point calibration (4.01 → 9.18), then check in 6.86. The rejected cases show their reasons: P2 first, buffer 6.86 as P1, pressing while settling. Offset +0.10 shifts the reading; reset to 0.
- [ ] **Step 5 (Review Focus 2, 3):** With the logged-in cookie in `$C` (PowerShell `curl.exe`):
  - The bodies `{}`, `{"channel":2}`, `{"channel":"0"}`, `{"point":3,"buffer_ph":4}`, `{"point":1,"buffer_ph":1e9}` and `{"ph_offset":5}` each return 400.
  - Sending 10 rapid `/api/water/offset` posts gives at least one 503 `queue_full`, and the web UI stays responsive.
  - Without the cookie, every POST returns 401.
- [ ] **Step 6 (Review Focus 5):** Open Sensors and restart the Wi-Fi AP or toggle the network. Live values resume within ~10 s without a page reload.
- [ ] **Step 7:** Record the results in the PR description, or tell the user.
