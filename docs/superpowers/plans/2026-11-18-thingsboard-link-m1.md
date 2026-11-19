# ThingsBoard Link — Milestone 1 (MQTT link, provisioning, SNTP, server setup) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The board connects to the ThingsBoard on `copter` over MQTT. It provisions itself on first connect and sends the existing uplink messages (telemetry, alarm events with real time, identity, rules). ThingsBoard turns alarm events into ThingsBoard alarms. The link is configured from a new card in the local web app.

**Architecture:**
- A pure-C component `tb_link_logic` holds everything testable in QEMU: config validation and JSON, the provisioning request and response, the default device name and the backoff.
- An ESP component `tb_link` owns one esp-mqtt client and is driven by its own task and command queue. It plugs into the existing `uplink` sink once at startup.
- A small `time_sync` component starts SNTP and hands the clock to `uplink`.
- The ThingsBoard side is a Python setup script with a rule-chain JSON, plus a rule-chain test script that speaks MQTT/REST, all under `thingsboard/` in the repo.

**Tech Stack:** ESP-IDF v6.0.1 (`esp_netif_sntp`; esp-mqtt is the managed component `espressif/mqtt ^1.1.0`, pulled in by `components/tb_link/idf_component.yml`), cJSON (managed `espressif/cjson`), vanilla JS web app, Python 3 standard library only (no pip packages) for the ThingsBoard scripts, ThingsBoard CE 4.3.1.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`. This plan covers milestone 1 of section 8a. Milestones 2–6 each get their own plan after this one is benched.

## Global Constraints

- **Toolchain and gates:**
  - ESP-IDF v6.0.1 on ESP32-S3. Build environment: `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM`, then `idf.py -C firmware build`.
  - The firmware build must have **0 warnings**.
  - The existing gates stay green: `python firmware/scripts/verify_alarm_system.py`, `verify_dry_run.py`, `verify_sensor_integration.py`, `verify_pre_mqtt.py`, `verify_pt100_inverter_system.py`, and the QEMU selftest `firmware/test/sensor_selftest/run_selftest.ps1` → `SELFTEST PASS`.
- **ThingsBoard host:** CE 4.3.1 at `192.168.1.51`, HTTP 8080, MQTT 1883, plain (no TLS).
- **Uplink contract** (`firmware/components/uplink/include/uplink.h`):
  - `uplink_set_sink` is only for setup/teardown, never from the MQTT event handler, a sink callback or a context holding a lock the sink takes.
  - The sink's `send` must not block. Use `esp_mqtt_client_enqueue`.
  - Do not change the uplink payload formats (contract v1, `fw_type:"fishpump"`).
- **Topics:**
  - Telemetry: `v1/devices/me/telemetry`; attributes: `v1/devices/me/attributes` (`UPLINK_TOPIC_*` already defined).
  - Provisioning: username `provision`, empty password; subscribe `/provision/response` **before** publishing `/provision/request`.
  - Request body: `{"deviceName","provisionDeviceKey","provisionDeviceSecret"}`.
  - Response: `"status":"SUCCESS"` with `"credentialsType":"ACCESS_TOKEN"` and `"credentialsValue"`, or `"status":"FAILURE"` with `"errorMsg"`.
- **Provisioning is one-time per device name.** A FAILURE is shown on the card and retried only at the 60 s backoff cap, never in a tight loop.
- **Device name default:** `fishpump-<last 3 MAC bytes, lowercase hex>`, for example `fishpump-a1b2c3`, using the Wi-Fi STA MAC.
- **Reconnect backoff:** starts at 5 s, doubles each failure, caps at 60 s, and resets to 5 s after an accepted connect.
- **SNTP:** servers `pool.ntp.org` and `time.google.com`, with `CONFIG_LWIP_SNTP_MAX_SERVERS=2`. The wall clock goes to `uplink_set_wall_clock()` only after the first sync.
- **Logging:** new log tags `tb_link` and `time_sync` are raised to INFO in `app_main.c` next to the other app tags. The global floor is WARN.
- **Secrets:**
  - The token and provision secret are never returned by any GET and never logged. The provision key is not secret and may be shown.
  - The ThingsBoard tenant-admin password is never stored in the repo or in a file. It is read from env `TB_PASSWORD` or typed at a `getpass` prompt by the user; Claude must not type it.
- **Budgets:** internal heap free ≥ 100 KB with the link connected, and every task's stack HWM ≥ 512 B.
- **Web UI copy is Thai**, matching the existing pages.
- **Git:**
  - Never stage `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`.
  - Commit each task on `feature/thingsboard-mqtt`. No push, no merge.
  - End commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Local functions must not depend on the link.** A down, wrong or unreachable ThingsBoard must not delay or break the web app, alarms, LED, pump or dry-run protection.

## Review Focus

1. **Wrong or unreachable host** (typo, server down). The link retries on the 5→60 s backoff, the card shows the error, and nothing else on the board slows down. Pinned by the backoff selftest (Task 1) and bench step B4 (Task 7).
2. **Saving the config again while connected or mid-provisioning.** The old client is torn down and a new one starts: no crash, no second client, no stale `is_connected`. Pinned by a `verify_tb_link.py` check (Task 6) that client create/destroy happens only in the `tb_link` task, and by bench step B5.
3. **Provisioning FAILURE because the device name already exists.** `errorMsg` shows on the card, and the retry waits at the 60 s cap. Pinned by the `tb_provision_parse` FAILURE test (Task 1) and bench step B3.
4. **Large attribute messages.** Identity (about 5 KB) and rules (up to about 9 KB) must arrive whole in ThingsBoard. Pinned by bench step B2, which checks the `signals` and `alarm_rules` attributes in ThingsBoard.
5. **Duplicate or replayed alarm events** (QoS 1 resend, backfill after a gap) must not create a second ThingsBoard alarm or re-open a cleared one. Pinned by `tb_event_test.py` (Task 5), which replays the same `(boot_id, seq)`.

---

### Task 1: `tb_link_logic` pure component + selftests

**Files:**
- Create: `firmware/components/tb_link_logic/CMakeLists.txt` (REQUIRES `cjson` only)
- Create: `firmware/components/tb_link_logic/include/tb_link_logic.h`
- Create: `firmware/components/tb_link_logic/tb_link_logic.c`
- Create: `firmware/test/sensor_selftest/main/test_tb_link.c`
- Modify:
  - `firmware/test/sensor_selftest/CMakeLists.txt`: add `../../components/tb_link_logic` to `EXTRA_COMPONENT_DIRS`.
  - `firmware/test/sensor_selftest/main/CMakeLists.txt`: add the source and `REQUIRES tb_link_logic`.
  - `selftest.h` and `selftest_main.c`: declare and call `test_tb_link()`.

**Interfaces:**
- Produces:
  ```c
  #define TB_HOST_MAX 64          /* incl. NUL */
  #define TB_NAME_MAX 48
  #define TB_TOKEN_MAX 65
  #define TB_PROV_FIELD_MAX 33    /* key and secret */
  #define TB_DEFAULT_PORT 1883
  #define TB_BACKOFF_MIN_MS 5000u
  #define TB_BACKOFF_MAX_MS 60000u

  typedef struct {
      char host[TB_HOST_MAX];          /* "" = link off */
      uint16_t port;
      bool tls;
      char device_name[TB_NAME_MAX];
      char token[TB_TOKEN_MAX];        /* "" = not provisioned */
      char provision_key[TB_PROV_FIELD_MAX];
      char provision_secret[TB_PROV_FIELD_MAX];
  } tb_link_config_t;

  void tb_link_config_defaults(tb_link_config_t *cfg, const uint8_t sta_mac[6]);   /* host "", port 1883, tls false, default name */
  void tb_default_device_name(const uint8_t sta_mac[6], char *out, size_t size);  /* "fishpump-a1b2c3" */
  bool tb_link_config_valid(const tb_link_config_t *cfg, const char **bad_field); /* *bad_field = JSON key name */

  /* Merge a POST body into *out (starting from *current). Absent key = keep; "" for token/provision_secret = clear.
   * Returns false with *bad_field set on wrong type, too long, or invalid result. */
  bool tb_link_config_from_json(const cJSON *body, const tb_link_config_t *current,
                                tb_link_config_t *out, const char **bad_field);
  /* Public view: host, port, tls, device_name, provision_key, has_token, has_provision_secret. Never token/secret. */
  cJSON *tb_link_config_to_json(const tb_link_config_t *cfg);

  int tb_provision_build_request(const tb_link_config_t *cfg, char *buf, size_t size); /* bytes or -1 */
  typedef enum { TB_PROV_OK = 0, TB_PROV_FAILURE, TB_PROV_BAD_REPLY } tb_prov_result_t;
  /* OK: token copied. FAILURE: errorMsg copied (may be ""). BAD_REPLY: not JSON, no status, or credentialsType != ACCESS_TOKEN. */
  tb_prov_result_t tb_provision_parse(const char *json, size_t len, char *token, size_t token_size,
                                      char *err, size_t err_size);

  uint32_t tb_backoff_next_ms(uint32_t current_ms); /* 0 -> 5000, else min(2*current, 60000) */
  ```

**Validation rules (exact):**
- `host`: empty, or 1–63 chars from `[A-Za-z0-9.-]`.
- `port`: 1–65535.
- `device_name`: 1–47 chars, printable ASCII without `"` and `\`.
- `token`: empty or 1–64 chars from `[A-Za-z0-9_-]`.
- `provision_key` and `provision_secret`: empty or 1–32 chars from `[A-Za-z0-9_-]`.
- A non-empty host needs a non-empty token, or both provision fields non-empty. Otherwise `bad_field` is `"token"`.

- [ ] **Step 1: Write the failing tests in `test_tb_link.c`**
  - `tb_default_device_name` for MAC `{0x24,0x6f,0x28,0xa1,0xb2,0xc3}` gives `"fishpump-a1b2c3"`.
  - Defaults: `host[0]==0`, `port==1883`, `!tls`, and `tb_link_config_valid` is true (link off is valid).
  - Host validation: `"192.168.1.51"` with a token is valid. `"bad host"` is invalid with `bad_field` `"host"`. A 64-char host is invalid.
  - Host `"192.168.1.51"` with an empty token and an empty secret is invalid with `"token"`. The same with key `"k1"` and secret `"s1"` is valid.
  - `from_json`:
    - `{"port":0}` is invalid with `"port"`; `{"port":"1883"}` is invalid with `"port"` (type).
    - `{"token":""}` clears the token.
    - An absent `provision_secret` keeps the current one.
    - `{"tls":1}` is invalid with `"tls"` (must be bool).
  - `to_json`: `has_token` is true, and neither the string `"token"` nor `"provision_secret"` appears as a key in the printed JSON (search the printed text for `"token":` and `"provision_secret":`).
  - `build_request` with name `n`, key `k`, secret `s` gives JSON whose parse has `deviceName=="n"`, `provisionDeviceKey=="k"` and `provisionDeviceSecret=="s"`. A 10-byte buffer returns -1.
  - `parse` SUCCESS: `{"status":"SUCCESS","credentialsType":"ACCESS_TOKEN","credentialsValue":"abc123"}` gives `TB_PROV_OK` and token `"abc123"`.
  - `parse` FAILURE: `{"status":"FAILURE","errorMsg":"Provision request is not found!"}` gives `TB_PROV_FAILURE` and that exact err text.
  - `parse` with `"credentialsType":"MQTT_BASIC"` gives `TB_PROV_BAD_REPLY`; `"not json"` gives `TB_PROV_BAD_REPLY`.
  - `parse` with a 70-char credentialsValue gives `TB_PROV_BAD_REPLY`, because it does not fit `TB_TOKEN_MAX`.
  - Backoff: `0→5000→10000→20000→40000→60000→60000`.

- [ ] **Step 2: Run the selftest and confirm it fails to build**
  - Run: `powershell firmware/test/sensor_selftest/run_selftest.ps1` (inside the IDF env).
  - Expected: build failure, because `tb_link_logic.h` does not exist.

- [ ] **Step 3: Implement the header and `tb_link_logic.c`** to the interfaces and rules above. Use cJSON for parse and build, and `strlcpy` plus explicit length checks; never truncate silently.

- [ ] **Step 4: Run the selftest and confirm it passes**
  - Run: `powershell firmware/test/sensor_selftest/run_selftest.ps1`
  - Expected: `SELFTEST PASS (N checks)`, where N is larger than the previous run.

- [ ] **Step 5: Commit** with `git add firmware/components/tb_link_logic firmware/test/sensor_selftest` and message `feat: add tb_link_logic (config, provisioning, backoff) with selftests`.

### Task 2: NVS storage of the link config

**Files:**
- Modify: `firmware/components/nvs_store/include/nvs_store.h` and `nvs_store.c`
- Modify: `firmware/components/nvs_store/CMakeLists.txt` (REQUIRES `tb_link_logic`)

**Interfaces:**
- Consumes: `tb_link_config_t`, `tb_link_config_defaults`, `tb_link_config_valid` (Task 1).
- Produces:
  ```c
  /* Namespace "tb_cfg", key "cfg": blob {uint8_t version=1; tb_link_config_t cfg}.
   * load: defaults (with sta_mac) when missing, wrong version/length, or !tb_link_config_valid. Returns true if a stored blob was used. */
  bool nvs_store_load_tb_link(tb_link_config_t *cfg, const uint8_t sta_mac[6]);
  bool nvs_store_save_tb_link(const tb_link_config_t *cfg);   /* refuses invalid cfg */
  ```
- Add `"tb_cfg"` to the namespaces the factory reset erases (the list near `nvs_store.c:2047`).

- [ ] **Step 1: Add these checks to a new `firmware/scripts/verify_tb_link.py`**, in the style of `verify_pre_mqtt.py` (`assert_test`, exit code = failures):
  - `nvs_store.h` declares both functions.
  - `nvs_store.c` uses the namespace `"tb_cfg"`.
  - The factory-reset erase list contains `tb_cfg`.
  - `nvs_store_save_tb_link` calls `tb_link_config_valid`.
- [ ] **Step 2: Run** `python firmware/scripts/verify_tb_link.py`. Expected: FAIL on all 4.
- [ ] **Step 3: Implement** in the style of `nvs_store_save_dry_run_config` (versioned blob, commit, log on failure without printing the config).
- [ ] **Step 4: Run** `verify_tb_link.py` (expect 4 PASS) and `idf.py -C firmware build` (expect 0 warnings).
- [ ] **Step 5: Commit** `feat: store ThingsBoard link config in NVS`.

### Task 3: `time_sync` (SNTP → uplink wall clock)

**Files:**
- Create: `firmware/components/time_sync/{CMakeLists.txt, include/time_sync.h, time_sync.c}`
- Modify: `firmware/sdkconfig.defaults` and `firmware/sdkconfig` (`CONFIG_LWIP_SNTP_MAX_SERVERS=2`)
- Modify: `firmware/main/app_main.c` (init after `wifi_manager_init()`, raise the `time_sync` tag to INFO)
- Modify: `firmware/main/CMakeLists.txt` (REQUIRES `time_sync`)

**Interfaces:**
- Produces:
  ```c
  #define TIME_SYNC_MIN_PLAUSIBLE_UNIX_MS 1767225600000LL   /* 2026-01-01T00:00:00Z */
  esp_err_t time_sync_init(void);        /* esp_netif_sntp_init with both servers, start immediately; non-blocking */
  int64_t time_sync_now_unix_ms(void);   /* 0 until the first sync callback; then gettimeofday in ms */
  bool time_sync_is_set(void);
  ```
- The sync callback: on the first sync whose time is ≥ `TIME_SYNC_MIN_PLAUSIBLE_UNIX_MS`, set the flag, call `uplink_set_wall_clock(time_sync_now_unix_ms)`, and log once at INFO `time set: <ISO UTC>`. Later syncs log nothing.

- [ ] **Step 1: Add `verify_tb_link.py` checks:**
  - `sdkconfig.defaults` contains `CONFIG_LWIP_SNTP_MAX_SERVERS=2`.
  - `time_sync.c` names both `pool.ntp.org` and `time.google.com`.
  - `time_sync.c` calls `uplink_set_wall_clock(`.
  - `app_main.c` calls `time_sync_init()` after `wifi_manager_init(` (string order in the source).
  - `app_main.c` raises `"time_sync"` to `ESP_LOG_INFO`.
- [ ] **Step 2: Run** the script. Expected: these checks FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the script (expect PASS) and the build (expect 0 warnings).
- [ ] **Step 5: Commit** `feat: SNTP wall clock for uplink events`.

### Task 4: `tb_link` component (MQTT client, provisioning, uplink sink) + wiring

**Files:**
- Create: `firmware/components/tb_link/{CMakeLists.txt, include/tb_link.h, tb_link.c}` (REQUIRES `mqtt tb_link_logic nvs_store uplink esp_wifi esp_event freertos log`)
- Modify: `firmware/main/app_main.c`:
  - `tb_link_init()` after `uplink_init()` and `wifi_manager_init()`;
  - `esp_log_level_set("tb_link", ESP_LOG_INFO)`;
  - `tb_link_stack_hwm()` added to the `[BOARD_DIAG]` line as `tb_hwm=`.
- Modify: `firmware/main/CMakeLists.txt` (REQUIRES `tb_link`)

**Interfaces:**
- Consumes: Task 1 logic, Task 2 load/save, `uplink_set_sink`, and `UPLINK_TOPIC_TELEMETRY` / `UPLINK_TOPIC_ATTRIBUTES`.
- Produces:
  ```c
  typedef enum { TB_STATE_OFF = 0, TB_STATE_WAIT_WIFI, TB_STATE_PROVISIONING, TB_STATE_CONNECTING,
                 TB_STATE_CONNECTED, TB_STATE_BACKOFF } tb_state_t;
  typedef struct {
      tb_state_t state;
      char last_error[96];          /* "" = none; Thai-free technical text, e.g. "provision: Provision request is not found!" */
      uint32_t connected_sec;       /* 0 unless CONNECTED */
      uint32_t sent, failed;        /* sink send results since boot */
      uint32_t retry_in_sec;        /* in BACKOFF */
  } tb_link_status_t;

  esp_err_t tb_link_init(void);                                  /* load NVS, start task, register sink once */
  esp_err_t tb_link_apply_config(const tb_link_config_t *cfg);   /* validate+save, then post RESTART to the task; returns at once */
  esp_err_t tb_link_reprovision(void);                           /* clear token, save, post RESTART */
  void tb_link_get_config(tb_link_config_t *out);                /* copy under lock (caller must not expose secrets) */
  void tb_link_get_status(tb_link_status_t *out);
  const char *tb_link_state_name(tb_state_t s);                  /* "off","wait_wifi","provisioning","connecting","connected","backoff" */
  int32_t tb_link_stack_hwm(void);
  ```

**Behaviour decisions:**
- **One task `tb_link`** (stack 6144, priority 2, not in the task watchdog) owns the client. A FreeRTOS queue carries the commands `RESTART`, `EV_CONNECTED`, `EV_DISCONNECTED`, `EV_PROV_REPLY` and `EV_SUBSCRIBED`.
  - **The MQTT event handler only posts to this queue and sets the atomic `connected` flag.** It never creates or destroys the client and never calls `uplink_set_sink`.
- **Sink:** registered once in `tb_link_init` with a static ctx.
  - `is_connected` returns the atomic flag, which is true only with a token session CONNECTED. It is false during provisioning.
  - `send` calls `esp_mqtt_client_enqueue(client, topic, json, len, 1, 0, true)`. msg_id ≥ 0 → `ESP_OK` and `sent++`; otherwise `ESP_FAIL` and `failed++`.
  - **Ruling (QoS 1 for all):** the uplink sends alarm events and periodic telemetry with the same kind, so they cannot be split. Cap the outbox at 16 KB (esp-mqtt `outbox.limit`, or the IDF 6.0.1 equivalent).
- **Connect:**
  - `disable_auto_reconnect = true`; the task reconnects on the backoff from `tb_backoff_next_ms`.
  - `client_id` = device name. Username = token, or `provision` when provisioning. The URI is `mqtt://host:port`, or `mqtts://` with `crt_bundle_attach` when `tls`.
  - The network timeout is 10 s, and a connect that is not accepted within 15 s counts as a failure.
- **No STA IP** (`wifi_manager_is_sta_connected()` false): state `WAIT_WIFI`. Re-check every 2 s with no log spam; log only on state change.
- **Provisioning:**
  1. Start only when the token is empty and both provision fields are set.
  2. On connect, subscribe to `/provision/response`. On SUBSCRIBED, publish the request.
  3. Wait up to 15 s for the reply.
  4. OK → save the token through `nvs_store_save_tb_link` and reconnect at once with the token.
  5. FAILURE or BAD_REPLY → `last_error`, then BACKOFF at the cap (60 s).
- **State changes** log one INFO line each. The token and secret never appear in logs.

- [ ] **Step 1: Add `verify_tb_link.py` checks:**
  - `uplink_set_sink(` appears exactly once in `tb_link.c`, inside `tb_link_init`.
  - `esp_mqtt_client_init`, `esp_mqtt_client_destroy` and `esp_mqtt_client_start` appear only inside the task function (use `function_body`).
  - The event-handler function body contains no `uplink_set_sink`, `esp_mqtt_client_destroy` or `nvs_store_save`.
  - The sink send function uses `esp_mqtt_client_enqueue` and not `esp_mqtt_client_publish`.
  - No `ESP_LOG` call in `tb_link.c` takes `.token` or `.provision_secret` as an argument.
  - `app_main.c` raises `"tb_link"` to INFO and calls `tb_link_init()` after `uplink_init()`.
  - The `BOARD_DIAG` format contains `tb_hwm=`.
- [ ] **Step 2: Run** the script. Expected: the new checks FAIL.
- [ ] **Step 3: Implement** `tb_link.c` and the `app_main.c` wiring.
- [ ] **Step 4: Run** `verify_tb_link.py` (all PASS), the five existing verify scripts (all PASS), and the build (0 warnings).
- [ ] **Step 5: Commit** `feat: tb_link MQTT transport with provisioning and uplink sink`.

### Task 5: ThingsBoard setup script, rule chain, and rule-chain test

**Files:**
- Create: `thingsboard/README.md` (Thai, short: what each script does and how to run it)
- Create: `thingsboard/tb_api.py` (stdlib `urllib` REST helper: login with username + `getpass`/env `TB_PASSWORD`, JWT header, `get`/`post`, find by name)
- Create: `thingsboard/tb_setup.py`
- Create: `thingsboard/rule_chains/fishpump.json`
- Create: `thingsboard/tb_event_test.py`
- Create: `thingsboard/mqtt_mini.py` (stdlib-socket MQTT 3.1.1: CONNECT with username, PUBLISH QoS 0, DISCONNECT; enough for tests)
- Modify: `.gitignore` (add `thingsboard/.local/`)

**Interfaces:**
- Produces:
  - `python thingsboard/tb_setup.py --url http://192.168.1.51:8080 --user <tenant admin email>`. Idempotent: it finds by name, then updates or creates. It ensures:
    - **Rule chain `fishpump`:** imported from `rule_chains/fishpump.json`, updated in place if it exists.
    - **Device profile `fishpump`:**
      - transport DEFAULT (MQTT default topics);
      - provision type `ALLOW_CREATE_NEW_DEVICES`, with a random 20-char key/secret generated on first run and reused afterwards;
      - default rule chain `fishpump`.
    - **Customer `ลูกค้าทดสอบ`.**
    - **Generated secrets** written to `thingsboard/.local/provision.json` (`{"provision_key","provision_secret"}`), with the key and secret printed for typing into the board card.
  - `--assign <deviceName>` assigns that device to the customer (run after provisioning).
  - `python thingsboard/tb_event_test.py --url ... --user ...` creates or reuses a test device `fishpump-ruletest` with the `fishpump` profile, reads its token, publishes events over MQTT, and checks the alarms through REST. It prints a PASS/FAIL table and exits with the failure count.

**Rule-chain behaviour (exact):**
- POST_TELEMETRY messages are saved as today (save timeseries).
- Messages whose data has `alarm_event` (plain, or inside `values` for the `{"ts","values"}` form) go through dedupe, then create or clear:
  - **Dedupe:** compare `(boot_id, seq)` with the device's server attribute `last_alarm_event` (`{"boot_id","seq"}`). Drop when `boot_id` is equal and `seq` is ≤ the stored value. Otherwise process, then save the new pair.
  - `type=="raised"` → **Create Alarm**:
    - alarm type `rule_<rule_id>`;
    - severity `critical`→CRITICAL, `warning`→WARNING, `info`→INDETERMINATE;
    - alarm details = the event object.
  - `type=="cleared"` → **Clear Alarm** of type `rule_<rule_id>`.
- Script nodes use TBEL.

**`tb_event_test.py` cases** (each a row in the table):
1. A raised event with `rule_id` 901 and severity `warning` gives an ACTIVE alarm `rule_901` of severity WARNING.
2. The same `(boot_id, seq)` re-sent gives no second alarm (count of `rule_901` alarms = 1).
3. Cleared with seq+1 gives the alarm CLEARED.
4. The old raised event re-sent with the original seq gives the alarm still CLEARED (not re-opened).
5. A new `boot_id` with seq 1 raised is processed: a new ACTIVE alarm.
6. The `{"ts":<ms>,"values":{"alarm_event":...}}` form is handled the same as the plain form.
7. Plain telemetry `{"water_ph":7.1}` is saved and the latest value is readable through REST.

The test deletes `fishpump-ruletest` at the end, unless `--keep` is given.

- [ ] **Step 1: Write `tb_event_test.py` and `mqtt_mini.py` first**, then run against copter before the rule chain exists. The user runs it and types the password in a terminal tab.
  - Expected: cases 1–6 FAIL (no alarms are created by the default chain), case 7 PASS.
- [ ] **Step 2: Write `tb_api.py`, `tb_setup.py` and `rule_chains/fishpump.json`.** Build the rule chain in the ThingsBoard UI if that is easier, then export it into the repo file. The repo file is the source of truth.
- [ ] **Step 3: Run `tb_setup.py` twice.**
  - Expected: the first run creates the rule chain, profile and customer and prints the key/secret. The second run reports "exists/updated" for each and creates no duplicates; verify in the UI that there is only one `fishpump` profile.
- [ ] **Step 4: Run `tb_event_test.py`.** Expected: 7/7 PASS.
- [ ] **Step 5: Commit** `thingsboard/` and `.gitignore`. Check `git status` first and make sure `thingsboard/.local/` is not staged. Message: `feat: ThingsBoard setup script, fishpump rule chain and rule-chain test`.

### Task 6: "การเชื่อมต่อ ThingsBoard" card and `/api/tb` endpoints

**Files:**
- Modify: `firmware/main/web_server.c`:
  - handlers and routes for `GET /api/tb`, `POST /api/tb` and `POST /api/tb/reprovision`;
  - raise `max_uri_handlers` if the route count needs it.
- Modify: `firmware/main/static/index.html`: a new `<div class="section-card" id="tb-link-card">` on the Status view, directly above `#uplink-preview-card`.
- Modify: `firmware/main/static/app.js`
- Modify: `firmware/main/CMakeLists.txt` (REQUIRES `tb_link tb_link_logic`)

**Interfaces:**
- Consumes: `tb_link_get_config`, `tb_link_get_status`, `tb_link_state_name`, `tb_link_apply_config`, `tb_link_reprovision` (Task 4), and `tb_link_config_from_json` / `tb_link_config_to_json` (Task 1).
- Produces the endpoints:
  - **`GET /api/tb`** (login required): `{"ok":true,"config":<tb_link_config_to_json>,"status":{"state","last_error","connected_sec","sent","failed","retry_in_sec"}}`
  - **`POST /api/tb`**: uses `api_begin_json_post` (auth + origin + JSON), so a body of any of the config keys works.
    - Invalid → `400 {"ok":false,"error":"invalid_field","message":"<bad_field>"}`.
    - OK → `{"ok":true}` plus the same object as the GET.
  - **`POST /api/tb/reprovision`**: `{"ok":true}`. It needs the provision key and secret set; otherwise `409 {"ok":false,"error":"no_provision_credentials"}`.

**Card copy and behaviour:**
- **Status line,** from `state`:
  - off → "ปิดอยู่ (ยังไม่ได้ตั้งเซิร์ฟเวอร์)"
  - wait_wifi → "รอ Wi-Fi"
  - provisioning → "กำลังลงทะเบียนอุปกรณ์"
  - connecting → "กำลังเชื่อมต่อ"
  - connected → "เชื่อมต่อแล้ว · <connected_sec> วิ"
  - backoff → "เชื่อมต่อไม่ได้ · ลองใหม่ใน <retry_in_sec> วิ"
  - Plus `last_error` in muted text when it is not empty, and `ส่งแล้ว <sent> · ล้มเหลว <failed>`.
- **Fields:**
  - เซิร์ฟเวอร์ (host), พอร์ต, ใช้ TLS (checkbox), ชื่ออุปกรณ์, Provision key.
  - Provision secret and Access token are password inputs. Their placeholder is "ตั้งไว้แล้ว — เว้นว่างเพื่อคงค่าเดิม" when `has_*` is true. Empty inputs are **not** sent.
  - A "ล้าง token" checkbox sends `"token":""`.
- **Buttons:**
  - บันทึก.
  - ลงทะเบียนใหม่, with the confirm text: "ThingsBoard ลงทะเบียนชื่ออุปกรณ์ได้ครั้งเดียว ถ้าอุปกรณ์ชื่อนี้มีอยู่แล้วใน ThingsBoard ต้องลบออกก่อน ไม่อย่างนั้นการลงทะเบียนจะล้มเหลว ดำเนินการต่อ?"
- **Polling:** refresh the status every 5 s while the Status view is visible **and** `document.hidden` is false, the same pattern as the other Status-page polling. Stop otherwise.

- [ ] **Step 1: Add `verify_tb_link.py` checks:**
  - Routes `/api/tb` (GET and POST) and `/api/tb/reprovision` are registered.
  - The `POST /api/tb` handler body calls `api_begin_json_post`.
  - The GET handler body does not reference `.token` or `.provision_secret`.
  - `index.html` has `id="tb-link-card"` before `id="uplink-preview-card"`.
  - `app.js` contains the exact reprovision confirm text above.
  - `app.js` checks `document.hidden` in the tb poll function.
- [ ] **Step 2: Run** the script. Expected: the new checks FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all six verify scripts (PASS), the build (0 warnings), and the QEMU selftest (PASS).
- [ ] **Step 5: Commit** `feat: ThingsBoard link card and /api/tb endpoints`.

### Task 7: Bench milestone 1 on the board + bench file

**Files:**
- Create: `docs/bench-test-thingsboard.md` (Thai, checklist style like `bench-test-dry-run.md`; section "รอบ 1: การเชื่อมต่อ"). Record each result in it.

**Bench steps:**
- The user flashes the board (`idf.py -C firmware -p COM12 flash`, no erase) and logs in to the web app and ThingsBoard. Claude drives the built-in browser and scripts.
- The board must be on the home Wi-Fi (STA), so that both the PC and the board reach `192.168.1.51`.

| # | Step | Expected |
|---|---|---|
| B1 | Run `tb_setup.py`, then type host `192.168.1.51`, port 1883, and the printed key/secret into the card. Save. | Card goes provisioning → connected within 30 s. A device `fishpump-xxxxxx` appears in ThingsBoard with profile `fishpump`. Run `tb_setup.py --assign fishpump-xxxxxx`. |
| B2 | ThingsBoard device → Attributes and Latest telemetry. | Client attributes `fw_type=fishpump`, `contract_version=1`, `signals` (whole array), `alarm_rules`. Telemetry keys update every 30 s. |
| B3 | Press ลงทะเบียนใหม่ without deleting the device in ThingsBoard. | Card shows backoff with `last_error` containing the ThingsBoard errorMsg, and retries about every 60 s. Then type the token from ThingsBoard (device → Copy access token) into the card → connected. |
| B4 | Change the host to `192.168.1.250` (nothing there). Save. | Backoff 5→10→20→40→60 s visible on the card. The web app stays responsive (`/api/status` < 300 ms). Restore the host → connected. |
| B5 | Press บันทึก twice within 1 s while connected. | Ends connected. Serial shows no crash or `E` lines from `tb_link`. `tasks` count unchanged in `BOARD_DIAG`. |
| B6 | Enable one rule (for example a pH or flow rule that can be triggered with the jumper) and trigger it, then clear it. | A ThingsBoard alarm `rule_<id>` opens and then clears. The event `ts` is the real time (SNTP), not the arrival time. |
| B7 | Set the host to `192.168.1.250`, trigger and clear the rule while disconnected, wait 60 s, then restore the host. | After reconnect both events arrive, in order, with their original times. The alarm opens and closes once. |
| B8 | Serial `BOARD_DIAG` over 15 min with the link connected. | `heap_free` ≥ 100 KB, `tb_hwm` ≥ 512, no reboot. |

- [ ] **Step 1: Write `docs/bench-test-thingsboard.md`** with B1–B8 as unchecked items.
- [ ] **Step 2: Run the bench with the user.** Record the results in the file. Any failure → `superpowers:systematic-debugging`, then a fix commit with a test (selftest or verify check) that failed first.
- [ ] **Step 3: Restore what the bench changed.** Turn rules back off, keep the link configured, and leave the host at `192.168.1.51`.
- [ ] **Step 4: Commit** `docs: bench milestone 1 of ThingsBoard link`. Update `docs/backlog/after-frontend.md` (c): tick SNTP and the `set_sink` contract, and note the 120 s hold review result from B6/B7.

## Rulings made in this plan

- **The plan covers milestone 1 only.** Milestones 2–6 get their own plans after this bench, because each needs the previous bench's facts (the OTA spike in particular).
- **QoS 1 for every uplink message.** The uplink kind cannot tell events from periodic telemetry. The outbox is capped at 16 KB.
- **TLS in M1 uses only the IDF certificate bundle.** The custom CA PEM field from spec 6.2 waits until there is a TLS server to test against. Cost if wrong: one more card field later.
- **The link card lives on the Status page**, above the uplink preview, not as a new page.
- **The ThingsBoard scripts use only the Python standard library.** This needs no pip install on this PC, at the cost of a minimal hand-written MQTT client for the tests.
