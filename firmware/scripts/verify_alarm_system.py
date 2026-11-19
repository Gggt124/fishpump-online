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

    status = function_body(read(MAIN_DIR / "dapi_status.c"), "dapi_sensors_get")
    assert_test('"flow"' in status, "sensors.get reports flow")
    cb = function_body(ws, "web_server_telemetry_cb")
    assert_test("case TELEMETRY_EVT_FLOW:" in cb and 'evt_name = "flow"' in cb,
                "SSE flow event emitted")
    assert_test('"pump1"' not in ws and '"pump2"' not in ws, "web_server.c has no pump1/pump2 names")
    assert_test("pzem_pump1" not in hw and "ปั๊ม 1" not in hw, "hardware_map.c has no pump1 names")
    assert_test("fixed_inputs" in function_body(read(MAIN_DIR / "dapi_hardware.c"), "api_hardware_add_fixed_bus_pins"),
                "/api/hardware/map exposes fixed_inputs")
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


ALARM_IDS = (
    "view-alarms", "nav-alarms", "m-nav-alarms", "alarm-banner", "alarm-badge",
    "alarm-active-list", "alarm-rule-list", "alarm-rule-count", "alarm-history-list",
    "alarm-add-rule", "alarm-engine-stalled", "alarm-rule-dialog", "alarm-f-name",
    "alarm-f-signal", "alarm-f-live", "alarm-f-condition", "alarm-f-threshold",
    "alarm-f-lo", "alarm-f-hi", "alarm-f-stale", "alarm-f-hysteresis", "alarm-f-delay",
    "alarm-f-severity", "alarm-f-latch", "alarm-f-gate-signal", "alarm-f-gate-when",
    "alarm-f-save",
)


def test_alarm_ui():
    print("\n--- Task 9: Alarms page, banner and badge ---")
    html = read(MAIN_DIR / "static" / "index.html")
    js = read(MAIN_DIR / "static" / "app.js")
    for i in ALARM_IDS:
        assert_test(f'id="{i}"' in html, f"index.html has id={i}")
    m = re.search(r"function handleRoute\(\) \{(.*?)\n\}\n", js, re.S)
    route = m.group(1) if m else ""
    assert_test(route.count("path === '/alarms'") >= 2, "handleRoute handles /alarms in both places")
    assert_test("addEventListener('alarm'" in js, "app.js registers the alarm SSE listener")
    assert_test("ALARM_SUMMARY_POLL_MS = 3000" in js, "summary poll is 3 s")
    assert_test("ALARM_VALUES_POLL_MS = 2000" in js, "values poll is 2 s")
    assert_test("fishpump_alarm_seen" in js, "unseen marker stored in localStorage")
    for fn in ("initAlarmsPage", "syncAlarmSummary", "syncAlarmStatus"):
        assert_test(("function " + fn) in js, f"app.js defines {fn}")
    m = re.search(r"sseReconnectTimer\s*=\s*setTimeout\(function \(\) \{(.*?)\}, 5000\)", js, re.S)
    assert_test(m is not None and "'/alarms'" in m.group(1), "SSE reconnect also fires on /alarms")
    sm = re.search(r"function syncAlarmSummary\([^)]*\) \{(.*?)\n\}\n", js, re.S)
    body = sm.group(1) if sm else ""
    assert_test("syncAlarmStatus(" in body, "syncAlarmSummary falls back to syncAlarmStatus (no free SSE slot)")
    assert_test("document.hidden" in body, "syncAlarmSummary skips hidden tabs")
    cu = re.search(r"function cleanupCurrentView\(\) \{(.*?)\n\}\n", js, re.S)
    assert_test(cu is not None and "alarmSummaryTimer" not in cu.group(1),
                "cleanupCurrentView does not stop the summary poll")
    gz = MAIN_DIR / "static_compressed" / "index.html.gz"
    content = gzip.open(gz, "rb").read().decode("utf-8", "replace") if gz.exists() else ""
    assert_test("view-alarms" in content, "static_compressed/index.html.gz contains view-alarms")


def test_uplink():
    print("\n--- Task 11: uplink preview ---")
    ws = read(MAIN_DIR / "web_server.c")
    html = read(MAIN_DIR / "static" / "index.html")
    js = read(MAIN_DIR / "static" / "app.js")
    routes = routes_block(ws)
    assert_test(route_entry(routes, "/api/uplink/preview", "HTTP_GET", "handle_api_uplink_preview"),
                "GET /api/uplink/preview -> handle_api_uplink_preview")
    assert_test("require_auth(" in function_body(ws, "handle_api_uplink_preview"),
                "handle_api_uplink_preview requires auth")
    for el in ("uplink-preview-card", "uplink-preview-json", "uplink-preview-refresh", "uplink-preview-summary"):
        assert_test(f'id="{el}"' in html, f"index.html has id={el}")
    status_view = html[html.find('id="view-status"'):html.find('id="view-wifi"')]
    assert_test('id="uplink-preview-card"' in status_view, "preview card sits at the end of view-status")
    assert_test("function initUplinkPreview" in js, "app.js defines initUplinkPreview")
    uh = read(COMP_DIR / "uplink" / "include" / "uplink.h")
    assert_test(re.search(r"#define\s+UPLINK_INTERVAL_SEC\s+30\b", uh) is not None, "uplink.h defines UPLINK_INTERVAL_SEC 30")
    uc = read(COMP_DIR / "uplink" / "uplink.c")
    assert_test(bool(uc) and "telemetry_hub_subscribe" not in uc, "uplink.c polls (no telemetry_hub_subscribe)")
    assert_test("MALLOC_CAP_SPIRAM" in uc, "uplink.c keeps its buffers in PSRAM")


def test_review_fixes():
    print("\n--- Review fixes: findings 5, 6, 8 ---")
    eng = read(COMP_DIR / "alarm_engine" / "alarm_engine.c")
    assert_test(re.search(r"ALARM_MAX_RULES\s*\|\|\s*next->next_id\s*==\s*0\s*\|\|\s*next->next_id\s*>=\s*ALARM_SYSTEM_ID_BASE\)\s*\{\s*err\s*=\s*ESP_ERR_INVALID_SIZE", eng) is not None,
                "upsert: rules full returns ESP_ERR_INVALID_SIZE")
    ws = read(MAIN_DIR / "web_server.c")
    dapi_al = read(MAIN_DIR / "dapi_alarms.c")
    post = function_body(dapi_al, "dapi_alarms_rules_set")
    assert_test(re.search(r'case ESP_ERR_INVALID_SIZE:\s*\*out = dapi_error\("rules_full", "rules"\);\s*return DAPI_CONFLICT', post) is not None,
                "rules POST maps INVALID_SIZE to rules_full (409 via dapi_error)")
    assert_test(re.search(r'case ESP_ERR_NO_MEM:\s*\*out = dapi_error\("out_of_memory"[^;]*;\s*return DAPI_UNAVAILABLE', post) is not None,
                "rules POST maps NO_MEM to 503 out_of_memory (DAPI_UNAVAILABLE)")
    js = read(MAIN_DIR / "static" / "app.js")
    assert_test("code === 'out_of_memory'" in js, "app.js shows a message for out_of_memory")

    eh = read(COMP_DIR / "alarm_engine" / "include" / "alarm_engine.h")
    assert_test(re.search(r"esp_err_t\s+alarm_engine_ack_all\s*\(\s*size_t\s*\*\s*acked\s*\)", eh) is not None,
                "alarm_engine_ack_all returns esp_err_t and reports the count through a pointer")
    ack = function_body(dapi_al, "dapi_alarms_ack")
    assert_test("alarms_ack_error(err," in ack, "ack method maps engine errors through alarms_ack_error")
    helper = function_body(dapi_al, "alarms_ack_error")
    assert_test('"out_of_memory"' in helper and '"unavailable"' in helper and '"not_found"' in helper,
                "ack error helper covers not_found, out_of_memory, unavailable")
    assert_test(not re.search(r"alarm_engine_ack\(id\)\s*!=\s*ESP_OK\)\s*\{\s*\*out = dapi_error\(\"not_found\"", ack),
                "ack no longer reports every failure as not_found")

    pm = read(COMP_DIR / "power_monitor" / "power_monitor.c")
    assert_test(bool(pm) and re.search(r'"pump %u', pm) is None, "power_monitor logs do not number pumps")
    assert_test(re.search(r'"ปั๊ม %u', pm) is None, "power_monitor notices do not number pumps")
    assert_test("pump 1" not in pm, "power_monitor has no 'pump 1' text")
    assert_test('"chiller"' in pm and '"Chiller"' in pm, "power_monitor names the chiller channel")


def main():
    test_flow_and_rename()
    test_alarm_api()
    test_alarm_ui()
    test_uplink()
    test_review_fixes()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
