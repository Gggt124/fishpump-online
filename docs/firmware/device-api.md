# device_api reference

This file lists every `device_api` method: its HTTP endpoint, flags, params, reply keys and error codes. It is written from the code. When the code and this file disagree, the code wins.

Sources: `firmware/components/device_api/` (core), `firmware/main/dapi_*.c` (methods), `firmware/main/web_server.c` (HTTP routes), `firmware/components/tb_link/` (ThingsBoard RPC), `firmware/components/dapi_guards/` (forget guard).

## 1. What device_api is

`device_api` is one core that runs every device command. Two callers use it:

- The local web app, over HTTP. The handler in `web_server.c` keeps its auth and body checks, then calls `http_dapi(req, "<method>", params)`.
- ThingsBoard, over MQTT RPC. The `tb_rpc` task in `tb_link` calls `device_api_call(method, params, DAPI_SRC_TB, &out)`.

Both callers get the same method code and the same reply object. A method returns a status (`dapi_status_t`) and a JSON object. The object is the body the web API always sent (`ok`, `error`, `message`, plus extra keys).

The web app does not change. HTTP endpoints, status lines and body bytes are the web app's contract.

### 1.1 Calling a method from ThingsBoard

Use two-way RPC to the device. The RPC method name is the device_api method name. The RPC params are the method params, as a JSON object.

```
request   topic v1/devices/me/rpc/request/<id>
          body  {"method":"pump.config.set","params":{ ... }}
response  topic v1/devices/me/rpc/response/<id>
          body  the method's reply object, for example {"ok":true, ...}
```

Rules:

- The board subscribes to `v1/devices/me/rpc/request/+` at QoS 1.
- `method` is required and must be shorter than 48 bytes (`TB_RPC_METHOD_MAX`). A missing or non-string `method` gets `unknown_method`.
- `params` may be left out for methods that take none.
- The reply body does not carry the HTTP status. Read `ok` and `error` in the body.
- A request longer than 1792 bytes (`TB_RPC_MAX_REQUEST`), or one that arrives in several MQTT fragments, gets `{"ok":false,"error":"too_large"}`.
- The receive side only copies the request into a queue of depth 4. When the queue is full the reply is `{"ok":false,"error":"busy"}`.
- The `tb_rpc` task (stack 6144 B) runs one request at a time and logs `rpc <method> id=<id> -> <status>` under the `tb_link` tag. Params are never logged.
- Duplicate requests are answered from a cache. The task keeps the last 4 handled requests. The key is the request id, the payload length and a hash (FNV-1a) of the payload bytes, so a restarted id counter with a different payload is not mistaken for a repeat. A request with the same key within 30 s (`TB_RPC_DEDUP_WINDOW_MS`, longer than the widget's 10 s timeout) is not run again: the board publishes the stored reply bytes again and logs `rpc <method> id=<id> -> duplicate, cached reply`. This covers every method, and a reply that was lost while the link was down. A fifth request evicts the oldest entry. If there is no memory for the stored reply, that request is simply not cached.

### 1.2 Status to HTTP line

| `dapi_status_t` | HTTP status line |
|---|---|
| `DAPI_OK` | 200 OK |
| `DAPI_ACCEPTED` | 202 Accepted |
| `DAPI_BAD_REQUEST` | 400 Bad Request |
| `DAPI_LOCAL_ONLY` | 403 Forbidden |
| `DAPI_NOT_FOUND` | 404 Not Found |
| `DAPI_CONFLICT` | 409 Conflict |
| `DAPI_UNAVAILABLE` | 503 Service Unavailable |
| `DAPI_INTERNAL` | 500 Internal Server Error |

Some Wi-Fi replies are `ok:false` with status `DAPI_OK` (HTTP 200). They are marked in section 7. Always check `ok`.

### 1.3 Fixed bodies

Built by `device_api` and `tb_link`, not by the methods:

| Case | Status | Body |
|---|---|---|
| Unknown method | `DAPI_NOT_FOUND` | `{"ok":false,"error":"unknown_method"}` |
| Local-only method called from ThingsBoard | `DAPI_LOCAL_ONLY` | `{"ok":false,"error":"local_only","message":"Use the local web app"}` |
| Lock not taken in 5000 ms | `DAPI_UNAVAILABLE` | `{"ok":false,"error":"busy","message":"Device busy, try again"}` |
| ThingsBoard params not a JSON object | `DAPI_BAD_REQUEST` | `{"ok":false,"error":"invalid_json","message":"Expected a JSON object"}` |
| ThingsBoard params at or over the method's max params bytes | `DAPI_BAD_REQUEST` | `{"ok":false,"error":"invalid_body","message":"Request body is missing or too large"}` |
| Method returned no object, or allocation failed | `DAPI_INTERNAL` | `{"ok":false,"error":"memory","message":"JSON allocation failed"}` |
| RPC payload is not valid JSON | (RPC only) | `{"ok":false,"error":"invalid_json","message":"Expected a JSON object"}` |
| RPC request over the size cap | (RPC only) | `{"ok":false,"error":"too_large"}` |
| RPC queue full | (RPC only) | `{"ok":false,"error":"busy"}` |

Note the two `busy` bodies. The device_api one has a `message` and means the 5 s lock timed out. The RPC one has no `message` and means the queue was full.

### 1.4 Flags

| Flag | Meaning |
|---|---|
| `READ` | Reads state. |
| `WRITE` | Changes state. `device_api` logs `write <method> src=<http\|tb>` at INFO. Params are never logged. |
| `LOCAL_ONLY` | Refused with `local_only` when the source is ThingsBoard. HTTP is allowed. |
| `NO_LOCK` | Runs without the call lock. Only `wifi.scan` uses it, because it waits seconds for the radio. |
| `STAGED` | Reserved for milestone 4. No method uses it yet. |

### 1.5 Max params bytes

Each method has `max_params_bytes`. It is the size of the HTTP body buffer. For ThingsBoard calls, `device_api` prints the params compactly and refuses them when the length is `>=` this value (`invalid_body`). The check applies only to `DAPI_SRC_TB`. A value of 0 means no check (the method takes no params, or ignores them). HTTP has its own body read in the handler.

The limits come from the macros in `firmware/main/dapi_*.h`.

### 1.6 Lock

`device_api_call` takes one mutex around every method except `NO_LOCK` ones. It waits up to 5000 ms (`DAPI_LOCK_TIMEOUT_MS`). If it cannot get the lock, the caller gets `busy` (503). Methods are short. The dry-run measurement runs in its own task and returns at once.

### 1.7 Differences between HTTP and ThingsBoard

- The HTTP layer keeps login, session, `force_password_change`, same-origin, body read and the other non-migrated routes (`/api/tb*`, login/logout/nonce/credentials/change-password, `/api/confirm`, `/api/ota`, `/api/events`). None of them is a device_api method.
- `force_password_change` does not apply to ThingsBoard. The MQTT token is the remote credential.
- Every value check a handler did after reading the body is inside the method. The remote path does not skip any of them.
- The forget guard (`in_use`) applies to ThingsBoard only. See section 7.1.

### 1.8 How to read the tables

- "Params": key, type, range or rule as the code checks it. "req" means the method returns `missing_field` when it is absent.
- Error codes are the `error` value. The message text is in the code. For 400s the `message` is often the field name.
- Generic errors from section 1.3 can occur on every method and are not repeated.

## 2. Alarms (8 methods)

Source: `firmware/main/dapi_alarms.c`. Body limit macro: `ALARM_POST_BODY_BYTES` = 768.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `alarms.signals` | GET /api/alarms/signals | READ | 0 |
| `alarms.values` | GET /api/alarms/values | READ | 0 |
| `alarms.status` | GET /api/alarms/status | READ | 0 |
| `alarms.summary` | GET /api/alarms/summary | READ | 0 |
| `alarms.rules.get` | GET /api/alarms/rules | READ | 0 |
| `alarms.rules.set` | POST /api/alarms/rules | WRITE | 768 |
| `alarms.rules.delete` | POST /api/alarms/rules/delete | WRITE | 768 |
| `alarms.ack` | POST /api/alarms/ack | WRITE | 768 |

Replies and errors:

| Method | Reply keys on success | Errors |
|---|---|---|
| `alarms.signals` | `ok`, `signals[]` (`key`, `label`, `unit`, `kind` bool/number, `group`, `period_sec`, `decimals`) | 500 |
| `alarms.values` | `ok`, `values{<signal key>:{v, trusted}}`. `v` is null when the signal has no value. | 500 |
| `alarms.status` | `ok`, `boot_id`, `last_seq`, `uptime_ms`, `engine_stalled`, `summary{active, critical, unacked, critical_unacked, warning_unacked, info_unacked, max_severity}`, `active[]` (`rule_id`, `name`, `signal`, `severity`, `state` active/latched, `acked`, `value`, `since_ms`, and `dry_run{state, trips, max_retries, wait_remaining_sec}` on the dry-run alarm only), `history[]` newest first (`seq`, `type`, `rule_id`, `name`, `signal`, `severity`, `value`, `uptime_ms`, `reason` condition/acked/rule_changed/null) | 500 |
| `alarms.summary` | `ok`, `boot_id`, `last_seq`, `last_critical_seq`, `engine_stalled`, `summary{...}` as above | 500 |
| `alarms.rules.get` | `ok`, `rules[]`, `max`, `storage` (saved/defaults/invalid) | 503 `unavailable` (engine not running) |
| `alarms.rules.set` | `ok`, `rule{...}` as stored | 400 `missing_field`, `invalid_type`, `value_out_of_range` (message = field); 404 `not_found` (id); 409 `rules_full`; 503 `out_of_memory`, `unavailable`; 500 `storage_failed` |
| `alarms.rules.delete` | `ok` | 400 `missing_field`, `invalid_type`, `value_out_of_range` (message `id`); 404 `not_found`; 500 `storage_failed` |
| `alarms.ack` | `ok`, `acked` (count) | 400 as delete; 404 `not_found`; 503 `out_of_memory`, `unavailable`; 500 `internal_error` |

Params:

- `alarms.rules.set`: a rule object. No `id` creates a rule. An `id` replaces that rule.
  - `id` whole number 1 to 65535, and below `ALARM_SYSTEM_ID_BASE`.
  - `name` string, shorter than `ALARM_NAME_BYTES`. `signal` string, a key from `alarms.signals`. `condition` one of `above`, `below`, `outside`, `inside`, `is_true`, `is_false`, `stale`. `severity` one of `info`, `warning`, `critical`. `enabled` bool (default true). `latch` bool.
  - By condition: `threshold` (above, below); `lo` and `hi` (outside, inside); `stale_sec` 0..65535 (stale).
  - Optional: `hysteresis` number, `delay_sec` 0..65535, `gate` null or `{signal:<key>, when:<bool>}`.
  - Full rules: `alarm_rule_from_json` in `firmware/components/alarm_rules/alarm_json.c`, and the engine's own check in `alarm_engine_upsert_rule`.
- `alarms.rules.delete`: `id` (req) whole number 1..65535.
- `alarms.ack`: `{"all":true}` acknowledges everything. Otherwise `id` (req) whole number 1..65535.

## 3. Protection / dry run (4 methods)

Source: `firmware/main/dapi_protection.c`. Macros: `PROTECTION_POST_BODY_BYTES` = 384, `PROTECTION_MEASURE_BODY_BYTES` = 128.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `protection.get` | GET /api/protection | READ | 0 |
| `protection.set` | POST /api/protection | WRITE | 384 |
| `protection.measure.get` | GET /api/protection/measure | READ | 0 |
| `protection.measure.set` | POST /api/protection/measure | WRITE | 128 |

| Method | Params | Reply keys on success | Errors |
|---|---|---|---|
| `protection.get` | none | `ok`, `pump{enabled, prime_sec, dry_trip_sec, retry_wait_min, retry_backoff, max_retries}`, `pump_status{runtime fields}` (from `dry_run_add_runtime_fields`) | 503 `unavailable` (guard not running) |
| `protection.set` | `pump` (req, object) with `enabled` bool, `prime_sec` 0..65535, `dry_trip_sec` 0..65535, `retry_wait_min` 0..65535, `retry_backoff` bool, `max_retries` 0..255. All six are required. Numbers must be whole. | Same as `protection.get` (the new state) | 400 `missing_field`, `invalid_type`, `value_out_of_range` (message = field; the guard may also reject a combination, see `dry_run_guard_set_config`); 503 `unavailable`; 500 `storage_failed` |
| `protection.measure.get` | none | `ok`, `state` (idle/running/done/no_flow/stopped), `elapsed_ms`, `suggested_prime_sec` | 503 `unavailable` |
| `protection.measure.set` | `action` (req) `"start"` or `"stop"` | Same as `protection.measure.get` | 400 `invalid_action`; 409 `flow_already_on`, `measure_unavailable` (start while the water pump runs); 503 `flow_unavailable`, `unavailable` |

## 4. Pump (6 methods)

Source: `firmware/main/dapi_pump.c`. Macros: `PUMP_CONFIG_BODY_BYTES` = 512, `PUMP_CONTROL_BODY_BYTES` = 256.

"Water" is the water pump (relay 1). "Wash" is the wash timer.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `pump.status` | GET /api/pump/status | READ | 0 |
| `pump.config.get` | GET /api/pump/config | READ | 0 |
| `pump.config.set` | POST /api/pump/config | WRITE | 512 |
| `pump.control` | POST /api/pump/control | WRITE | 256 |
| `pump.start` | POST /api/pump/start | WRITE | 0 |
| `pump.stop` | POST /api/pump/stop | WRITE | 0 |

| Method | Reply keys on success | Errors |
|---|---|---|
| `pump.status` | `ok`, `system_water{running, relay_energized, float_state, active_set, phase, countdown_sec, total_duration_sec, cooldown_remaining_sec, auto_start}`, `system_wash{...}`, flat legacy keys (`running`, `initialized`, `fault`, `phase`, `active_relay`, GPIO keys, `settings_status`, ...), `dry_run{...}`. Built in `api_pump_add_status_fields_gen`. | 409 `status_unavailable` |
| `pump.config.get` | `ok`, config keys (timers, `auto_start`, `min_dwell_sec`, polarity, read-only GPIO keys, `hardware_map_status`, `settings_status`). Built in `api_pump_add_config_fields`. | 500 `settings_load_failed` |
| `pump.config.set` | Same as `pump.config.get`, plus `status{...}` | 400 (below); 500 `settings_save_failed`, `settings_apply_failed` |
| `pump.control` | `ok`, `status{...}`, `dry_run{...}` | 400 `invalid_action`, `invalid_system`; 409 `dry_run_waiting`, `dry_run_locked`, `measuring` (water start blocked); 500 `control_failed` (reply also has `status`, `dry_run`) |
| `pump.start` | `ok`, `already_running`, `status{...}`, `dry_run{...}` | 409 `status_unavailable`, `dry_run_waiting` (also `wait_remaining_sec`), `dry_run_locked`, `measuring`; 500 `start_failed` |
| `pump.stop` | `ok`, `already_stopped`, `status{...}`, `dry_run{...}` | 409 `status_unavailable`, `stop_failed` (relay state not confirmed); 500 `stop_failed` |

`pump.config.set` params (full replacement of the editable settings; each key has an alias):

| Key (alias) | Rule |
|---|---|
| `water_set1_on` (`timer1_on_sec`), `water_set1_off` (`timer1_off_sec`), `water_set2_on` (`timer2_on_sec`), `water_set2_off` (`timer2_off_sec`) | req, number, 5..86400 s |
| `water_auto_start` (`auto_start`) | req, bool |
| `relay1_polarity` (legacy `relay_polarity`) | req, `"active_low"` or `"active_high"` |
| `water_set1_start_phase` (`timer1_start_phase`), `water_set2_start_phase` (`timer2_start_phase`) | req, `"on"` or `"off"` (any case); must be valid for the hardware map |
| `relay2_polarity` | optional, same values as relay1 |
| `min_dwell_sec` | optional, whole number 0..3600 |
| `wash_on` (`wash_on_sec`), `wash_off` (`wash_off_sec`) | optional, 5..86400 s |
| `wash_start_phase` | optional, `"on"` or `"off"` |
| `wash_auto_start` | optional, bool |

Read-only keys (`float_gpio`, `relay_gpio`, `debounce_ms`, `pump_relay1_gpio`, `pump_relay2_gpio`, `ds18b20_gpio`, `cooling_relay_gpio`, `cooling_relay_polarity`, `hardware_reboot_required`) are refused with `read_only_field`.

`pump.config.set` 400 codes: `read_only_field`, `missing_or_invalid_field` (a required timer or `auto_start`), `invalid_type`, `min_dwell_out_of_range`, `duration_out_of_range`, `missing_field`, `invalid_relay_polarity`, `invalid_start_phase`. The `message` names the field.

`pump.control` params: `system` `"water"`, `"wash"` or `"all"` (default `"water"`); `action` `"start"` or `"stop"` (default `"start"`). A non-string value is ignored and the default is used. A water start runs the same block checks as `pump.start`.

Always send `action`. A `pump.control` call without `action` (for example `{}` or `{"system":"water"}`) starts the water pump, because `"start"` is the default.

Relay polarity from ThingsBoard: `relay1_polarity`, the legacy `relay_polarity` and `relay2_polarity` are accepted only when they equal the stored value. A different valid value is refused before anything is saved with 400 `{"ok":false,"error":"read_only_field","message":"<key>"}`. A value that is not `"active_low"` or `"active_high"` still gets `invalid_relay_polarity`. The check is `dapi_polarity_write_blocked()` in `firmware/components/dapi_guards/`. HTTP is not restricted. The dashboard template loads `pump.config.get` first. That reply is about 800 bytes compact and holds the read-only GPIO keys, so it is over the 512 byte `pump.config.set` limit and would be refused with `read_only_field`; remove the read-only keys and the aliases you do not need before sending.

`pump.start` and `pump.stop` are idempotent. A blocked water start does not touch the pump.

## 5. Cooling (4 methods)

Source: `firmware/main/dapi_cooling.c`. Macros: `COOLING_CONFIG_BODY_BYTES` = 1536, `COOLING_MODE_BODY_BYTES` = 256.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `cooling.status` | GET /api/cooling/status | READ | 0 |
| `cooling.config.get` | GET /api/cooling/config | READ | 0 |
| `cooling.config.set` | POST /api/cooling/config | WRITE | 1536 |
| `cooling.mode.set` | POST /api/cooling/mode | WRITE | 256 |

| Method | Reply keys on success | Errors |
|---|---|---|
| `cooling.status` | `ok`, runtime status keys (built in `api_cooling_add_status_fields`) | 409 `status_unavailable` |
| `cooling.config.get` | `ok`, config keys (`api_cooling_add_config_fields`), limits (`api_cooling_add_limits`), `status{...}` | 409 `status_unavailable`; 500 `settings_load_failed` |
| `cooling.config.set` | Same as `cooling.config.get` | 400 (below); 500 `settings_save_failed`; 500 `runtime_apply_failed` (reply has config and `status`); 409 `status_unavailable` |
| `cooling.mode.set` | Same as `cooling.config.get` | 400 `missing_field`, `invalid_cooling_mode`, `value_out_of_range`; 409 `mode_change_failed`, `status_unavailable`; 500 `settings_save_failed` |

`cooling.mode.set` params: `mode` (req) `"auto"`, `"force_off"`, `"test_on"` or `"test"`. For test mode, `test_mv` (0..10000) or `test_volts` (0.0..10.0); the default is 10000 mV. `auto` and `force_off` are saved with the mode. `test_on` is a runtime state only.

`cooling.config.set` has two forms:

- Curve form, when `points` is present. `points` is an array of 2 to 8 objects. Each has `temp_c` (or `temp_c_x10`) in -50.0..150.0 C, and `output_mv` 0..10000 (or `output_volts` 0.0..10.0). Temperatures must rise strictly. Optional: `curve_type` (`"linear"` or `"step"`), `min_off_sec` (alias `compressor_min_off_sec`) 120..86400, `auto_enable` bool, `test_timeout_sec` 1..3600, `mode`, `cooling_relay_polarity` (`"active_low"` or `"active_high"`).
- Legacy form, without `points`. Required: `threshold_c_x10` -550..1250, `hysteresis_c_x10` 1..500, `auto_enable` bool, `test_timeout_sec` 1..3600, `compressor_min_off_sec` 120..86400, `mode`, `cooling_relay_polarity`. It builds a 2-point linear curve.

Relay polarity from ThingsBoard: `cooling_relay_polarity` is accepted only when it equals the stored value. A different valid value gets 400 `{"ok":false,"error":"read_only_field","message":"cooling_relay_polarity"}` before anything is saved. A value that is not `"active_low"` or `"active_high"` still gets `invalid_relay_polarity`. HTTP is not restricted. The dashboard template loads `cooling.config.get` first.

`cooling.config.set` 400 codes: `invalid_type`, `missing_field`, `value_out_of_range`, `invalid_points_count`, `invalid_point`, `invalid_curve_type`, `hardware_safety` (min off time), `invalid_cooling_mode`, `test_mode_not_persistent` (`test_on` cannot be saved; use `cooling.mode.set`), `invalid_relay_polarity`, `invalid_curve_points`.

## 6. Water and power (4 methods)

Source: `firmware/main/dapi_water_power.c`. Macro: `SENSORS_BODY_BYTES` = 256 for all four.

All four are WRITE. A good request is queued to the sensor task and answers `DAPI_ACCEPTED` (HTTP 202) with `{"ok":true,"queued":true}`.

| Method | HTTP endpoint | Params | Errors |
|---|---|---|---|
| `water.calibrate` | POST /api/water/calibrate | `point` (req) 1 or 2; `buffer_ph` (req) 0..14 | 400 `missing_field` (message = the missing key), `value_out_of_range` |
| `water.calibrate.cancel` | POST /api/water/calibrate/cancel | none | queue errors only |
| `water.offset` | POST /api/water/offset | exactly one of `ph_offset` (-2..2) or `temp_offset` (-10..10) | 400 `missing_field` (message `ph_offset\|temp_offset`; also when both are sent), `value_out_of_range` |
| `power.reset_energy` | POST /api/power/reset-energy | `channel` (req) 0 or 1 (0 = water pump, 1 = chiller) | 400 `missing_field`, `value_out_of_range` |

Queue errors for all four: 400 `value_out_of_range` (sensor task refused the value), 503 `queue_full`, 503 `sensor_unavailable`.

## 7. Wi-Fi (7 methods)

Source: `firmware/main/dapi_wifi.c`. Macros: `WIFI_CONNECT_BODY_BYTES` = 512, `WIFI_PROFILE_BODY_BYTES` = 256, `WIFI_SAVE_BODY_BYTES` = 384.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `wifi.scan` | GET /api/wifi/scan | READ, NO_LOCK | 0 |
| `wifi.profiles.get` | GET /api/wifi/profiles | READ | 0 |
| `wifi.profiles.save` | POST /api/wifi/profiles/save | WRITE | 384 |
| `wifi.profiles.setauto` | POST /api/wifi/profiles/setauto | WRITE | 256 |
| `wifi.profiles.forget` | POST /api/wifi/profiles/forget | WRITE | 256 |
| `wifi.connect` | POST /api/wifi/connect | WRITE, LOCAL_ONLY | 512 |
| `wifi.disconnect` | POST /api/wifi/disconnect | WRITE, LOCAL_ONLY | 0 |

Replies. Some `ok:false` replies carry status 200, as the old handlers did. The status is shown after the code.

| Method | Reply keys on success | Errors (code, status) |
|---|---|---|
| `wifi.scan` | `ok`, `networks[]` (`ssid`, `rssi`, `auth`, `channel`, `connected`). Cached for 10 s. | `scan_busy` 503 (a scan is already running); `scan_failed` 200; `scan_timeout` 200 (waits 5 s); `memory` 500 |
| `wifi.profiles.get` | `ok`, `auto_index`, `profiles[]` (`ssid`, `auto`, `connected`). No passwords. | 500 |
| `wifi.profiles.save` | `ok`, `reboot_pending` (always false) | `invalid_ssid` 400, `invalid_password` 400, `missing_ssid` 400; `not_connected` 200 (only the connected SSID can be saved); `in_use` 409 (ThingsBoard only); `save_failed` 200 |
| `wifi.profiles.setauto` | `ok` | `in_use` 409 (ThingsBoard only); `setauto_failed` 200 |
| `wifi.profiles.forget` | `ok` | `invalid_ssid` 400, `missing_ssid` 400, `in_use` 409 (ThingsBoard only); `forget_failed` 200 |
| `wifi.connect` | `ok`, `connecting`, `ssid`; or `ok`, `already_connected`, `ssid`, `ip` | `invalid_ssid`, `invalid_password`, `invalid_ip`, `invalid_gateway`, `invalid_netmask`, `invalid_dns` (too long) 400; `ssid_too_long`, `password_too_long`, `missing_gateway` 400; `missing_ssid` 200; `connect_error` 200 |
| `wifi.disconnect` | `ok`, `disconnect_pending` (the link drops 500 ms after the reply) | `disconnect_task_failed` 500 |

Params:

- `wifi.profiles.save`: `ssid` (req, up to 32 bytes), `password` (up to 64 bytes; empty keeps the saved one).
- `wifi.profiles.setauto`: `index` (number, -1 clears) or `ssid` (string). An `ssid` that is not saved clears the setting (index -1). An `index` that is not a valid profile number (for example 99 or 1e300) gets `setauto_failed` 200 on HTTP.
- `wifi.profiles.forget`: `ssid` (req, up to 32 bytes).
- `wifi.connect`: `ssid` (req), `password` (empty uses the saved one), and for a static address `ip` (then `gateway` is req), `netmask`, `dns`.

### 7.1 Forget guard

`wifi.profiles.forget` from ThingsBoard fails with `in_use` (409) when `ssid` is the network the board is connected to now. The check is `dapi_wifi_forget_blocked()` in `firmware/components/dapi_guards/`. From HTTP it never blocks. If the board is not connected, it never blocks.

`wifi.profiles.setauto` from ThingsBoard fails with `in_use` (409, "From ThingsBoard auto-connect can only be set to the network in use") unless the resolved target is the saved profile of the network the board is connected to now. Index -1, an unknown `ssid`, an out-of-range `index` and another profile are all refused, and so is every call while the board is not connected, because safety cannot be proven. Clearing auto-connect would leave the board AP-only after a power cycle, out of reach of ThingsBoard. The check is `dapi_wifi_setauto_blocked()`. From HTTP it never blocks. The dashboard template sends `{"ssid": "..."}` with an edit placeholder.

`wifi.profiles.save` from ThingsBoard fails with `in_use` (409, "Cannot change the password of the network in use from ThingsBoard") when `password` is not empty and differs from the station's active password (read with `esp_wifi_get_config`). An empty password (keeps the saved one) or the same password is allowed. If the active password cannot be read it is refused. The check is `dapi_wifi_save_password_blocked()`, and it runs after the `not_connected` check, so it only concerns the connected network. The password is never logged. From HTTP it never blocks.

### 7.2 wifi.scan and the lock

`wifi.scan` is the only `NO_LOCK` method. A scan can take seconds, and a locked scan would block every other call. A second scan is refused at once (`scan_busy`). The cache has its own mutex.

## 8. Status, sensors, uplink and hardware (5 methods)

Sources: `firmware/main/dapi_status.c`, `firmware/main/dapi_hardware.c`. Macro: `HARDWARE_MAP_BODY_BYTES` = 512.

| Method | HTTP endpoint | Flags | Max params |
|---|---|---|---|
| `status.get` | GET /api/status | READ | 0 |
| `sensors.get` | GET /api/sensors/status | READ | 0 |
| `uplink.preview` | GET /api/uplink/preview | READ | 0 |
| `hardware.map.get` | GET /api/hardware/map | READ | 0 |
| `hardware.map.set` | POST /api/hardware/map | WRITE, LOCAL_ONLY | 512 |

| Method | Reply keys on success | Errors |
|---|---|---|
| `status.get` | `ok`, system keys (`chip_model`, `board_name`, `project_version`, `reset_reason`, `reset_detail`, `mac_sta`, `mac_ap`), memory keys (`free_heap`, `min_free_heap`, `largest_free_block`, `psram_free`, ...), `uptime_ms`, Wi-Fi keys (`wifi_mode`, `ap_*`, `sta_connected`, `sta_ip`, `sta_ssid`, `sta_rssi`, ...), HTTP counters, `stg_type`. See `dapi_status_get`. | 500 |
| `sensors.get` | `ok`, `power[]` (channel 0 `pump`, channel 1 `chiller`: `valid`, `fault`, `voltage`, `current`, `power`, `energy_wh`, `frequency`, `pf`, age), `power_notice`, `water{valid, fault, stable, ph, temperature_c, offsets{known, ph, temp}, calibration{state, error}, notice, age}`, `flow{on, age_ms}` | 500 |
| `uplink.preview` | The JSON the uplink would send (`uplink_preview_json`) | 503 `memory` ("Uplink preview unavailable") |
| `hardware.map.get` | `ok`, `active_status`, `pending_status`, `pending_valid`, `reboot_required`, `pending` (map or null), `options`, `board`, `locked_pins`, `fixed_bus_pins[]`, `fixed_inputs[]`. See `hardware_map_reply`. | 500 |
| `hardware.map.set` | Same as `hardware.map.get` | 400 `missing_field`, `invalid_type`, `gpio_not_allowed`, `confirmation_required`, `invalid_hardware_map`; 500 `hardware_map_load_failed`, `pending_save_failed` |

`hardware.map.set` params: `map` (req, object) with `float_input_gpio`, `pump_relay1_gpio`, `pump_relay2_gpio`, `ds18b20_gpio`, `cooling_relay_gpio` (all req, whole numbers, allowed per role by `hardware_map_gpio_allowed_for_role`), and `confirm_reboot_required` (req, must be `true`). The map is saved as pending and applies after a reboot. A map equal to the active one clears the pending map.

## 9. Spelling the frontend will use

The method names are exactly as registered. They are lowercase and dotted, and they match the tables above. The 38 names:

```
alarms.signals alarms.values alarms.status alarms.summary
alarms.rules.get alarms.rules.set alarms.rules.delete alarms.ack
protection.get protection.set protection.measure.get protection.measure.set
pump.status pump.config.get pump.config.set pump.control pump.start pump.stop
cooling.status cooling.config.get cooling.config.set cooling.mode.set
water.calibrate water.calibrate.cancel water.offset power.reset_energy
wifi.scan wifi.profiles.get wifi.profiles.save wifi.profiles.setauto
wifi.profiles.forget wifi.connect wifi.disconnect
status.get sensors.get uplink.preview hardware.map.get hardware.map.set
```

Notes:

- Params use the key names the code reads (sections 2 to 8). Where a method accepts an alias (pump timers, `compressor_min_off_sec`), prefer the first name in the table.
- The reply object is the web API's body. Keys are the same as the HTTP response.
- The dashboard sends `{"method": <name>, "params": {...}}` and reads `ok` and `error` from the reply.
- Method names differ from the HTTP paths in a few places: `alarms.rules.*` for `/api/alarms/rules*`, `power.reset_energy` for `/api/power/reset-energy`, `protection.measure.*` for `/api/protection/measure`.
- HTTP endpoints stay the web app's contract. They do not change when a method is added or renamed.

## 10. Known limits

- `wifi.connect` is LOCAL_ONLY until milestone 4 adds the staged commit. The `STAGED` flag is reserved for it.
- `wifi.disconnect` and `hardware.map.set` are LOCAL_ONLY for good. The first would cut the link the command arrived on. The second needs a reboot and a local confirmation.
- ThingsBoard may deliver an RPC again after a reconnect (seen at checkpoint A, id 90). The board answers a repeat of the same id and payload within 30 s from its reply cache and does not run the method again (section 1.1). The cache holds 4 requests and is lost on a reboot. A repeat after 30 s, or after a restart, runs again.
- `pump.start` has no confirm on the dashboard, as the web app has none.
- `wifi.scan` does not return a `scanning` flag (spec 6.1 suggested one). A scan in progress answers `scan_busy` (503). Poll again after a few seconds.
- A method may run on the `tb_rpc` task (6144 B stack). Buffers over 256 B in method code live on the heap.
- One method runs at a time. A caller that waits behind a slow one gets `busy` after 5 s.
