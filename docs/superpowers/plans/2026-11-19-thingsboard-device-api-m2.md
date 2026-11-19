# ThingsBoard Link — Milestone 2 (`device_api` + RPC, one API group at a time) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every operational web API function can be called as a ThingsBoard RPC and gives the same body and error code as the web API, because both paths run one implementation. A plain test dashboard in ThingsBoard reaches every method and shows each reply.

**Architecture:**
- A new component `device_api` holds a table of named methods: JSON params in, a response object plus a status out. One mutex serializes every call.
- The method bodies move out of `web_server.c` into `firmware/main/dapi_<group>.c`, one API group at a time. The HTTP handlers keep auth, same-origin, the default-password check and the body read, then call `device_api_call(..., DAPI_SRC_HTTP, ...)`.
- `tb_link` subscribes to ThingsBoard RPC. Its MQTT event handler only queues requests. A new `tb_rpc` task calls `device_api_call(..., DAPI_SRC_TB, ...)` and publishes the reply.
- Guard rails:
  - A golden-response harness records the board's current HTTP replies before any migration. Each migrated firmware must reproduce them.
  - A parity script calls each method through ThingsBoard and through HTTP and compares the two.
- The test dashboard is generated from a page list by a script, using one data-driven custom RPC widget.

**Tech Stack:** ESP-IDF v6.0.1, managed `espressif/mqtt ^1.1.0` and `espressif/cjson`, FreeRTOS, ThingsBoard CE 4.3.1 REST/MQTT. The PC scripts use the Python 3 standard library only.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`, sections 4, 5, 6.1, 6.2 (RPC worker), 7.2, 7.3, 8 and 8a item 2. Milestone 1 is done (bench B1–B8 pass, `docs/bench-test-thingsboard.md`). Out of scope here:
- the settings and state attributes and the status telemetry (M3);
- remote Wi-Fi connect through staging (M4);
- OTA (M5).

## Global Constraints

- **Toolchain and gates:**
  - Build in PowerShell: `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM`, then `idf.py -C firmware build`. The build must have **0 warnings** in project code.
  - These gates stay green after every task:
    - `python firmware/scripts/verify_alarm_system.py`
    - `verify_dry_run.py`
    - `verify_sensor_integration.py`
    - `verify_pre_mqtt.py`
    - `verify_pt100_inverter_system.py`
    - `verify_tb_link.py`
    - the QEMU selftest `firmware/test/sensor_selftest/run_selftest.ps1` → `SELFTEST PASS (<n> checks)`. The baseline is 1253; it only grows.
- **The web app does not change.**
  - No file under `firmware/main/static/` changes in this milestone.
  - Every migrated HTTP endpoint keeps the same status line and the same body bytes.
  - Each handler keeps exactly the pre-checks it has today (unauthorized, `force_password_change`, cross-origin, body), in the same order and with the same bodies.
- **The HTTP layer keeps** `require_auth`, the session, the `force_password_change` check, `is_same_origin`, the body read and each handler's existing unauthorized/forbidden/`invalid_body`/`invalid_json` replies.
  - `/api/tb*`, login/logout/nonce/credentials/change-password, `/api/confirm`, `/api/ota` and `/api/events` (SSE) are **not** migrated.
- **`force_password_change` does not apply to `DAPI_SRC_TB`** (spec 6.1, call 2).
- **Every check a handler does after reading the body moves into the method.** This covers value ranges, rule validation, the dry-run lock (409), the pump-start block and the Wi-Fi guards. The remote path must never skip one (spec anti-goal).
- **Status → HTTP line**, exactly:

  | `dapi_status_t` | HTTP status line |
  |---|---|
  | `DAPI_OK` | `200 OK` |
  | `DAPI_ACCEPTED` | `202 Accepted` |
  | `DAPI_BAD_REQUEST` | `400 Bad Request` |
  | `DAPI_LOCAL_ONLY` | `403 Forbidden` |
  | `DAPI_NOT_FOUND` | `404 Not Found` |
  | `DAPI_CONFLICT` | `409 Conflict` |
  | `DAPI_UNAVAILABLE` | `503 Service Unavailable` |
  | `DAPI_INTERNAL` | `500 Internal Server Error` |

- **Fixed reply bodies** that `device_api` and `tb_rpc` build themselves:
  - unknown method: `{"ok":false,"error":"unknown_method"}` (`DAPI_NOT_FOUND`)
  - local-only from ThingsBoard: `{"ok":false,"error":"local_only","message":"Use the local web app"}` (`DAPI_LOCAL_ONLY`)
  - lock not taken within 5000 ms: `{"ok":false,"error":"busy","message":"Device busy, try again"}` (`DAPI_UNAVAILABLE`)
  - params from ThingsBoard not a JSON object: `{"ok":false,"error":"invalid_json","message":"Expected a JSON object"}` (`DAPI_BAD_REQUEST`)
  - params from ThingsBoard longer than the method's HTTP body buffer: `{"ok":false,"error":"invalid_body","message":"Request body is missing or too large"}` (`DAPI_BAD_REQUEST`)
  - a method returned no object: `{"ok":false,"error":"memory","message":"JSON allocation failed"}` (`DAPI_INTERNAL`)
  - RPC request over the size cap: `{"ok":false,"error":"too_large"}`
  - RPC queue full: `{"ok":false,"error":"busy"}` (spec 6.2)
- **Logging:**
  - `device_api` logs each WRITE at INFO as `write <method> src=<http|tb>`.
  - `tb_rpc` logs `rpc <method> id=<id> -> <status>`.
  - **Params are never logged** (Wi-Fi passwords travel in them).
  - The new tag `device_api` is raised to INFO in `app_main.c`. The `tb_rpc` code logs under the existing `tb_link` tag.
- **Stack:** a method may run on the `tb_rpc` task (6144 B). In method code, any buffer over 256 B lives on the heap (PSRAM via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` where the data is not used by DMA). Do not put it on the stack.
- **Budgets on the bench:**
  - internal heap free ≥ 100 KB during an RPC burst;
  - every task's HWM ≥ 512 B, including the new `tb_rpc_hwm` in `BOARD_DIAG`.
- **ThingsBoard side:**
  - The tenant password and the board's web password come only from env (`TB_PASSWORD`, `BOARD_PASSWORD`) or a `getpass` prompt the user types. Never a file, never Claude.
  - Golden files and anything holding board data (SSIDs, rules) go under the git-ignored `thingsboard/.local/`.
- **Git:**
  - Never stage `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`.
  - Commit each task on `feature/thingsboard-mqtt`. No push, no merge.
  - End messages with the attribution line given to you.
- **Do not change the board's state while golden or parity runs are pending.** That covers settings, rules and Wi-Fi profiles. Exact-mode golden cases depend on it.

## Review Focus

1. **A check that the remote path skips.** Examples: the dry-run lock 409, a value range, rule validation, the pump-start block. Expected: the same error code from ThingsBoard as from HTTP.
   - Pinned by each group's error-path parity cases.
   - Also pinned by the `verify_device_api.py` check that a migrated handler body holds no engine or driver calls.
2. **Two callers at once**: a dashboard RPC while the web app polls or saves. Expected: both get correct, complete replies and nothing deadlocks.
   - Pinned by `parity_test.py --burst`, which runs RPCs while a thread polls HTTP (Tasks 5, 6, 13).
3. **Wrong-shaped params from ThingsBoard**: an array, a string, a number, `null`, absent, or too long. Expected: the HTTP body for the same mistake, or `{}` when absent.
   - Pinned by the `device_api` selftests (Task 2) and the parity error cases.
4. **An RPC that arrives while the link restarts** (card save, Wi-Fi drop). Expected: no crash and no publish on a destroyed client. The reply is dropped and ThingsBoard times out.
   - Pinned by a `verify_tb_link.py` check (Task 3) and bench step A6.
5. **Large replies** (`alarms.signals` and `alarms.rules.get` are several KB). Expected: they arrive whole through MQTT.
   - Pinned by parity read cases that compare full bodies (Tasks 6 and 13).

---

## Group migration recipe (used by Tasks 4 and 7–12)

Each group task migrates the endpoints it lists. Work in this order:

1. **Method file `firmware/main/dapi_<group>.c`**, exporting `void dapi_<group>_register(void)`. It calls `device_api_register()` with the group's table. Each method:
   - is the handler's code after the body was parsed, moved and not rewritten;
   - returns the exact object the handler sends today;
   - for an endpoint built with `json_gen` or a literal string, may wrap the finished text with `cJSON_CreateRaw()` so the bytes match. Rule: cJSON prints numbers differently from `%g`.
   - Helpers that both files need move to `firmware/main/api_json.c` / `api_json.h`, non-static, with unchanged behaviour.
2. **HTTP handler** in `web_server.c`: keep its pre-checks verbatim, then `return http_dapi(req, "<method>", params);`. For a GET handler, `params` is `NULL`. For a POST handler, it is the parsed root, which the handler deletes after the call.
3. **Registration:** call `dapi_<group>_register()` from `web_server_start()` (or the init function that already registers URIs), before the server starts.
4. **`firmware/scripts/verify_device_api.py`** gains the group's checks:
   - each listed handler contains `http_dapi(req, "<method>"`;
   - each listed handler contains none of the group's engine or driver calls (the task lists the prefixes);
   - each method name appears in the group's table with the listed flags.
5. **`thingsboard/golden_cases.json`** already holds the group's cases (Task 1). Add any case that turns out to be missing.
6. **Dashboard page** entry in `thingsboard/dashboard_pages.json` (format in Task 5):
   - one action per method;
   - the web app's confirm text, copied from `firmware/main/static/app.js`, on the actions the spec lists (measure, pump start, ack of the dry-run alarm, energy reset, rule delete);
   - the area's telemetry keys.
7. **Parity cases** in `thingsboard/parity_cases.json` (format in Task 5): a read case per READ method, the listed error cases, and the listed reversible writes.
8. **Run** the build (0 warnings), every gate in Global Constraints, `python firmware/scripts/verify_device_api.py`, `python thingsboard/tb_dashboard.py --check` and `python thingsboard/parity_test.py --selftest`. Commit `refactor: <group> API through device_api`.

---

### Task 1: Golden-response harness and the pre-migration baseline

**Files:**
- Create: `thingsboard/board_http.py`, `thingsboard/api_golden.py`, `thingsboard/golden_cases.json`
- Modify: `thingsboard/README.md` (a short section on golden runs)

**Interfaces:**
- Produces:
  - `board_http.BoardSession(base_url, user, password, timeout=20)` with `.login()` and `.request(method, path, body=None) -> (status:int, text:str)`.
    - It sends `Origin: <base_url>` and keeps the session cookie.
    - It logs in the way `firmware/main/static/app.js` does: read `apiPost('/api/login', …)` and `handle_api_login`.
  - `board_http.add_board_args(ap)` adds `--board` (default `http://192.168.1.34`) and `--board-user` (default `admin`). `board_http.board_from_args(args)` reads env `BOARD_PASSWORD` or prompts with `getpass`.
  - `api_golden.py capture --label <name>` writes `thingsboard/.local/golden/<name>.json`. `api_golden.py compare --base <name> --label <name2>` captures again, compares and prints one line per case. The exit code is 0 only when every case passes.
  - `golden_cases.json`: a list of `{"name","method":"GET"|"POST","path","body":obj|null,"mode":"exact"|"shape","ignore":[keys]}`.
    - `exact` compares the status and the raw body text.
    - `shape` compares the status and, recursively, the key sets and JSON types. It ignores values, and ignores `ignore` keys entirely.

- [ ] **Step 1: Write the failing self-check.** `python thingsboard/api_golden.py --selftest` runs the comparator on built-in pairs and prints `SELFTEST PASS`. The pairs:
  - an exact match;
  - a body that differs in one byte → FAIL;
  - shape mode with a changed number → PASS;
  - shape mode with a missing key → FAIL;
  - shape mode with a changed type (number vs string) → FAIL;
  - an `ignore` key that is missing → PASS;
  - a status that differs → FAIL.
- [ ] **Step 2: Run it.** Expected: it fails, because the module does not exist.
- [ ] **Step 3: Implement** `board_http.py` and `api_golden.py`, standard library only (`urllib.request`, `http.cookiejar`, `getpass`, `json`).
- [ ] **Step 4: Write `golden_cases.json`** by reading each handler in `web_server.c`:
  - one case for every GET endpoint in Tasks 4 and 7–12. Use `shape` for status, sensor and value endpoints, and `exact` for config, rules, signals, profiles and hardware map;
  - for every POST endpoint in those tasks, one case per error code that can be reached **without changing state**: missing field, wrong type, out of range, `not_found` with an id like 65000, an unknown mode string;
  - **idempotent writes** that post back the value just read (protection, pump config, cooling config, cooling mode, water offset, a rule replace, Wi-Fi setauto to the current index). Mark them `"needs":"get:<case>"` so the body is filled from that GET at run time. They run only with `--with-writes`.
  - Never start the pump, a measurement, a calibration, an energy reset, a Wi-Fi connect/disconnect/forget or a hardware map save.
  - `/api/wifi/scan` is `shape`, and it runs last.
- [ ] **Step 5: Run the self-check.** Expected: `SELFTEST PASS`. Commit `test: golden-response harness for the web API`.
- [ ] **Step 6 (controller with the user): capture the baseline** from the board, which still runs `45a5fe8`. Run `python thingsboard/api_golden.py capture --label base --with-writes` in a terminal tab; the user types the board password. Then run `compare --base base --label base2 --with-writes` once. Expected: every case passes. Any `exact` case that differs between two runs on unchanged firmware moves to `shape` with the varying keys in `ignore`; amend and commit.

### Task 2: `device_api` core component + selftests

**Files:**
- Create: `firmware/components/device_api/CMakeLists.txt`, `include/device_api.h`, `device_api.c`
- Create: `firmware/test/sensor_selftest/main/test_device_api.c`; register it in `selftest_main.c` and the selftest CMake like `test_tb_link.c`
- Modify: `firmware/main/app_main.c` (call `device_api_init()` before the web server starts; `esp_log_level_set("device_api", ESP_LOG_INFO)`)

**Interfaces:**
- Produces (in `device_api.h`):
  ```c
  typedef enum { DAPI_OK = 0, DAPI_ACCEPTED, DAPI_BAD_REQUEST, DAPI_LOCAL_ONLY,
                 DAPI_NOT_FOUND, DAPI_CONFLICT, DAPI_UNAVAILABLE, DAPI_INTERNAL } dapi_status_t;
  typedef enum { DAPI_SRC_HTTP = 0, DAPI_SRC_TB } dapi_source_t;
  #define DAPI_F_READ       0x01
  #define DAPI_F_WRITE      0x02
  #define DAPI_F_LOCAL_ONLY 0x04   /* refused when src == DAPI_SRC_TB */
  #define DAPI_F_STAGED     0x08   /* reserved for M4 */
  #define DAPI_MAX_METHODS  64
  #define DAPI_LOCK_TIMEOUT_MS 5000
  typedef dapi_status_t (*dapi_fn_t)(const cJSON *params, dapi_source_t src, cJSON **out);
  typedef struct { const char *name; uint8_t flags; uint16_t max_params_bytes; dapi_fn_t fn; } dapi_method_t;
  esp_err_t device_api_init(void);                                   /* creates the mutex; second call ESP_OK */
  esp_err_t device_api_register(const dapi_method_t *methods, size_t count); /* duplicate name or table full: ESP_ERR_INVALID_STATE, nothing added */
  const dapi_method_t *device_api_find(const char *name);
  dapi_status_t device_api_call(const char *method, const cJSON *params, dapi_source_t src, cJSON **out);
  const char *device_api_http_status(dapi_status_t st);
  cJSON *dapi_error(const char *code, const char *message);          /* {"ok":false,"error":code[,"message":message]}; message NULL → no key */
  ```
- What `device_api_call` does, in order:
  1. Unknown name → unknown-method body.
  2. `LOCAL_ONLY` with `DAPI_SRC_TB` → local-only body.
  3. With `DAPI_SRC_TB`:
     - `params` that is not NULL and not an object → `invalid_json` body;
     - `params` whose `cJSON_PrintUnformatted` length is `>= max_params_bytes` (when that value is not 0) → `invalid_body` body.
  4. NULL `params` → the method gets an empty object.
  5. Take the mutex (5000 ms) or return the busy body.
  6. Log the WRITE line, call `fn`, give the mutex back.
  7. NULL `*out` → memory body.
  - `*out` is always set; the caller deletes it.

- [ ] **Step 1: Write `test_device_api.c`**, using two fake methods. `fake_read` returns `{"ok":true,"n":1}`. `fake_local`, flags `DAPI_F_WRITE|DAPI_F_LOCAL_ONLY` with `max_params_bytes = 32`, echoes params. Assertions:
  - each status line in the Global Constraints table;
  - `device_api_find("nope") == NULL`;
  - registering `fake_read` twice → `ESP_ERR_INVALID_STATE` and still one entry;
  - `device_api_call("nope", …)` → `DAPI_NOT_FOUND` and the printed body equals `{"ok":false,"error":"unknown_method"}`;
  - `fake_local` from TB → `DAPI_LOCAL_ONLY` plus the local-only body; from HTTP → `DAPI_OK` plus the echo;
  - TB params `[]`, `"x"`, `3` and JSON `null` → `invalid_json` body; HTTP with NULL params → the method sees `{}`;
  - TB params with a 40-byte string → `invalid_body` body;
  - a method that sets `*out = NULL` → the memory body and `DAPI_INTERNAL`;
  - `dapi_error("a", NULL)` prints `{"ok":false,"error":"a"}`; `dapi_error("a","b")` prints `{"ok":false,"error":"a","message":"b"}`.
- [ ] **Step 2: Run the selftest.** Expected: the build fails because `device_api.h` does not exist.
- [ ] **Step 3: Implement the component.** It REQUIRES `json` (the managed cJSON name the build accepts) and `freertos`. It has no dependency on `main`.
- [ ] **Step 4: Run the selftest and the firmware build.** Expected: `SELFTEST PASS` with the count raised, and 0 warnings.
- [ ] **Step 5: Create `firmware/scripts/verify_device_api.py`.** Static checks, same style as `verify_tb_link.py`:
  - the component exists;
  - the WRITE log format string is `write %s src=%s` and no `ESP_LOG*` in `device_api.c` takes `params`;
  - `app_main.c` raises `device_api` to INFO and calls `device_api_init()` before the web server starts.

  Commit `feat: device_api method table`.

### Task 3: ThingsBoard RPC transport in `tb_link`

**Files:**
- Modify: `firmware/components/tb_link_logic/` (header and source), plus `test_tb_link.c`
- Modify: `firmware/components/tb_link/tb_link.c`, `tb_link.h`, `CMakeLists.txt` (REQUIRES `device_api`)
- Modify: `firmware/main/app_main.c` (`tb_rpc_hwm=` in `BOARD_DIAG`, next to `tb_hwm=`)
- Modify: `firmware/scripts/verify_tb_link.py`

**Interfaces:**
- Consumes: `device_api_call`, `device_api_http_status` (Task 2).
- Produces:
  - in `tb_link_logic.h`:
    ```c
    #define TB_RPC_REQ_PREFIX  "v1/devices/me/rpc/request/"
    #define TB_RPC_RESP_PREFIX "v1/devices/me/rpc/response/"
    #define TB_RPC_MAX_REQUEST 1792   /* > largest HTTP body (1536) + envelope; < mqtt buffer 2048 */
    #define TB_RPC_METHOD_MAX  48
    bool tb_rpc_parse_topic(const char *topic, size_t topic_len, uint32_t *req_id);
    typedef enum { TB_RPC_PARSE_OK = 0, TB_RPC_PARSE_BAD_JSON, TB_RPC_PARSE_NO_METHOD } tb_rpc_parse_t;
    tb_rpc_parse_t tb_rpc_parse_request(const char *payload, size_t len, char *method, size_t method_size, cJSON **params); /* *params NULL when absent; caller deletes */
    int tb_rpc_response_topic(uint32_t req_id, char *out, size_t out_size);                                            /* length, or -1 */
    ```
  - in `tb_link.h`: `long tb_link_rpc_stack_hwm(void);`, which returns -1 before the task exists.
- How it works:
  - **Subscribe:** on an accepted connect in the token session, subscribe to `v1/devices/me/rpc/request/+` with QoS 1.
  - **Event handler** (`MQTT_EVENT_DATA` with the RPC prefix):
    - A fragmented message (`total_data_len > data_len`) or one longer than `TB_RPC_MAX_REQUEST` → reply `too_large` once, on the fragment with `current_data_offset == 0`.
    - Otherwise copy the payload into PSRAM and push `{req_id, gen, payload, len}` onto a depth-4 queue without waiting.
    - Queue full or no memory → reply `busy`.
    - The handler never calls `device_api_call`.
  - **`tb_rpc` task** (6144 B, priority 2, created in `tb_link_init`):
    - pops a request and parses it;
    - `BAD_JSON` → the `invalid_json` body; `NO_METHOD` → the unknown-method body;
    - otherwise calls `device_api_call(method, params, DAPI_SRC_TB, &out)` and prints `out`;
    - publishes the reply with QoS 1 on the response topic through the same client-lifetime guard and lock the uplink sink's `send` uses;
    - drops the reply when the link is not connected, or when `gen` differs from the current generation;
    - frees everything and logs the `rpc … -> <status>` line;
    - on a client destroy (RESTART or provisioning → token), drains the queue and frees every item.

- [ ] **Step 1: Add the failing selftests** to `test_tb_link.c`:
  - `tb_rpc_parse_topic("v1/devices/me/rpc/request/17", …)` → true with id 17;
  - the request prefix with no id, an id with letters, an id over 4294967295, the wrong prefix, and `topic_len` shorter than the string → false;
  - `{"method":"alarms.status"}` → OK, the method copied, `*params == NULL`;
  - `{"method":"x","params":{"a":1}}` → an object;
  - `{"method":"x","params":[1]}` → OK with an array (`device_api` rejects it later);
  - `{"params":{}}`, `{"method":3}`, and a method of 48 characters (equal to `TB_RPC_METHOD_MAX`, no room for the NUL) → `NO_METHOD`;
  - `not json` and `[]` → `BAD_JSON`;
  - `tb_rpc_response_topic(17, buf, 64)` → `v1/devices/me/rpc/response/17`; a buffer that is too small → -1.
- [ ] **Step 2: Run the selftest.** Expected: compile failure.
- [ ] **Step 3: Implement the logic functions.** Run the selftest. Expected: `SELFTEST PASS`.
- [ ] **Step 4: Implement the transport** in `tb_link.c` as described above. Add `tb_rpc_hwm=%ld` to `BOARD_DIAG`.
- [ ] **Step 5: Extend `verify_tb_link.py`:**
  - the RPC subscribe is in the token-connect path;
  - the event handler's RPC branch has no `device_api_call`, and has an `xQueueSend` with zero wait;
  - `tb_rpc` publishes through the guarded path, not a raw stored client pointer read without the lock;
  - the queue is drained on client destroy;
  - `BOARD_DIAG` prints `tb_rpc_hwm=`.
- [ ] **Step 6: Run** the build (0 warnings), every gate and both verify scripts. Commit `feat: ThingsBoard RPC worker in tb_link`.

### Task 4: Alarms group

**Files:** `firmware/main/dapi_alarms.c` (create), `web_server.c`, `api_json.c/.h` (create if a helper must be shared), `verify_device_api.py`, `thingsboard/golden_cases.json`. The dashboard page and parity cases for this group come in Task 5.

**Interfaces:**
- Produces: `static esp_err_t http_dapi(httpd_req_t *req, const char *method, const cJSON *params)` in `web_server.c`. It calls `device_api_call(..., DAPI_SRC_HTTP, &out)`, prints `out` unformatted and sends it with `send_json(req, text, device_api_http_status(st))`. Every later group uses it.
- Methods. Every HTTP POST here uses `ALARM_POST_BODY_BYTES` as `max_params_bytes`; GETs use 0.

  | Method | Endpoint | Flags |
  |---|---|---|
  | `alarms.signals` | GET /api/alarms/signals | READ |
  | `alarms.values` | GET /api/alarms/values | READ |
  | `alarms.status` | GET /api/alarms/status | READ |
  | `alarms.summary` | GET /api/alarms/summary | READ |
  | `alarms.rules.get` | GET /api/alarms/rules | READ |
  | `alarms.rules.set` | POST /api/alarms/rules | WRITE |
  | `alarms.rules.delete` | POST /api/alarms/rules/delete | WRITE |
  | `alarms.ack` | POST /api/alarms/ack | WRITE |

- Engine-call prefixes the handlers must no longer hold: `alarm_engine_`, `alarm_rule_`, `signal_registry_`, `telemetry_hub_get_latest`.
- `alarms_read_id` and `alarms_send_json_error` become method-side helpers that return objects, not `httpd` sends.

- [ ] **Steps:** follow the recipe, steps 1–5 and 8. In the verify step, also check that `http_dapi` maps the status through `device_api_http_status`. Commit `refactor: alarms API through device_api`.

### Task 5: Test dashboard framework, the Alarms page and the parity harness

**Files:**
- Create: `thingsboard/widgets/fishpump_rpc_panel.json`, `thingsboard/tb_dashboard.py`, `thingsboard/dashboard_pages.json`, `thingsboard/parity_test.py`, `thingsboard/parity_cases.json`, `thingsboard/tb_dump.py`
- Modify: `thingsboard/tb_setup.py` (import the widget bundle and the dashboard), `thingsboard/tb_api.py` (add `rpc_twoway(device_id, method, params, timeout_ms=10000) -> (http_status, body_obj_or_text)` using `POST /api/plugins/rpc/twoway/{deviceId}`), `thingsboard/README.md`

**Interfaces:**
- Consumes: the Alarms methods (Task 4) and RPC transport (Task 3) on the bench only. Every file here is testable offline.
- Produces:
  - `dashboard_pages.json`: `[{"id","title","telemetry":[keys],"actions":[{"label","method","params":obj|null,"edit":bool,"load":"<get method>"|null,"confirm":"<Thai text>"|null}]}]`. Later groups append pages.
  - `parity_cases.json`: `{"<group>":{"reads":[{"method","http":"/api/…","ignore":[keys]}],"errors":[{"method","params","http":"/api/…","body"}],"writes":[{"name","get","get_http","path","change","set","set_http"}]}}`.
    - `path` is a dotted path into the GET result.
    - `change` is `{"add":n}` or `{"value":v}`.
    - The `set` body is the GET result with `ok` removed and the path changed. The implementer may extend the format; document it in the README.
  - `tb_dashboard.py --check` validates both JSON files offline:
    - page ids are unique;
    - every action `method` exists in `parity_cases.json` or in an allow-list of non-parity actions;
    - every `confirm` is non-empty Thai text.
  - `tb_dashboard.py` (no flag) is called by `tb_setup.py`. It creates or updates the widget bundle `fishpump`, the widget type `fishpump_rpc_panel` and the dashboard `Fishpump — ทดสอบ` by name. The dashboard has one state per page, assigned to the test customer.
  - **The `fishpump_rpc_panel` widget** (custom static widget, RPC-capable), configured per page through widget settings:
    - renders one button per action, plus a params textarea for `edit` actions;
    - a "โหลดค่าปัจจุบัน" button runs `load` and fills the textarea with the result minus `ok`;
    - `confirm` shows `window.confirm(text)` before sending;
    - sends with `self.ctx.controlApi.sendTwoWayCommand(method, params, 10000)`;
    - shows the reply JSON and a clear ok/error line after every action.
    - Each page also has a latest-telemetry widget for its keys.
  - `parity_test.py`:
    - arguments: `--device fishpump-1de564 [--group g]… [--writes] [--burst N] [--selftest]`, plus the TB args (`tb_api.add_common_args`) and board args (`board_http.add_board_args`);
    - output: one line per case, `PASS`/`FAIL`/`SKIP` with the reason, and a summary; exit code 0 only with no FAIL;
    - READ cases compare the RPC body with the HTTP body, ignoring the listed keys;
    - ERROR cases compare the `error` code;
    - WRITE cases: read via TB, write the change via TB, read back via HTTP (must show the change), restore via HTTP, read via TB (must show the original). If the restore fails, the script stops all remaining writes and prints the original value;
    - `--burst N` sends N `alarms.rules.get` RPCs back to back while a thread polls `GET /api/alarms/summary` every 200 ms. PASS when every reply is complete and `ok`, or exactly `busy`, and the poller saw only 200s;
    - an HTTP reply `403` with `force_password_change` → SKIP "board web password is still the default".
  - `tb_dump.py --widget-type <fqn> | --dashboard <title>` saves the server's JSON for that object to `thingsboard/.local/dump/`.

- [ ] **Step 1 (controller with the user):** run `python thingsboard/tb_dump.py --widget-type <an RPC-capable system widget, e.g. the RPC debug terminal>` and `--dashboard <one demo dashboard>`. Hand both files to the implementer as the 4.3.1 format reference. Ruling to carry in: the widget and dashboard JSON are hand-written against these dumps. Fix rounds after the checkpoint bench are expected (as with the M1 rule chain).
- [ ] **Step 2: Write the failing self-checks:**
  - `python thingsboard/tb_dashboard.py --check` on a deliberately broken pages fixture → non-zero exit;
  - `python thingsboard/parity_test.py --selftest` runs the compare, ignore, path-edit and change functions on built-in samples and prints `SELFTEST PASS`.
- [ ] **Step 3: Run them.** Expected: fail, because the files do not exist.
- [ ] **Step 4: Implement the files.**
  - The Alarms page: actions for all 8 alarm methods. Rule delete and ack carry the web app's confirm text; ack is with `{"all":true}` and with an id.
  - Alarms parity cases:
    - **reads:** all 5 READ methods; `values` and `status` with their volatile keys ignored.
    - **errors:**
      - `alarms.rules.delete {"id":0}` → `value_out_of_range`
      - `alarms.rules.delete {"id":65000}` → `not_found`
      - `alarms.ack {"id":65000}` → `not_found`
      - `alarms.rules.set {}` → `missing_field`
      - `alarms.rules.set` with `"severity":"x"` → the code the HTTP side gives
      - params `[1]` → `invalid_json`
      - method `nope.nope` → `unknown_method` (no HTTP side)
    - **writes:** `alarms.rules.set` on the rule given by `--rule-id` (must be a disabled rule), changing `delay_sec` by +1.
- [ ] **Step 5: Run** `tb_dashboard.py --check`, `parity_test.py --selftest`, `py_compile` on every script, and `tb_setup.py --check`. Expected: all pass. Commit `feat: ThingsBoard test dashboard and parity harness (alarms)`.

### Task 6: Checkpoint bench A (controller with the user)

Flash Tasks 2–5. Prove the pattern on the board before migrating the other groups. Add a section "รอบ 2: device_api + RPC" to `docs/bench-test-thingsboard.md` and record each result there.

| # | Step | Expected |
|---|---|---|
| A1 | The user closes any monitor; Claude flashes `idf.py -C firmware -p COM12 flash` and opens the monitor. | The board boots and the link reaches connected. `BOARD_DIAG` shows `tb_rpc_hwm`. |
| A2 | `api_golden.py compare --base base --label a2 --with-writes` | Every case passes: the alarm cases (now migrated) and every other case (not yet migrated). |
| A3 | `tb_setup.py` twice, then open the dashboard in the built-in browser (the user logs in to ThingsBoard). | The bundle, widget and dashboard are created once and updated on the second run. The Alarms page loads. |
| A4 | `parity_test.py --group alarms --writes --rule-id <a disabled rule>` | All PASS. |
| A5 | On the Alarms page, click every action. Confirm the rule-delete warning appears, then cancel it. | Every reply shows. Cancelling sends nothing (nothing logged on serial). |
| A6 | `parity_test.py --group alarms --burst 20`; during the burst the user presses บันทึก on the ThingsBoard card once. | PASS or `busy` replies only, and no crash. RPCs sent during the restart time out on the ThingsBoard side. Heap free ≥ 100 KB, `tb_rpc_hwm` ≥ 512. |

Any failure → `superpowers:systematic-debugging`, then a fix commit with a test that failed first. When everything passes, commit `docs: checkpoint bench A of device_api`.

### Task 7: Protection group

**Methods** (`max_params_bytes` = that handler's body buffer):

| Method | Endpoint | Flags |
|---|---|---|
| `protection.get` | GET /api/protection | READ |
| `protection.set` | POST /api/protection | WRITE |
| `protection.measure.get` | GET /api/protection/measure | READ |
| `protection.measure.set` | POST /api/protection/measure | WRITE |

- **Engine prefixes:** `dry_run_guard_`, `nvs_store_save_dry_run`, `nvs_store_load_dry_run`.
- **Page:** the Protection page.
  - The measure start carries the web app's measure warning ("ปั๊มอาจเดินแห้งได้นานสุด 120 วินาทีระหว่างวัด ดำเนินการต่อ?").
  - The dry-run alarm ack carries the web app's warning for that ack. It is whichever method the web app uses to clear a dry-run lock; find it in `app.js`. If that method is `alarms.ack`, put the action on this page too.
- **Parity:**
  - **reads:** both GETs, ignoring the timers, ages and counters the implementer finds volatile;
  - **errors:** each 400 code `protection.set` can give without saving; `protection.measure.set` with an unknown action; `protection.measure.set` "save" with no finished measurement → the HTTP code;
  - **writes:** `protection.set` changing `prime_sec` by +1. Never run a measure start in parity.
- Commit `refactor: protection API through device_api`.

### Task 8: Pump group

**Methods:**

| Method | Endpoint | Flags |
|---|---|---|
| `pump.status` | GET /api/pump/status | READ |
| `pump.config.get` | GET /api/pump/config | READ |
| `pump.config.set` | POST /api/pump/config | WRITE |
| `pump.control` | POST /api/pump/control | WRITE |
| `pump.start` | POST /api/pump/start | WRITE |
| `pump.stop` | POST /api/pump/stop | WRITE |

- `pump.status` is built with `json_gen` today; keep the bytes with `cJSON_CreateRaw`.
- The errors that carry pump status (`send_api_error_with_pump_status`) keep that shape. The dry-run lock 409 and the start block move into the methods.
- **Engine prefixes:** `pump_control_`, `nvs_store_save_pump`, `nvs_store_load_pump`, `hardware_map_` (pump config reads the active map).
- **Page:** the Pump page. Start carries the web app's pump-start warning; stop and control go without a confirm, as in the web app.
- **Parity:**
  - **reads:** `pump.status` and `pump.config.get`;
  - **errors:** each 400 code `pump.config.set` gives on a bad field; `pump.control` with an unknown system or action;
  - **writes:** `pump.config.set` changing one set's off time by +1 minute.
  - Pump start/stop/control success is a guided bench step (Task 13), never automatic.
- Commit `refactor: pump API through device_api`.

### Task 9: Cooling group

**Methods:**

| Method | Endpoint | Flags |
|---|---|---|
| `cooling.status` | GET /api/cooling/status | READ |
| `cooling.config.get` | GET /api/cooling/config | READ |
| `cooling.config.set` | POST /api/cooling/config | WRITE |
| `cooling.mode.set` | POST /api/cooling/mode | WRITE |

- `cooling.status` uses `json_gen`, so wrap it raw.
- **Engine prefixes:** `cooling_control_`, `nvs_store_save_cooling`, `nvs_store_load_cooling`.
- **Page:** the Cooling page.
- **Parity:**
  - **reads:** both GETs;
  - **errors:** each range error of `cooling.config.set`; `cooling.mode.set {"mode":"bogus"}`;
  - **writes:** `cooling.config.set` changing `min_off` by +1; `cooling.mode.set` with the current mode (a no-op).
- Commit `refactor: cooling API through device_api`.

### Task 10: Water and power group

**Methods:**

| Method | Endpoint | Flags |
|---|---|---|
| `water.calibrate` | POST /api/water/calibrate | WRITE |
| `water.calibrate.cancel` | POST /api/water/calibrate/cancel | WRITE |
| `water.offset` | POST /api/water/offset | WRITE |
| `power.reset_energy` | POST /api/power/reset-energy | WRITE |

- `sensors_send_queue_result` and its `202 Accepted` reply (`DAPI_ACCEPTED`) move into the methods.
- **Engine prefixes:** `water_quality_`, `power_monitor_`.
- **Page:** the Water/Power page. The energy reset carries the web app's confirm text.
- **Parity:**
  - **errors:** `water.offset {}` → the missing or out-of-range code; `water.calibrate` with a bad point;
  - **writes:** `water.offset` set to the current offset read from `sensors.get` (a no-op). It runs only after Task 12; until then it SKIPs with "needs sensors.get".
  - Calibrate and energy reset success are guided bench steps.
- Commit `refactor: water and power API through device_api`.

### Task 11: Wi-Fi group (without remote connect)

**Methods:**

| Method | Endpoint | Flags |
|---|---|---|
| `wifi.scan` | GET /api/wifi/scan | READ |
| `wifi.profiles.get` | GET /api/wifi/profiles | READ |
| `wifi.profiles.save` | POST /api/wifi/profiles/save | WRITE |
| `wifi.profiles.setauto` | POST /api/wifi/profiles/setauto | WRITE |
| `wifi.profiles.forget` | POST /api/wifi/profiles/forget | WRITE |
| `wifi.connect` | POST /api/wifi/connect | WRITE \| LOCAL_ONLY |
| `wifi.disconnect` | POST /api/wifi/disconnect | WRITE \| LOCAL_ONLY |

- `wifi.connect` is `LOCAL_ONLY` until milestone 4 adds the staging commit and switches it to `STAGED`. `wifi.disconnect` stays `LOCAL_ONLY` for good (spec 6.1).
- **New guard:** `wifi.profiles.forget` with `DAPI_SRC_TB`, naming the SSID the board is connected to now → `{"ok":false,"error":"in_use","message":"Cannot forget the network in use"}` with `DAPI_CONFLICT`. The HTTP path keeps today's behaviour.
- `wifi.scan` keeps the exact HTTP behaviour of today's asynchronous scan (spec 6.1).
- **Engine prefixes:** `wifi_manager_`, `nvs_store_save_wifi`, `nvs_store_load_wifi`, `esp_wifi_`.
- The `force_password_change` checks these handlers have today stay in HTTP.
- **Tests:** the guard decision is a pure function `bool dapi_wifi_forget_blocked(dapi_source_t src, const char *ssid, const char *connected_ssid)` in `dapi_wifi.c`, declared in `dapi_wifi.h`. Selftest it: TB with a match → true; TB with a different SSID → false; HTTP with a match → false; TB with `connected_ssid` empty → false. If the selftest app cannot link `main` sources, move the function to a small pure component `dapi_guards` with the same signature.
- **Page:** the Wi-Fi page. Scan, profiles and setauto as normal actions. Save is an `edit` action whose label says the password is sent in the RPC. Connect and disconnect are shown with a note that they work only from the local web app.
- **Parity:**
  - **reads:** `wifi.profiles.get` (exact, SSIDs only) and `wifi.scan` (shape);
  - **errors:** `wifi.connect` and `wifi.disconnect` from TB → `local_only`; `wifi.profiles.forget` of the connected SSID from TB → `in_use`; forget of a made-up SSID → the HTTP code; `setauto` with index 99 → the HTTP code;
  - **writes:** `setauto` with the current index (a no-op).
- Commit `refactor: Wi-Fi API through device_api`.

### Task 12: Status, sensors, hardware map and uplink preview

**Methods:**

| Method | Endpoint | Flags |
|---|---|---|
| `status.get` | GET /api/status | READ |
| `sensors.get` | GET /api/sensors/status | READ |
| `hardware.map.get` | GET /api/hardware/map | READ |
| `hardware.map.set` | POST /api/hardware/map | WRITE \| LOCAL_ONLY |
| `uplink.preview` | GET /api/uplink/preview | READ |

- `status.get` uses `json_gen` with a 3072-byte buffer, so move that buffer to the heap and wrap the result raw.
- **Engine prefixes:** `esp_get_free_heap_size`, `heap_caps_get_`, `water_quality_get`, `power_monitor_get`, `flow_switch_get`, `nvs_store_save_hardware`, `uplink_`.
- **Pages:** Status, Sensors, and Hardware (read-only: `hardware.map.get` only, plus a note that changes need the local web app).
- **Parity:**
  - **reads:** all four READ methods as `shape`, except `hardware.map.get` (exact);
  - **errors:** `hardware.map.set` from TB → `local_only`.
- Commit `refactor: status, sensors and hardware API through device_api`.

### Task 13: Final bench of milestone 2 + method reference (controller with the user)

**Files:**
- Create: `docs/firmware/device-api.md`. It lists every method with its endpoint, flags, params, reply keys and error codes, and it records the spelling the frontend will use (spec 6.1, section 9).
- Modify: `docs/bench-test-thingsboard.md` (section "รอบ 2" results), `docs/backlog/after-frontend.md` (anything found on the bench)

| # | Step | Expected |
|---|---|---|
| F1 | Flash Tasks 7–12; monitor. | Boot, connected, no `E` lines from `device_api` or `tb_link`. |
| F2 | `api_golden.py compare --base base --label f2 --with-writes` | Every case passes. |
| F3 | `git diff --stat be0ab72 -- firmware/main/static` | Empty. |
| F4 | `parity_test.py --writes --rule-id <disabled rule>`, all groups | All PASS. SKIPs are listed with reasons. |
| F5 | Dashboard walk-through in the built-in browser: every page, every action except the guided ones below. | Every reply visible; every confirm text matches the web app. |
| F6 | Guided, with the user watching the relay: on the Pump page start, then stop. Then run the measure from the Protection page and stop it. | Same behaviour as the web buttons. The pump-start warning appears first. |
| F7 | `parity_test.py --burst 30` while the web app's Status page is open, then `BOARD_DIAG` for 15 min. | No crash. Heap free ≥ 100 KB, every HWM ≥ 512 B including `tb_rpc_hwm`. |

- [ ] **Steps:**
  1. Run F1–F7 and record the results.
  2. Write `device-api.md`.
  3. Fix any failure through `superpowers:systematic-debugging` with a failing test first.
  4. Commit `docs: bench milestone 2 of ThingsBoard link and device_api reference`.

## Rulings made in this plan

- **Two status codes beyond the spec's list:** `DAPI_ACCEPTED` (202) and `DAPI_NOT_FOUND` (404). Today's handlers return both (sensor queue results, rule not found), and the spec requires the same codes both ways.
- **Method bodies live in `firmware/main/dapi_<group>.c`, not inside the `device_api` component.** They need `main`'s helpers and drivers. The component stays a small pure core.
- **`json_gen` and literal-string replies are wrapped with `cJSON_CreateRaw`** rather than rebuilt. cJSON prints numbers differently from `%g`, and the bytes must not change (spec section 9 leaves this choice open).
- **`wifi.connect` is `LOCAL_ONLY` during M2.** Remote Wi-Fi is milestone 4, which needs the staging commit first.
- **Golden files are stored under `thingsboard/.local/` (git-ignored)**, because they hold the user's SSIDs and rules.
- **Two flashes in M2:** checkpoint A after the first group, and the final bench. That proves the pattern on hardware before the five remaining groups, without flashing seven times.
- **The dashboard uses one data-driven custom widget.** The widget JSON is hand-written against dumps of 4.3.1 objects taken at the start of Task 5, and fix rounds at checkpoint A are expected.
