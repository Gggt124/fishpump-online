# Generic Alarm Rules + ThingsBoard-Ready Uplink Layer: Design

**Date:** 2026-11-15
**Status:** Draft, waiting for review
**Target:** `firmware/` (ESP32-S3 DevKitC-1 N16R8, ESP-IDF, C), branch `firmware/esp32s3-dev`
**Starting notes:** `docs/backlog/alarm-rules.md`
**Builds on:** `docs/superpowers/specs/2026-11-13-pzem-ph-sensor-integration-design.md` (validity contract §5.1a)

---

## 1. Intent

The firmware reads electrical power for the water pump and the chiller (one PZEM each), water pH/temperature (SEN0708), tank water temperature (PT100, the input of the cooling control), pump/relay/float state and system health. All of it already flows through `telemetry_hub`. Nothing watches those values.

This project adds three things:

0. **A flow switch input** (added by the user on 2026-11-15, built first). A paddle switch in the water pipe, a plain contact wired GPIO → switch → GND like the float switch. It is the direct way to know water is actually moving, and it is the first sensor added through the new registry.
1. **A generic alarm system.** The user picks any input, sets their own threshold, and is told when it is out of bounds. It is not tied to any one sensor. A new sensor later means one new table row, not an engine change.
2. **A ThingsBoard-ready uplink layer.** Everything the board will send to ThingsBoard (sensor values, alarm state, alarm events, the rule list) is formatted and queued on the firmware side now, behind a transport plug. The next project only has to add the MQTT transport.

**Where this sits in the bigger plan.** The user intends a ThingsBoard-based platform serving several customers (one account each), several devices per customer, and devices running different firmware types, with a frontend app that renders each type appropriately. None of that is designed yet. The agreed order is:

1. **This project:** firmware alarms + uplink layer. Works with no server.
2. **ThingsBoard foundation + MQTT connector**, done together so each side proves the other: a ThingsBoard preset (device profile) for this firmware type, the customer/account structure, device registration, and the firmware's MQTT transport.
3. **Frontend app**, once the connection points and the data contract are settled.

What this project owes the later ones is a **device data contract that does not assume this is the only firmware type**: the device identifies its type and contract version and describes its own signals (§5.8).

**Why rules live in firmware:** the pond controller must keep detecting problems when the internet or the server is down. The board is the single place thresholds are evaluated. ThingsBoard, which the wider system will have anyway, handles long-term storage and forwarding to social apps and the frontend app. The firmware does not duplicate that.

## 2. Goals, non-goals, anti-goals

**Goals**
- Read the flow switch, debounce it, publish it on `telemetry_hub`, and show it on the Sensors page.
- User-defined rules (up to 24) over any registered signal: above, below, outside range, inside range, bool is true/false, data stale. Sensor faults are bool signals, so "sensor is gone" is an ordinary rule.
- Per rule: hysteresis, raise delay, severity, optional latch-until-acknowledged, optional gate ("only while pump 1 is on/off").
- Alarms evaluated on the board every second, with no network needed.
- Web UI: an **Alarms** page (active alarms, rule editor, recent history), a banner and a nav badge on every page, and the status LEDs blinking on a critical alarm.
- One **signal registry** shared by the alarm engine, the REST API and the uplink, so a signal has the same name everywhere.
- Uplink layer: ThingsBoard-format telemetry and attribute messages, a send cadence, a store-and-forward path for alarm events, a transport plug, and a preview endpoint for bench checks.
- Rule JSON has one shape and one validator, used by the web API now and by a ThingsBoard downlink later.

**Non-goals (this project)**
- MQTT client, TLS, device token setup, SNTP. That is the next project.
- Setting up ThingsBoard itself (device profile, rule chains, customers, device registration) and anything about the frontend app.
- Receiving rules or commands from ThingsBoard. Only the shared rule format and validator are prepared.
- Telegram, LINE, webhooks, buzzer. ThingsBoard forwards notifications.
- New hardware inputs to diagnose the chiller directly (inverter fault contact, 0–10 V read-back). The chiller is judged from command versus measured power only (§5.6).
- Alarm history that survives reboot, and real date/time on the board. ThingsBoard stores history.
- Any automatic action on pumps, relays or cooling. Alarms only report. Auto-cutoff is a separate safety discussion. In particular the flow switch does **not** stop the pump.
- Choosing the flow switch pin from the web UI. It is fixed in code, like the sensor UART pins.
- Rules combining several inputs beyond the single gate (no AND/OR expressions, no rate-of-change).
- The deferred minor fixes on the Sensors page listed in the backlog §8 (notice sequence numbers and so on).

**Anti-goals (a failure even if it "works")**
- The alarm engine or uplink delays or blocks pump control, cooling, the sensor tasks, the web server or boot.
- A stale number from a faulted sensor is compared against a threshold, so a dead sensor looks "normal" or raises a false value alarm.
- A value hovering at a threshold produces a stream of raise/clear events.
- Anyone without a login can read rules, edit rules or acknowledge alarms.
- A signal is called one thing in the alarm API and another in the uplink payload.
- Periodic NVS writes. Rules are written only when the user edits them.

## 3. Constraints (what exists today)

- **`telemetry_hub`** (`firmware/components/telemetry_hub/`): publish/subscribe plus a cached `telemetry_snapshot_t` read with `telemetry_hub_get_latest()`. Max 4 subscribers; the only one today is `web_server_telemetry_cb`. Callbacks must not block. The snapshot has **no timestamps**.
- **Publish cadence:** power every 2 s per channel, water every 2 s, cooling every 1 s, health every 5 s (`app_main` loop). Pump and float publish on change. Power and water publish on every poll whether it succeeded or not.
- **Validity contract (sensor spec §5.1a):** `valid` = this poll succeeded; `fault` = 3+ consecutive failures or init failed; numeric fields keep the last good value. Thresholds must only be evaluated on trustworthy readings.
- **Sensor tasks are not in TWDT on purpose**, so a hung sensor task cannot reboot the pump controller. Nothing currently notices a hung sensor task.
- **`web_server.c`:** routes in `s_routes[]` (GET and POST, plus one DELETE for `/api/confirm`). Writes use the guard `sensors_begin_post()` (auth + same origin + JSON object, 256-byte body). SSE events are built in `web_server_telemetry_cb` with a 256-byte JSON buffer and broadcast through `httpd_queue_work`. `/api/events` has no auth; 3 SSE clients max.
- **`nvs_store`** owns all NVS access. Settings are versioned blobs (`pump_blob`, v3, append-only). The NVS partition is 24 KB.
- **LEDs:** `hardware_ui_task` in `app_main.c` drives the WS2812 (GPIO48) and the external LED (GPIO39) from `app_led_state_t`, on a 50 ms loop.
- **UI:** single-page app in `main/static/{index.html,app.js,style.css}`, views are `<section id="view-...">`, routing in `handleRoute()`, desktop nav plus mobile bottom nav with a "more" menu. The UI opens the SSE stream only on `/cooling` and `/sensors` and closes it on every route change; other views poll their own status endpoint on a timer. Assets are minified by `scripts/optimize_assets.py` during the build. UI text is Thai.
- **Self-test:** `firmware/test/sensor_selftest/` builds pure-C components for QEMU and runs `CHECK()` assertions (`run_selftest.ps1`).
- **Wi-Fi:** STA with saved profiles already works. The board is AP-only on the bench, so nothing in this project may need the internet to be tested.
- **Thai text:** 3 bytes per character in UTF-8. Size buffers for it (a past build failed on `-Werror=format-truncation`).

## 4. Approaches considered

How the alarm engine and the uplink get their values:

| | Approach | Trade-off | Verdict |
|---|---|---|---|
| **A** | Central signal registry over the existing snapshot; alarm engine evaluates all rules on a 1 s tick | Up to 1 s detection latency (sensors poll every 1–2 s anyway). Hub keeps its shape and gains only timestamps. Stale detection is direct. All logic is pure and testable in QEMU. Pump and cooling code untouched. | **Chosen by the user** |
| B | Engine subscribes to hub events and evaluates the affected rules per event | Immediate reaction, but delay timers and stale detection still need a periodic tick, so two mechanisms. Harder to test. Uses one of four subscriber slots. | Rejected |
| C | Rebuild the hub as a flat key/value signal table | Cleanest long term, but rewrites the hub, every publisher and the SSE code, while the sensor work's real-hardware bench (Part B) is still pending | Rejected |

Decided in conversation with the user:
- Rules are evaluated **in firmware**, not in ThingsBoard, so alarms work offline.
- Storage of history and outbound notifications belong to **ThingsBoard**.
- The firmware side of the ThingsBoard link is prepared **now**; only the MQTT transport is left for the next project.
- Rules are edited on the board's web UI now. The rule format must allow ThingsBoard to push rules later.

## 5. Design

### 5.1 Components

```
firmware/components/
  telemetry_hub/     + per-group "updated at" timestamps, + TELEMETRY_EVT_FLOW, + TELEMETRY_EVT_ALARM
  flow_switch/       NEW: GPIO contact input, debounce (pure), small task, hub publish
  signal_registry/   NEW, pure C: the table of signals and how to read each from a snapshot
  alarm_rules/       NEW, pure C (+cJSON): rule type, validation, JSON <-> rule, evaluation state machine
  alarm_engine/      NEW: task, rule storage, history ring, acknowledge, hub publish
  uplink/            NEW: ThingsBoard message formatting (pure), cadence task, transport plug, preview
  nvs_store/         + load/save of the alarm rule blob
main/
  web_server.c       + /alarms page, /api/alarms/*, /api/uplink/preview, SSE "alarm"
  app_main.c         + init order, LED alarm state
  static/            + Alarms view, banner, badge
test/sensor_selftest/  + tests for signal_registry, alarm_rules, uplink formatting
```

Dependency rule: `signal_registry`, `alarm_rules` and the formatting part of `uplink` must build in the QEMU self-test app without NVS, Wi-Fi or HTTP. `nvs_store` may depend on `alarm_rules` (types only), as it already depends on `cooling_control`'s rule header. Alarm logic does **not** go inside `power_monitor` or `water_quality`.

### 5.2 `telemetry_hub` changes

- The snapshot gains an uptime timestamp (ms, 0 = never) per group, set inside `telemetry_hub_publish()`: `pump_updated_ms`, `float_updated_ms`, `cooling_updated_ms`, `health_updated_ms`, `power_updated_ms[2]`, `water_updated_ms`, `flow_updated_ms`.
- New event `TELEMETRY_EVT_FLOW` with payload `{bool flow_on}`, cached in the snapshot (`flow`, `has_flow`).
- New event `TELEMETRY_EVT_ALARM` with payload `{seq, rule_id, type (raised|cleared|acked), severity, signal_id, value, active_count, critical_count}`, cached in the snapshot like the others. It exists so the web SSE path gets alarm changes the same way it gets everything else.

### 5.2a `flow_switch` (built first)

- **Hardware:** a paddle flow switch in the water pump's pipe. Wiring: **GPIO 9 → switch → GND**, using the switch's normally-open terminal so the contact **closes when water flows**. Internal pull-up on; no external parts.
- **Pin:** fixed at GPIO 9. It is outside the user-selectable pool in `hardware_map` (currently listed there as unassigned), so it cannot collide with a role the user picks and the saved hardware map format does not change. The Hardware page's fixed-pin list shows it; the comment block in `hardware_map.c` is updated.
- **Reading:** `flow_on` = contact closed (pin low). The raw level is sampled every 100 ms and a change is accepted only after it has been stable for 1 s, because a paddle chatters in turbulent water. The debounce is a pure function so it can be tested in QEMU.
- **Task:** priority 2, not in TWDT. Publishes `TELEMETRY_EVT_FLOW` on every accepted change and every 5 s as a heartbeat (so a late subscriber gets the state and a stale rule can watch the task).
- **API (C):** `flow_switch_init()` (returns quickly), `flow_switch_get_status()` → `{flow_on, raw_closed, last_change_ms}`.
- **No fault state.** A bare contact cannot report that it is broken or unplugged; an unconnected input reads "no flow". The rules in §5.6 compare it with the pump command instead.
- **Web:** `/api/sensors/status` gains `"flow":{"on":true,"age_ms":300}`; SSE gains `event: flow`; the Sensors page gets a small card "การไหลของน้ำ" showing ไหล / ไม่ไหล.

Assumptions (mine, to be corrected if wrong): the switch sits in the pipe of the water pump on relay 1, and it is wired on its normally-open terminal. If the installer uses the normally-closed terminal the reading is inverted; a polarity setting is not included.

### 5.3 `signal_registry`

One static table. Each row:

| Field | Meaning |
|---|---|
| `id` | small stable number, stored in the NVS rule blob; never renumbered or reused |
| `key` | stable name used in the REST API, rule JSON **and** as the ThingsBoard telemetry key |
| `label_th`, `unit`, `decimals` | for the UI |
| `group` | short group name for display (`pump_power`, `chiller_power`, `water`, `cooling`, `pump`, `float`, `system`) |
| `kind` | number or bool |
| `source` | which snapshot group it reads (decides trust and staleness) |
| `period_sec` | how often the source publishes; 0 = on change only (no stale rules) |

Reading a signal returns `{present, trusted, value, age_ms}`:
- `present` = the source has published at least once.
- `trusted` = safe to compare. For power and water values: `valid && !fault`. For `pt100_temp_c`: `is_sensor_valid`. For `wifi_rssi_dbm`: value is not 0 (`app_main` publishes 0 while STA is disconnected). For everything else: same as `present`.
- `*_fault` signals are always trusted when present. They are the "sensor is gone" inputs.

**Signal table (v1).** Keys are `snake_case` so one name works in the API and directly in ThingsBoard rule-engine scripts.

| Source | Keys | Kind |
|---|---|---|
| Power ch 0 (water pump) | `pump_voltage_v`, `pump_current_a`, `pump_power_w`, `pump_energy_wh`, `pump_frequency_hz`, `pump_pf` | number |
| | `pump_meter_fault` | bool |
| Power ch 1 (chiller) | same with `chiller_`, and `chiller_meter_fault` | |
| Water | `water_ph`, `water_temp_c` | number |
| | `water_ph_stable`, `water_sensor_fault` | bool |
| Cooling | `pt100_temp_c`, `cooling_duty_pct`, `cooling_output_mv`, `cooling_mode`, `cooling_lockout_sec` | number |
| | `pt100_fault` (= `!is_sensor_valid`), `cooling_on` (= `output_mv > 0`) | bool |
| Pump | `relay1_on`, `relay2_on`, `pump_is_running` | bool |
| | `pump_active_set` | number |
| Float | `float_high` | bool |
| Flow | `flow_on` | bool |
| Health | `free_heap_bytes`, `free_psram_bytes`, `wifi_rssi_dbm`, `uptime_sec`, `oom_strikes` | number |

`period_sec`: power 2, water 2, cooling 1, health 5, flow 5. Pump and float are 0 (on change).

**What the two PZEMs measure (from the user, 2026-11-15).** They measure real devices, not relays: **one water pump and one chiller**. The measured pump is the water pump switched by **relay 1**. The wash pump on relay 2 is a separate part and is not measured. The chiller is driven through the inverter (0–10 V from `cooling_control`), not by a relay; `cooling_on` is provided so a rule can be gated on it.

The existing code calls both channels "pump 1 / pump 2". That is wrong and is corrected as part of this project, so the UI, the API and the uplink agree:
- Sensors page card titles (`main/static/index.html`, "ไฟฟ้า ปั๊ม 1 / ปั๊ม 2") become "ไฟฟ้า ปั๊มน้ำ" and "ไฟฟ้า Chiller".
- `/api/sensors/status` `name` values `pump1` / `pump2` become `pump` / `chiller`.
- The fixed-bus table in `hardware_map.c` (ids `pzem_pump1` / `pzem_pump2`, labels "PZEM-004T ปั๊ม 1 / ปั๊ม 2") becomes `pzem_pump` / `pzem_chiller` with matching labels.
- Hub and component code stay indexed by channel 0/1; only names and labels change.

**Channel convention (agreed with the user):** channel 0 (UART0) is the water pump and channel 1 (UART1) is the chiller. The current clamp of each meter must be put on the matching device at install time. The Hardware page's fixed-bus list states this next to each bus.

**The PT100 measures tank water temperature**, the input of the cooling control (the cooling page labels it "อุณหภูมิน้ำ (PT100)"). The backlog note called it inverter temperature; that was wrong. Hence `pt100_temp_c` and `pt100_fault`. `water_temp_c` is the separate temperature reading from the pH probe.

### 5.4 Rule model (`alarm_rules`)

```c
#define ALARM_MAX_RULES 24
#define ALARM_NAME_BYTES 97            /* 96 bytes of UTF-8 (32 Thai code points incl. vowel/tone marks) + NUL */

typedef struct {
  uint16_t id;             /* assigned by the engine, monotonically increasing, never reused */
  bool     enabled;
  uint8_t  signal_id;
  uint8_t  condition;      /* above | below | outside | inside | is_true | is_false | stale */
  uint8_t  severity;       /* info | warning | critical */
  bool     latch;          /* stay raised until acknowledged */
  uint8_t  gate_mode;      /* none | when_true | when_false */
  uint8_t  gate_signal_id; /* must be a bool signal */
  float    threshold;      /* above/below; low bound for ranges */
  float    threshold_hi;   /* ranges only */
  float    hysteresis;     /* >= 0, same unit as the signal */
  uint16_t delay_sec;      /* 0..3600, violation must persist this long */
  uint16_t stale_sec;      /* max(5, 3 x source period)..3600, `stale` only */
  char     name[ALARM_NAME_BYTES];
} alarm_rule_t;
```

**Rule JSON** (web API now; ThingsBoard shared attribute later):

```json
{"id":3,"name":"pH ผิดช่วง","enabled":true,"signal":"water_ph","condition":"outside",
 "lo":6.5,"hi":8.5,"hysteresis":0.1,"delay_sec":60,"severity":"warning","latch":false,
 "gate":{"signal":"relay1_on","when":true}}
```

`above`/`below` use `"threshold"`; ranges use `"lo"`/`"hi"`; `stale` uses `"stale_sec"`; `"gate"` is optional. Signals are named by `key`, never by numeric id.

**Validation** (one pure function, returns the offending field name):
- signal exists; the condition fits the signal kind (numeric conditions on numbers, `is_true`/`is_false` on bools, `stale` only on sources with `period_sec > 0`)
- numbers finite; `lo < hi`; `hysteresis >= 0` and, for ranges, less than half the span
- `stale_sec` is at least `max(5, 3 × period_sec)` of the signal's source (so 15 s for health signals), so a stale rule cannot flap on normal publish jitter
- gate signal exists, is a bool, and is not the rule's own signal
- name is non-empty printable UTF-8, at most 96 bytes, with no `"`, `\` or control characters (so it can be embedded in JSON and SSE without escaping)
- wrong JSON type (string where a number is expected) is reported as `invalid_type` for that field, not as missing

### 5.5 Evaluation (`alarm_rules`, pure)

Called once per second with the snapshot, the uptime in ms, and each rule's runtime state. Runtime state lives in RAM only.

```
NORMAL ──violating──► PENDING ──held for delay_sec──► ACTIVE ──clear──► NORMAL
                         │ not violating                 │ clear, latch on, not acked
                         ▼                               ▼
                       NORMAL                     LATCHED ──ack──► NORMAL
```

Per rule, per tick:

1. **Gate.** If a gate is set and the gate signal is not present, or its value does not match, the rule is *not applicable*: treated as "not violating" (PENDING drops to NORMAL, ACTIVE clears). The delay therefore restarts each time the gate opens.
2. **Trust.** For value and bool conditions, if the signal is not `present`, do nothing. If present but not `trusted`:
   - source `fault` (debounced): PENDING drops to NORMAL; ACTIVE stays ACTIVE. We cannot confirm either way, and the matching `*_fault` rule tells the user.
   - a single transient miss (`valid=false` without `fault`): no change this tick.
3. **Condition with hysteresis.**
   - `above`: violates when `v > threshold`; once PENDING or ACTIVE, keeps violating until `v <= threshold - hysteresis`. `below` mirrors it.
   - `outside [lo,hi]`: violates when `v < lo` or `v > hi`; clears when `lo + hysteresis <= v <= hi - hysteresis`.
   - `inside [lo,hi]`: violates when `lo <= v <= hi`; clears when `v < lo - hysteresis` or `v > hi + hysteresis`.
   - `is_true` / `is_false`: direct, no hysteresis.
   - `stale`: violates when the source's `age_ms > stale_sec * 1000`, or when it has never published and uptime exceeds `stale_sec`. This is the detector for a hung sensor task.
4. **Transitions** produce events. There is no clear delay; hysteresis handles flicker.

**Event contract.** Every alarm produces exactly one `raised` and, later, exactly one `cleared`. A consumer (the UI, or a ThingsBoard rule chain) can open on `raised` and close on `cleared` and never be left with an open alarm.

| Transition | Event | `reason` |
|---|---|---|
| PENDING → ACTIVE | `raised` | — |
| ACTIVE → NORMAL (condition gone, or gate closed) | `cleared` | `condition` |
| ACTIVE → LATCHED (condition gone, latch on, not acknowledged) | none | — |
| acknowledge while ACTIVE | `acked` | — |
| acknowledge while LATCHED → NORMAL | `cleared` | `acked` |
| rule edited or deleted while ACTIVE or LATCHED | `cleared` | `rule_changed` |

Acknowledging an ACTIVE alarm marks it acknowledged; it stays listed until the condition clears, then goes straight to NORMAL even if latch is on.

**Counts.** One definition, used by the API summary, the SSE event, the banner and badge, the LEDs and the uplink:
- `active` = rules in ACTIVE or LATCHED, acknowledged or not (everything shown in the "active" list)
- `critical` = the subset of `active` with severity critical
- `unacked` = ACTIVE and not acknowledged, plus LATCHED
- `critical_unacked` = the subset of `unacked` with severity critical (drives the LED)
- `max_severity` = highest severity in `active`: 0 none, 1 info, 2 warning, 3 critical

Editing or deleting a rule resets that rule's state. After a reboot every rule starts NORMAL and re-raises after its delay if the condition still holds. A LATCHED alarm does not survive a reboot.

### 5.6 `alarm_engine`

- **Task:** 1 s tick, priority 2 (below the sensor tasks at 3), not in TWDT, same reasoning as the sensor tasks. It publishes its own `last_tick_ms`; the API reports the engine as stalled if that is older than 5 s.
- **Rules:** held in RAM behind a mutex, loaded at init. Stored through `nvs_store` as one versioned blob (namespace `alarm_cfg`, header `{version, count, next_id}`), written only on create/update/delete. An invalid blob loads the presets and is reported in the API status, like the other settings.
- **Events:** each has a global `seq` (starts at 1 each boot). A random `boot_id` is generated at init so clients can tell a reboot from a continuing sequence.
- **History:** RAM ring of the last 32 events: `{seq, type, rule_id, rule name copy, signal_id, severity, value, uptime_ms, reason}`. This ring is also the uplink's store-and-forward buffer (§5.8).
- **API (C):** list/upsert/delete rules, acknowledge one or all, get active alarms, get events since a `seq`, get a small summary `{active, critical, unacked, critical_unacked, max_severity, last_seq, boot_id, last_tick_ms}` (counts as defined in §5.5) that is cheap enough for the LED task.
- On every event it publishes `TELEMETRY_EVT_ALARM`.

**Preset rules.** Created when no blob exists. **All disabled**, so a bench board without sensors does not start blinking. The user enables and tunes them.

| Name | Signal | Condition | Delay | Severity | Notes |
|---|---|---|---|---|---|
| pH ผิดช่วง | `water_ph` | outside 6.5–8.5, hyst 0.1 | 60 s | warning | range still to be confirmed for this pond |
| น้ำร้อนเกิน | `pt100_temp_c` | above 32, hyst 0.5 | 60 s | warning | also catches "chiller runs but does not cool" |
| ไฟตก/ไฟเกิน | `pump_voltage_v` | outside 200–245, hyst 2 | 5 s | warning | |
| รีเลย์ปั๊มน้ำค้าง | `pump_current_a` | above 0.1 A, gate `relay1_on` false | 10 s | critical | latch. Relay 1 commanded off but the pump still draws current |
| ปั๊มน้ำเปิดแต่น้ำไม่ไหล | `flow_on` | is_false, gate `relay1_on` true | 15 s | critical | dry run, blocked pipe, lost prime. No threshold to tune |
| น้ำไหลทั้งที่ปั๊มปิด | `flow_on` | is_true, gate `relay1_on` false | 30 s | warning | stuck paddle, siphoning, or relay stuck |
| ปั๊มน้ำไม่ทำงานตามคำสั่ง | `pump_current_a` | below 0.3 A, gate `relay1_on` true | 10 s | critical | **placeholder threshold.** Relay 1 commanded on but almost no current: relay, breaker, wiring or motor |
| Chiller ไม่ทำงานตามคำสั่ง | `chiller_power_w` | below 50 W, gate `cooling_on` true | 180 s | critical | **placeholder threshold.** 0–10 V commanded above zero but the chiller draws no real power |
| Chiller ทำงานทั้งที่สั่งหยุด | `chiller_power_w` | above 200 W, gate `cooling_on` false | 120 s | warning | **placeholder threshold.** Commanded to 0 V but still drawing running power |
| เซนเซอร์ PT100 ไม่ตอบ | `pt100_fault` | is_true | 0 | critical | cooling control forces 0 V on a PT100 fault, so the chiller stops |
| เซนเซอร์ pH ไม่ตอบ | `water_sensor_fault` | is_true | 0 | warning | |
| มิเตอร์ไฟปั๊มน้ำไม่ตอบ | `pump_meter_fault` | is_true | 0 | warning | |
| มิเตอร์ไฟ Chiller ไม่ตอบ | `chiller_meter_fault` | is_true | 0 | warning | |

**Placeholder thresholds.** The three marked rules compare a command with measured power. The right numbers (pump running current, chiller standby and running power) are only known after the real devices are measured on sensor bench Part B. The presets ship as templates to be tuned then. The rule dialog shows the live value of the selected signal so the user can pick a threshold while watching the device.

**What the chiller rules can and cannot tell.** The firmware knows what it *commands* (`cooling_output_mv`, `cooling_on`) and what the chiller *draws* (its PZEM). It has no read-back of the real 0–10 V line and no status signal from the inverter. So:
- It **can** detect "commanded on but no power", "commanded off but still running" and "running but the water is not getting cooler". Compressor lockout does not cause false alarms, because the output is 0 V during lockout and the gate is closed.
- It **cannot** tell which part failed: the PWM-to-0–10 V module, the wire, the inverter, or the compressor. The alarm says the chiller did not follow the command.
- A meter that stops answering (`chiller_meter_fault`) is itself a hint that mains is missing at the chiller (tripped breaker), if the PZEM only answers while its voltage terminals are powered. To confirm on Part B.
- Pinpointing the inverter would need extra wiring, such as the inverter's fault-relay contact on a GPIO, or an ADC read-back of the 0–10 V line. That is hardware work outside this project; each would become one more registry signal.

There is no overload preset (current too high). It needs the pump's rated current; the user adds it after Part B.

### 5.7 Web

**REST.** Every endpoint requires login. POSTs also require same origin and a JSON object body. The existing `sensors_begin_post()` guard is generalised to take a body buffer size, because a rule with a 96-byte Thai name exceeds 256 bytes (use 768). Delete stays a POST so the UI can use the existing `apiPost` helper.

| Method | Path | Body | Result |
|---|---|---|---|
| GET | `/alarms` | — | SPA shell, like `/sensors` |
| GET | `/api/alarms/signals` | — | the registry: `key`, `label`, `unit`, `kind`, `group`, `period_sec`, `decimals` (same objects as the uplink `signals` attribute) |
| GET | `/api/alarms/values` | — | current reading of every signal: `{"values":{"pump_current_a":{"v":1.234,"trusted":true}, ...}}`. Polled every 2 s only while the rule dialog is open |
| GET | `/api/alarms/rules` | — | `{"rules":[...],"max":24,"storage":"saved"|"defaults"|"invalid"}` |
| POST | `/api/alarms/rules` | rule JSON | no `id` = create, with `id` = replace. `200` with the stored rule, `400` with `{error, field}`, `409` when full |
| POST | `/api/alarms/rules/delete` | `{"id":3}` | `200` / `404` |
| GET | `/api/alarms/status` | — | see below |
| GET | `/api/alarms/summary` | — | only `boot_id`, `last_seq`, `engine_stalled` and `summary` from the status response; small enough to poll |
| POST | `/api/alarms/ack` | `{"id":3}` or `{"all":true}` | `200` / `404` |
| GET | `/api/uplink/preview` | — | see §5.8 |

`GET /api/alarms/status`:
```json
{"ok":true,"boot_id":2891734,"last_seq":12,"uptime_ms":734000,"engine_stalled":false,
 "summary":{"active":2,"critical":1,"unacked":2,"critical_unacked":1,"max_severity":3},
 "active":[{"rule_id":3,"name":"pH ผิดช่วง","signal":"water_ph","severity":"warning",
            "state":"active","acked":false,"value":8.92,"since_ms":652000}],
 "history":[{"seq":12,"type":"raised","rule_id":3,"name":"pH ผิดช่วง","severity":"warning",
             "value":8.92,"uptime_ms":652000}]}
```

**SSE.** `web_server_telemetry_cb` adds `event: alarm` with `{"seq","type","rule_id","sev","active","critical","unacked"}` (no name, so it stays well under the 256-byte buffer). As with every SSE event today, it is visible to any device on the LAN.

**How the UI stays current.** SSE is not made global: the server allows 3 SSE clients, and holding a slot on every open tab would lock other tabs out.
- On every logged-in view, the UI polls `GET /api/alarms/summary` every 10 s (paused while the tab is hidden) and updates the banner and badge from it.
- The Alarms view opens the SSE stream the same way `/cooling` and `/sensors` do. On any view where SSE is open, an `alarm` event triggers an immediate refresh (summary everywhere, full `/api/alarms/status` on the Alarms view).
- Result: the Alarms page updates within about a second; other pages within 10 s.

**UI.** New nav item "แจ้งเตือน" (desktop nav and mobile "more" menu) with a count badge, and a banner above every view while any alarm is active (red for critical, amber otherwise). The Alarms view, in the visual language of the Sensors page:

```
┌ กำลังแจ้งเตือน ─────────────────────────────────────────────┐
│ ● วิกฤต  รีเลย์ปั๊มน้ำค้าง   0.42 A   3 นาทีที่แล้ว   [รับทราบ] │
│ ● เตือน  pH ผิดช่วง          8.92     12 นาทีที่แล้ว  [รับทราบ] │
└────────────────────────────────────────────────────────────┘
┌ กฎแจ้งเตือน (5/24) ──────────────────────────── [+ เพิ่มกฎ] ┐
│ [✓] pH ผิดช่วง        pH นอกช่วง 6.5–8.5 นาน 60 วิ   [แก้][ลบ]│
│ [ ] น้ำร้อนเกิน        อุณหภูมิน้ำ > 32 °C นาน 60 วิ   [แก้][ลบ]│
└────────────────────────────────────────────────────────────┘
┌ ประวัติล่าสุด (หายเมื่อรีบูต) ───────────────────────────────┐
│ 14:02  เกิด   pH ผิดช่วง        8.92                         │
│ 13:40  หาย   ไฟตก/ไฟเกิน      221 V                         │
└────────────────────────────────────────────────────────────┘
```

- **Rule dialog:** name; signal (dropdown grouped by source, from `/api/alarms/signals`), with the signal's current value shown live beside it; condition (only those valid for the signal kind); value field(s) with the unit; hysteresis; delay; severity; "ค้างจนกดรับทราบ"; "เฉพาะตอน" (none, or a bool signal is on/off). Delete asks for confirmation.
- **Times:** the board has no clock. The UI shows clock time computed in the browser (`browser now − (uptime_ms − event uptime_ms)`).
- **Unseen marker:** the browser stores `{boot_id, last seen seq}` in `localStorage`; history rows above it are marked new and counted on the badge together with active alarms. A changed `boot_id` resets it.
- Save buttons stay disabled while a POST is in flight.

**LEDs.** New `LED_STATE_ALARM`: WS2812 red and the external LED, blinking at 1 Hz, while `critical_unacked > 0`. Priority in `hardware_ui_task`: button-hold states, then staging-pending, then alarm, then the normal AP on/off state. The task reads the engine summary at most once per second.

### 5.8 `uplink` (ThingsBoard-ready, transport-agnostic)

**Target contract** (ThingsBoard device MQTT API; checked against thingsboard.io docs on 2026-11-15, not yet against a live server):
- telemetry → topic `v1/devices/me/telemetry`, payload `{"key":value,...}` (server timestamp) or `{"ts":<ms>,"values":{...}}` (device timestamp); values may be number, bool, string or nested JSON
- client attributes → topic `v1/devices/me/attributes`, payload `{"key":value,...}`

**Messages produced**

1. *Periodic telemetry*, every 30 s (compile-time constant for now), and immediately (at most once per 2 s) when `relay1_on`, `relay2_on`, `float_high` or `flow_on` changes or any alarm event occurs:
   ```json
   {"pump_current_a":1.234,"pump_voltage_v":229.8,"chiller_power_w":640.2,"water_ph":7.12,"water_sensor_fault":false,
    "relay1_on":true,"float_high":false,"uptime_sec":734,
    "alarm_active":2,"alarm_critical":1,"alarm_unacked":2,"alarm_max_severity":3}
   ```
   One key per registry signal. A signal that is not `trusted` is **omitted** (ThingsBoard keeps its last value) while its `*_fault` key reports the problem. The `alarm_*` keys are the counts defined in §5.5.
2. *Alarm event telemetry*, one message per engine event, in `seq` order:
   ```json
   {"alarm_event":{"seq":12,"boot_id":2891734,"type":"raised","rule_id":3,"name":"pH ผิดช่วง",
                   "signal":"water_ph","severity":"warning","value":8.92,"reason":null}}
   ```
   `type` and `reason` follow the event contract in §5.5.
   Wrapped as `{"ts":...,"values":{...}}` when a wall clock is available (below).
3. *Identity attributes*, on sink connect. This is what lets a server or frontend handle several firmware types without hard-coding this one:
   ```json
   {"fw_type":"fishpump","fw_version":"0.2.0-dev","contract_version":1,
    "signals":[{"key":"pump_current_a","label":"กระแสปั๊มน้ำ","unit":"A","kind":"number",
                "group":"pump_power","decimals":3,"period_sec":2}, ...]}
   ```
   - `fw_type` names the firmware family; a ThingsBoard device profile (the "preset") is expected per `fw_type`.
   - `contract_version` is bumped when a key is renamed or removed, or a meaning changes. Adding a signal does not bump it.
   - `signals` is the registry, serialised. A frontend can build a device's screen from it.
   - About 5 KB for the v1 table, built in an 8 KB PSRAM buffer allocated only for the build and send.
4. *Rule attributes*, on sink connect and whenever rules change: `{"alarm_rules":[<rule JSON>...]}`. Kept separate from identity so a rule edit does not resend the signal list. Up to about 9 KB with 24 rules and long names, built in a 12 KB PSRAM buffer the same way.

**Transport plug**

```c
typedef enum { UPLINK_MSG_TELEMETRY, UPLINK_MSG_ATTRIBUTES } uplink_msg_kind_t;

typedef struct {
  bool      (*is_connected)(void *ctx);
  esp_err_t (*send)(void *ctx, uplink_msg_kind_t kind, const char *json, size_t len); /* ESP_OK = accepted */
  void       *ctx;
} uplink_sink_t;

esp_err_t uplink_set_sink(const uplink_sink_t *sink);        /* NULL = no transport */
void      uplink_set_wall_clock(int64_t (*now_unix_ms)(void)); /* NULL = no clock */
```

The next project implements a sink over `esp-mqtt` and a wall clock over SNTP. Nothing else in this layer should need to change.

**Store-and-forward.** The uplink keeps `last_forwarded_seq`. Each second it asks the engine for events after it and sends them in order, advancing only when `send` returns `ESP_OK`. While there is no sink, or it is disconnected, events simply wait in the engine's 32-entry ring. If the ring overwrites unsent events they are lost; the uplink counts them in `dropped` and continues from the oldest event still held. Periodic telemetry is **not** backfilled: with no connected sink it is formatted for the preview only.

**Timestamps.** Events record uptime. When a wall clock is set, the event's `ts` is `now_unix_ms − (uptime_now − uptime_event)`, so an alarm that happened while offline arrives with the right time. With no wall clock the plain form is used and the server stamps arrival time.

**Task.** One low-priority task (priority 2, not in TWDT), 1 s tick. It polls the snapshot and the engine; it does not need a hub subscription. Telemetry and event buffers (about 2 KB each) come from PSRAM.

**Preview.** `GET /api/uplink/preview` (login required) returns the latest message of each kind as it would be sent, plus `{"sink":"none","connected":false,"interval_sec":30,"pending_events":3,"dropped":0}`. The Status page gets a collapsed card "ข้อมูลที่จะส่งขึ้น ThingsBoard (ตัวอย่าง)" showing it. This is how the layer is checked on the bench until MQTT exists.

### 5.9 Boot order (`app_main.c`)

`flow_switch_init()` goes with the other sensor inits. After the sensor inits and `pump_control` start, before the web server: `alarm_engine_init()` then `uplink_init()`. Both return quickly. If either fails, log it and continue; the web API then reports the engine as unavailable. Raise the log level for the new tags to INFO alongside the other application modules.

## 6. Testing

**QEMU self-test (no hardware), added to `firmware/test/sensor_selftest/`:**
- `flow_switch` debounce: chatter shorter than 1 s is ignored; a stable change is accepted once; both directions.
- `telemetry_hub`: timestamps set per group; flow and alarm events cached.
- `signal_registry`: ids and keys unique; each row reads the right field; `trusted` follows the validity contract; `*_fault` rows.
- `alarm_rules` validation: every rejection in §5.4, and JSON round-trip of every condition type including a Thai name.
- `alarm_rules` evaluation, driven by synthetic snapshots and time: delay; hysteresis for all four numeric conditions; value flicker at the threshold raises once; gate opening restarts the delay and gate closing clears; fault holds ACTIVE and drops PENDING; a transient miss changes nothing; stale including never-published; latch and both acknowledge paths; rule edit emits `cleared`; every `raised` is matched by exactly one `cleared` with the right `reason`; the five counts in each state.
- `uplink` formatting: untrusted signals omitted; alarm summary keys; event JSON; identity attributes list every registry row and fit the buffer; rule attributes with 24 maximum-length rules fit the buffer; `ts` arithmetic with a fake clock; store-and-forward with a fake sink that refuses, then accepts; ring overrun counted in `dropped`.

**Build:** `idf.py -C firmware build` clean with warnings as errors.

**Bench, AP mode, run by the user from a Thai checklist** (new `docs/bench-test-alarms.md`, same style as `docs/bench-test-sensors.md`). Everything is reachable without real sensors:
0. Flow switch, no sensor needed: with GPIO 9 open the Sensors page shows "ไม่ไหล"; a jumper from GPIO 9 to GND shows "ไหล" after about 1 s; tapping the jumper quickly changes nothing. With the pump running and the jumper removed, the preset "ปั๊มน้ำเปิดแต่น้ำไม่ไหล" raises after 15 s and clears when the jumper is put back.
1. Fresh NVS: presets listed, all disabled, nothing active, LED normal.
2. Enable "เซนเซอร์ pH ไม่ตอบ" with no sensor attached: raises within a few seconds; the Alarms page updates without a reload, and the banner and badge on another page (for example the dashboard) appear within 10 s.
3. A rule on `uptime_sec` above a value a minute ahead, delay 5 s: raises at the expected time. Used to check delay, severity colours and the critical LED blink.
4. Acknowledge: the LED stops blinking; a latched rule stays listed until acknowledged.
5. A gated rule on `uptime_sec` with gate `relay1_on`: only raises while the water pump runs, clears when it stops.
6. Rule persistence across reboot; history empty after reboot; the 25th rule is refused; invalid input shows the field error.
7. Logged out: every `/api/alarms/*` and `/api/uplink/preview` request returns 401. A POST without a matching `Origin` returns 403.
8. Preview card shows telemetry keys matching §5.3, the alarm summary, the identity attributes (`fw_type`, `contract_version`, signal list), and `pending_events` growing as alarms occur. On the AP-only bench `wifi_rssi_dbm` is absent from the telemetry.
9. Serial log (USB port): no new repeating errors; heap steady over 10 minutes with several rules active.
10. Pumps, cooling and the Sensors page behave as before, except that the two power cards and the fixed-bus list on the Hardware page now read "ปั๊มน้ำ" and "Chiller".

Real-sensor checks (pH out of range, relay-stuck with real current) wait for sensor bench Part B.

## 7. Build order

1. `flow_switch` + hub event + Sensors page card (small, independent, usable on its own).
2. Hub timestamps, `signal_registry`, and the pump/chiller renaming.
3. `alarm_rules` + `alarm_engine` + storage, then the web API, UI and LEDs.
4. `uplink` and the preview.

Each step ends with the firmware building and the self-test passing.

## 8. Left to the builder

- Exact struct layouts, the blob byte layout and its version constant.
- How `alarm_rules` and `alarm_engine` are split into files, as long as the pure parts build in the self-test app.
- Stack sizes; cJSON versus the existing `json_gen` helpers for building responses.
- Exact markup, CSS and Thai wording, matching the Sensors page style.
- Whether `sensors_begin_post()` is renamed when generalised.
- Whether to work directly on `firmware/esp32s3-dev` or on a branch cut from it (it is not merged to `main`; sensor bench Part B is pending).
