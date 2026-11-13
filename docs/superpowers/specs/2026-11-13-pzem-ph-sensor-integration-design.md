# PZEM-004T x2 Power Monitoring + SEN0708 pH Sensor Integration: Design

**Date:** 2026-11-13
**Status:** Draft, waiting for review
**Target:** `firmware/` (ESP32-S3 DevKitC-1 N16R8, ESP-IDF, C)
**Reference projects (bench-tested, read-only):**
- `C:\Users\Copter\OneDrive\Desktop\test\pzem004t`: PZEM-004T driver, two channels, one per UART
- `C:\Users\Copter\OneDrive\Desktop\test\sen0708_rs485`: SEN0708 driver, Modbus proto, two-point calibration state machine, self-test

---

## 1. Intent

The main firmware drives two pumps, a float switch and PT100 inverter cooling. We are adding two kinds of sensor that were proven in separate test projects:

1. **Electrical readings per pump** from two PZEM-004T meters: voltage, current, power, energy, frequency and power factor.
2. **Water pH and water temperature** from a DFRobot SEN0708 over RS485, with two-point calibration and offsets set from the web UI.

**Why:** the current readings will later feed a **separate, generic alarm system**, a project of its own that comes after this one. It will watch any input against user thresholds. pH is read and shown only; it does not control anything.

So this project has one more job besides showing the values: **publish every reading in a uniform, validity-flagged form on `telemetry_hub`**, so the alarm system can subscribe later without knowing about individual drivers.

## 2. Goals, non-goals, anti-goals

**Goals**
- Read both PZEMs and the SEN0708 continuously, independent of Wi-Fi and of each other.
- Show the live values on a new **Sensors** page in the existing web UI, through the existing SSE stream.
- pH calibration (two-point) and pH/temperature offsets from the web UI, behind login.
- Reset accumulated energy per PZEM from the web UI, behind login.
- Mark a sensor as faulted after repeated read failures, and clear the fault on the next good read.

**Non-goals (this project)**
- Alarms, thresholds, notifications. That is the next project.
- Using current or pH to change pump or cooling behaviour.
- Changing UART pins from the web UI (decided: **fixed in code**).
- Changing sensor Modbus address or baud rate.
- Storing history or trends.
- ThingsBoard / cloud upload.

**Anti-goals (a failure even if it "works")**
- A missing, unplugged or misbehaving sensor delays or blocks pump control, cooling, the web server or boot.
- One slow sensor delays the others (each sensor family has its own task).
- Anyone without a login can write calibration, offsets or reset energy.
- The pin map in code, `hardware_map` comments and the `/hardware` page disagree about which pins are in use.

## 3. Constraints

### 3.1 Pins and UARTs (fixed, already wired)

| UART | ESP32 TX | ESP32 RX | Device | Baud | Addr |
|---|---|---|---|---|---|
| UART0 | GPIO4 | GPIO42 | PZEM-004T, pump 1 | 9600 | 0x01 |
| UART1 | GPIO40 | GPIO41 | PZEM-004T, pump 2 | 9600 | 0x01 |
| UART2 | GPIO21 | GPIO14 | RS485_TTL (auto-direction) → SEN0708 | 4800 | 0x01 |

- None of these GPIOs is used by current code (checked: relays 5/6, float 16, MAX31865 10–13, PWM 8, buttons/LEDs 0/38/39/48).
- None of them is in the `hardware_map` user-selectable pool, so they cannot collide with a role the user picks. That stays the same.
- The comments in `hardware_map.c` (lines ~18–35) currently describe GPIO 4, 14, 21 and 38–42 as "weak pull-up", "archived peripheral wiring" and "USB OTG/boot". Those comments must be updated to say which are now UART pins. The 2026-11-10 PT100 spec's pin table also lists *planned* UART pins (1/2, 17/18) that are now wrong. Add a note to this spec rather than editing the old one.

### 3.2 Console must move off UART0

`firmware/sdkconfig.defaults` has `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, so UART0 (GPIO43/44) is the console. To use all three UARTs, the console moves to the board's USB port:

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

Consequences:
- Logs and `idf.py monitor` now use the **"USB"** connector, not the "UART" one. Flashing works on either.
- The ROM bootloader still prints on GPIO43 at reset. That is not on GPIO4, so the PZEM on UART0 does not see it.
- `sdkconfig` must be regenerated (delete it or run `idf.py reconfigure` after editing defaults). The plan must say so explicitly.

### 3.3 Existing architecture to fit into

- `telemetry_hub`: publish/subscribe with a cached snapshot. Event types today: PUMP, FLOAT, COOLING, HEALTH.
- `web_server.c`: `web_server_telemetry_cb` turns hub events into SSE `event: <name>\ndata: <json>` with a 256-byte JSON buffer. Routes live in the `s_routes[]` table. Writes use `require_auth(req)`.
- `cooling_control`: the model for "component owns a task, exposes `*_get_status()`, publishes to the hub".
- Static UI: `main/static/{index.html,app.js,style.css}`, compressed into `static_compressed/` by `scripts/optimize_assets.py`. Views are `<section id="view-...">` with a desktop nav and a mobile nav.
- 24/7 settings: TWDT with panic, 10 s timeout. Logging defaults to WARN.

## 4. Approaches considered

| | Approach | Trade-off | Verdict |
|---|---|---|---|
| **A** | Separate driver components (`pzem004t`, `sen0708`) plus two owner components (`power_monitor`, `water_quality`), each with its own task, publishing to `telemetry_hub` | More files and two tasks. Sensors cannot delay each other, power and water are separate events for the alarm system, and it matches `cooling_control`. | **Chosen** |
| B | One `sensor_bus` component, one task polling all three UARTs | Less code, but a PZEM timeout delays pH polling and calibration writes, and the alarm project has to untangle mixed concerns | Rejected |
| C | Reuse the test projects' `web_dashboard` components | Duplicate Wi-Fi/HTTP, no login | Rejected |

## 5. Design

### 5.1 Components

```
firmware/components/
  pzem004t/        driver, copied from the reference project, unchanged API
  sen0708/         driver + sen0708_proto + ph_cal, copied, unchanged API, keeps Kconfig (raw frame log)
  power_monitor/   owns 2 PZEM channels: task, status, energy-reset command, hub publish
  water_quality/   owns the SEN0708: task, stability, calibration state, command queue, hub publish
```

The drivers keep their current, tested APIs (see the reference headers `pzem004t.h`, `sen0708.h`, `sen0708_proto.h`, `ph_cal.h`). Changes are limited to what the main project needs: log tag names and log levels that suit the WARN default. They do **not** depend on `telemetry_hub` or `web_server`.

### 5.1a Reading validity contract (applies to power and water)

This is the contract the alarm project will rely on.

- **Every poll publishes exactly one event per sensor/channel**, whether it succeeded or not.
- `valid` means **this poll's read succeeded**. A single missed Modbus frame gives `valid=false`.
- `fault` means **3 or more consecutive failures, or init failed**. This is the debounced "sensor is gone" signal. The next good read clears it.
- On a failed poll the numeric fields carry the **last good values**, or 0 if there has never been one, and `valid=false`.
- The alarm project should treat **`fault` as "do not trust this sensor"**. It should evaluate thresholds only on events with `valid=true`. A single `valid=false` without `fault` is a transient miss.
- UI: show the last values. Dim them when `valid=false`, and show the red fault banner only when `fault=true`.

### 5.2 `power_monitor`

```c
#define POWER_MONITOR_CHANNELS 2   /* 0 = pump 1, 1 = pump 2 */

typedef struct {
  bool  valid;          /* this poll's read succeeded (see 5.1a) */
  bool  fault;          /* >= 3 consecutive failures, or init failed */
  float voltage_v, current_a, power_w, energy_wh, frequency_hz, pf;
  uint16_t alarms;      /* PZEM's own register, passed through */
  uint32_t last_ok_ms;  /* uptime of last good read; 0 = never */
} power_monitor_channel_t;

esp_err_t power_monitor_init(void);   /* installs UARTs, starts task; never blocks on the sensors */
bool      power_monitor_get_status(power_monitor_channel_t out[POWER_MONITOR_CHANNELS]);
esp_err_t power_monitor_request_reset_energy(uint8_t channel); /* queued; executed by the task */
```

- Pins, UART ports and addresses come from the shared fixed-bus-pin constant in `hardware_map` (§5.6), so code and the `/hardware` page have one source. *(Builder-check call.)*
- Task: every **2 s** read channel 0, then channel 1, and publish per §5.1a.
- Energy reset result (success or failure) is reported in a `notice` (text + ok) in `power_monitor` status and in `/api/sensors/status` → `power_notice`, just like the water notice. *(Builder-check call.)*
- If init fails for a channel (UART install error), that channel is permanently `fault` and the other one still runs.
- Energy reset goes through a small queue (length 2) that the task drains between reads, so the HTTP handler never touches the UART.
- Publishes **one hub event per channel per poll**: `TELEMETRY_EVT_POWER` with the channel index.

### 5.3 `water_quality`

Port of the reference project's `main.c` sensor task, minus its web code.

```c
typedef struct {
  bool  valid, fault, stable;
  float ph, temperature_c;
  float ph_offset, temp_offset; bool offsets_known;
  const char *cal_state;        /* "idle" | "waiting_point2" | "done" */
  const char *cal_error;        /* NULL or "out_of_range" | "not_stable" | ... */
  char  notice[64]; bool notice_ok;  /* result of the last command */
  uint32_t last_ok_ms;
} water_quality_status_t;

esp_err_t water_quality_init(void);
bool      water_quality_get_status(water_quality_status_t *out);
esp_err_t water_quality_request_calibrate(uint8_t point, float buffer_ph); /* ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM if queue full */
esp_err_t water_quality_request_cancel(void);
esp_err_t water_quality_request_ph_offset(float offset);
esp_err_t water_quality_request_temp_offset(float offset);
```

- UART2, GPIO21/14, 4800 baud, address 0x01, all fixed.
- Behaviour is identical to the reference spec §5.2–5.3: poll every 2 s, 3 failures mean fault, stability is 5 samples within 0.05 pH, offsets are read at boot and after each change, there is a command queue of length 4, calibration ranges are P1 3.0–5.0 and P2 8.5–10.5, the point-2 step times out after 10 min, and a refused step does not change the state.
- Calibration lives inside the sensor. The firmware stores nothing in NVS, and after a reboot the state is `idle`.
- Publishes `TELEMETRY_EVT_WATER` after every poll and after every command.

### 5.4 `telemetry_hub` changes

Add two event types and payloads. The shape is chosen so the alarm system can treat any value generically: each payload has `valid` and `fault`.

```c
TELEMETRY_EVT_POWER,
TELEMETRY_EVT_WATER,

typedef struct {
  uint8_t channel; bool valid; bool fault;
  float voltage_v, current_a, power_w, energy_wh, frequency_hz, pf;
} telemetry_power_payload_t;

typedef struct {
  bool valid; bool fault; bool stable;
  float ph, temperature_c;
} telemetry_water_payload_t;
```

The snapshot gains `power[2]`, `has_power[2]`, `water` and `has_water`. Calibration and offset details are **not** put on the hub (they are UI state, not readings). The UI gets them from `water_quality_get_status()` through REST.

### 5.5 Web API (`main/web_server.c`)

| Method | Path | Auth | Body (form-encoded) | Result |
|---|---|---|---|---|
| GET | `/sensors` | page | — | SPA shell, like `/cooling` |
| GET | `/api/sensors/status` | yes | — | full JSON below |
| POST | `/api/power/reset-energy` | yes | `{"channel":0}` | `202` / `400` / `503` |
| POST | `/api/water/calibrate` | yes | `{"point":1,"buffer_ph":4.01}` | `202` / `400` / `503` |
| POST | `/api/water/calibrate/cancel` | yes | — | `202` |
| POST | `/api/water/offset` | yes | `{"ph_offset":0.10}` or `{"temp_offset":-0.5}` | `202` / `400` / `503` |

POST bodies are **JSON**, parsed with cJSON and sent by the UI with the existing `apiPost` helper, like every other write in `web_server.c`. This differs from the reference project's form encoding. *(Builder-check call.)* The `/sensors` page route is registered the same way `/hardware` is.

`GET /api/sensors/status`:
```json
{
  "power": [
    {"channel":0,"name":"pump1","valid":true,"fault":false,"voltage":229.8,"current":1.234,
     "power":250.1,"energy_wh":1520,"frequency":50.0,"pf":0.88,"age_ms":800},
    {"channel":1, "...": "..."}
  ],
  "water": {"valid":true,"fault":false,"stable":true,"ph":7.12,"temperature_c":27.4,
            "offsets":{"known":true,"ph":0.00,"temp":0.0},
            "calibration":{"state":"idle","error":null},
            "notice":{"text":"","ok":true},"age_ms":1200}
}
```

SSE: `web_server_telemetry_cb` adds `event: power` (one per channel) and `event: water`, compact JSON matching the hub payloads, under the existing 256-byte buffer. After a POST the UI re-fetches `/api/sensors/status` to show the calibration state and notice.

Range checks: the handler rejects obvious nonsense, and the component re-checks.

### 5.6 UI: new **Sensors** page

- New nav item "เซนเซอร์" (desktop and mobile "more" menu) → `view-sensors`, in the same visual language as the cooling page.
- **Power card** ×2 (Pump 1, Pump 2): large current (A) and power (W), plus smaller V, Hz, PF and kWh. A red "sensor fault" banner shows when faulted. "Reset kWh" opens a confirm dialog.
- **Water card:** large pH, water temperature, and a "stable / settling" badge. Fault banner.
- **Calibration card:** the two-step wizard from the reference page (defaults 4.01 / 9.18), cancel, and a status line in Thai. Buttons are disabled while not stable or faulted. Every write asks for confirmation.
- **Offset card:** current offsets, input and save, reset to 0.
- Live values come from SSE. Calibration, offsets and the notice come from `/api/sensors/status` on page open and after each action.
- Rebuild `static_compressed/` with `scripts/optimize_assets.py`.

`/hardware` page: add a read-only "Fixed bus pins" list (UART0 4/42 PZEM1, UART1 40/41 PZEM2, UART2 21/14 RS485 pH). It comes from a constant in `hardware_map` (e.g. `hardware_map_fixed_bus_pins()`), so the code and UI share one source.

### 5.7 Boot order (`app_main.c`)

After `telemetry_hub_init()` and before the web server: `power_monitor_init()` and then `water_quality_init()`. Both return quickly whether or not the sensors answer. If either returns an error, log it and continue. The sensor tasks run at a priority **below** pump and cooling control.

### 5.8 Self-test

Bring the reference project's `selftest.c` (proto CRC/frames, parser errors, calibration state machine, 50 checks) over as `CONFIG_APP_SENSOR_SELFTEST`, off by default, run at boot when enabled. It needs no sensor.

## 6. Testing

1. `idf.py build` clean after the sdkconfig change. Self-test enabled once: all pass.
2. Boot with **no sensors connected**: pumps, cooling and the web UI work. All three sensors show fault after about 6 s.
3. Both PZEMs connected, pump relays toggled: current follows the pump that runs. Unplug one: only that one faults.
4. SEN0708: stable reading in water. Full two-point calibration from the web with 4.01/9.18, checked in 6.86. Rejected cases: point 2 first, buffer out of range, not stable.
5. Offsets: +0.10 moves the reading, then reset to 0.
6. Logged out: every POST returns 401.
7. Console log visible on the "USB" port.

## 7. Left to the builder

- Exact task priorities and stack sizes. Use `cooling_control` as the reference and stay below pump and cooling.
- Whether the sensor tasks subscribe to TWDT. Follow what `cooling_control`'s task does. Worst case per transaction is the PZEM driver's 500 ms timeout (SEN0708: 200 ms), well under 10 s.
- Exact CSS and markup, as long as it matches the cooling page style.
- Where the shared "fixed bus pins" constant lives inside `hardware_map`.
