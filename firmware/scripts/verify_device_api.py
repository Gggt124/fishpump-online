#!/usr/bin/env python3
"""
verify_device_api.py

Source contract checks for the device_api method table
(docs/superpowers/plans: 2026-11-19 thingsboard-device-api-m2).
"""

import re
import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
MAIN_DIR = FIRMWARE_DIR / "main"
COMP_DIR = FIRMWARE_DIR / "components" / "device_api"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

passed_tests = 0
failed_tests = 0


def assert_test(condition, name, details=""):
    global passed_tests, failed_tests
    if condition:
        passed_tests += 1
        print(f"  [PASS] {name}")
    else:
        failed_tests += 1
        print(f"  [FAIL] {name} - {details}")


def read(path):
    p = Path(path)
    return p.read_text(encoding="utf-8") if p.exists() else ""


def function_body(source, name, rettype=r"[\w\s\*]+?"):
    m = re.search(rettype + r"\b" + re.escape(name) + r"\s*\((?:[^()]|\([^()]*\))*\)\s*\{", source)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(source) and depth:
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
        i += 1
    return source[m.start():i]


def test_component():
    print("\n--- device_api component ---")
    cmake = read(COMP_DIR / "CMakeLists.txt")
    hdr = read(COMP_DIR / "include" / "device_api.h")
    src = read(COMP_DIR / "device_api.c")
    assert_test(bool(cmake) and bool(hdr) and bool(src), "component CMakeLists, header and source exist")
    assert_test("REQUIRES" in cmake and "cjson" in cmake and "freertos" in cmake,
                "CMakeLists REQUIRES cjson and freertos")
    assert_test(not re.search(r"\bmain\b", cmake.split("REQUIRES", 1)[-1]), "component has no dependency on main")
    assert_test(re.search(r"#define\s+DAPI_LOCK_TIMEOUT_MS\s+5000\b", hdr) is not None,
                "DAPI_LOCK_TIMEOUT_MS is 5000")
    assert_test(re.search(r"#define\s+DAPI_MAX_METHODS\s+64\b", hdr) is not None, "DAPI_MAX_METHODS is 64")
    for sym in ("device_api_init", "device_api_register", "device_api_find", "device_api_call",
                "device_api_http_status", "dapi_error"):
        assert_test(re.search(r"\b" + sym + r"\s*\(", hdr) is not None, f"header declares {sym}")


def test_logging_and_lock():
    print("\n--- logging and lock ---")
    src = read(COMP_DIR / "device_api.c")
    assert_test('"write %s src=%s"' in src, "WRITE log format string is 'write %s src=%s'")
    bad = [ln for ln in src.splitlines() if "ESP_LOG" in ln and "params" in ln]
    assert_test(not bad, "no ESP_LOG* line in device_api.c mentions params", "; ".join(bad))
    call = function_body(src, "device_api_call")
    assert_test("DAPI_LOCK_TIMEOUT_MS" in call and "xSemaphoreTake" in call,
                "device_api_call takes the lock with DAPI_LOCK_TIMEOUT_MS")
    take = call.find("xSemaphoreTake")
    busy = call.find('"busy"')
    assert_test(take != -1 and busy > take and "Device busy, try again" in call and "DAPI_UNAVAILABLE" in call,
                "busy body is built on the lock timeout path")
    assert_test(call.find("xSemaphoreGive") > call.find("m->fn("), "lock is given back after fn returns")
    assert_test("device_api_http_status" in src and '"503 Service Unavailable"' in src,
                "status lines are defined in device_api_http_status")


def test_app_main():
    print("\n--- app_main wiring ---")
    app = read(MAIN_DIR / "app_main.c")
    assert_test(re.search(r'esp_log_level_set\(\s*"device_api"\s*,\s*ESP_LOG_INFO\s*\)', app) is not None,
                "app_main raises device_api to INFO")
    init = app.find("device_api_init()")
    web = app.find("web_server_start()")
    assert_test(init != -1 and web != -1 and init < web,
                "device_api_init() is called before the first web_server_start()")


def method_table_entry(src, name, flags, max_bytes):
    pat = (r'\{\s*"' + re.escape(name) + r'"\s*,\s*' + flags + r'\s*,\s*' + max_bytes + r'\s*,\s*\w+\s*\}')
    return re.search(pat, src) is not None


def check_group(title, file_name, register_fn, methods, handlers, forbidden):
    """methods: (name, flags regex, max bytes regex); handlers: (handler, method)."""
    print(f"\n--- {title} group ---")
    ws = read(MAIN_DIR / "web_server.c")
    src = read(MAIN_DIR / file_name)
    assert_test(bool(src), f"{file_name} exists")
    assert_test(f"{file_name}" in read(MAIN_DIR / "CMakeLists.txt"), f"{file_name} is in main/CMakeLists.txt")
    assert_test(re.search(r"void\s+" + register_fn + r"\s*\(\s*void\s*\)", src) is not None
                and "device_api_register(" in src, f"{register_fn} registers the table")
    start = function_body(ws, "web_server_start", r"bool\s+")
    assert_test(register_fn + "()" in start, f"web_server_start calls {register_fn}()")
    assert_test(start.find(register_fn + "()") < start.find("httpd_start("),
                f"{register_fn}() runs before the server starts")
    for name, flags, max_bytes in methods:
        assert_test(method_table_entry(src, name, flags, max_bytes), f"table entry {name} {flags} {max_bytes}")
    for handler, method in handlers:
        body = function_body(ws, handler)
        assert_test(f'http_dapi(req, "{method}"' in body, f"{handler} calls http_dapi(req, \"{method}\"")
        bad = [p for p in forbidden if re.search(r"\b" + p, body)]
        assert_test(not bad, f"{handler} holds no engine calls", ", ".join(bad))


def test_http_dapi():
    print("\n--- http_dapi ---")
    ws = read(MAIN_DIR / "web_server.c")
    body = function_body(ws, "http_dapi")
    assert_test(bool(body), "http_dapi exists in web_server.c")
    assert_test("device_api_call(" in body and "DAPI_SRC_HTTP" in body, "http_dapi calls device_api_call with DAPI_SRC_HTTP")
    assert_test("device_api_http_status(" in body, "http_dapi maps the status through device_api_http_status")
    assert_test("cJSON_PrintUnformatted" in body and "send_json(" in body, "http_dapi prints unformatted and sends with send_json")
    assert_test('"memory"' in body and "JSON allocation failed" in body, "http_dapi keeps the memory reply for a NULL out")


def test_alarms_group():
    methods = [
        ("alarms.signals", r"DAPI_F_READ", "0"),
        ("alarms.values", r"DAPI_F_READ", "0"),
        ("alarms.status", r"DAPI_F_READ", "0"),
        ("alarms.summary", r"DAPI_F_READ", "0"),
        ("alarms.rules.get", r"DAPI_F_READ", "0"),
        ("alarms.rules.set", r"DAPI_F_WRITE", "ALARM_POST_BODY_BYTES"),
        ("alarms.rules.delete", r"DAPI_F_WRITE", "ALARM_POST_BODY_BYTES"),
        ("alarms.ack", r"DAPI_F_WRITE", "ALARM_POST_BODY_BYTES"),
    ]
    handlers = [
        ("handle_api_alarms_signals", "alarms.signals"),
        ("handle_api_alarms_values", "alarms.values"),
        ("handle_api_alarms_status", "alarms.status"),
        ("handle_api_alarms_summary", "alarms.summary"),
        ("handle_api_alarms_rules_get", "alarms.rules.get"),
        ("handle_api_alarms_rules_post", "alarms.rules.set"),
        ("handle_api_alarms_rules_delete", "alarms.rules.delete"),
        ("handle_api_alarms_ack", "alarms.ack"),
    ]
    forbidden = ["alarm_engine_", "alarm_rule_", "signal_registry_", "telemetry_hub_get_latest"]
    check_group("alarms", "dapi_alarms.c", "dapi_alarms_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    for h in ("handle_api_alarms_rules_post", "handle_api_alarms_rules_delete", "handle_api_alarms_ack"):
        body = function_body(ws, h)
        assert_test("api_begin_json_post(" in body and "cJSON_Delete(root)" in body, f"{h} keeps its body pre-check and frees the tree")
    for h in ("handle_api_alarms_signals", "handle_api_alarms_values", "handle_api_alarms_status",
              "handle_api_alarms_summary", "handle_api_alarms_rules_get"):
        assert_test("require_auth(req)" in function_body(ws, h) and "alarms_unauthorized(req)" in function_body(ws, h),
                    f"{h} keeps its unauthorized pre-check")


def test_protection_group():
    methods = [
        ("protection.get", r"DAPI_F_READ", "0"),
        ("protection.set", r"DAPI_F_WRITE", "PROTECTION_POST_BODY_BYTES"),
        ("protection.measure.get", r"DAPI_F_READ", "0"),
        ("protection.measure.set", r"DAPI_F_WRITE", "PROTECTION_MEASURE_BODY_BYTES"),
    ]
    handlers = [
        ("handle_api_protection_get", "protection.get"),
        ("handle_api_protection_post", "protection.set"),
        ("handle_api_protection_measure_get", "protection.measure.get"),
        ("handle_api_protection_measure_post", "protection.measure.set"),
    ]
    forbidden = ["dry_run_guard_", "nvs_store_save_dry_run", "nvs_store_load_dry_run"]
    check_group("protection", "dapi_protection.c", "dapi_protection_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_protection.h")
    assert_test(re.search(r"#define\s+PROTECTION_POST_BODY_BYTES\s+384\b", hdr) is not None,
                "PROTECTION_POST_BODY_BYTES is 384")
    assert_test(re.search(r"#define\s+PROTECTION_MEASURE_BODY_BYTES\s+128\b", hdr) is not None,
                "PROTECTION_MEASURE_BODY_BYTES is 128")
    for h in ("handle_api_protection_post", "handle_api_protection_measure_post"):
        body = function_body(ws, h)
        assert_test("api_begin_json_post(" in body and "cJSON_Delete(root)" in body,
                    f"{h} keeps its body pre-check and frees the tree")
    for h in ("handle_api_protection_get", "handle_api_protection_measure_get"):
        body = function_body(ws, h)
        assert_test("require_auth(req)" in body and "alarms_unauthorized(req)" in body,
                    f"{h} keeps its unauthorized pre-check")


def test_pump_group():
    methods = [
        ("pump.status", r"DAPI_F_READ", "0"),
        ("pump.config.get", r"DAPI_F_READ", "0"),
        ("pump.config.set", r"DAPI_F_WRITE", "PUMP_CONFIG_BODY_BYTES"),
        ("pump.control", r"DAPI_F_WRITE", "PUMP_CONTROL_BODY_BYTES"),
        ("pump.start", r"DAPI_F_WRITE", "0"),
        ("pump.stop", r"DAPI_F_WRITE", "0"),
    ]
    handlers = [
        ("handle_api_pump_status", "pump.status"),
        ("handle_api_pump_config_get", "pump.config.get"),
        ("handle_api_pump_config_post", "pump.config.set"),
        ("handle_api_pump_control_post", "pump.control"),
        ("handle_api_pump_start", "pump.start"),
        ("handle_api_pump_stop", "pump.stop"),
    ]
    forbidden = ["pump_control_", "nvs_store_save_pump", "nvs_store_load_pump", "hardware_map_"]
    check_group("pump", "dapi_pump.c", "dapi_pump_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_pump.h")
    assert_test(re.search(r"#define\s+PUMP_CONFIG_BODY_BYTES\s+512\b", hdr) is not None,
                "PUMP_CONFIG_BODY_BYTES is 512")
    assert_test(re.search(r"#define\s+PUMP_CONTROL_BODY_BYTES\s+256\b", hdr) is not None,
                "PUMP_CONTROL_BODY_BYTES is 256")
    for h in ("handle_api_pump_config_get", "handle_api_pump_status"):
        assert_test("require_auth(req)" in function_body(ws, h), f"{h} keeps its unauthorized pre-check")
    for h in ("handle_api_pump_config_post", "handle_api_pump_control_post", "handle_api_pump_start",
              "handle_api_pump_stop"):
        body = function_body(ws, h)
        assert_test("require_auth(req)" in body and "is_password_default()" in body and "is_same_origin(req, false)" in body,
                    f"{h} keeps auth, force_password_change and same-origin pre-checks")
    for h in ("handle_api_pump_config_post", "handle_api_pump_control_post"):
        assert_test("cJSON_Delete(root)" in function_body(ws, h), f"{h} frees the parsed tree")
    src = read(MAIN_DIR / "dapi_pump.c")
    start = function_body(src, "dapi_pump_start")
    assert_test(start.find("pump_start_blocked") != -1 and start.find("pump_start_blocked") < start.find("pump_control_start_water"),
                "pump.start checks the dry-run/measure block before pump_control_start_water")
    ctl = function_body(src, "dapi_pump_control")
    assert_test(ctl.find("pump_start_blocked") != -1 and ctl.find("pump_start_blocked") < ctl.find("pump_control_start_water"),
                "pump.control checks the dry-run/measure block before pump_control_start_water")
    blocked = function_body(src, "pump_start_blocked")
    assert_test("pump_control_stop_water" not in blocked and "dry_run_guard_get_measure" in blocked,
                "pump start block reads guard and measure and never stops the pump")
    assert_test("cJSON_CreateRaw" in function_body(src, "dapi_pump_status"),
                "pump.status keeps the json_gen bytes via cJSON_CreateRaw")


def test_cooling_group():
    methods = [
        ("cooling.status", r"DAPI_F_READ", "0"),
        ("cooling.config.get", r"DAPI_F_READ", "0"),
        ("cooling.config.set", r"DAPI_F_WRITE", "COOLING_CONFIG_BODY_BYTES"),
        ("cooling.mode.set", r"DAPI_F_WRITE", "COOLING_MODE_BODY_BYTES"),
    ]
    handlers = [
        ("handle_api_cooling_status", "cooling.status"),
        ("handle_api_cooling_config_get", "cooling.config.get"),
        ("handle_api_cooling_config_post", "cooling.config.set"),
        ("handle_api_cooling_mode_post", "cooling.mode.set"),
    ]
    forbidden = ["cooling_control_", "nvs_store_save_cooling", "nvs_store_load_cooling"]
    check_group("cooling", "dapi_cooling.c", "dapi_cooling_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_cooling.h")
    assert_test(re.search(r"#define\s+COOLING_CONFIG_BODY_BYTES\s+1536$", hdr, re.M) is not None,
                "COOLING_CONFIG_BODY_BYTES is 1536")
    assert_test(re.search(r"#define\s+COOLING_MODE_BODY_BYTES\s+256$", hdr, re.M) is not None,
                "COOLING_MODE_BODY_BYTES is 256")
    for h in ("handle_api_cooling_config_get", "handle_api_cooling_status"):
        assert_test("require_auth(req)" in function_body(ws, h), f"{h} keeps its unauthorized pre-check")
    for h in ("handle_api_cooling_config_post", "handle_api_cooling_mode_post"):
        body = function_body(ws, h)
        assert_test("require_auth(req)" in body and "is_same_origin(req, false)" in body and "cJSON_Delete(root)" in body,
                    f"{h} keeps auth and same-origin pre-checks and frees the tree")
    src = read(MAIN_DIR / "dapi_cooling.c")
    assert_test("cJSON_CreateRaw" in function_body(src, "dapi_cooling_status"),
                "cooling.status keeps the json_gen bytes via cJSON_CreateRaw")
    setm = function_body(src, "dapi_cooling_config_set")
    assert_test("api_cooling_parse_config_payload" in setm and setm.find("api_cooling_parse_config_payload") < setm.find("nvs_store_save_cooling_settings"),
                "cooling.config.set validates the payload before saving")


def test_water_power_group():
    methods = [
        ("water.calibrate", r"DAPI_F_WRITE", "SENSORS_BODY_BYTES"),
        ("water.calibrate.cancel", r"DAPI_F_WRITE", "SENSORS_BODY_BYTES"),
        ("water.offset", r"DAPI_F_WRITE", "SENSORS_BODY_BYTES"),
        ("power.reset_energy", r"DAPI_F_WRITE", "SENSORS_BODY_BYTES"),
    ]
    handlers = [
        ("handle_api_water_calibrate", "water.calibrate"),
        ("handle_api_water_calibrate_cancel", "water.calibrate.cancel"),
        ("handle_api_water_offset", "water.offset"),
        ("handle_api_power_reset_energy", "power.reset_energy"),
    ]
    forbidden = ["water_quality_", "power_monitor_", "sensors_send_queue_result"]
    check_group("water and power", "dapi_water_power.c", "dapi_water_power_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_water_power.h")
    assert_test(re.search(r"#define\s+SENSORS_BODY_BYTES\s+256$", hdr, re.M) is not None,
                "SENSORS_BODY_BYTES is 256")
    assert_test("char body[SENSORS_BODY_BYTES]" in function_body(ws, "sensors_begin_post", r"static\s+cJSON\s*\*\s*"),
                "sensors_begin_post sizes its buffer with SENSORS_BODY_BYTES")
    for h, _ in handlers:
        body = function_body(ws, h)
        assert_test("sensors_begin_post(req" in body and "cJSON_Delete(root)" in body,
                    f"{h} keeps the shared post guard and frees the tree")
    assert_test("sensors_send_queue_result" not in ws, "web_server.c no longer holds sensors_send_queue_result")
    src = read(MAIN_DIR / "dapi_water_power.c")
    assert_test("DAPI_ACCEPTED" in function_body(src, "queue_result") and '"queued"' in function_body(src, "queue_result"),
                "the queue helper answers DAPI_ACCEPTED with the queued body")


def test_wifi_group():
    local = r"DAPI_F_WRITE\s*\|\s*DAPI_F_LOCAL_ONLY"
    staged = r"DAPI_F_WRITE\s*\|\s*DAPI_F_STAGED"
    methods = [
        ("wifi.scan", r"DAPI_F_READ\s*\|\s*DAPI_F_NO_LOCK", "0"),
        ("wifi.profiles.get", r"DAPI_F_READ", "0"),
        ("wifi.profiles.save", r"DAPI_F_WRITE", "WIFI_SAVE_BODY_BYTES"),
        ("wifi.profiles.setauto", r"DAPI_F_WRITE", "WIFI_PROFILE_BODY_BYTES"),
        ("wifi.profiles.forget", r"DAPI_F_WRITE", "WIFI_PROFILE_BODY_BYTES"),
        ("wifi.connect", staged, "WIFI_CONNECT_BODY_BYTES"),
        ("wifi.disconnect", local, "0"),
    ]
    handlers = [
        ("handle_api_wifi_scan", "wifi.scan"),
        ("handle_api_wifi_profiles", "wifi.profiles.get"),
        ("handle_api_wifi_profiles_save", "wifi.profiles.save"),
        ("handle_api_wifi_profiles_setauto", "wifi.profiles.setauto"),
        ("handle_api_wifi_profiles_forget", "wifi.profiles.forget"),
        ("handle_api_wifi_connect", "wifi.connect"),
        ("handle_api_wifi_disconnect", "wifi.disconnect"),
    ]
    forbidden = ["wifi_manager_", "nvs_store_save_wifi", "nvs_store_load_wifi", "nvs_store_forget_wifi",
                 "nvs_store_set_wifi", "esp_wifi_"]
    check_group("wifi", "dapi_wifi.c", "dapi_wifi_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_wifi.h")
    for name, value in (("WIFI_CONNECT_BODY_BYTES", "512"), ("WIFI_PROFILE_BODY_BYTES", "256"),
                        ("WIFI_SAVE_BODY_BYTES", "384")):
        assert_test(re.search(r"#define\s+" + name + r"\s+" + value + r"$", hdr, re.M) is not None,
                    f"{name} is {value}")
    for h in ("handle_api_wifi_scan", "handle_api_wifi_profiles", "handle_api_wifi_profiles_forget",
              "handle_api_wifi_profiles_setauto", "handle_api_wifi_connect", "handle_api_wifi_disconnect"):
        assert_test("is_password_default()" in function_body(ws, h), f"{h} keeps its force_password_change check")
    assert_test("is_password_default()" not in function_body(ws, "handle_api_wifi_profiles_save"),
                "handle_api_wifi_profiles_save has no force_password_change check, as before")
    for h in ("handle_api_wifi_profiles_save", "handle_api_wifi_profiles_setauto", "handle_api_wifi_profiles_forget",
              "handle_api_wifi_connect", "handle_api_wifi_disconnect"):
        assert_test("is_same_origin(req, true)" in function_body(ws, h), f"{h} keeps its same-origin check")
    for h in ("handle_api_wifi_profiles_save", "handle_api_wifi_profiles_setauto", "handle_api_wifi_profiles_forget",
              "handle_api_wifi_connect"):
        body = function_body(ws, h)
        assert_test("read_request_body(" in body and "cJSON_Delete(root)" in body,
                    f"{h} keeps the body read and frees the parsed tree")
    for sym in ("s_scan_in_progress", "s_scan_cache_json", "s_wifi_disconnect_task_pending",
                "wifi_disconnect_after_response_task", "scan_complete_cb"):
        assert_test(sym not in ws, f"web_server.c no longer holds {sym}")
    src = read(MAIN_DIR / "dapi_wifi.c")
    forget = function_body(src, "dapi_wifi_profiles_forget")
    assert_test("dapi_wifi_forget_blocked(" in forget and "DAPI_CONFLICT" in forget and '"in_use"' in forget
                and "Cannot forget the network in use" in forget
                and forget.find("dapi_wifi_forget_blocked(") < forget.find("nvs_store_forget_wifi_profile"),
                "wifi.profiles.forget refuses the connected network (in_use, 409) before forgetting")
    guards_h = read(FIRMWARE_DIR / "components" / "dapi_guards" / "include" / "dapi_guards.h")
    guards_c = read(FIRMWARE_DIR / "components" / "dapi_guards" / "dapi_guards.c")
    assert_test(re.search(r"bool\s+dapi_wifi_forget_blocked\s*\(\s*dapi_source_t\s+src\s*,\s*const\s+char\s*\*\s*ssid\s*,"
                          r"\s*const\s+char\s*\*\s*connected_ssid\s*\)", guards_h) is not None
                and "DAPI_SRC_TB" in guards_c,
                "dapi_guards declares dapi_wifi_forget_blocked and it checks the ThingsBoard source")
    assert_test("dapi_guards" in read(MAIN_DIR / "CMakeLists.txt"), "main requires dapi_guards")
    assert_test(method_table_entry(src, "wifi.scan", r"DAPI_F_READ\s*\|\s*DAPI_F_NO_LOCK", "0"),
                "wifi.scan carries DAPI_F_NO_LOCK")
    all_src = "".join(read(f) for f in MAIN_DIR.glob("dapi_*.c"))
    assert_test(len(re.findall(r"DAPI_F_NO_LOCK", all_src)) == 1,
                "no method other than wifi.scan carries DAPI_F_NO_LOCK")
    call = function_body(read(COMP_DIR / "device_api.c"), "device_api_call")
    assert_test("DAPI_F_NO_LOCK" in call and call.find("DAPI_F_NO_LOCK") < call.find("xSemaphoreTake")
                and "if (locked)" in call,
                "device_api_call skips the lock take and give only for DAPI_F_NO_LOCK")
    scan = function_body(src, "dapi_wifi_scan")
    assert_test("scan_try_begin()" in scan and "scan_cache_get()" in scan and "scan_cache_put(" in scan
                and "taskENTER_CRITICAL" in function_body(src, "scan_try_begin"),
                "wifi.scan test-and-sets the in-progress flag atomically and guards the cache with its own mutex")
    assert_test("cJSON_CreateRaw" in function_body(src, "scan_cache_get"), "wifi.scan keeps the cached bytes via cJSON_CreateRaw")


def test_status_group():
    methods = [
        ("status.get", r"DAPI_F_READ", "0"),
        ("sensors.get", r"DAPI_F_READ", "0"),
        ("uplink.preview", r"DAPI_F_READ", "0"),
    ]
    handlers = [
        ("handle_api_status", "status.get"),
        ("handle_api_sensors_status", "sensors.get"),
        ("handle_api_uplink_preview", "uplink.preview"),
    ]
    forbidden = ["esp_get_free_heap_size", "heap_caps_get_", "water_quality_get", "power_monitor_get",
                 "flow_switch_get", "nvs_store_save_hardware", "uplink_", "telemetry_hub_get_latest",
                 "esp_wifi_", "wifi_manager_", "json_gen_"]
    check_group("status and sensors", "dapi_status.c", "dapi_status_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    src = read(MAIN_DIR / "dapi_status.c")
    body = function_body(ws, "handle_api_status")
    assert_test("require_auth(req)" in body and "is_password_default()" in body,
                "handle_api_status keeps auth and force_password_change pre-checks")
    assert_test("require_auth(req)" in function_body(ws, "handle_api_sensors_status")
                and "is_password_default()" not in function_body(ws, "handle_api_sensors_status"),
                "handle_api_sensors_status keeps its auth-only pre-check")
    up = function_body(ws, "handle_api_uplink_preview")
    assert_test("require_auth(req)" in up and "alarms_unauthorized(req)" in up,
                "handle_api_uplink_preview keeps its unauthorized pre-check")
    status = function_body(src, "dapi_status_get")
    assert_test("cJSON_CreateRaw" in status and "heap_caps_malloc" in status and "MALLOC_CAP_SPIRAM" in status
                and "malloc(" in status and "api_pump_add_status_fields_gen" in status,
                "status.get builds on the heap (PSRAM first) and wraps the json_gen bytes raw")
    assert_test(not re.search(r"char\s+\w+\[\s*\d{3,}", status), "status.get has no big stack buffer")
    assert_test("cJSON_CreateRaw" in function_body(src, "dapi_uplink_preview") and "free(" in function_body(src, "dapi_uplink_preview"),
                "uplink.preview wraps the preview text raw and frees it")
    sens = function_body(src, "dapi_sensors_get")
    assert_test('"flow"' in sens and '"offsets"' in sens, "sensors.get reports flow and water offsets")
    assert_test("calloc(" in sens and not re.search(r"telemetry_snapshot_t\s+\w+\s*;", sens)
                and not re.search(r"(power_monitor_status_t|water_quality_status_t)\s+\w+\s*;", sens),
                "sensors.get keeps its big structs on the heap")
    for sym in ("sensors_add_age", "sensors_add_notice", "chip_model_to_string", "reset_reason_to_string",
                "format_mac", "json_gen_add_string_safe"):
        assert_test(sym not in ws, f"web_server.c no longer holds {sym}")
    wh = read(MAIN_DIR / "web_server.h")
    assert_test("web_server_get_http_counters" in wh and "web_server_get_http_counters" in ws
                and "web_server_get_http_counters" in src,
                "the HTTP failure counters reach dapi_status.c through web_server_get_http_counters")


def test_hardware_group():
    local = r"DAPI_F_WRITE\s*\|\s*DAPI_F_LOCAL_ONLY"
    methods = [
        ("hardware.map.get", r"DAPI_F_READ", "0"),
        ("hardware.map.set", local, "HARDWARE_MAP_BODY_BYTES"),
    ]
    handlers = [
        ("handle_api_hardware_map_get", "hardware.map.get"),
        ("handle_api_hardware_map_post", "hardware.map.set"),
    ]
    forbidden = ["nvs_store_save_pending_hardware", "nvs_store_load_hardware", "nvs_store_load_pending_hardware",
                 "nvs_store_clear_pending_hardware", "nvs_store_save_hardware", "hardware_map_", "api_hardware_"]
    check_group("hardware map", "dapi_hardware.c", "dapi_hardware_register", methods, handlers, forbidden)
    ws = read(MAIN_DIR / "web_server.c")
    hdr = read(MAIN_DIR / "dapi_hardware.h")
    src = read(MAIN_DIR / "dapi_hardware.c")
    assert_test(re.search(r"#define\s+HARDWARE_MAP_BODY_BYTES\s+512$", hdr, re.M) is not None,
                "HARDWARE_MAP_BODY_BYTES is 512")
    assert_test("require_auth(req)" in function_body(ws, "handle_api_hardware_map_get")
                and "is_password_default()" in function_body(ws, "handle_api_hardware_map_get"),
                "handle_api_hardware_map_get keeps auth and force_password_change pre-checks")
    post = function_body(ws, "handle_api_hardware_map_post")
    assert_test("require_auth(req)" in post and "is_password_default()" in post and "is_same_origin(req, false)" in post
                and "read_request_body(" in post and '"empty_body"' in post and "cJSON_Delete(root)" in post,
                "handle_api_hardware_map_post keeps auth, force_password_change, same-origin and body pre-checks")
    setm = function_body(src, "dapi_hardware_map_set")
    assert_test("api_hardware_parse_map_payload" in setm
                and setm.find("api_hardware_parse_map_payload") < setm.find("nvs_store_save_pending_hardware_map"),
                "hardware.map.set validates the payload before saving")
    assert_test("hardware_map_load_failed" in setm and "pending_save_failed" in setm
                and "nvs_store_clear_pending_hardware_map" in setm,
                "hardware.map.set keeps its load-failed and save-failed replies and the clear-when-equal path")
    for sym in ("api_hardware_parse_map_payload", "api_hardware_map_send", "api_hardware_map_add_json",
                "api_hardware_add_fixed_bus_pins", "api_hardware_add_board_info"):
        assert_test(sym not in ws, f"web_server.c no longer holds {sym}")


def test_final_review_guards():
    print("\n--- final review: remote-only guards and re-delivery cache ---")
    wifi = read(MAIN_DIR / "dapi_wifi.c")
    pump = read(MAIN_DIR / "dapi_pump.c")
    cool = read(MAIN_DIR / "dapi_cooling.c")
    link = read(FIRMWARE_DIR / "components" / "tb_link" / "tb_link.c")
    app = read(MAIN_DIR / "app_main.c")
    assert_test("dapi_wifi_setauto_blocked(" in function_body(wifi, "dapi_wifi_profiles_setauto"),
                "wifi.profiles.setauto asks dapi_wifi_setauto_blocked")
    assert_test("dapi_wifi_save_password_blocked(" in function_body(wifi, "dapi_wifi_profiles_save"),
                "wifi.profiles.save asks dapi_wifi_save_password_blocked")
    assert_test("dapi_polarity_write_blocked(" in function_body(pump, "dapi_pump_config_set"),
                "pump.config.set asks dapi_polarity_write_blocked")
    assert_test("dapi_polarity_write_blocked(" in function_body(cool, "dapi_cooling_config_set"),
                "cooling.config.set asks dapi_polarity_write_blocked")
    body = function_body(link, "rpc_process")
    assert_test("tb_rpc_dedup_find(" in body and "tb_rpc_dedup_insert(" in body and "duplicate, cached reply" in body,
                "rpc_process answers a repeated request from the reply cache")
    assert_test(re.search(r'esp_log_level_set\(\s*"dapi_wifi"\s*,\s*ESP_LOG_INFO\s*\)', app) is not None,
                "app_main raises dapi_wifi to INFO")


def test_timed_stop_guards():
    print("\n--- firmware 0.5: timed pump stop service ---")
    pump = read(MAIN_DIR / "dapi_pump.c")
    svc = read(MAIN_DIR / "pump_resume_service.c")
    link = read(FIRMWARE_DIR / "components" / "tb_link" / "tb_link.c")
    ctl = function_body(pump, "dapi_pump_control")
    cancel_at = ctl.find("pump_resume_service_cancel(")
    blocked_at = ctl.find("pump_start_blocked(")
    assert_test(cancel_at != -1 and blocked_at != -1 and cancel_at < blocked_at,
                "pump.control cancels a timed stop before a water start can be refused (same as pump.start)")
    # The resume task runs pump.start through device_api_call, the same path the tb_rpc task runs.
    svc_stack = re.search(r"#define RESUME_TASK_STACK\s+(\d+)", svc)
    rpc_stack = re.search(r"#define TB_RPC_TASK_STACK\s+(\d+)", link)
    assert_test(svc_stack is not None and rpc_stack is not None
                and int(svc_stack.group(1)) >= int(rpc_stack.group(1)),
                "pump_resume task stack is at least the tb_rpc task stack")
    app = read(MAIN_DIR / "app_main.c")
    assert_test(re.search(r'esp_log_level_set\(\s*"pump_resume"\s*,\s*ESP_LOG_INFO\s*\)', app) is not None,
                "app_main raises pump_resume to INFO so 'timed stop over' reaches the log")


def test_json_gen_overflow():
    print("\n--- bench 0.5: json_gen documents never go out cut short ---")
    hdr = read(MAIN_DIR / "api_json.h")
    assert_test("bool json_gen_overflowed(const json_gen_t *gen);" in hdr, "api_json.h declares json_gen_overflowed")
    for file_name, fn in (("dapi_status.c", "dapi_status_get"), ("dapi_pump.c", "dapi_pump_status"),
                          ("dapi_cooling.c", None)):
        src = read(MAIN_DIR / file_name)
        inits = len(re.findall(r"json_gen_init\(", src))
        checks = len(re.findall(r"json_gen_overflowed\(", src))
        assert_test(inits > 0 and checks >= inits, f"{file_name}: every json_gen_init has a json_gen_overflowed check")
    status = function_body(read(MAIN_DIR / "dapi_status.c"), "dapi_status_get")
    cap = re.search(r"#define STATUS_JSON_CAP\s+(\d+)", read(MAIN_DIR / "dapi_status.c"))
    assert_test(cap is not None and int(cap.group(1)) >= 4096 and "2048" not in status,
                "status.get builds into a STATUS_JSON_CAP (>= 4096) buffer")


if __name__ == "__main__":
    test_component()
    test_logging_and_lock()
    test_app_main()
    test_http_dapi()
    test_alarms_group()
    test_protection_group()
    test_pump_group()
    test_cooling_group()
    test_water_power_group()
    test_wifi_group()
    test_status_group()
    test_hardware_group()
    test_final_review_guards()
    test_timed_stop_guards()
    test_json_gen_overflow()
    print(f"\nPassed: {passed_tests}  Failed: {failed_tests}")
    sys.exit(1 if failed_tests else 0)
