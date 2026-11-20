# ThingsBoard Link — Milestone 5 (firmware OTA from ThingsBoard with automatic rollback) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An OTA package assigned to the board in ThingsBoard is downloaded over MQTT, checked, written to the other OTA slot and booted. The new image stays only if it reaches ThingsBoard again within 5 minutes; otherwise it rolls back by itself. ThingsBoard shows each step in `fw_state`.

**Architecture:**
- `tb_link_logic` gains pure OTA helpers, which are selftested in QEMU:
  - parse the `fw_*` attributes;
  - decide start / ignore / fail;
  - chunk topics and lengths;
  - the `fw_state` JSON;
  - work out the state at boot.
- `tb_link` gains generic plumbing and stays protocol-only:
  - three more subscriptions;
  - a data hook for every message that is not RPC or provisioning, with fragments routed to the hook;
  - `tb_link_publish`;
  - a session hook that runs after the subscriptions.
- The new `firmware/main/tb_ota.c` holds the policy. On START it creates a short-lived `tb_ota` task, which:
  1. requests 16 KB chunks one at a time;
  2. hashes them with PSA SHA-256 and writes them with `esp_ota_*`;
  3. waits while a dry-run measurement runs;
  4. stores an NVS "pending" version and reboots.
- On boot, `app_main` skips its two mark-valid calls while the pending image waits for ThingsBoard. The session hook marks the image valid and reports `UPDATED`; a 5-minute timer rolls it back instead.
- A tiny `ota_lock` stops the web upload and the ThingsBoard download from writing the slot at the same time.

**Tech Stack:** ESP-IDF v6.0.1 (`app_update`, PSA Crypto through mbedTLS 4 / TF-PSA-Crypto, `esp_timer`), managed `espressif/mqtt ^1.1.0`, cJSON, FreeRTOS, ThingsBoard CE 4.3.1. The PC scripts use the Python 3 standard library only.

**Spec:** `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`. The relevant sections are:
- **6.5**, including **"Spike results (2026-11-20)"**. That subsection is binding for topics, payloads and hazards.
- 3 (anti-goal: "A remote Wi-Fi change or OTA that leaves the board unreachable without someone visiting it");
- 6.2 (subscriptions);
- 6.7 (budget);
- 7.2 (Firmware page);
- 8 (tests);
- 8a item 5;
- 10 call 5.

Milestones 1–4 are done (`docs/bench-test-thingsboard.md`, rounds 1–5).

Out of scope:
- software OTA (`sw_*`);
- TLS;
- the ThingsBoard-down test, I7 and the full J4 (all M6);
- any change to the web upload's format, key or size limit.

## Global Constraints

- **Gates.** These stay green after every task:
  - The build with **0 warnings**: `. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM; idf.py -C firmware build`.
  - Every `firmware/scripts/verify_*.py` script. Baselines: `verify_tb_link.py` 148, `verify_device_api.py` 285; the counts only grow.
  - The QEMU selftest `firmware/test/sensor_selftest/run_selftest.ps1`. The baseline is 1976 checks; the count only grows.
  - `python thingsboard/parity_test.py --selftest` and `python thingsboard/tb_dashboard.py --check`.
- **The web app does not change.**
  - No file under `firmware/main/static/` changes.
  - Every `/api/*` response stays the same. The one exception is a new `409 Conflict` with `{"ok":false,"error":"ota_busy"}` from `POST /api/ota`, and only while a ThingsBoard download holds the OTA lock.
- **Protocol values** (spec 6.5 spike results). Put them in `tb_link_logic.h`:

  | Name | Value |
  |---|---|
  | `TB_FW_TITLE` | `"fishpump"` |
  | `TB_FW_CHUNK_SIZE` / `TB_FW_CHUNK_SIZE_STR` | `16384u` / `"16384"` |
  | `TB_FW_CHUNK_TIMEOUT_MS` | `10000u` per attempt |
  | `TB_FW_CHUNK_TRIES` | `3` attempts per chunk |
  | `TB_FW_VERIFY_TIMEOUT_MS` | `300000u` (5 min) |
  | `TB_FW_ATTR_TOPIC` | `"v1/devices/me/attributes"` |
  | `TB_FW_ATTR_REQ_PREFIX` / `TB_FW_ATTR_RESP_PREFIX` | `"v1/devices/me/attributes/request/"` / `"v1/devices/me/attributes/response/"` |
  | `TB_FW_REQ_PREFIX` / `TB_FW_RESP_PREFIX` | `"v2/fw/request/"` / `"v2/fw/response/"` |
  | `TB_FW_ERROR_TOPIC` | `"v2/fw/error"` |
  | `TB_TELEMETRY_TOPIC` | `"v1/devices/me/telemetry"` |
  | `TB_FW_ATTR_REQUEST_BODY` | `{"sharedKeys":"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm"}` |
  | `TB_FW_VERSION_MAX` | `33` (incl. NUL) |

- **A chunk request payload is always `TB_FW_CHUNK_SIZE_STR`.** Never send it empty (the server would return the whole image in one message) and never non-numeric (the broker would drop the connection).
- **`fw_state` sequence** (device telemetry): `DOWNLOADING` → `DOWNLOADED` → `VERIFIED` → `UPDATING` → (reboot) → `UPDATED`. Any failure gives `FAILED` plus `fw_error`. `DOWNLOADING` and `UPDATED` also carry `current_fw_title` and `current_fw_version`.
- **`fw_error` codes.** "Final" means the version is remembered in RAM and ignored until the next reboot; the others are retried at the next connect or the next push.

  | Code | When | Final |
  |---|---|---|
  | `bad_attributes` | title is `fishpump` but the other `fw_*` fields are missing or wrong, or the algorithm is not `SHA256` | yes |
  | `too_large` | `fw_size` > the size of the next update partition | yes |
  | `busy` | the OTA lock is held (web upload) | no |
  | `no_memory` | the chunk buffer or the task could not be allocated | no |
  | `ota_begin_failed` | `esp_ota_begin` failed (e.g. the running image is still pending verification) | no |
  | `chunk_timeout` | `TB_FW_CHUNK_TRIES` attempts without a complete chunk | no |
  | `server_error` | a message on `v2/fw/error` (for example `PAYLOAD_TOO_LARGE`) | no |
  | `bad_chunk` | a chunk whose total length is not the expected length | yes |
  | `write_failed` | `esp_ota_write` failed | no |
  | `checksum_mismatch` | the SHA-256 of all bytes differs from `fw_checksum` | yes |
  | `invalid_image` | `esp_ota_end` rejected the image | yes |
  | `set_boot_failed` | `esp_ota_set_boot_partition` failed | no |
  | `rolled_back` | after the reboot the running version is not the pending one | — (reported once at boot) |

- **Version:**
  - `firmware/CMakeLists.txt` takes the version from `firmware/version.txt`, which must be numeric `x.y.z`. It sets both `PROJECT_VER` and `project(... VERSION ...)`.
  - The running version is `esp_app_get_description()->version`.
  - Versions compare equal when they differ only by one leading `v`/`V`, so `"v0.3.3"` equals `"0.3.3"`.
- **NVS:** the pending version lives under key `tb_ota_ver`, in the same namespace as the restart detail. It is set right before the reboot and cleared only after the boot result has been reported.
- **Restart detail** for a ThingsBoard update: `"TB_OTA_REBOOT"`.
- **Memory:**
  - The esp-mqtt `buffer.size` stays 2048. A 16 KB chunk arrives as fragments, which are assembled into one 16384-byte PSRAM buffer that exists only during an update.
  - The `tb_ota` task stack (6144 bytes) is internal RAM (`xTaskCreate`, never a PSRAM stack: flash writes disable the cache). The task exists only during an update and deletes itself.
- **Log tag** `tb_ota`, raised to INFO in `app_main.c`. It logs:
  - start: title, version and size;
  - every 25 % of progress;
  - each state;
  - each failure code;
  - the mark-valid;
  - the rollback timer firing.
- **Never stage** `firmware/.vscode/settings.json` or `docs/bench-test-alarms.md`. No merge or push. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **The link drops in the middle of a download** (Wi-Fi blip, broker restart).
   - Expected: the current chunk is retried, and the download resumes once esp-mqtt reconnects. If 3 × 10 s pass without the chunk, the result is `FAILED chunk_timeout`. Nothing is booted, and the same version is tried again at the next connect because the error is not final.
   - Pinned by: the Task 1 `tb_fw_error_is_final` tests and a Task 4 verify check that the chunk loop retries before failing.
2. **The same assignment arrives twice:** the push, plus the reply to the request made on connect, or ThingsBoard re-sending the attributes.
   - Expected: exactly one download.
   - Pinned by: the Task 1 `tb_fw_decide(..., busy=true)` → IGNORE test, and Task 4 setting busy before the task is created.
3. **The new image boots and ThingsBoard still shows the package it now runs.**
   - Expected: IGNORE, with no download loop, even when one side writes `v0.3.3` and the other `0.3.3`.
   - Pinned by: the Task 1 `tb_fw_version_equal` and decide tests.
4. **Power cut during a download, or during the 5-minute verify window.**
   - Expected: during a download, the running slot is untouched and the pending flag is not yet set, so the next boot is normal and the download starts again on connect. During the verify window, the bootloader drops the unconfirmed image, and the next connect reports `FAILED rolled_back`.
   - Pinned by: the Task 1 `tb_fw_boot_state` tests, and bench K4.
5. **A web upload while a ThingsBoard download runs, or the reverse.**
   - Expected: the second writer is refused and the slot is never written by both. The web upload gets 409 `ota_busy`; ThingsBoard gets `FAILED busy`, retried later.
   - Pinned by: Task 3 verify checks on where the lock is claimed and released, and the Task 4 claim check.

## Calls in this plan (the user can overturn any)

1. **The ThingsBoard package is the plain `firmware/build/fishpump_firmware.bin`, not the AES-CTR file the web upload expects.**
   - No `secrets.h` exists, so today's web key is all zeros. ThingsBoard's SHA-256 plus IDF's own image check give integrity, and ThingsBoard's login and the device token decide who can push firmware.
   - Cost: if a real web key is set later, the two paths use different files.
2. **The ThingsBoard path is a new module.** "Reusing the `esp_ota_*` path" (spec 6.5) is read as the same IDF calls and partitions. The web handler is not refactored; it only gains the lock.
3. **16 KB chunks, with esp-mqtt's 2 KB buffer kept** (fragments assembled in PSRAM). Internal heap reached 104.96 KB at M4, so a 16 KB internal buffer would break the 100 KB floor.
4. **A short-lived `tb_ota` task** instead of a permanent one, for the same heap reason. Its 6 KB stack is taken only while an update runs.
5. **A newer assignment during a download is not chased.** The running download finishes or fails. After the reboot (or the next connect) the board reads the assignment again and follows the newer one.
6. **The never-connects test image is a compile option** (`TB_TEST_BLOCK_CONNECT`, OFF by default), built in its own build directory `firmware/build_block`. Changing the host on the card, the spec's other option, would also stop the old image from downloading.
7. **The test-dashboard placeholder fix from the M4 bench backlog is folded in.** The Wi-Fi page's `wifi.connect` defaults to `ssid ""`, so an unedited send gets `missing_ssid` and no reboot.
8. **Found while planning, not fixed here:** `POST /api/ota` rejects files over `0x1E0000` (1,966,080 bytes). Today's image is 2,061,648 bytes plus the 16-byte IV, so **a web upload of the current build gets 413 `file_too_large`**. The web app stays unchanged in this milestone; Task 6 writes this into the backlog.
9. **The internal-heap floor during a download is 90 KB, not 100 KB.** The spec's 100 KB floor (6.7) is for the link plus an RPC burst. The `tb_ota` task's 6 KB stack exists only during the roughly 1-minute update, and the 100 KB floor still applies before and after it (bench K7).

---

### Task 1: Pure OTA helpers

**Files:**
- Modify: `firmware/components/tb_link_logic/include/tb_link_logic.h`, `firmware/components/tb_link_logic/tb_link_logic.c`
- Test: new `firmware/test/sensor_selftest/main/test_tb_fw.c`, registered and added to the build the same way as `test_tb_link.c`.

**Interfaces (produces, in `tb_link_logic.h`, plus every `#define` in Global Constraints):**
```c
typedef struct {
    char title[33];
    char version[TB_FW_VERSION_MAX];
    uint32_t size;
    char checksum[65]; /* lowercase hex */
} tb_fw_info_t;

typedef enum { TB_FW_ATTR_NONE = 0, TB_FW_ATTR_CLEARED, TB_FW_ATTR_INFO, TB_FW_ATTR_INVALID } tb_fw_attr_t;
tb_fw_attr_t tb_fw_parse_attrs(const char *json, size_t len, tb_fw_info_t *out);

typedef enum { TB_FW_IGNORE = 0, TB_FW_START, TB_FW_FAIL } tb_fw_decision_t;
tb_fw_decision_t tb_fw_decide(const tb_fw_info_t *fw, const char *running_version, const char *last_failed_version,
                              uint32_t max_size, bool busy, const char **reason);

bool tb_fw_version_equal(const char *a, const char *b);
bool tb_fw_error_is_final(const char *code);
uint32_t tb_fw_chunk_count(uint32_t size);
uint32_t tb_fw_chunk_len(uint32_t size, uint32_t idx);
int tb_fw_request_topic(uint32_t req_id, uint32_t idx, char *out, size_t size);
bool tb_fw_parse_chunk_topic(const char *topic, size_t topic_len, uint32_t *req_id, uint32_t *idx);
bool tb_fw_digest_matches(const uint8_t digest[32], const char *hex64);
int tb_fw_state_json(char *buf, size_t size, const char *state, const char *error,
                     const char *cur_title, const char *cur_version);

typedef enum { TB_FW_BOOT_NONE = 0, TB_FW_BOOT_VERIFY, TB_FW_BOOT_UPDATED, TB_FW_BOOT_ROLLED_BACK } tb_fw_boot_t;
tb_fw_boot_t tb_fw_boot_state(const char *pending_version, const char *running_version, bool pending_verify);
```

**Rules:**
- **`tb_fw_parse_attrs`:**
  - Input that is not a JSON object gives INVALID.
  - If the object has a `"shared"` object (the reply form), that object is the source, and a source without `fw_title` gives CLEARED. So `{"shared":{}}` and `{}` both give CLEARED.
  - Otherwise it is the push form:
    - a `"deleted"` array that contains `"fw_title"` gives CLEARED;
    - no `fw_title` at all gives NONE. This covers `{"deleted":["fw_tag","fw_url"]}`.
  - With `fw_title` (a string of at most 32 bytes), the title is copied into `out` **first**. Then INFO needs all of these, or the result is INVALID:
    - `fw_version`: a string of 1–32 bytes;
    - `fw_size`: an integral JSON number from 1 to 4294967295;
    - `fw_checksum_algorithm`: exactly `"SHA256"`;
    - `fw_checksum`: 64 hex characters, stored lowercase.
- **`tb_fw_decide`**, checked in this order:
  1. Title ≠ `TB_FW_TITLE` → IGNORE.
  2. `busy` → IGNORE.
  3. Version equals the running version → IGNORE.
  4. Version equals `last_failed_version` (NULL = none) → IGNORE.
  5. `size > max_size` → FAIL, with `*reason = "too_large"`.
  6. Otherwise → START.
  - `*reason` is set only on FAIL; `reason` itself may be NULL.
- **`tb_fw_version_equal`:** strips one leading `v` or `V` from each side, then compares exactly. NULL on either side gives false.
- **`tb_fw_error_is_final`:** true exactly for the codes marked "yes" in Global Constraints.
- **Chunks:**
  - `tb_fw_chunk_count` = ceil(size / 16384).
  - `tb_fw_chunk_len` = the expected length of chunk `idx`, or 0 when `idx` ≥ the count.
- **`tb_fw_request_topic`** builds `v2/fw/request/<req>/chunk/<idx>` and returns its length, or -1 when it does not fit.
- **`tb_fw_parse_chunk_topic`** parses `v2/fw/response/<req>/chunk/<idx>`. It is not NUL terminated, and uses the same decimal rules as `tb_rpc_parse_topic`.
- **`tb_fw_digest_matches`** compares the digest with the hex string, case-insensitive.
- **`tb_fw_state_json`:**
  - The keys appear in this order: `fw_state`, `fw_error`, `current_fw_title`, `current_fw_version`.
  - A NULL argument leaves its key out. When all four are NULL, the result is `{}`.
  - Returns the length, or -1 when the JSON does not fit.
- **`tb_fw_boot_state`:**
  - `pending_version` NULL → NONE.
  - Pending equals running (`tb_fw_version_equal`): VERIFY if `pending_verify`, otherwise UPDATED.
  - Pending differs from running → ROLLED_BACK.

- [ ] **Step 1: Write the failing tests** in `test_tb_fw.c`. Use the spike payloads verbatim. `C` below is the spike checksum `a4710f239f3187ac0a067a1230e6c3ee7ad7191724cd49253779906ec129fd6f`.
  - **`tb_fw_parse_attrs`:**
    - `{"deleted":["fw_tag","fw_url"]}` → NONE.
    - The push `{"fw_title":"fishpump","fw_version":"0.3.3","fw_size":2061648,"fw_checksum_algorithm":"SHA256","fw_checksum":"<C>"}` → INFO, with size 2061648 and the checksum copied.
    - `{"shared":{…same…}}` → INFO.
    - `{"shared":{}}` → CLEARED.
    - `{}` → CLEARED.
    - `{"deleted":["fw_title","fw_version"]}` → CLEARED.
    - The push with `"fw_checksum_algorithm":"MD5"` → INVALID, and `out->title` is `"fishpump"`.
    - `fw_size` given as `0`, as `-1`, as `1.5`, and as the string `"2061648"` → INVALID each.
    - A checksum of 63 characters, or with a `g` in it → INVALID.
    - An uppercase checksum → INFO, stored lowercase.
    - `[]`, `not json` and `null` → INVALID.
  - **`tb_fw_decide`** (running `"0.3.2"`, max 0x380000):
    - title `"other"` → IGNORE;
    - busy → IGNORE;
    - version `"v0.3.2"` → IGNORE;
    - `last_failed_version` `"0.3.3"` with version `"0.3.3"` → IGNORE;
    - size 0x380001 → FAIL `"too_large"`;
    - version `"0.3.3"` → START, with `reason` untouched;
    - `reason` NULL on the FAIL case → no crash.
  - **`tb_fw_version_equal`:**
    - `("v0.3.3","0.3.3")` and `("V1","v1")` → true;
    - `("0.3.3","0.3.30")` → false;
    - `(NULL,"x")` → false;
    - `("vv1","v1")` → false.
  - **`tb_fw_error_is_final`:**
    - `bad_attributes`, `too_large`, `bad_chunk`, `checksum_mismatch` and `invalid_image` → true;
    - `busy`, `chunk_timeout`, `server_error`, `write_failed`, `no_memory`, `ota_begin_failed` and `set_boot_failed` → false;
    - `"nonsense"` and NULL → false.
  - **Chunks:**
    - `tb_fw_chunk_count(2061648)` = 126, `(16384)` = 1, `(16385)` = 2.
    - `tb_fw_chunk_len(2061648,125)` = 13648, `(2061648,0)` = 16384, `(2061648,126)` = 0.
  - **Topics:**
    - `tb_fw_request_topic(1, 0, …)` = `"v2/fw/request/1/chunk/0"`, and a 10-byte buffer gives -1.
    - `tb_fw_parse_chunk_topic("v2/fw/response/2/chunk/125")` → req 2, idx 125.
    - `".../chunk/"`, `".../chunk/x"`, `"v2/fw/response/2/chunk"` and `"v2/fw/error"` → false each.
  - **`tb_fw_digest_matches`:** the SHA-256 of `"abc"` (`ba7816bf…20015ad`) as bytes matches its lowercase and its uppercase hex, and a 1-bit change does not match.
  - **`tb_fw_state_json`:**
    - `("DOWNLOADING", NULL, "fishpump", "0.3.2")` = `{"fw_state":"DOWNLOADING","current_fw_title":"fishpump","current_fw_version":"0.3.2"}`;
    - `("FAILED", "rolled_back", NULL, NULL)` = `{"fw_state":"FAILED","fw_error":"rolled_back"}`;
    - all NULL = `{}`;
    - a 10-byte buffer gives -1.
  - **`tb_fw_boot_state`:**
    - `(NULL,"0.3.3",true)` → NONE;
    - `("0.3.3","v0.3.3",true)` → VERIFY;
    - `("0.3.3","0.3.3",false)` → UPDATED;
    - `("0.3.3","0.3.2",false)` and `("0.3.3","0.3.2",true)` → ROLLED_BACK.
- [ ] **Step 2: Run the QEMU selftest and see it fail.** Expected: a build error on the missing names.
- [ ] **Step 3: Implement the functions** in `tb_link_logic.c`, using cJSON as the existing parsers do.
- [ ] **Step 4: Run the selftest and see it pass.** Expected: `SELFTEST PASS (<n>)` with n > 1976. The firmware build has 0 warnings.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/tb_link_logic firmware/test/sensor_selftest
  git commit -m "feat: pure ThingsBoard OTA helpers: attribute parse, decision, chunks, state JSON, boot state"
  ```

### Task 2: `tb_link` plumbing, the version from `version.txt`, and the test-block option

**Files:**
- Modify:
  - `firmware/components/tb_link/include/tb_link.h`, `firmware/components/tb_link/tb_link.c`
  - `firmware/CMakeLists.txt`
  - `.gitignore`: add `firmware/build_block/`
- Test: `firmware/scripts/verify_tb_link.py`. Add a new function `test_ota_link()` and call it from `main()`.

**Interfaces:**
- Consumes: the Task 1 topic defines.
- Produces (in `tb_link.h`):
  ```c
  typedef struct {
      const char *topic; int topic_len;  /* the message's topic, also on later fragments */
      const char *data;  int data_len;   /* this fragment */
      int offset;        int total;      /* fragment offset and the message's total length */
  } tb_link_data_t;
  void tb_link_set_data_hook(void (*hook)(const tb_link_data_t *d)); /* set once at boot, before tb_link_init */
  void tb_link_set_session_hook(void (*hook)(void));                 /* same */
  int  tb_link_publish(const char *topic, const char *data, int len, int qos); /* msg id, or -1 */
  bool tb_link_is_connected(void);
  ```
- **The data hook:**
  - It runs on the esp-mqtt task, for every `MQTT_EVENT_DATA` of a token session whose topic is neither the RPC prefix nor the provision response.
  - tb_link copies the first fragment's topic into a static 64-byte buffer and uses it for that message's later fragments, which esp-mqtt delivers without a topic.
  - A topic longer than 63 bytes is not delivered.
  - The hook must not block.
- **`tb_link_publish`:** usable from any task. It uses the same guard as `rpc_publish` (`client_lock` for `TB_RPC_LOCK_MS`, `s.client_pub`, connected flag) and `esp_mqtt_client_enqueue`.
- **In `CMD_CONNECTED` for a token session, keep this order:**
  1. the connected hook (M4);
  2. subscribe `TB_RPC_SUB_TOPIC`;
  3. subscribe `TB_FW_ATTR_TOPIC`;
  4. subscribe `TB_FW_ATTR_RESP_PREFIX "+"`;
  5. subscribe `TB_FW_RESP_PREFIX "+/chunk/+"`;
  6. the session hook.
  - All subscriptions use QoS 1. A failed subscribe is a WARN, as for RPC today.
  - `v2/fw/error` needs no subscription; the spike saw it delivered anyway.
- **`firmware/CMakeLists.txt`:**
  ```cmake
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" FISHPUMP_VERSION LIMIT_COUNT 1)
  set(PROJECT_VER "${FISHPUMP_VERSION}")
  option(TB_TEST_BLOCK_CONNECT "Test image that never connects to ThingsBoard (OTA rollback bench)" OFF)
  # project(fishpump_firmware VERSION ${FISHPUMP_VERSION}) replaces the literal 0.3.2
  # after project(): if(TB_TEST_BLOCK_CONNECT) add_compile_definitions(TB_LINK_TEST_BLOCK_CONNECT=1) endif()
  ```
  `APP_TEMPLATE_FIRMWARE_VERSION` keeps its `"v${PROJECT_VERSION}"` form.
- **`TB_LINK_TEST_BLOCK_CONNECT`:** `task_start_session` sets the error `"test build: connect blocked"`, publishes `TB_STATE_OFF` and returns before any client is created. This code exists only under `#ifdef TB_LINK_TEST_BLOCK_CONNECT`.

- [ ] **Step 1: Write the failing verify checks** in `test_ota_link()`:
  - `tb_link.h` declares the four functions and `tb_link_data_t`.
  - In the `CMD_CONNECTED` body, these appear in this order:
    1. the connected hook call;
    2. `TB_RPC_SUB_TOPIC`;
    3. `TB_FW_ATTR_TOPIC`;
    4. `TB_FW_ATTR_RESP_PREFIX`;
    5. `TB_FW_RESP_PREFIX`;
    6. the session hook call.
  - `tb_link_publish` calls `esp_mqtt_client_enqueue` while holding `client_lock`.
  - `mc.buffer.size = 2048` is unchanged.
  - Every occurrence of `TB_LINK_TEST_BLOCK_CONNECT` in `tb_link.c` is inside an `#ifdef`.
  - `firmware/CMakeLists.txt`:
    - reads `version.txt`;
    - declares `TB_TEST_BLOCK_CONNECT` with default `OFF`;
    - contains no literal `VERSION 0.`.
  - `.gitignore` lists `firmware/build_block/`.
- [ ] **Step 2: Run `python firmware/scripts/verify_tb_link.py` and see the new checks fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run the gates.**
  - All verify scripts pass, the build has 0 warnings, and the selftest count is unchanged from Task 1.
  - `firmware/build/project_description.json` shows `"project_version": "0.3.2"`.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/components/tb_link firmware/CMakeLists.txt .gitignore firmware/scripts/verify_tb_link.py
  git commit -m "feat: tb_link data and session hooks, publish, OTA subscriptions; version from version.txt; test-block option"
  ```

### Task 3: The pending flag in NVS and the OTA lock

**Files:**
- Create: `firmware/main/ota_lock.c`, `firmware/main/ota_lock.h`
- Modify:
  - `firmware/components/nvs_store/include/nvs_store.h` and `firmware/components/nvs_store/nvs_store.c`
  - `firmware/main/web_server.c` (`handle_api_ota` only)
  - `firmware/main/CMakeLists.txt` (SRCS)
- Test: `verify_tb_link.py`. Add a new function `test_ota_lock()`.

**Interfaces (produces):**
```c
/* nvs_store.h — key "tb_ota_ver" in the restart-detail namespace */
bool nvs_store_set_tb_ota_pending(const char *version);         /* false on NVS error or version too long */
bool nvs_store_get_tb_ota_pending(char *version, size_t size);  /* false when absent */
bool nvs_store_clear_tb_ota_pending(void);                      /* true also when already absent */
/* ota_lock.h */
bool ota_lock_claim(void);   /* atomic compare-exchange false -> true */
void ota_lock_release(void);
bool ota_lock_held(void);
```

**`handle_api_ota`:**
- Call `ota_lock_claim()` immediately before `esp_ota_begin`. When it fails, reply `409 Conflict` with `{"ok":false,"error":"ota_busy"}`.
- Call `ota_lock_release()` on every return after a successful claim. That means each of the error returns (`ota_begin_failed`, `recv_failed`, `ota_write_failed`, `incomplete`, `ota_end_failed`, `ota_set_boot_failed`) and the success return.
- Nothing before the claim changes.

- [ ] **Step 1: Write the failing verify checks** in `test_ota_lock()`:
  - `ota_lock.c` is in SRCS.
  - In `handle_api_ota`:
    - `ota_lock_claim` comes before `esp_ota_begin`;
    - `"ota_busy"` comes with `"409 Conflict"`;
    - the number of `ota_lock_release` calls is at least the number of `return` statements after the claim.
  - `nvs_store.c` uses the literal key `"tb_ota_ver"`.
- [ ] **Step 2: Run it and see the new checks fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run every gate.** All verify scripts pass, the build has 0 warnings and the QEMU selftest passes.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/main/ota_lock.c firmware/main/ota_lock.h firmware/main/CMakeLists.txt firmware/main/web_server.c firmware/components/nvs_store firmware/scripts/verify_tb_link.py
  git commit -m "feat: OTA writer lock shared by the web upload and ThingsBoard; pending OTA version in NVS"
  ```

### Task 4: `tb_ota` — download, verify, write and reboot

**Files:**
- Create: `firmware/main/tb_ota.c`, `firmware/main/tb_ota.h`
- Modify: `firmware/main/CMakeLists.txt` (SRCS; REQUIRES `app_update` if it is not already there)
- Test: `verify_tb_link.py`. Add a new function `test_tb_ota()`.

**Interfaces:**
- Consumes:
  - Task 1 (all of it);
  - Task 2 `tb_link_publish`, `tb_link_is_connected` and `tb_link_data_t`;
  - Task 3 `ota_lock_*` and `nvs_store_set_tb_ota_pending`;
  - `dry_run_guard_get_measure` (running = `DRY_RUN_MEASURE_RUNNING`, as in `dapi_wifi.c`);
  - `nvs_store_set_restart_detail`, `session_invalidate_all` and `uplink_note_settings_changed`.
- Produces (in `tb_ota.h`; Task 5 adds the boot functions):
  ```c
  void tb_ota_on_data(const tb_link_data_t *d); /* tb_link data hook (esp-mqtt task) */
  bool tb_ota_busy(void);
  ```

**`tb_ota_on_data`:**
- **Attributes.** The topic is `TB_FW_ATTR_TOPIC` or starts with `TB_FW_ATTR_RESP_PREFIX`, and the message is whole (`offset == 0 && data_len == total`). Parse it with `tb_fw_parse_attrs`.
  - INFO: run `tb_fw_decide` with:
    - the running version;
    - the final failed version, kept in RAM;
    - the size of `esp_ota_get_next_update_partition(NULL)`;
    - `tb_ota_busy()`.
  - START:
    1. `ota_lock_claim()`. If that fails, report `FAILED busy`.
    2. Allocate the chunk buffer with `heap_caps_malloc(TB_FW_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.
    3. Set busy.
    4. Create the task `tb_ota`: `xTaskCreate`, stack 6144, priority 2.
    5. If either allocation fails, release everything and report `FAILED no_memory`.
  - FAIL: report `FAILED <reason>` and remember the version, since the decision's codes are final.
  - INVALID with `out->title == "fishpump"`: report `FAILED bad_attributes` and remember the version.
  - NONE, CLEARED and IGNORE: nothing.
- **Chunks.** The topic parses with `tb_fw_parse_chunk_topic`, and `req` and `idx` equal what the task is waiting for:
  - `total != tb_fw_chunk_len(size, idx)` → wake the task with an error.
  - Otherwise copy the fragment to `buf + offset`, and wake the task when `offset + data_len == total`.
  - Messages that do not match are dropped.
- **`TB_FW_ERROR_TOPIC`** → wake the task with `server_error`.
- Wake the task with `xTaskNotify`. The waited-for `req` and `idx` are atomics written by the task before it publishes each request.

**The `tb_ota` task:**
1. Report `DOWNLOADING` with the current title and version.
2. `esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, …)` → else `ota_begin_failed`. Then `psa_hash_setup(PSA_ALG_SHA_256)`.
3. For each chunk index, with `req` = a counter bumped per download:
   - Publish `tb_fw_request_topic(req, idx)` with the payload `TB_FW_CHUNK_SIZE_STR`, QoS 0.
   - Wait `TB_FW_CHUNK_TIMEOUT_MS` for the notify. A timeout re-publishes the same request, up to `TB_FW_CHUNK_TRIES` attempts → `chunk_timeout`.
   - While `tb_link_is_connected()` is false, wait in 500 ms steps without spending an attempt, for at most `TB_FW_CHUNK_TIMEOUT_MS × TB_FW_CHUNK_TRIES` in total.
   - Then `psa_hash_update` and `esp_ota_write` → else `write_failed`.
4. `psa_hash_finish`, then `tb_fw_digest_matches` → else `checksum_mismatch`. Report `DOWNLOADED`.
5. `esp_ota_end` → else `invalid_image`. Report `VERIFIED`.
6. While a dry-run measurement runs, wait in 1 s steps and log once. Then:
   1. `nvs_store_set_tb_ota_pending(version)`;
   2. `esp_ota_set_boot_partition` → else `set_boot_failed`, and clear the pending version;
   3. report `UPDATING`;
   4. `uplink_note_settings_changed()`;
   5. `nvs_store_set_restart_detail("TB_OTA_REBOOT")`;
   6. `session_invalidate_all()`;
   7. wait 1500 ms so the queued telemetry gets out;
   8. `esp_restart()`.

**Any failure** in the task:
- calls `esp_ota_abort` if begin succeeded, and `psa_hash_abort`;
- reports `FAILED <code>`;
- remembers the version if `tb_fw_error_is_final`;
- frees the buffer, clears busy and releases the lock;
- then `vTaskDelete(NULL)`.

**Reports** use `tb_fw_state_json` and `tb_link_publish(TB_TELEMETRY_TOPIC, …, 1)`. A report while disconnected is simply lost, and the next connect re-reads the state.

- [ ] **Step 1: Write the failing verify checks** in `test_tb_ota()`:
  - `tb_ota.c` is in SRCS.
  - Every `tb_link_publish` whose topic comes from `tb_fw_request_topic` passes `TB_FW_CHUNK_SIZE_STR` as the payload.
  - It uses `psa_hash_setup`, `psa_hash_update`, `psa_hash_finish` and `tb_fw_digest_matches`.
  - It calls `esp_ota_abort` and `ota_lock_release`, and calls `ota_lock_claim` before `xTaskCreate`.
  - `heap_caps_malloc(TB_FW_CHUNK_SIZE` is used with `MALLOC_CAP_SPIRAM`.
  - It makes no `xTaskCreateWithCaps` call and passes no `MALLOC_CAP_SPIRAM` to any stack.
  - `dry_run_guard_get_measure` comes before `nvs_store_set_tb_ota_pending`, which comes before `esp_restart`.
  - `"TB_OTA_REBOOT"` is present.
  - The chunk loop references `TB_FW_CHUNK_TRIES`.
  - It uses no `esp_https_ota`.
- [ ] **Step 2: Run it and see the new checks fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run every gate.** All verify scripts pass, the build has 0 warnings and the QEMU selftest passes.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/main/tb_ota.c firmware/main/tb_ota.h firmware/main/CMakeLists.txt firmware/scripts/verify_tb_link.py
  git commit -m "feat: ThingsBoard OTA download, SHA-256 check, slot write and reboot"
  ```

### Task 5: Boot gating, the 5-minute rollback and the boot report

**Files:**
- Modify: `firmware/main/tb_ota.c`, `firmware/main/tb_ota.h`, `firmware/main/app_main.c`
- Test: `verify_tb_link.py`. Add a new function `test_ota_boot()`.

**Interfaces:**
- Consumes:
  - Task 1 `tb_fw_boot_state`;
  - Task 2 `tb_link_set_session_hook` and `tb_link_set_data_hook`;
  - Task 3 `nvs_store_get_tb_ota_pending` and `nvs_store_clear_tb_ota_pending`.
- Produces:
  ```c
  void tb_ota_boot_init(void);        /* app_main, before web_server_start */
  bool tb_ota_boot_holds_valid(void); /* true while the boot state is VERIFY */
  void tb_ota_on_session(void);       /* tb_link session hook (tb_link task, after the subscriptions) */
  ```

**`tb_ota_boot_init`:**
- Reads the pending version, the running version, and whether the running partition is `ESP_OTA_IMG_PENDING_VERIFY`, then stores the result of `tb_fw_boot_state`.
- On VERIFY, it starts a one-shot `esp_timer` of `TB_FW_VERIFY_TIMEOUT_MS`. When it fires, it logs and calls `esp_ota_mark_app_invalid_rollback_and_reboot()`.
- It logs the boot state.

**`tb_ota_on_session`**, every accepted connect:
1. Resolve the boot state:
   - VERIFY: `esp_ota_mark_app_valid_cancel_rollback()`, stop the timer, report `UPDATED` with the current title and version, then clear the pending version.
   - UPDATED: report `UPDATED`, then clear.
   - ROLLED_BACK: report `FAILED rolled_back`, then clear.
   - In each case the state then becomes NONE, and `uplink_note_settings_changed()` runs.
2. Report the current title and version only (`tb_fw_state_json(NULL, NULL, title, version)`).
3. Publish `TB_FW_ATTR_REQUEST_BODY` on `TB_FW_ATTR_REQ_PREFIX "<n>"`, with `n` a counter.

**`app_main.c`:**
- Call `tb_ota_boot_init()` before `web_server_start()`.
- Wrap both existing `esp_ota_mark_app_valid_cancel_rollback()` calls (after the first start and after the retry start) in `if (!tb_ota_boot_holds_valid())`, and log `"OTA from ThingsBoard: waiting for the ThingsBoard connect to mark valid"` instead.
- Register `tb_link_set_data_hook(tb_ota_on_data)` and `tb_link_set_session_hook(tb_ota_on_session)` next to the M4 connected hook, before `tb_link_init()`.
- Add `esp_log_level_set("tb_ota", ESP_LOG_INFO)`.
- Under `#ifdef TB_LINK_TEST_BLOCK_CONNECT`, log `ESP_LOGW` `"TEST BUILD: ThingsBoard connect is blocked"` at boot.

- [ ] **Step 1: Write the failing verify checks** in `test_ota_boot()`:
  - `app_main.c`:
    - every `esp_ota_mark_app_valid_cancel_rollback` call is inside a block guarded by `tb_ota_boot_holds_valid`;
    - `tb_ota_boot_init` comes before the first `web_server_start`;
    - both hook registrations come before `tb_link_init`;
    - the `tb_ota` tag is raised.
  - `tb_ota.c`:
    - uses `esp_ota_mark_app_invalid_rollback_and_reboot` and `TB_FW_VERIFY_TIMEOUT_MS`;
    - in `tb_ota_on_session`, `esp_ota_mark_app_valid_cancel_rollback` comes before the `UPDATED` report, which comes before `nvs_store_clear_tb_ota_pending`;
    - `nvs_store_clear_tb_ota_pending` is not called from `tb_ota_boot_init`.
- [ ] **Step 2: Run it and see the new checks fail.**
- [ ] **Step 3: Implement it.**
- [ ] **Step 4: Run every gate.** All verify scripts pass, the build has 0 warnings and the QEMU selftest passes.
  - Also build the test image once and confirm it builds with 0 warnings: `idf.py -C firmware -B firmware/build_block -DTB_TEST_BLOCK_CONNECT=ON build`.
  - Then rebuild the normal `firmware/build`, and confirm its compile commands do not contain `TB_LINK_TEST_BLOCK_CONNECT`.
- [ ] **Step 5: Commit**
  ```bash
  git add firmware/main/tb_ota.c firmware/main/tb_ota.h firmware/main/app_main.c firmware/scripts/verify_tb_link.py
  git commit -m "feat: a ThingsBoard OTA image is marked valid only after it reaches ThingsBoard; 5 min rollback"
  ```

### Task 6: ThingsBoard side: the package tool, the Firmware page and the placeholder fix

**Files:**
- Create: `thingsboard/tb_ota.py`
- Modify:
  - `thingsboard/dashboard_pages.json`, `thingsboard/tb_dashboard.py`, `thingsboard/README.md`
  - `docs/firmware/device-api.md`: a new section "ThingsBoard OTA (milestone 5)" next to the remote Wi-Fi section
  - `docs/backlog/after-frontend.md`: the web OTA size limit (call 8) under "จาก milestone 5 ThingsBoard"

**Interfaces:**
- **`thingsboard/tb_ota.py`** uses `tb_api.add_common_args` and `connect_from_args`, so the password comes from env or a prompt. It never uses a device token: one session per device (spike).
  - `upload IMAGE --version V [--title fishpump] [--checksum HEX]` creates the package with `POST /api/otaPackage` (type `FIRMWARE`, device profile `fishpump`), then uploads it multipart to `/api/otaPackage/<id>?checksumAlgorithm=SHA256[&checksum=HEX]`. It prints the id, the size and the checksum ThingsBoard stored.
  - `assign DEVICE VERSION` and `unassign DEVICE` set or clear the device's `firmwareId` (`GET` then `POST /api/device`).
  - `status DEVICE` prints the latest `fw_state`, `fw_error`, `current_fw_title`, `current_fw_version`, `target_fw_title`, `target_fw_version` and `target_fw_ts`, and the shared `fw_*`.
  - `delete VERSION` deletes the `fishpump` package with that version.
  - `--selftest` runs offline. It checks that the multipart body has the boundary framing, the file part named `file` and the exact bytes, and that each subcommand's argument parsing works. It prints `SELFTEST PASS`.
- **Dashboard page `firmware`:**
  - title `เฟิร์มแวร์ (Firmware)`;
  - telemetry `["fw_state","fw_error","current_fw_title","current_fw_version","target_fw_version"]`;
  - attributes `["tb_link"]`;
  - actions `[]`.
  - `tb_dashboard.py` gets a `FW_TELEMETRY_KEYS` set with these five keys, added to the allowed telemetry.
- **Wi-Fi page `wifi.connect` params** become `{"ssid": "", "password": ""}`.
- **New `tb_dashboard.py --check` rule:** a `wifi.connect` action whose params' `ssid` is not `""` gives `wifi.connect default ssid must be empty (an unedited send must be refused)`. The existing `wc_bad` fixture (`ssid "x"`) gains that expected text in `BROKEN_EXPECT`.
- **README:** how to run an OTA test with `tb_ota.py` (upload → assign → status → unassign → delete), and that assignment can also be done on the device page in ThingsBoard.
- **`device-api.md`:**
  - the `fw_state` sequence;
  - the `fw_error` table from Global Constraints;
  - the 5-minute rule;
  - that the package is the plain build `.bin`;
  - the version source (`version.txt`);
  - the test-block build command.

- [ ] **Step 1: Write the failing checks.**
  - The new `tb_dashboard.py` rule plus its `BROKEN_EXPECT` text. `--check` must fail on the current page file (`ssid "แก้ก่อนส่ง"`).
  - The `tb_ota.py --selftest` cases. They fail because the file does not exist yet.
- [ ] **Step 2: Implement `tb_ota.py`, then update the pages, the README and the two docs.**
- [ ] **Step 3: Run the gates and see them pass.**
  - `python thingsboard/tb_ota.py --selftest` prints `SELFTEST PASS`.
  - `python thingsboard/tb_dashboard.py --check` passes.
  - `python thingsboard/parity_test.py --selftest` passes.
  - `python firmware/scripts/verify_tb_link.py` passes.
- [ ] **Step 4: Commit**
  ```bash
  git add thingsboard/tb_ota.py thingsboard/dashboard_pages.json thingsboard/tb_dashboard.py thingsboard/README.md docs/firmware/device-api.md docs/backlog/after-frontend.md
  git commit -m "feat: ThingsBoard OTA package tool, Firmware page, empty wifi.connect default"
  ```

### Task 7: Bench round 6 (the controller runs it with the user; not a subagent task)

**Files:** `docs/bench-test-thingsboard.md` (new section "รอบ 6: อัปเดตเฟิร์มแวร์จาก ThingsBoard (milestone 5)"), `docs/backlog/after-frontend.md`.

How the work is split:
- The user flashes and types every password.
- Claude runs the scripts through tabs it opens.
- Test images go in `thingsboard/.local/ota/fishpump-<ver>.bin`, which git ignores.
- Each test image is built by temporarily setting `firmware/version.txt`. The bench ends with `version.txt` back at `0.3.2`, and the bench file records which version the board runs.

The rounds:
- [ ] **K0 (before flashing):** run `api_golden.py capture --label m5pre --with-writes` with the board still on the M4 firmware (7c3cacc, 0.3.2).
- [ ] **K1:**
  - Flash the M5 build, with `version.txt` still at 0.3.2.
  - Run `api_golden.py compare --base m5pre --label m5post --with-writes`. Expected: ALL PASS.
  - Run `tb_setup.py`, then `parity_test.py`. Expected: 0 FAIL.
  - The Firmware page shows `current_fw_version` 0.3.2, and the board log shows the attribute request on connect.
- [ ] **K2 (success):**
  - Build 0.3.3, then `tb_ota.py upload … --version 0.3.3` and `assign fishpump-1de564 0.3.3`.
  - Expected:
    - `status` walks through `DOWNLOADING` → `DOWNLOADED` → `VERIFIED` → `UPDATING`;
    - the board reboots, logs "waiting for the ThingsBoard connect", then marks valid;
    - the state reaches `UPDATED` with `current_fw_version` 0.3.3;
    - no second download starts after the reboot.
  - Record the download time.
- [ ] **K3 (bad checksum):**
  - `upload` 0.3.4 with `--checksum 000…0` (64 zeros), then `assign`.
  - Expected: `FAILED checksum_mismatch`, no reboot, the board stays on 0.3.3, and no re-download until the next reboot.
  - If ThingsBoard refuses the wrong checksum at upload, upload a random 200 KB file as 0.3.4 instead. Expect `FAILED invalid_image`, and record which variant ran.
- [ ] **K4 (never connects):**
  - Build 0.3.5 in `firmware/build_block` with `-DTB_TEST_BLOCK_CONNECT=ON`, then `upload` and `assign`.
  - Expected:
    - the board reboots into 0.3.5;
    - the card shows `test build: connect blocked`;
    - about 5 minutes after boot the log shows the rollback timer, and the board reboots onto 0.3.3;
    - ThingsBoard shows `FAILED rolled_back`;
    - the web app works throughout.
- [ ] **K5 (dry-run measurement during the update):**
  - Start a measurement, then assign a good 0.3.6.
  - Expected: `VERIFIED` arrives, the reboot waits until the measurement ends, then `UPDATED`.
- [ ] **K6 (unassign):**
  - `unassign`. Expected: no action on the board.
  - Record the attribute message the board receives (closes the spike's open point).
- [ ] **K7 (budget):** `BOARD_DIAG` during K2's download and after it.
  - `heap_min` stays ≥ 90 KB during the download and ≥ 100 KB after it.
  - Every HWM is ≥ 512, including `tb_ota` while it exists.
- [ ] **Clean up:**
  - Set `version.txt` back to 0.3.2.
  - Delete the test packages with `tb_ota.py delete`, after the user's OK.
  - Write the results into the bench file, move failures into the backlog, and commit `docs: bench milestone 5`.
