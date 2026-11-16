# Pump Dry-Run Protection — Design

Date: 2026-11-16 · Branch: `feature/alarm-rules` (follow-on) · Status: draft for approval

Background: [bench findings §1](../results/2026-11-16-bench-alarms-findings.md). The alarm system it builds on: [alarm rules spec](2026-11-15-alarm-rules-and-uplink-design.md).

## 1. Intent

The water pump runs on a timer, for example ON 30 s then OFF 5 min, looping. If water stops reaching it (lost prime, blocked intake, pond level too low), it keeps running dry every cycle until it is destroyed. The current alarm rule "ปั๊มน้ำเปิดแต่น้ำไม่ไหล" cannot prevent this:

- If the ON time is 15 s or less (the rule's delay), the alarm never raises.
- If it raises, it is cleared the moment the pump stops ("gate closed"). The LED goes back to green for the whole OFF period, and the history fills with raised/cleared pairs. Their value shows "no flow", which reads as if the problem had gone away.
- Alarms only report. Nothing stops the pump.

The firmware will be installed by other people, with different pumps and the flow switch mounted in different places. So the protection is **set up at installation**, with defaults taken from commercial dry-run protectors.

## 2. Goals, non-goals, anti-goals

**Goals**
- Detect dry running even when each ON period is short, by counting dry time across cycles.
- Stop the pump on detection. Wait, retry automatically a limited number of times, then lock the pump until a person acknowledges.
- Keep the alarm visible (LED red, alarm list, ThingsBoard event) from the first trip until the problem is really over: flow is seen, or a person acknowledges.
- Give the installer one settings card to configure it, including a "measure" button that finds how long this installation takes to show flow after the pump starts.

**Non-goals (this round)**
- Chiller interlock (no cooling without flow). The settings API is shaped so it can be added later.
- The wash pump (relay 2). The flow switch is only on the water-pump line.
- Current-based dry-run detection with the PZEM. It can be added later as a second input.
- The front-panel button as an unlock. Unlocking is web only.
- The alert-visibility work (bell, LED colours) from bench findings §2–3. That is a separate project.

**Anti-goals: this is a failure even if it "works"**
- Stopping a healthy pump because no flow switch is installed, or it is unplugged. An unconnected input reads "no flow".
- The protection being editable or deletable as an ordinary alarm rule.
- A reboot or power cut silently releasing a locked pump.
- The water pump starting while locked, from any path: schedule, auto-start at boot, dashboard start, the API.

## 3. Reference practice (where the defaults come from)

| Product | What it does |
|---|---|
| Grundfos SQ/SQE, CU 301 | Stops on dry run and retries after **5 min** (factory setting) |
| Grundfos CRE (LiqTec) | Start-up delay before dry-run detection, **0–254 s** |
| Franklin SubDrive Connect Plus | First off time **5 min**, then **doubles** if it trips again straight after |
| Littelfuse 111P/233P | Restart delay **2–225 min** or manual |
| Littelfuse 520CP | Restart attempts **0–4** or unlimited; **0 = manual reset** |
| Chiller practice | Flow must be proven before start, within **10–30 s**, typically 15 s |

Sources are listed in the bench findings conversation. The key ones: Grundfos SQ/SQE FAQ, the CU 301 and CRE manuals, the Franklin SubDrive Connect Plus manual, and the Littelfuse 233P and 520CP manuals.

## 4. Approaches considered

- **A. A dedicated protection module (chosen).** A new pure-logic component holds the state machine. A thin task feeds it the relay and flow inputs and acts through one new inhibit call on `pump_control`, and it reports through the existing alarm engine. It is testable in the QEMU self-test like `alarm_rules`. User rule edits cannot touch it, and the later chiller interlock follows the same pattern.
- B. Rejected: give generic alarm rules "actions" such as stopping the pump. Too much for the problem. Protection would live in user-editable rules, and the NVS rule format and the UI would both have to change.
- C. Rejected: put the logic inside `pump_control.c`. The file is about 870 lines of schedule logic already. Mixing in protection makes both harder to test.

## 5. Behaviour

### 5.1 Inputs, each 1 s tick

- `relay_on`: water-pump relay (relay 1) is energised. From `pump_control` status `water.relay_energized`.
- `flow_on`: debounced flow switch, already stable for 1 s in `flow_switch`.
- `on_since_ms`: when the relay last turned on. The module tracks this itself from `relay_on` edges.

### 5.2 Settings (installer, persisted in NVS)

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `enabled` | **false** | bool | Off until the installer turns it on (anti-goal 1) |
| `prime_sec` | 15 | 1–600 | Grace after each relay-on before "no flow" counts |
| `dry_trip_sec` | 10 | 3–120 | Accumulated dry seconds that count as a dry run |
| `retry_wait_min` | 5 | 1–240 | Wait after the first trip |
| `retry_backoff` | true | bool | Double the wait after each further trip (5 → 10 → 20 min). Any single wait is capped at 240 min (my call) |
| `max_retries` | 3 | 0–5 | Automatic retries before lock. 0 = lock on the first trip |

Validation happens on save, and the API answers 400 with the field name.

When protection is on, the settings card shows a **warning** if any timer set's `on_sec` is not longer than `prime_sec`. Dry time never accumulates in that case, so protection cannot work. The warning text is in Thai. Saving is not blocked, because the installer may be about to change the timer.

### 5.3 State machine

States: `DISABLED`, `MONITOR`, `WAIT`, `LOCKED`.

- **Accumulating (in `MONITOR`):** while `relay_on` and `now - on_since ≥ prime_sec` and `!flow_on`, add the tick to `dry_accum`.
  - `dry_accum` is **not** reset when the relay turns off. That is what catches short ON periods.
  - It resets to 0 when `flow_on` is seen while `relay_on`, at any point of an ON period.
- **Trip** (`dry_accum ≥ dry_trip_sec`), `trips` is incremented:
  - if `trips > max_retries`, go to `LOCKED`
  - otherwise go to `WAIT` for `retry_wait_min × 2^(trips-1)` with backoff, or `retry_wait_min` without
  - In both cases set the pump inhibit, so the relay goes off and the schedule pauses.
  - **Every trip alerts again**, my call: the operator must not miss a later trip or the lock after acknowledging once.
    - The first trip raises the dry-run alarm.
    - Each later trip, and the move to `LOCKED`, clears the alarm's acknowledged flag, so the LED blinks again and it counts as unacknowledged.
    - It also records a new `raised` event in history, SSE and the uplink, with the trip number in its value field. The alarm instance itself stays the same one.
- **`WAIT` ends:** clear the inhibit and restart the water schedule **from its ON phase**, so the retry happens now and not one OFF period later. Go to `MONITOR` with `dry_accum = 0`. `trips` is kept.
- **Recovery:** in `MONITOR` after a trip, flow seen while `relay_on` for `flow_confirm_sec` continuously means the system recovered. `flow_confirm_sec` is a fixed 5 s, builder's call. Then set `trips = 0` and **clear** the alarm with reason `condition`.
- **`LOCKED`:** the inhibit stays set. Only an acknowledge leaves this state (§5.5).
- **`enabled` turned off:** go to `DISABLED`, clear the inhibit, and clear the alarm with reason `rule_changed`. Turning it on goes to `MONITOR` with the counters at 0.

The alarm stays ACTIVE through `WAIT`, the retry and the OFF periods. It is not cleared because the pump is off.

### 5.4 The pump while inhibited

New `pump_control` call: `bool pump_control_set_water_inhibit(bool inhibit)`, plus a status field `water.inhibited`.

While inhibited:
- relay 1 is forced off and the water timer does not advance
- `pump_control_start_water()` and the dashboard/API start return a refusal. `POST /api/pump/start` and `POST /api/pump/control` answer `409` with `"error":"dry_run_locked"` or `"dry_run_waiting"`, and the Dashboard shows a Thai message with the remaining wait
- **stop** still works. If the user stops the water system during `WAIT`, the retry at the end of `WAIT` does not start the pump: the inhibit is cleared and the schedule stays stopped. The alarm stays active.
- the "running" flag (the user's intent) is kept, so auto-resume after `WAIT` knows whether to restart the schedule

Wash (relay 2), the float switch and set switching are unaffected.

### 5.5 Acknowledge = "I checked the pump"

The dry-run alarm appears in the same active list as rule alarms.

Pressing **รับทราบ** on it, or "รับทราบทั้งหมด", in **any state while the dry-run alarm is active** (`WAIT`, `LOCKED`, or `MONITOR` after a retry). My call, following the §2 goal that a person's acknowledge ends the alarm. If the pump is still dry, it simply trips again from zero.
- sets `trips = 0` and `dry_accum = 0`
- clears the inhibit
- restarts the water schedule from ON if the user's running flag is set
- goes to `MONITOR`
- clears the alarm with reason `acked`

This also covers a user stop during `WAIT`. The alarm stays until someone acknowledges it, and the acknowledge resets `trips`.

### 5.6 Reboot and power cut

- `LOCKED` is **persisted** in NVS as a flag. After boot the inhibit is set before auto-start can run, and the alarm is raised again.
- `WAIT`, `trips` and `dry_accum` are **not** persisted. A reboot during `WAIT` starts in `MONITOR` with `trips = 0`. Accepted cost: repeated power cuts reset the retry count. Each restart still trips after `prime_sec + dry_trip_sec` of dry running.

### 5.7 Measure button ("วัดให้")

- **Requires:** protection settings card open, water system stopped, not `LOCKED`.
- **Does:** energises relay 1 directly through a `pump_control` test call, which honours the inhibit. It waits up to **120 s** for `flow_on`, then switches the relay off and reports the time from relay-on to `flow_on`. That time already includes the 1 s debounce.
- **Suggests** `prime_sec = ceil(measured × 1.5) + 5`, clamped to 600. The installer accepts or edits it. Nothing is saved automatically.
- **No flow in 120 s:** reports "ไม่พบน้ำไหลใน 120 วินาที ตรวจ flow switch / การล่อน้ำ" and saves nothing.
- One measurement at a time. The page polls a status endpoint, and leaving the page does not stop a running measurement.
- **Protection is paused during a measurement** (user's decision, 2026-11-16).
  - The measurement's relay-on is invisible to the state machine: no dry time accumulates, and no trip or alarm happens.
  - A "no flow in 120 s" result raises nothing. It only reports on the card.
  - Before starting, the card warns: "ปั๊มอาจเดินแห้งได้นานสุด 120 วินาทีระหว่างวัด" and asks for confirmation.
  - The card has a **หยุดวัด** button that switches the relay off at once.

### 5.8 Alarm-engine integration

The engine gains **system alarms**: alarms owned by firmware modules, not by the rule set.

- They use a reserved `rule_id` range that the rule set never issues. Dry-run is `rule_id 1000`, builder's call on the exact value.
- Name: "ปั๊มน้ำเดินแห้ง". Severity: critical. `signal_id` = `flow_on`. Value: `dry_accum` in seconds.
- The owning module raises and clears them. They appear in `/api/alarms/active`, the counts (LED), history, SSE and the uplink exactly like rule alarms.
- An acknowledge on a system alarm calls back into the owning module (§5.5) instead of `alarm_rule_ack`.
- `LOCKED` shows in the active list as "ล็อกปั๊ม รอรับทราบ". `WAIT` shows "หยุดปั๊ม ลองใหม่ใน mm:ss (ครั้งที่ n/max)".

### 5.9 Retire the preset

Remove "ปั๊มน้ำเปิดแต่น้ำไม่ไหล" from `alarm_presets.c`, because it is replaced by this protection. Rule sets already saved in NVS keep their copy. The user deletes it, and the bench checklist says so.

### 5.10 Visibility

- `GET /api/pump/status` gains `dry_run`: `{state, trips, max_retries, wait_remaining_sec, dry_accum_sec, enabled}`.
- New signal for the registry and uplink: `pump_dry_run_state`, a number: 0 disabled, 1 monitor, 2 wait, 3 locked.
- Logs, in English:
  - one line per state change, for example `dry-run trip 2/3, pump stopped, retry in 600 s`
  - no repeating per-second logs

### 5.11 Settings API and UI

- `GET` and `POST /api/protection`, body `{"pump": {...settings...}}`.
  - Use the same guard as the alarm API: auth, same-origin, JSON, field-level 400.
  - The `"pump"` object leaves room for a later `"chiller"` object.
- `POST /api/protection/measure` starts a measurement, and `GET /api/protection/measure` returns its status.
- UI: a card **"การป้องกันปั๊มเดินแห้ง"** on the pump settings page with:
  - the six settings with Thai labels and short help text
  - the measure button
  - the warning from §5.2
  - a line showing the current state
- While protection is off, the card shows "ยังไม่ได้เปิดการป้องกัน" in amber, so an installer sees it is not set up.

## 6. Testing

**Pure state machine, QEMU self-test, synthetic time:**
- a short ON period that never reaches `prime_sec + dry_trip_sec` in one cycle still trips across cycles
- a flow blip resets `dry_accum`
- `prime_sec` grace is honoured on every relay-on
- backoff times 5/10/20 min and no backoff
- `max_retries = 0` locks on the first trip
- lock after `max_retries`
- recovery clears the alarm and resets `trips`
- ack in `WAIT`, `LOCKED` and `MONITOR`
- disable while `LOCKED` releases
- a user stop during `WAIT` does not restart at the end of `WAIT`
- 32-bit uptime wrap during `WAIT`

**Static contract checks** in a verify script, following `verify_alarm_system.py`:
- routes and the guard
- the 409 codes on start
- preset removed
- no `ESP_LOGW` inside the per-tick path

**Bench (the user, documented in a new section of `docs/bench-test-alarms.md`):**
- jumper GPIO 9 to simulate flow, with a short ON timer
- measure button with and without the jumper
- reboot while `LOCKED`
- dashboard start while locked

## 7. Left to the builder

- Exact module, task and NVS key names.
- Whether the tick runs in its own task or in the alarm engine's 1 s task. It must not block `pump_control`.
- The exact system-alarm `rule_id` value and how the engine stores system-alarm runtime.
- `flow_confirm_sec` (5 s suggested).
- UI layout details inside the card.
