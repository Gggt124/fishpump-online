# ThingsBoard Milestone 6 (Soak and Bench File) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the ThingsBoard link on the bench in everything earlier milestones left open, and finish `docs/bench-test-thingsboard.md`. The open items are:
- ThingsBoard down, then back up;
- link off and on (I7);
- TLS failing cleanly;
- a 1-hour soak with an RPC burst;
- the full J4, when a second access point exists.

**Architecture:** M6 has no firmware change. The board stays on e9f9a92, which is 0.3.2 flashed by USB on 2026-11-21.
- Task 1 writes the round-7 checklist with an expected result for each step.
- Task 2 runs it with the user.
- Task 3 closes the backlog and the bench file.

**Tech Stack:** ESP-IDF v6.0.1 monitor on COM12, ThingsBoard CE 4.3.1 on copter (`192.168.1.51`), `thingsboard/tb_ota.py`/`tb_api.py`, the scratchpad bench driver (`ota_bench.py` queue), the board web app in the browser pane.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`. The relevant sections are:
- 8a item 6 (soak and the bench file);
- 8 "Bench" (ThingsBoard down, 2-minute offline gap, 1-hour soak);
- 6.6 (TLS must fail cleanly);
- 6.7 (resource budget).

## Global Constraints

- **Running `docker stop tb-node`:**
  - Run it on copter only with the user's OK, given at that moment.
  - The user runs it, together with `docker start tb-node`.
- **Local functions must be unaffected while ThingsBoard is down.** That means:
  - no reboot;
  - the pump schedule keeps running;
  - the web page keeps answering.
- **After ThingsBoard comes back:**
  - The board reconnects by itself.
  - It backfills the alarm events from the outage, in seq order, each with its original `ts`.
- **Budget (spec 6.7):**
  - internal heap free stays ≥ 100 KB;
  - every task HWM stays ≥ 512 B: `main_stack_hwm`, `hwm_alarm`, `hwm_uplink`, `hwm_dry_run`, `tb_hwm`, `tb_rpc_hwm`.
- **TLS (spec 6.6):** with `tls` on and pointed at the plain port 1883, the board must fail cleanly. The card must show a clear error, the board retries with backoff, and there is no crash.
- **Who does what:**
  - Claude never types passwords. The user types the ThingsBoard password into the driver tab and logs in to the board web app and the ThingsBoard UI.
  - Change board settings only inside a step that restores them afterwards.
  - Test rule id 19 must end the round disabled, with its original fields.
- **Repo hygiene:**
  - Never stage `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`.
  - No merge or push.
  - Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Firmware:** no change in M6. If a step fails because of firmware, record it with `[x]`, debug it (superpowers:systematic-debugging), and fix it through TDD in its own commit before that step is re-run.

## Review Focus

1. **The broker port opens before ThingsBoard is ready** (just after `docker start`). The board may get refused connects or auth errors. It must keep retrying with backoff ≤ 60 s and reach connected without a reboot. Pinned by L3 step "back up".
2. **Events overflow during the outage.** The history ring is `ALARM_HISTORY_LEN` 32. At about 2 events a minute, 3 minutes gives about 6 events, so `unsent alarm event(s) overwritten` must not appear. Pinned by L3.
3. **The web page stalls during MQTT connect attempts.** `GET /` must answer 200 on every 30 s poll while ThingsBoard is down. Pinned by the L0 poller, read in L1–L3.
4. **Timestamps after the gap.** SNTP does not depend on ThingsBoard, so backfilled events must carry the board's event time, not the reconnect time. Pinned by L3 (within 2 s).
5. **Attributes after a reconnect.** All 6 client attributes must be re-sent after every reconnect, including one caused by a config change, which drops the MQTT outbox (a known limit). Pinned by L1 and L2.

---

### Task 1: Round-7 checklist in the bench file

**Files:**
- Modify: `docs/bench-test-thingsboard.md`: add `## รอบ 7: ทนทานและ ThingsBoard ล่ม (milestone 6)` before `## หลังทดสอบ`.

**Interfaces:**
- Produces: step ids L0–L6, used by Task 2 and the backlog.

- [ ] **Step 1: Write the section in Thai, in the file's style.** Each item is `- [ ] **Lx ชื่อ** — ทำอะไร` followed by `**ต้องเห็น:**`, and Task 2 adds a result line `บันทึก:` under it. Steps and expectations:
  - **L0 เตรียม**
    - Board on e9f9a92: banner `v0.3.2`, `ota_0` `VALID`, no firmware assigned.
    - Monitor running on COM12.
    - Driver logged in (the user types the password).
    - A background poller writes the status and time of `GET http://192.168.1.34/` every 30 s to a scratchpad log.
  - **L1 TLS ผิดพอร์ต**
    - On the ThingsBoard card, tick TLS with host `192.168.1.51` and port `1883`, then save.
    - Expect the card to show an error, not "connected".
    - Expect backoff of 5 → 10 → 20 s, no `E` line from tag `tb_link` other than mqtt/transport errors, and no reboot. The poller stays 200.
    - Untick TLS and save. Expect connected within 10 s and 6 `uplink: attribute … sent` lines within 2 s of connected.
  - **L2 I7 ปิดลิงก์แล้วต่อใหม่**
    - Set the host to `192.168.1.250` and save, wait ≥ 60 s, then set it back to `192.168.1.51`.
    - While the host is wrong, expect backoff growing to 60 s.
    - After the host is restored, expect connected, the 6 attributes re-sent, and the ThingsBoard latest `uptime` updating again within 30 s.
  - **L3 ThingsBoard ล่ม**
    - **Before:** read rule 19 with `rpc alarms.rules.get {}` and note its fields. Set it with `alarms.rules.set` to `signal` `relay1_on`, `condition` `is_true`, `enabled` true. Expect alarm `rule_19` to open and clear in one pump cycle.
    - **Outage:** with the user's OK, the user runs `docker stop tb-node` and waits ≥ 3 min. Expect:
      - reconnect attempts capped at 60 s;
      - no reboot (the BOARD_DIAG uptime keeps rising);
      - PUMP_DIAG phase changes continue;
      - the poller stays 200;
      - `heap_free` ≥ 100 KB;
      - no `overwritten` warning.
      Note the board time of each rule-19 raise and clear in the serial log.
    - **Back up:** the user runs `docker start tb-node`. Expect:
      - connected within 60 s of ThingsBoard answering on 8080;
      - the `alarm_event` timeseries for the outage holds every seq, in order, each `ts` within 2 s of the serial-log time;
      - the `rule_19` alarm history shows matching opens and clears;
      - the attributes re-sent.
    - **Restore** rule 19 to its noted fields (disabled) and confirm with `alarms.rules.get`.
  - **L4 ทนทาน 1 ชั่วโมง**
    - Keep the ThingsBoard dashboard open in the browser pane (the user logs in) for 60 min.
    - Every 10 min, queue an RPC burst of 10 reads (`status.get`, `pump.status`, `cooling.status`, `sensors.get`, `alarms.status`, each twice). Expect every reply `ok` within the 10 s timeout.
    - Across the hour, expect:
      - `heap_free` ≥ 100 KB and `heap_min` ≥ 100 KB;
      - every HWM ≥ 512;
      - `tasks` constant;
      - no reboot;
      - `HTTP_DIAG failed=0` (or unchanged);
      - poller 200.
    - Record the minimum of each value.
  - **L5 J4 แบบเต็ม (ถ้ามี access point ตัวที่สองใน LAN เดียวกับ ThingsBoard)**
    - Read `wifi.profiles.get` first.
    - Send `wifi.connect` to the second AP from ThingsBoard. Expect the board to stage it, connect, reach ThingsBoard, and commit.
    - Read `wifi.profiles.get` again. Expect the new SSID present, `auto_index` pointing at it, and the old profiles intact.
    - Power-cycle the board. Expect it to come back on the new network.
    - Switch back to the home network the same way.
    - Without a second AP, leave it `[ ]` with the reason. F1 then stays unverified on hardware (M4 plan, "Changes after the final review").
  - **L6 สรุปงบหน่วยความจำ:** the minimum heap and each HWM over L1–L4, against the budget.

- [ ] **Step 2: Check the section.** `grep -c "^- \[ \] \*\*L" docs/bench-test-thingsboard.md` must print `7`. Every L item must have a `**ต้องเห็น:**` line.

- [ ] **Step 3: Commit**
  ```bash
  git add docs/bench-test-thingsboard.md
  git commit -m "docs: bench round 7 checklist (milestone 6: TLS, link off/on, ThingsBoard down, soak, J4)"
  ```

### Task 2: Run round 7 with the user

**Files:**
- Modify: `docs/bench-test-thingsboard.md`: mark L0–L6 `[/]` or `[x]` and fill each `บันทึก:` line.
- Scratchpad only: the poller script and log, and a `history <key> <startTs> <endTs>` command added to `ota_bench.py`. It runs `GET /api/plugins/telemetry/DEVICE/<id>/values/timeseries?keys=<key>&startTs=&endTs=&limit=100` and `/api/alarm/DEVICE/<id>` for `rule_19`.

**Interfaces:**
- Consumes: the L ids from Task 1.

- [ ] **Step 1: L0.** Start the poller, the driver and the monitor. Ask the user to type the ThingsBoard password.
- [ ] **Step 2: L1 and L2 in the browser pane.**
  - The user logs in to the board web app at `http://192.168.1.34`.
  - Claude edits the card.
  - Expected: as in the checklist. Record the times from the serial log.
- [ ] **Step 3: L3.**
  - Ask the user for the OK to stop `tb-node`, and only then ask them to run it.
  - Expected: as in the checklist.
  - Rule 19 must be restored before the step is marked.
- [ ] **Step 4: L4.**
  - Run 60 min with the RPC bursts.
  - Expected: as in the checklist. Copy the BOARD_DIAG minimums from the monitor.
- [ ] **Step 5: L5.** Ask the user whether a second 2.4 GHz access point on the LAN exists. Run it only if one does.
- [ ] **Step 6: L6.** Fill in the summary. After the round, check:
  - host `192.168.1.51`, TLS off, link connected;
  - rule 19 disabled with its original fields;
  - driver quit and poller stopped.
- [ ] **Step 7: Commit**
  ```bash
  git add docs/bench-test-thingsboard.md
  git commit -m "docs: bench round 7 results (milestone 6)"
  ```

### Task 3: Close the backlog and the bench file

**Files:**
- Modify: `docs/backlog/after-frontend.md`:
  - strike I7 ("เลื่อนไปรวมกับทดสอบ ThingsBoard ล่มใน milestone 6") with the round-7 result;
  - mark J4 done, or keep it with "needs a second AP";
  - add every `[x]` finding from round 7.
- Modify: `docs/bench-test-thingsboard.md`: change the header's firmware line to the range that was benched (`c058d1a`..`e9f9a92`), and add a short "ครอบคลุม spec ข้อ 8" list mapping each bench bullet of spec section 8 to its round and step.

- [ ] **Step 1: Make the edits above.**
- [ ] **Step 2: Check.** Every bench bullet of spec section 8 must appear in the coverage list with a step id, or with "ยังค้าง" and a reason.
- [ ] **Step 3: Commit**
  ```bash
  git add docs/backlog/after-frontend.md docs/bench-test-thingsboard.md
  git commit -m "docs: milestone 6 close-out (backlog, spec coverage)"
  ```
- [ ] **Step 4: Report.**
  - Tell the user that the MQTT work is complete, or list what is still owed.
  - The next step is the squash-merge into a clean branch, which happens only when the user asks. Raising `version.txt` to 0.4.0 at that point is the user's call.
