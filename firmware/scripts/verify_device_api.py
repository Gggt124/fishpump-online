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


if __name__ == "__main__":
    test_component()
    test_logging_and_lock()
    test_app_main()
    test_http_dapi()
    test_alarms_group()
    print(f"\nPassed: {passed_tests}  Failed: {failed_tests}")
    sys.exit(1 if failed_tests else 0)
