# ThingsBoard MQTT Link + Remote Operation: Design

Date: 2026-11-18 · Branch base: `feature/alarm-rules` @ `1f2e9f4` · Firmware 0.3.2 · ESP-IDF v6.0.1 · ESP32-S3

## 1. Intent

The fishpump board must be monitored and **operated from ThingsBoard exactly as well as from the web app built into the firmware**. The user will later build a separate frontend app on top of ThingsBoard that has to offer the same functions with a much nicer look. This project builds the plumbing and proves it works. A plain **test dashboard** in ThingsBoard shows each function working before frontend design starts.

Where this sits (the agreed platform order): (1) firmware alarms + transport-free uplink layer, which is done; (2) **this project**: the ThingsBoard foundation and the firmware MQTT connector, built together; (3) the frontend app.

Rules stay in firmware so alarms and protection work offline. ThingsBoard stores history, shows data, and sends commands.

## 2. Decisions the user made

- **Full parity.** Every operational function of the built-in web app can be used from ThingsBoard. This covers pump start/stop/config, cooling mode/config, dry-run protection settings and measurement, alarm ack, alarm rule edit/delete, pH calibration/offset, energy reset, Wi-Fi scan/profiles, and every status/sensor read.
- **Risky functions:**
  - Firmware update (OTA) is allowed through ThingsBoard's own OTA feature.
  - A Wi-Fi change is allowed only through the existing staging/rollback path.
  - **The GPIO hardware map and the board's web password are read-only remotely**; changing them needs the local web app.
- **ThingsBoard host:** the existing ThingsBoard CE **4.3.1** in Docker on the LAN host `copter` (192.168.1.51). HTTP is on 8080 and MQTT is plain on 1883, with compose files in `~/infra/thingsboard`. Checked on 2026-11-18: the login page returns 200, a bogus MQTT token gets CONNACK 5, and the server holds only the 9 ThingsBoard demo devices.
- **Scope on the ThingsBoard side** includes a test dashboard that can drive every function, not just the device connection and API.
- **Approach A** (section 5): a shared command layer used by both the HTTP handlers and MQTT.

## 3. Goals, non-goals, anti-goals

**Goals**
1. The board connects to ThingsBoard over MQTT. It provisions itself the first time and reconnects by itself.
2. The existing uplink messages flow for real: telemetry, alarm events with real timestamps, identity, and rules.
3. Every operational web API function is callable as a ThingsBoard RPC. It returns the same result and the same error codes as the web API, because both go through one implementation.
4. The board publishes its current settings and status, so ThingsBoard always shows the board's real state, including changes made on the local web app.
5. ThingsBoard firmware OTA works with automatic rollback. A remote Wi-Fi change works with automatic rollback.
6. ThingsBoard setup is reproducible from scripts in this repo: device profile, provisioning, rule chain and test dashboard. A parity test script calls every RPC through ThingsBoard's REST API and compares the result with the board's own HTTP API.

**Non-goals**
- The frontend app and its visual design.
- Notifications to chat apps (LINE/Telegram) through ThingsBoard rule chains.
- TLS on the server and exposing ThingsBoard to the internet. The firmware is built TLS-capable (section 6.6), but the bench uses plain 1883 on the LAN.
- A multi-customer account structure beyond one test customer (section 7.1).
- Syncing a ThingsBoard alarm "acknowledge" click back to the board. The dashboard's ack button, which uses the RPC, is the parity path.
- Removing the ThingsBoard demo devices. They are left alone unless the user asks.

**Anti-goals** (a failure even if everything else works)
- Any loss of local function when ThingsBoard or the internet is down. This covers the web app, alarms, LED, dry-run protection and pump schedule.
- A remote command path that skips a check the web app enforces, such as the dry-run lock (409), rule validation, value ranges or the pump-start block.
- Any remote way to change the GPIO map or the web password.
- A remote Wi-Fi change or OTA that leaves the board unreachable without someone visiting it.
- The two paths drifting apart, for example a web fix that the RPC path does not get.

## 4. Constraints (from the code today)

- `firmware/main/web_server.c` is 6,782 lines with about 45 `/api/*` handlers. Each handler does its own auth (`require_auth`), password-default check, same-origin check, body read, JSON parse, validation, internal call and response build.
  - Responses are built with cJSON or the local `json_gen` helper.
  - Error bodies have the shape `{"ok":false,"error":"<code>","message":"..."}` (`send_api_error`); some include pump status.
- **httpd runs handlers on one task, so the handlers are serialized today.** A second caller (the MQTT side) breaks that assumption: static buffers, scan callbacks and caches inside the handlers may not be safe for two callers at once. Section 5 deals with this.
- The uplink plug contract in `firmware/components/uplink/include/uplink.h` applies:
  - `uplink_set_sink` is only for setup/teardown, never from the MQTT event handler or a sink callback.
  - `send` must not block (use `esp_mqtt_client_enqueue`).
  - The old `ctx` is never called after `set_sink` returns.
- `uplink_set_wall_clock()` waits for a Unix-ms clock. Events are held for at most `UPLINK_CLOCK_HOLD_MAX_MS` (120 s), or until the ring is nearly full, waiting for it.
- esp-mqtt is included in IDF 6.0.1 (`components/mqtt`), as are `esp-tls`, `esp_https_ota` and `esp_http_client`.
- PSRAM is caps-alloc only (`CONFIG_SPIRAM_USE_CAPS_ALLOC=y`, no malloc redirect). On the 2026-11-18 bench, internal heap free was 158–172 KB. `CONFIG_LWIP_MAX_SOCKETS=32`. `CONFIG_LWIP_SNTP_MAX_SERVERS=1`.
- App rollback is enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`), with two 3.5 MB OTA slots. Today `app_main.c` marks a new app valid as soon as the HTTP server starts (around lines 1218 and 1362).
- Wi-Fi changes are staged in NVS (`nvs_store_*staging*`). A timer in `app_main.c` rolls back unless `POST /api/confirm` commits; `DELETE /api/confirm` rolls back and reboots.
- The log floor is WARN, and each app tag is raised to INFO explicitly in `app_main.c`. New tags must be added there; this bit the dry-run guard on 2026-11-18.
- Uplink contract `fw_type:"fishpump"`, `contract_version:1`. Adding keys does not bump the version; renaming or removing keys does.

## 5. Approaches (the user picked A)

| | Approach | Trade-off |
|---|---|---|
| **A, chosen** | **Shared command layer `device_api`.** Pull each handler's validate → act → build-result part into a table of named methods with JSON in and JSON+status out. HTTP handlers keep auth/origin/body reading and become thin wrappers. MQTT RPC calls the same methods. | Parity by construction, and the frontend gets the same method names. Cost: a large refactor of `web_server.c` with regression risk, controlled by migrating one API group at a time with the verify scripts and a golden-response check. |
| B | Forward each RPC as an HTTP request to the board's own web server. | Least code, but it needs a hole through login/Origin/CSRF (a security weak point), competes for httpd sockets and ties MQTT to HTTP. |
| C | Write separate MQTT handlers. | No change to existing code, but two copies of every function drift apart, which works against the main goal. Dropped. |

## 6. Firmware design

### 6.1 `device_api` component (new)

```c
typedef enum {
  DAPI_OK = 0,            /* HTTP 200 */
  DAPI_BAD_REQUEST,       /* 400 */
  DAPI_CONFLICT,          /* 409 (e.g. pump locked, status unavailable) */
  DAPI_UNAVAILABLE,       /* 503 (engine/sensor not ready) */
  DAPI_LOCAL_ONLY,        /* remote caller asked for a local-only method; 403 */
  DAPI_INTERNAL           /* 500 */
} dapi_status_t;

typedef enum { DAPI_SRC_HTTP = 0, DAPI_SRC_TB } dapi_source_t;

/* params may be NULL. *out is the full response object ({"ok":...}) the web API sends today; caller deletes. */
dapi_status_t device_api_call(const char *method, const cJSON *params, dapi_source_t src, cJSON **out);
```

- **Each method's response is the web API's current body** (`ok`, `error`, `message`, and extra fields such as pump status), so the web app's JavaScript needs no change. The HTTP wrapper maps `dapi_status_t` to the HTTP status line. The RPC path returns the same object as the RPC response.
- **One mutex serializes every `device_api_call`.** That keeps today's one-caller-at-a-time assumption. Calls are short; the long-running dry-run measurement already runs in its own task and returns immediately.
- **Method flags:** `READ`, `WRITE`, `LOCAL_ONLY` (refused with `DAPI_LOCAL_ONLY` when `src == DAPI_SRC_TB`), and `STAGED` (Wi-Fi; see 6.4).
- **What stays in the HTTP layer:** auth, session, password-default (403 `force_password_change`), same-origin, body read, static files, SSE, login/logout/nonce/credentials/change-password, multipart OTA upload, and `/api/confirm`. The password-default rule does **not** apply to `DAPI_SRC_TB`. A board still on the default web password can be operated from ThingsBoard, because the MQTT token is the remote credential.
- **Method names** (builder may adjust spelling; the frontend will use them, so the final table goes in the firmware reference doc):

| Method | Today's endpoint | Flags |
|---|---|---|
| `status.get` | GET /api/status | READ |
| `sensors.get` | GET /api/sensors/status | READ |
| `pump.status` / `pump.config.get` / `pump.config.set` | /api/pump/status, /api/pump/config | READ / READ / WRITE |
| `pump.control` (`{system,action}`), `pump.start`, `pump.stop` | /api/pump/control, /start, /stop | WRITE |
| `cooling.status` / `cooling.config.get` / `cooling.config.set` / `cooling.mode.set` | /api/cooling/* | READ / READ / WRITE / WRITE |
| `protection.get` / `protection.set` | /api/protection | READ / WRITE |
| `protection.measure.get` / `protection.measure.set` (start/stop/save as today) | /api/protection/measure | READ / WRITE |
| `alarms.status` / `alarms.summary` / `alarms.values` / `alarms.signals` | /api/alarms/* | READ |
| `alarms.rules.get` / `alarms.rules.set` / `alarms.rules.delete` / `alarms.ack` | /api/alarms/rules*, /ack | READ / WRITE |
| `water.calibrate` / `water.calibrate.cancel` / `water.offset` | /api/water/* | WRITE |
| `power.reset_energy` | /api/power/reset-energy | WRITE |
| `wifi.scan` / `wifi.profiles.get` / `wifi.profiles.save` / `wifi.profiles.setauto` / `wifi.profiles.forget` / `wifi.connect` / `wifi.disconnect` | /api/wifi/* | READ / READ / WRITE / WRITE / WRITE / STAGED / WRITE |
| `hardware.map.get` | GET /api/hardware/map | READ |
| `hardware.map.set` | POST /api/hardware/map | LOCAL_ONLY |
| `uplink.preview` | GET /api/uplink/preview | READ |

- **Remote guard rails:**
  - `wifi.disconnect` from ThingsBoard is refused (`DAPI_LOCAL_ONLY`), because it would cut the link it arrived on.
  - `wifi.profiles.forget` from ThingsBoard is refused for the profile currently connected.
- `wifi.scan` is asynchronous today (callback). The method returns the cached result plus a `scanning` flag, so callers poll it. The builder must keep the HTTP behaviour identical.

### 6.2 `tb_link` component (new): the MQTT transport

- **Config in NVS:** host, port, `tls` flag, optional CA PEM, device name, access token, provision key and provision secret.
  - It is edited on a new **"ThingsBoard" card in the local web app**. That card is local-only, behind login and same-origin.
  - The card shows the link state, last error, connected time and messages sent/failed.
  - It has a "ลงทะเบียนใหม่" (re-provision) button that clears the token.
  - Defaults: host empty (link off), port 1883, device name `fishpump-<last 3 MAC bytes hex>`.
- **Provisioning (first connect, no token):**
  1. Connect with username `provision`.
  2. Publish `/provision/request` `{"deviceName","provisionDeviceKey","provisionDeviceSecret"}`.
  3. Read `/provision/response`. On `"status":"SUCCESS"`, store `credentialsValue` as the token and reconnect with it.
  4. On failure, back off and show the error on the card.
  5. A token typed in by hand on the card works too, which skips provisioning.
  6. Provisioning is one-time. Per the ThingsBoard docs (checked 2026-11-18), "re-sending a provisioning request after that will be rejected with a failure response", and a name already registered fails. So "ลงทะเบียนใหม่" only works after the device is deleted in ThingsBoard. The card says so and shows the `errorMsg`. Otherwise the fix is to type in the device's token from ThingsBoard by hand.
- **Connected session:**
  - Subscribe to `v1/devices/me/rpc/request/+`, `v1/devices/me/attributes` (shared attribute updates; used only for OTA in this project), `v1/devices/me/attributes/response/+` and `v2/fw/response/+/chunk/+`.
  - Then call `uplink_set_sink()` (once at setup, per the contract) and request the `fw_*` shared attributes so a pending OTA is noticed after a reboot.
- **Sink:**
  - `is_connected` reads an atomic flag that the event handler sets.
  - `send` uses `esp_mqtt_client_enqueue`: QoS 1 for attributes and alarm events, QoS 0 for periodic telemetry. The builder may choose QoS 1 for everything if the outbox stays bounded.
  - The outbox size is capped so a dead link cannot eat the heap. Store-and-forward of events is the uplink ring's job, not the outbox's.
- **RPC worker:**
  - The MQTT event handler only copies `(request id, payload)` into a FreeRTOS queue (depth about 4; payloads up to the alarm-rules body size, in PSRAM).
  - A `tb_rpc` task parses `{"method","params"}`, calls `device_api_call(..., DAPI_SRC_TB, ...)`, and enqueues the response on `v1/devices/me/rpc/response/<id>`.
  - Unknown method → `{"ok":false,"error":"unknown_method"}`. Queue full → `{"ok":false,"error":"busy"}` sent from the handler.
  - Every remote WRITE is logged at INFO with the source `tb`.
- **Reconnect:** esp-mqtt's own reconnect, with a backoff of 5 s, doubling to a 60 s cap. A link failure never blocks or restarts anything else.
- **Log tags** `tb_link` and `device_api` are raised to INFO in `app_main.c`.

### 6.3 What the board publishes

- **Existing uplink messages, unchanged:** periodic telemetry, alarm events, `identity` and `alarm_rules`.
- **New client attributes "current settings and state":**
  - `cfg_pump`, `cfg_cooling` and `cfg_protection`: the same objects the `*.config.get` / `protection.get` methods return.
  - `wifi`: SSID, RSSI, IP and auto flag. Never passwords.
  - `hardware_map`: read-only.
  - `tb_link`: device name and firmware version.
  - When they are sent: on connect, and within 2 s of any change from either path. The uplink layer gains a "settings changed" trigger that `device_api` WRITE methods raise; local-only writes such as the GPIO map raise it too.
  - The HTTP-only paths that change state raise it as well: `/api/confirm` commit/rollback, credentials, change-password (only a "changed" note, never the password) and OTA.
  - Effect: when someone edits a setting on the local web app, ThingsBoard shows it.
- **New status telemetry:**
  - What it covers: everything the web app's Status, Pump, Cooling and Protection cards show that is not already a registry signal. Examples: pump phase and timers, protection state and retry count, cooling mode and output, Wi-Fi RSSI, heap free, stack HWM.
  - Size: a flat JSON object of at most `UPLINK_TELEMETRY_BUF`, or a second message if it does not fit.
  - When it is sent: every 30 s, and immediately when a value changes, at most once every 2 s, like today's telemetry.
  - Key names are snake_case and go in the firmware reference doc. Adding them does not bump `contract_version`.
- **Why writes are RPC and not shared attributes (a change from what I first told the user):** a shared "desired" attribute stays on the server. After a local web edit and a reconnect, the board would fetch the old shared value and overwrite the local edit. With RPC for all writes, plus the board reporting its real state as client attributes, the newest change wins whichever path made it, and ThingsBoard never shows a stale value. Commands only work while the board is online, which matches how the web app works.

### 6.4 Remote Wi-Fi change

- `wifi.connect` from ThingsBoard uses the existing staging path, exactly as the web app does.
- Commit rule: if the board reconnects to ThingsBoard over MQTT (CONNACK accepted) on the new network before the staging timer ends, `tb_link` commits the staging itself, with the same effect as `POST /api/confirm`, type 1 only. Otherwise the existing timer rolls back and reboots onto the old network.
- The RPC answers `{"ok":true,"staged":true}` before the switch, and the dashboard explains that the board will drop off and return.
- **Reply timing:** the switch starts only after the reply's PUBACK arrives, or after 2 s, whichever comes first. The reply is therefore normally delivered, and the switch never waits forever.
- **Timing during staging:**
  - The staging timer stays at the existing 180 s (`APP_CONFIG_ROLLBACK_CONFIRM_TIMEOUT_MS`).
  - While staging is active, the MQTT reconnect delay is a fixed 5 s instead of the backoff.
- **Which networks:** the commit needs the new network to reach the configured ThingsBoard host. A network that cannot reach it, such as a phone hotspot while ThingsBoard is LAN-only, **always rolls back**. That is intended: a change that cuts the board off from ThingsBoard is exactly what rollback protects against. The dashboard's warning text says so.
- **Passwords:**
  - `wifi.connect` and `wifi.profiles.save` carry a Wi-Fi password in their params. They are sent as plain (non-persistent) two-way RPC, so ThingsBoard does not keep them in its persistent-RPC table.
  - The board logs only the method name for them, never the params.
  - On the LAN bench the password crosses plain MQTT 1883. This is accepted for the bench and closed by TLS later.

### 6.5 ThingsBoard firmware OTA

- Use ThingsBoard's device OTA over MQTT:
  - shared attributes `fw_title`, `fw_version`, `fw_size`, `fw_checksum` and `fw_checksum_algorithm` (SHA256);
  - chunks requested with `v2/fw/request/<req>/chunk/<n>` (chunk size in the payload; builder picks, for example 4 KB) and received on `v2/fw/response/<req>/chunk/<n>`;
  - progress reported as telemetry `current_fw_title`, `current_fw_version`, `fw_state` (`DOWNLOADING`, `DOWNLOADED`, `VERIFIED`, `UPDATING`, `UPDATED`, `FAILED`) and `fw_error`.
- **Source check:**
  - The ThingsBoard OTA guide (checked 2026-11-18) confirms the states QUEUED → INITIATED, which are set by the server, then DOWNLOADING … UPDATED/FAILED, which are reported by the device.
  - The public MQTT reference pages do not document the chunk topics. The `v2/fw/...` topics and `fw_*` attribute names above come from ThingsBoard's device SDKs.
  - So the **first implementation task is a spike against copter**. Upload a small package, assign it, and record the exact attributes and topics the server sends, before the OTA code is written.
- The flash write reuses the existing `esp_ota_*` path (shared with the web upload).
- If `fw_title` is not `fishpump`, or the version equals the running one, nothing happens.
- A wrong size or checksum gives `FAILED`, and nothing is booted.
- **Rollback for a remotely delivered image:**
  1. Before rebooting, set an NVS flag "came from ThingsBoard".
  2. On the new image, with the flag set, do **not** mark the app valid when the HTTP server starts, as happens today. Mark it valid only after the first accepted MQTT connect.
  3. If that does not happen within 5 minutes, call `esp_ota_mark_app_invalid_rollback_and_reboot()`.
  4. The web-upload path keeps today's behaviour.
  5. After the valid boot, report `UPDATED`; after a rollback, report `FAILED` with `fw_error:"rolled_back"`.
- The reboot waits while a dry-run measurement is running, then goes ahead. Relays come up safe, as on any reboot.

### 6.6 SNTP and TLS readiness

- **SNTP:**
  - Use `esp_netif_sntp` with `pool.ntp.org` and `time.google.com` (raise `CONFIG_LWIP_SNTP_MAX_SERVERS` to 2). It starts when Wi-Fi gets an IP.
  - On the first sync, call `uplink_set_wall_clock()` with a function that returns Unix ms. The 120 s hold stays as it is; review it on the bench (backlog item).
- **TLS:**
  - When the `tls` flag is set, `tb_link` connects `mqtts://`, using the CA PEM from NVS, or the IDF certificate bundle when no PEM is set.
  - Bench test: none against copter yet, which is plain only. It must build, and it must fail cleanly with a clear card error when pointed at a plain port.
  - Memory: mbedTLS should allocate from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`) if internal heap gets tight. The builder decides from measured heap.

### 6.7 Resource budget

- New tasks: `tb_rpc` (about 6 KB stack, priority 2) and the esp-mqtt task (about 6 KB).
- Buffers: RPC queue and OTA chunk buffer in PSRAM.
- Minimums on the bench, with the link connected and an RPC burst:
  - internal heap free must stay ≥ 100 KB;
  - every task's HWM must stay ≥ 512 B (`BOARD_DIAG` already prints it; add the two new tasks).

## 7. ThingsBoard side (in repo: `thingsboard/`)

All of this is Python scripts using ThingsBoard's REST API. They are idempotent: safe to run again, updating by name instead of creating duplicates.

- **Credentials:** the tenant-admin login is read from environment variables or typed at a prompt by the user. Never stored in the repo.
- **Generated secrets:** the provision key/secret go to an untracked file `thingsboard/.local/` (already ignored, or add it to `.gitignore`).

### 7.1 Setup script

1. **Device profile `fishpump`** (one per `fw_type`, which is the start of the "preset per firmware" idea):
   - transport MQTT with the default topics;
   - provisioning "allow to create new devices" with a generated key/secret;
   - default rule chain `fishpump`;
   - OTA enabled.
2. **Rule chain `fishpump`:**
   - the default save-telemetry and save-attributes path;
   - `alarm_event` handling: `raised` creates a ThingsBoard alarm (type `rule_<id>`, severity mapped warning→WARNING, critical→CRITICAL, info→INDETERMINATE, details = the event); `cleared` clears it.
   - The type is unique per rule, so a rule's raise/clear pair opens and closes one alarm.
   - The node reads the event exactly as `uplink_format_event` builds it today (contract v1, plain or `{"ts","values"}` form; do not change it). It must tolerate duplicates from QoS 1 resend and offline backfill: it keeps the last handled `(boot_id, seq)` per device as a server attribute and ignores events at or below it within the same `boot_id`. ThingsBoard's own create-alarm is also idempotent for an alarm that is already open.
3. **One test customer**, `ลูกค้าทดสอบ`. The device is assigned to it once provisioned, so customer-scoped access is exercised from day one.
4. **Dashboard `Fishpump — ทดสอบ`**, imported from `thingsboard/dashboards/fishpump_test.json`.

### 7.2 Test dashboard (plain, function over form)

- One state per web page: Status, Sensors, Pump, Cooling, Protection, Alarms, Wi-Fi, Hardware (read-only), Firmware.
- **Each page shows:**
  - the live telemetry and client attributes for that area;
  - an action for every method in section 6.1. Use RPC buttons for actions, and JSON or form inputs that send the `*.set` RPCs for settings (TB control widgets or a small custom HTML widget; builder's choice);
  - the RPC response (`ok`/`error`) visibly after each action.
- Actions that the web app confirms show the same warning text before sending:
  - measure, pump start, ack of the dry-run alarm, Wi-Fi connect, energy reset, rule delete.
- The Firmware state uses ThingsBoard's built-in OTA package assignment and shows `fw_state`.
- The dashboard's look is not judged. What is judged: every function can be reached and its result is visible.

### 7.3 Parity test script

`thingsboard/parity_test.py`:

1. Log in to ThingsBoard and to the board's web API. The user types both passwords; the PC must be able to reach both, so the board must be on the home LAN.
2. For each READ method: call it through ThingsBoard (`POST /api/plugins/rpc/twoway/{deviceId}`) and through HTTP, then compare after removing fields that change over time (`uptime`, ages and the like). The builder lists them in the script.
3. For each WRITE method, test a reversible change:
   - read the current value;
   - write a changed one through ThingsBoard;
   - read it back through HTTP;
   - restore through HTTP;
   - check that the ThingsBoard client attribute followed both changes.
4. Check the error paths:
   - a bad value gives the same `error` code both ways;
   - `hardware.map.set` and `wifi.disconnect` from ThingsBoard give `local_only`;
   - an unknown method gives `unknown_method`.
5. Write a pass/fail table. The script never runs the pump-start, Wi-Fi or OTA tests by itself; those are guided bench steps.

## 8. Testing

- **Before each `web_server.c` group migration:**
  - Record golden responses: HTTP status + body for each endpoint, with volatile fields removed, from the board or from a static harness.
  - The migration must reproduce them.
  - Existing gates stay green: `verify_alarm_system.py`, `verify_dry_run.py`, `verify_sensor*.py`, `verify_pre_mqtt.py`, a 0-warning build, and the QEMU selftest.
- **New pure units with selftests:**
  - the method table: lookup, flags, `LOCAL_ONLY` refusal for TB;
  - RPC envelope parse and response build;
  - the provisioning response parse;
  - OTA attribute parse and the decision "start / ignore / fail";
  - the remote-forget and remote-disconnect guards.
- **New verify script `verify_tb_link.py`** (static checks, like the existing ones):
  - `uplink_set_sink` is never called from the event handler;
  - `send` uses `esp_mqtt_client_enqueue`;
  - the new tags are raised to INFO;
  - `LOCAL_ONLY` is set on `hardware.map.set`;
  - mark-valid is gated for ThingsBoard OTA.
- **Bench (new `docs/bench-test-thingsboard.md`):**
  - provisioning from empty;
  - telemetry and events with correct `ts`, after SNTP and after a 2-minute offline gap;
  - the parity script;
  - the dashboard walk-through of every page;
  - remote Wi-Fi change, with success and with a wrong password that rolls back;
  - OTA success, OTA with a bad checksum, and an OTA image that never connects (rolls back after 5 min);
  - ThingsBoard down: run `docker stop tb-node` on copter **only with the user's OK**. Local functions must be unaffected, and the board must reconnect and backfill events afterwards;
  - heap and HWM over a 1-hour soak with the dashboard open.

## 8a. Delivery in milestones

Each milestone is flashed and benched before the next one starts. The user does the flashing and logins; Claude drives the browser and scripts.

1. **Link**
   - Firmware: `tb_link` with provisioning, the uplink sink, SNTP and the ThingsBoard card.
   - Server: the setup script with the profile and rule chain.
   - Bench: telemetry, events and alarms showing up in ThingsBoard.
2. **`device_api` + RPC, one API group at a time.** The order is in section 9. Each group ships with its dashboard page and its part of the parity script. The web app must stay byte-identical after each group.
3. **Settings and state attributes + status telemetry.**
4. **Remote Wi-Fi.**
5. **OTA**, after the spike in 6.5.
   - Version: from `firmware/version.txt`, bumped for each OTA test image.
   - Failure test: the "never connects" image is a normal build whose ThingsBoard host is changed on the card before the update is triggered. It is not a special firmware flag. Alternatively, use a build with the compile option `TB_LINK_TEST_BLOCK_CONNECT` that is left out of release builds; the builder picks one.
6. **Soak and the bench file.**

## 9. Left to the builder

- The exact JSON key names of the new status telemetry, and any method-name spelling changes. Record them in the firmware reference doc.
- Which ThingsBoard widgets to use on the dashboard.
- QoS per message kind within the bounds above; outbox and queue sizes; chunk size.
- The order of migrating API groups. Suggested: alarms (already structured with `api_begin_json_post`), then protection, pump, cooling, water/power, Wi-Fi, status/sensors.
- Whether `json_gen`-built responses are converted to cJSON or wrapped. The output bytes must match the golden responses either way.

## 10. Calls I made (the user can overturn any)

1. **All remote writes go through RPC, not shared attributes** (reason in 6.3). This changes what I said in chat earlier.
2. **A ThingsBoard command does not need the web password, and still works while the web password is the default.** The MQTT token is the remote credential, and ThingsBoard's own users and customer access decide who can send commands.
3. **Remote `wifi.disconnect` and forgetting the Wi-Fi profile in use are refused.**
4. **The Wi-Fi staging commit happens automatically** when MQTT reconnects on the new network.
5. **An OTA image from ThingsBoard stays unconfirmed until it reaches ThingsBoard.** If it doesn't within 5 minutes, it rolls back.
6. **One test customer is created.** The ThingsBoard demo devices are left alone.
7. **Device name default `fishpump-<MAC>`; provisioning first, with a manual token as the fallback.** Re-provisioning needs the device deleted in ThingsBoard first, because ThingsBoard provisions a name only once.
8. **A remote Wi-Fi change only commits if the new network can reach ThingsBoard.** Otherwise it always rolls back.
9. **Wi-Fi passwords may go through RPC.** They are not persisted in ThingsBoard and not logged on the board, and on the LAN they travel over plain MQTT until TLS.
10. **The ThingsBoard rule chain de-duplicates alarm events by `(boot_id, seq)`.** The event format stays at contract v1.
11. **Delivery in milestones (8a), each benched.** This avoids doing the whole refactor before any proof.
