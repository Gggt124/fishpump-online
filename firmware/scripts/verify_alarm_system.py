#!/usr/bin/env python3
"""
verify_alarm_system.py

Source and asset contract checks for the flow switch, generic alarm rules and
ThingsBoard-ready uplink project
(docs/superpowers/specs/2026-11-15-alarm-rules-and-uplink-design.md):
- flow switch on the web (sensors status, SSE, Sensors page)
- pump / chiller naming (no "pump1" / "pump2" for the PZEM channels)
- alarm REST routes, guards, SSE event size, Alarms page, uplink preview
"""

import gzip
import re
import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
MAIN_DIR = FIRMWARE_DIR / "main"
COMP_DIR = FIRMWARE_DIR / "components"

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


def function_body(source, name):
    """Return the text of `static <type> name(...) { ... }`, or ''."""
    m = re.search(r"static\s+[\w\s\*]+?\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
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


def routes_block(source):
    m = re.search(r"s_routes\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    return m.group(1) if m else ""


def test_flow_and_rename():
    print("\n--- Task 3: flow switch on the web, pump/chiller naming ---")
    ws = read(MAIN_DIR / "web_server.c")
    html = read(MAIN_DIR / "static" / "index.html")
    js = read(MAIN_DIR / "static" / "app.js")
    hw = read(COMP_DIR / "hardware_map" / "hardware_map.c")

    status = function_body(ws, "handle_api_sensors_status")
    assert_test('"flow"' in status, "handle_api_sensors_status reports flow")
    cb = function_body(ws, "web_server_telemetry_cb")
    assert_test("case TELEMETRY_EVT_FLOW:" in cb and 'evt_name = "flow"' in cb,
                "SSE flow event emitted")
    assert_test('"pump1"' not in ws and '"pump2"' not in ws, "web_server.c has no pump1/pump2 names")
    assert_test("pzem_pump1" not in hw and "ปั๊ม 1" not in hw, "hardware_map.c has no pump1 names")
    assert_test("fixed_inputs" in function_body(ws, "api_hardware_add_fixed_bus_pins") or
                "fixed_inputs" in ws, "/api/hardware/map exposes fixed_inputs")
    for el in ('id="flow-card"', 'id="flow-state"', 'id="hw-fixed-inputs"',
               "ไฟฟ้า ปั๊มน้ำ", "ไฟฟ้า Chiller"):
        assert_test(el in html, f"index.html has {el}")
    assert_test("ไฟฟ้า ปั๊ม 1" not in html, "index.html has no 'ไฟฟ้า ปั๊ม 1'")
    assert_test("addEventListener('flow'" in js, "app.js registers the flow SSE listener")


SSE_ALARM_FMT = ('{"seq":%lu,"type":"%s","rule_id":%u,"sev":%u,"active":%u,'
                 '"critical":%u,"unacked":%u}')
SSE_JSON_BUF = 256

ALARM_GET_ROUTES = {
    "/alarms": "handle_get_alarms",
    "/api/alarms/signals": "handle_api_alarms_signals",
    "/api/alarms/values": "handle_api_alarms_values",
    "/api/alarms/rules": "handle_api_alarms_rules_get",
    "/api/alarms/status": "handle_api_alarms_status",
    "/api/alarms/summary": "handle_api_alarms_summary",
}
ALARM_POST_ROUTES = {
    "/api/alarms/rules": "handle_api_alarms_rules_post",
    "/api/alarms/rules/delete": "handle_api_alarms_rules_delete",
    "/api/alarms/ack": "handle_api_alarms_ack",
}


def route_entry(routes, uri, method, handler):
    pat = (r'\.uri\s*=\s*"' + re.escape(uri) + r'"\s*,\s*\.method\s*=\s*' + method +
           r'\s*,\s*\.handler\s*=\s*' + re.escape(handler) + r'\b')
    return re.search(pat, routes) is not None


def test_alarm_api():
    print("\n--- Task 8: alarm REST API and SSE ---")
    ws = read(MAIN_DIR / "web_server.c")
    routes = routes_block(ws)
    for uri, handler in ALARM_GET_ROUTES.items():
        assert_test(route_entry(routes, uri, "HTTP_GET", handler), f"GET {uri} -> {handler}")
    for uri, handler in ALARM_POST_ROUTES.items():
        assert_test(route_entry(routes, uri, "HTTP_POST", handler), f"POST {uri} -> {handler}")
    assert_test("/api/alarms/status" in routes and "/api/events" in routes, "routes table intact")

    # GET handlers check auth themselves; POST handlers go through the shared guard,
    # which holds auth + same-origin (so every alarm endpoint is login-protected).
    for handler in ALARM_GET_ROUTES.values():
        if handler == "handle_get_alarms":
            continue  # the SPA shell redirects to /login (checked via require_auth too)
        body = function_body(ws, handler)
        assert_test("require_auth(" in body, f"{handler} requires auth")
    assert_test("require_auth(" in function_body(ws, "handle_get_alarms"), "handle_get_alarms requires auth")
    for handler in ALARM_POST_ROUTES.values():
        body = function_body(ws, handler)
        assert_test("api_begin_json_post(" in body, f"{handler} uses api_begin_json_post")
    guard = function_body(ws, "api_begin_json_post")
    assert_test("require_auth(" in guard, "api_begin_json_post requires auth")
    assert_test("is_same_origin(" in guard, "api_begin_json_post checks same origin")

    flat = re.sub(r'"\s*\n\s*"', "", ws)
    c_fmt = SSE_ALARM_FMT.replace('"', '\\"')
    assert_test(c_fmt in flat, "SSE alarm format in web_server.c matches the contract")
    worst = ('{"seq":4294967295,"type":"cleared","rule_id":65535,"sev":3,"active":255,'
             '"critical":255,"unacked":255}')
    assert_test(len(worst.encode()) < SSE_JSON_BUF, f"worst-case SSE alarm JSON fits {SSE_JSON_BUF} B ({len(worst)} B)")
    cb = function_body(ws, "web_server_telemetry_cb")
    assert_test("case TELEMETRY_EVT_ALARM:" in cb and 'evt_name = "alarm"' in cb, "SSE alarm event emitted")


def main():
    test_flow_and_rename()
    test_alarm_api()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
