# ThingsBoard Link — Milestone 4 (remote Wi-Fi change with automatic rollback) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `wifi.connect` works from ThingsBoard without risk of stranding the board. The board stages the new network, reboots onto it, and keeps it only if it reaches ThingsBoard again within 180 s. Otherwise it rolls back to the old network by itself.

**Architecture:**
- The firmware already has a boot-time Wi-Fi staging path that nothing uses yet:
  - `nvs_store_stage_wifi()` sets `stg_type=1`.
  - On the next boot, `wifi_manager` connects with the staged SSID.
  - `app_main` starts a 30 s "Wi-Fi up" timer and a 180 s confirm timer, then rolls back and reboots unless something cancels them.
- **`wifi.connect` from ThingsBoard:**
  - It checks its parameters and the new guards, stages the network, and answers `{"ok":true,"staged":true}`.
  - It asks `tb_link` to run a restart action once the reply's PUBACK arrives, or after 2 s.
- **Commit:** on the next accepted MQTT connect, a hook in `firmware/main/wifi_remote.c` commits the staging, makes the new profile the auto-connect one and cancels the timers. This happens before the RPC subscription is renewed.
- The HTTP `wifi.connect` path does not change.

**Tech Stack:** ESP-IDF v6.0.1, managed `espressif/mqtt ^1.1.0`, cJSON, FreeRTOS, ThingsBoard CE 4.3.1. The PC scripts use the Python 3 standard library only.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`, sections 3 (anti-goal: "A remote Wi-Fi change or OTA that leaves the board unreachable without someone visiting it"), 6.1 (remote guard rails), 6.4, 7.2, 8 and 8a item 4. Milestones 1–3 are done (`docs/bench-test-thingsboard.md`, rounds 1–4).

Out of scope:
- OTA (M5);
- the ThingsBoard-down test and I7 (M6);
- changing the web app's own connect flow.

## Global Constraints

- **Gates.** These stay green after every task:
  - The build with **0 warnings**: `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM; idf.py -C firmware build`.
  - Every `firmware/scripts/verify_*.py` script.
  - The QEMU selftest `firmware/test/sensor_selftest/run_selftest.ps1`. The baseline is 1955 checks; the count only grows.
  - `python thingsboard/parity_test.py --selftest` and `python thingsboard/tb_dashboard.py --check`.
- **The web app does not change.**
  - No file under `firmware/main/static/` changes.
  - The HTTP `wifi.connect`, `wifi.profiles.*` and `/api/confirm` keep their status lines and bodies.
  - Every new guard returns "not blocked" for `DAPI_SRC_HTTP`.
- **Timing values** (spec 6.4 and `app_config.h`, unchanged):

  | What | Value |
  |---|---|
  | Confirm timer | `APP_CONFIG_ROLLBACK_CONFIRM_TIMEOUT_MS` 180000 |
  | Wi-Fi-up timer | `APP_CONFIG_ROLLBACK_WIFI_TIMEOUT_MS` 30000 |
  | Reply wait, new `TB_AFTER_REPLY_MAX_MS` in `tb_link_logic.h` | 2000 ms |
  | MQTT reconnect delay while `stg_type == 1` | fixed `TB_BACKOFF_MIN_MS` (5000) |

- **Replies from ThingsBoard `wifi.connect`**, exactly:

  | Case | Status | Body |
  |---|---|---|
  | Staged | `DAPI_OK` | `{"ok":true,"staged":true}` |
  | Static-IP fields (`ip` present and non-empty) | `DAPI_LOCAL_ONLY` | `{"ok":false,"error":"local_only","message":"Static IP needs the local web app"}` |
  | `stg_type != 0` | `DAPI_CONFLICT` | `{"ok":false,"error":"staging_active"}` |
  | Dry-run measurement running | `DAPI_CONFLICT` | `{"ok":false,"error":"measure_running"}` |
  | NVS stage write failed | `DAPI_INTERNAL` | `{"ok":false,"error":"stage_failed"}` |

  - The existing validation (`missing_ssid`, `ssid_too_long`, `invalid_ssid`, `password_too_long`, `invalid_password`, the saved-password lookup) and the `already_connected` reply stay exactly as they are, for both sources.
  - The guards run after validation and after the `already_connected` check.
- **Method flags:** `wifi.connect` becomes `DAPI_F_WRITE | DAPI_F_STAGED`, no longer LOCAL_ONLY. `wifi.disconnect` stays LOCAL_ONLY.
- **Restart detail** for the staged switch: `"WIFI_REMOTE_STAGE"` (`nvs_store_set_restart_detail`).
- **Passwords:** never logged and never in a reply or attribute. `device_api` already logs only the method name.
- **Log tag:** the new file `wifi_remote.c` logs under the tag `wifi_remote`, raised to INFO in `app_main.c`. Its lines are: staged commit, auto-connect set, and restart for the staged switch.
- **Never stage** `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`. No merge or push.

## Review Focus

1. **ThingsBoard re-delivers the `wifi.connect` request after the reboot.**
   - Risk: the reply's PUBACK was lost, and ThingsBoard re-sends pending requests on re-subscribe; the M2 bench saw this.
   - Expected: the board must not stage again.
   - Protection: the connected hook (commit) runs **before** the RPC topic is re-subscribed. After that the SSID equals the connected one, so the request gets `already_connected`. The `staging_active` guard covers any other order.
   - Pinned by: a Task 2 verify check on the order in `CMD_CONNECTED`, and a Task 1 guard test.
2. **The link drops between the reply and its PUBACK.**
   - Expected: the switch still happens within 2 s and never waits forever.
   - Pinned by: Task 1 `tb_after_reply_ready(false, 2000) == true`.
3. **Power cycle after a successful remote switch.**
   - Expected: the board comes back on the new network, not the old one, because commit alone adds the profile without making it auto.
   - Pinned by: a Task 3 verify check that the hook calls `nvs_store_set_wifi_auto_connect`, and bench J4.
4. **`wifi.connect` from ThingsBoard during a dry-run measurement or while staging is already active.**
   - Expected: refused, with no reboot.
   - Pinned by: Task 1 guard tests.
5. **Wrong password, or a network that cannot reach ThingsBoard.**
   - Expected: automatic rollback within 30 s (no Wi-Fi) or 180 s (no ThingsBoard), back on the old network.
   - Pinned by: bench J2 and J3, which use the existing timers.

## Calls in this plan (the user can overturn any)

1. **The spec assumed the web app's connect already used staging; it does not.**
   - `POST /api/wifi/connect` switches at once and saves the profile on success. The web user standing next to the board can recover through the AP.
   - So only the ThingsBoard path stages. It uses the existing boot-time staging, which means **one reboot when the switch starts and one more if it rolls back**. The pump schedule restarts and the relays come up safe, as on any reboot.
   - A live switch without a reboot would need new switch-back code in `wifi_manager`. It is not chosen.
2. **The commit also makes the new network the auto-connect profile.** Committing alone adds the profile but leaves auto-connect on the old one, so a power cut would silently move the board back.
3. **A static IP cannot be set from ThingsBoard** (`local_only`). Staging stores only the SSID and password.
4. **`wifi.connect` from ThingsBoard is refused while a dry-run measurement runs**, rather than rebooting in the middle of it.
5. **The M2 backlog guard is folded in:** `wifi.profiles.save` from ThingsBoard with an empty password for an SSID that has no saved profile gets `in_use`.
6. **The parity case that sends `wifi.connect` with a real-looking SSID is removed before the new firmware is flashed.** Today it expects `local_only`; after this milestone it would stage and reboot the board.

---

### Task 1: Pure helpers and guards

**Files:**
- Modify:
  - `firmware/components/tb_link_logic/include/tb_link_logic.h` and `firmware/components/tb_link_logic/tb_link_logic.c`
  - `firmware/components/dapi_guards/include/dapi_guards.h` and `firmware/components/dapi_guards/dapi_guards.c`
- Test: `firmware/test/sensor_selftest/main/test_tb_link.c` and `test_dapi_guards.c`

**Interfaces:**
- Produces:
  - `#define TB_AFTER_REPLY_MAX_MS 2000u`
  - `uint32_t tb_backoff_after_fail_ms(uint32_t current_ms, bool at_cap, bool staging);`
    - `staging` → `TB_BACKOFF_MIN_MS`.
    - Else `at_cap` → `TB_BACKOFF_MAX_MS`.
    - Else `tb_backoff_next_ms(current_ms)`.
  - `bool tb_after_reply_ready(bool acked, uint32_t elapsed_ms);` = `acked || elapsed_ms >= TB_AFTER_REPLY_MAX_MS`.
  - `const char *dapi_wifi_connect_refusal(dapi_source_t src, bool static_ip, bool staging_active, bool measuring);`
    - Returns NULL for HTTP, or when nothing blocks.
    - Otherwise returns, checked in this order: `"local_only"` (static IP), `"staging_active"`, `"measure_running"`.
  - `bool dapi_wifi_save_empty_blocked(dapi_source_t src, const char *password, bool profile_exists);` True only for TB, with an empty or NULL password and no existing profile.

- [ ] **Step 1: Write the failing tests.**
  - `tb_backoff_after_fail_ms`:
    - `(0, false, true)`, `(60000, true, true)` and `(20000, false, true)` all give 5000.
    - `(0, false, false)` gives 5000.
    - `(5000, false, false)` gives 10000.
    - `(40000, false, false)` gives 60000.
    - `(0, true, false)` gives 60000.
  - `tb_after_reply_ready`:
    - `(true, 0)` is true.
    - `(false, 1999)` is false.
    - `(false, 2000)` and `(false, 60000)` are true.
  - `dapi_wifi_connect_refusal`:
    - `(DAPI_SRC_HTTP, true, true, true)` gives NULL.
    - `(TB, false, false, false)` gives NULL.
    - `(TB, true, true, true)` gives `"local_only"`.
    - `(TB, false, true, true)` gives `"staging_active"`.
    - `(TB, false, false, true)` gives `"measure_running"`.
  - `dapi_wifi_save_empty_blocked`:
    - `(HTTP, "", false)` is false.
    - `(TB, "", false)` and `(TB, NULL, false)` are true.
    - `(TB, "", true)` is false.
    - `(TB, "pw12345678", false)` is false.
- [ ] **Step 2: Run the QEMU selftest and see it fail.** Expected: a build error on the missing names.
- [ ] **Step 3: Implement the four functions.**
- [ ] **Step 4: Run the selftest and see it pass.** Expected: `SELFTEST PASS (<n>)` with n > 1955. The build has 0 warnings.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/tb_link_logic firmware/components/dapi_guards firmware/test/sensor_selftest/main
  git commit -m "feat: remote Wi-Fi helpers: staging backoff, reply wait, connect and save guards"
  ```

### Task 2: `tb_link` — defer an action until the reply is acked, connected hook, staging backoff

**Files:**
- Modify: `firmware/components/tb_link/include/tb_link.h`, `firmware/components/tb_link/tb_link.c`
- Test: `firmware/scripts/verify_tb_link.py`. Add a new function `test_remote_wifi_link()` and call it from `main()`.

**Interfaces:**
- Consumes: Task 1 `tb_backoff_after_fail_ms` and `tb_after_reply_ready`; `nvs_store_get_staging_type`.
- Produces (in `tb_link.h`):
  - `bool tb_link_defer_after_reply(void (*action)(void));`
    - Valid only on the `tb_rpc` task while it runs a method. It returns false anywhere else, which it detects by comparing the current task handle with the `tb_rpc` handle.
    - It records the action for the request being processed.
    - After `rpc_process` enqueues that reply, the `tb_rpc` task waits until `tb_after_reply_ready(acked, elapsed)`, then calls `action()`.
    - `acked` means `MQTT_EVENT_PUBLISHED` reported the reply's `msg_id`. Record it in an atomic, set by the event handler.
    - The action also runs if enqueueing the reply failed, after the 2 s.
    - A cached (dedup) reply never runs an action.
  - `void tb_link_set_connected_hook(void (*hook)(void));` An atomic pointer, set once at boot.
    - It is called on the `tb_link` task in `CMD_CONNECTED` for a token session (not provisioning).
    - It is called **before** `esp_mqtt_client_subscribe_single(..., TB_RPC_SUB_TOPIC, ...)`.
  - In `task_fail`, the backoff becomes `tb_backoff_after_fail_ms(tc->backoff_ms, at_cap, stg_type == 1)`, with `stg_type` read through `nvs_store_get_staging_type`.

- [ ] **Step 1: Write the failing verify checks** in `test_remote_wifi_link()`.
  - `tb_link.h` declares both functions.
  - The event handler has a `case MQTT_EVENT_PUBLISHED`.
  - In the body of the `CMD_CONNECTED` handling, the connected hook call comes before the text `TB_RPC_SUB_TOPIC`.
  - `task_fail` uses `tb_backoff_after_fail_ms` and `nvs_store_get_staging_type`, and no longer calls `tb_backoff_next_ms` directly.
  - The `tb_rpc` code calls `tb_after_reply_ready`.
- [ ] **Step 2: Run `python firmware/scripts/verify_tb_link.py` and see the new checks fail.**
- [ ] **Step 3: Implement it.** The wait loop sleeps in 50 ms steps. While it waits, no other RPC is processed; the restart follows anyway.
- [ ] **Step 4: Run the gates.**
  - `verify_tb_link.py` passes.
  - The build has 0 warnings.
  - The QEMU selftest count is unchanged from Task 1.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/tb_link firmware/scripts/verify_tb_link.py
  git commit -m "feat: tb_link defers an action until the RPC reply is acked; connected hook; fixed backoff while staging"
  ```

### Task 3: `wifi_remote` — commit the staged network on the ThingsBoard connect

**Files:**
- Create: `firmware/main/wifi_remote.c`, `firmware/main/wifi_remote.h`
- Modify: `firmware/main/CMakeLists.txt` (SRCS) and `firmware/main/app_main.c` (register the hook after `tb_link_init()`, and the log tag).
- Test: `firmware/scripts/verify_tb_link.py`. Add a new function `test_wifi_remote()`.

**Interfaces:**
- Consumes:
  - Task 2 `tb_link_set_connected_hook`;
  - `nvs_store_get_staging_type`, `nvs_store_commit_staging`, `nvs_store_load_wifi_profiles`, `nvs_store_set_wifi_auto_connect`, `nvs_store_set_restart_detail`;
  - `session_invalidate_all`;
  - `uplink_note_settings_changed`;
  - `g_cancel_rollback_timer` (the `atomic_bool` already defined in `app_main.c`; declare it `extern` in `wifi_remote.c`).
- Produces:
  - `void wifi_remote_on_tb_connected(void);`
    - Does nothing unless `stg_type == 1`.
    - Otherwise:
      1. Read the staged SSID: the `wifi_cfg` NVS key `stg_sta_ssid`.
      2. Call `nvs_store_commit_staging()`. It must return 1.
      3. Find that SSID in the profile list and call `nvs_store_set_wifi_auto_connect(index)`.
      4. Store `true` in `g_cancel_rollback_timer`.
      5. Call `uplink_note_settings_changed()`.
      6. Log `staged Wi-Fi <ssid> committed by ThingsBoard connect`.
    - A failure at any step logs a WARN and leaves the timers running. Rollback is then still possible.
  - `void wifi_remote_restart_for_stage(void);` Calls `nvs_store_set_restart_detail("WIFI_REMOTE_STAGE")`, then `session_invalidate_all()`, then `esp_restart()`.
  - `app_main.c` calls `tb_link_set_connected_hook(wifi_remote_on_tb_connected)` right after a successful `tb_link_init()`, and adds `esp_log_level_set("wifi_remote", ESP_LOG_INFO)`.

- [ ] **Step 1: Write the failing verify checks** in `test_wifi_remote()`.
  - `wifi_remote.c` is in SRCS.
  - `app_main.c` registers the hook after `tb_link_init` and raises the `wifi_remote` tag.
  - The body of `wifi_remote_on_tb_connected` calls, in this order: `nvs_store_get_staging_type`, `nvs_store_commit_staging`, `nvs_store_set_wifi_auto_connect`, `g_cancel_rollback_timer`, `uplink_note_settings_changed`.
  - `wifi_remote_restart_for_stage` contains `"WIFI_REMOTE_STAGE"` and `esp_restart`.
  - `wifi_remote.c` has no `ESP_LOG` line that contains `pass`.
- [ ] **Step 2: Run `verify_tb_link.py` and see the new checks fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run the gates.** All verify scripts pass, the build has 0 warnings and the selftest passes.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/main/wifi_remote.c firmware/main/wifi_remote.h firmware/main/CMakeLists.txt firmware/main/app_main.c firmware/scripts/verify_tb_link.py
  git commit -m "feat: commit a remotely staged Wi-Fi network on the ThingsBoard connect"
  ```

### Task 4: `wifi.connect` and `wifi.profiles.save` from ThingsBoard

**Files:**
- Modify: `firmware/main/dapi_wifi.c`, `docs/firmware/device-api.md` (the method table, the reply table, and the note that `wifi.connect` is LOCAL_ONLY "until milestone 4")
- Test: `firmware/scripts/verify_device_api.py` (the flag pinning) and `verify_tb_link.py` (new function `test_wifi_connect_remote()`)

**Interfaces:**
- Consumes:
  - Task 1 `dapi_wifi_connect_refusal` and `dapi_wifi_save_empty_blocked`;
  - Task 2 `tb_link_defer_after_reply`;
  - Task 3 `wifi_remote_restart_for_stage`;
  - `nvs_store_get_staging_type`, `nvs_store_stage_wifi`, `nvs_store_rollback_staging`, `dry_run_guard_get_measure`.
- **In `dapi_wifi_connect`, when `src == DAPI_SRC_TB`**, after the existing validation and the `already_connected` check:
  1. Call `dapi_wifi_connect_refusal(src, has_static_ip, stg_type != 0, measuring)`.
     - `measuring` = `dry_run_guard_get_measure` reports a running state.
     - A non-NULL result returns the reply from Global Constraints.
  2. Call `nvs_store_stage_wifi(ssid, password)`, where the password may be the saved one. On failure, reply `stage_failed`.
  3. Call `tb_link_defer_after_reply(wifi_remote_restart_for_stage)`. If it returns false, call `nvs_store_rollback_staging()` and reply `stage_failed`.
  4. Reply `{"ok":true,"staged":true}`.
  - The HTTP branch is the current code, unchanged.
- **In `dapi_wifi_profiles_save`**, when `dapi_wifi_save_empty_blocked(src, password, <SSID already in profiles>)` is true, reply `{"ok":false,"error":"in_use"}` with `DAPI_CONFLICT`. This uses the same body helper as the other `in_use` guards.
- The method table: `{"wifi.connect", DAPI_F_WRITE | DAPI_F_STAGED, WIFI_CONNECT_BODY_BYTES, dapi_wifi_connect}`.

- [ ] **Step 1: Write the failing verify checks.**
  - `verify_device_api.py`:
    - `wifi.connect` has `DAPI_F_STAGED` and not `DAPI_F_LOCAL_ONLY`;
    - `wifi.disconnect` still has `DAPI_F_LOCAL_ONLY`.
  - `test_wifi_connect_remote()`:
    - the TB branch of `dapi_wifi_connect` calls `dapi_wifi_connect_refusal`, `nvs_store_stage_wifi`, `tb_link_defer_after_reply(wifi_remote_restart_for_stage)` and `nvs_store_rollback_staging`, and contains `"staged"`;
    - `dapi_wifi_profiles_save` calls `dapi_wifi_save_empty_blocked`;
    - no `ESP_LOG` line in `dapi_wifi.c` prints the `password` variable.
- [ ] **Step 2: Run both scripts and see the new checks fail.**
- [ ] **Step 3: Implement it, and update `device-api.md`.**
  - In the method table, the flags become WRITE, STAGED.
  - Add the TB-only replies from Global Constraints.
  - Add a short section "Remote Wi-Fi change (milestone 4)":
    - the stage → reboot → commit-on-connect flow;
    - the 30 s and 180 s rollbacks;
    - that a network which cannot reach ThingsBoard always rolls back;
    - that the commit makes the new profile auto-connect.
- [ ] **Step 4: Run every gate.** All verify scripts pass, the build has 0 warnings and the QEMU selftest passes.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/main/dapi_wifi.c firmware/scripts/verify_device_api.py firmware/scripts/verify_tb_link.py docs/firmware/device-api.md
  git commit -m "feat: wifi.connect from ThingsBoard stages the network and restarts after the reply"
  ```

### Task 5: Parity and dashboard for remote Wi-Fi

**Files:**
- Modify: `thingsboard/parity_cases.json`, `thingsboard/parity_test.py` (selftest only, if a case shape changes), `thingsboard/dashboard_pages.json`, `thingsboard/tb_dashboard.py` (the check), `thingsboard/README.md`

**Interfaces:**
- The Wi-Fi group's error cases from ThingsBoard. None of them may ever stage:
  - **Remove** `{"method": "wifi.connect", "params": {"ssid": "parity-no-such-network"}, ..., "error": "local_only"}`.
  - **Add** `{"method": "wifi.connect", "params": {"ssid": "parity-no-such-network", "ip": "192.168.1.250", "gateway": "192.168.1.1"}, "http": null, "error": "local_only"}`.
  - **Add** `wifi.connect` `{}` → `missing_ssid`, compared with HTTP.
  - **Add** `wifi.connect` with an SSID of 33 characters → `ssid_too_long`, compared with HTTP.
  - Keep `wifi.disconnect` → `local_only`.
- `tb_dashboard.py --check` gains one rule, with a matching `BROKEN_FIXTURE` entry: an action whose method is `wifi.connect` must have a non-empty Thai `confirm`.
- The dashboard Wi-Fi page action:
  - label: `wifi.connect (สลับเครือข่าย: บอร์ดจะรีบูตแล้วกลับมา)`
  - params: `{"ssid": "แก้ก่อนส่ง", "password": "แก้ก่อนส่ง"}`
  - confirm: `บอร์ดจะรีบูตไปต่อเครือข่ายใหม่ ถ้าภายใน 3 นาทีต่อ ThingsBoard ไม่ได้ (รหัสผิด หรือเครือข่ายนั้นมองไม่เห็น ThingsBoard เช่น hotspot มือถือ) บอร์ดจะกลับไปเครือข่ายเดิมเองและรีบูตอีกครั้ง ดำเนินการต่อ?`

- [ ] **Step 1: Write the failing check** in `tb_dashboard.py`, with its `BROKEN_FIXTURE` entry. Run `--check` and see it fail on the current page file, which has no confirm on `wifi.connect`.
- [ ] **Step 2: Update the cases and the page entry**, and add a README line: "the Wi-Fi group never sends a `wifi.connect` that could stage".
- [ ] **Step 3: Run the gates and see them pass.**
  - `python thingsboard/tb_dashboard.py --check`
  - `python thingsboard/parity_test.py --selftest`
  - `python firmware/scripts/verify_tb_link.py`
- [ ] **Step 4: Commit**
  ```bash
  git add thingsboard/parity_cases.json thingsboard/parity_test.py thingsboard/dashboard_pages.json thingsboard/tb_dashboard.py thingsboard/README.md
  git commit -m "feat: parity and dashboard for the remote Wi-Fi change"
  ```

### Task 6: Bench round 5 (the controller runs it with the user; not a subagent task)

**Files:** `docs/bench-test-thingsboard.md` (new section "รอบ 5: เปลี่ยน Wi-Fi จากระยะไกล (milestone 4)"), `docs/backlog/after-frontend.md`.

How the work is split:
- The user flashes and types every password, including the Wi-Fi password on the dashboard.
- Claude runs the scripts through the user's Terminal 2 scripts and drives the dashboard.
- Before J2–J4, ask the user which second 2.4 GHz network they have. It may be a phone hotspot, which cannot reach ThingsBoard on the LAN, or a second access point on the LAN.

**Changes after the final review (2026-11-20, commit f07e58b):**
- The final review found that a committed remote switch never saved the new profile (the staging overlay in `nvs_store_load_wifi_profiles`). It is fixed, but the QEMU selftest does not link `nvs_store`, so only J4 proves the fix on hardware. **J4 is required for sign-off**; if no second LAN access point exists, record F1 as unverified on hardware.
- J2, J3 and J4: before and after each, read `wifi.profiles.get`. J2/J3 expect the list and `auto_index` unchanged; J4 expects the new SSID present, `auto_index` pointing at it and the old profiles intact.
- J2 may roll back through either path: within about 30 s, or, when wifi_manager falls back to the old credential, through the 180 s confirm timer (the log shows `connected on <old>, not on staged <new>; leaving it staged`). Both pass.
- J5 step 3: the empty-password `in_use` guard applies only to the connected network when it has no saved profile; another SSID gets `not_connected` first. Accept `not_connected` for an unsaved, unconnected SSID.
- J6: read the tb_link stack HWM after a successful J4 commit.

- [ ] **J0 (before flashing):** run `api_golden.py capture --label m4pre --with-writes`.
- [ ] **J1:**
  - Flash.
  - Run `api_golden.py compare --base m4pre --label m4post --with-writes`. Expected: ALL PASS (the web app is unchanged).
  - Run `tb_setup.py` (upload the dashboard), then `parity_test.py`. Expected: 0 FAIL; the new Wi-Fi error rows pass and nothing stages.
- [ ] **J2 (wrong password):**
  - Send `wifi.connect` from the dashboard to the second network with a wrong password.
  - Expected:
    - the reply is `{"ok":true,"staged":true}`;
    - the board reboots;
    - within about 30 s of boot, the log shows the Wi-Fi rollback, then `STAGING_ROLLBACK`;
    - the board reboots onto the old network and ThingsBoard reconnects;
    - auto-connect is still the old profile.
- [ ] **J3 (a network that cannot reach ThingsBoard, e.g. a phone hotspot with the right password):**
  - Expected:
    - the board gets an IP there;
    - MQTT retries every 5 s (fixed backoff);
    - after 180 s the confirm timer rolls back and the board returns to the old network.
- [ ] **J4 (success). Only if a second access point on the LAN exists; otherwise record it as not run.**
  - Expected:
    - after the reboot the board reconnects to ThingsBoard on the new network;
    - the log shows `staged Wi-Fi … committed by ThingsBoard connect`;
    - no rollback after 180 s;
    - the `wifi` attribute shows the new SSID;
    - after a power cycle the board stays on it.
  - Then switch back the same way.
- [ ] **J5 (the Wi-Fi buttons pressed live, from the M2 backlog):**
  - `wifi.profiles.save` on the connected network with its current password → `ok`.
  - `wifi.profiles.setauto` on the connected index → `ok`.
  - `wifi.profiles.save` with an empty password for an unsaved SSID → `in_use`.
- [ ] **J6 (budget):** `BOARD_DIAG` shows `heap_min` ≥ 100 KB and every HWM ≥ 512.
- [ ] **Write the results into the bench file, move failures into the backlog, and commit** `docs: bench milestone 4`.
