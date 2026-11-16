#!/usr/bin/env python3
"""
verify_dry_run.py

Source contract checks for pump dry-run protection
(docs/superpowers/specs/2026-11-16-pump-dry-run-protection-design.md).
"""

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


def function_body(source, name, rettype=r"static\s+[\w\s\*]+?"):
    """Return the text of `<rettype> name(...) { ... }`, or ''. The default matches static functions."""
    m = re.search(rettype + r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
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


GIVE_THEN_CB = r"xSemaphoreGive\(s\.lock\)[\s\S]*on_ack\("


def test_engine():
    print("\n--- Task 3: system alarms in the alarm engine ---")
    hdr = read(COMP_DIR / "alarm_engine" / "include" / "alarm_engine.h")
    eng = read(COMP_DIR / "alarm_engine" / "alarm_engine.c")
    presets = read(COMP_DIR / "alarm_rules" / "alarm_presets.c")
    for fn in ("alarm_engine_system_register", "alarm_engine_system_raise", "alarm_engine_system_clear"):
        assert_test(re.search(r"esp_err_t\s+" + fn + r"\s*\(", hdr) is not None, f"alarm_engine.h declares {fn}")
    ack = function_body(eng, "alarm_engine_ack", r"esp_err_t\s+")
    assert_test(ack != "" and re.search(GIVE_THEN_CB, ack) is not None,
                "alarm_engine_ack invokes the callback after releasing s.lock")
    ack_all = function_body(eng, "alarm_engine_ack_all", r"esp_err_t\s+")
    assert_test(ack_all != "" and re.search(GIVE_THEN_CB, ack_all) is not None,
                "alarm_engine_ack_all invokes the callbacks after releasing s.lock")
    assert_test("ปั๊มน้ำเปิดแต่น้ำไม่ไหล" not in presets, "alarm_presets.c no longer has the dry-run preset")
    assert_test("ALARM_SYSTEM_ID_BASE" in eng, "alarm_engine.c guards next_id with ALARM_SYSTEM_ID_BASE")


def test_pump_control():
    print("\n--- Task 4: pump_control guard, test mode and signal ---")
    hdr = read(COMP_DIR / "pump_control" / "include" / "pump_control.h")
    src = read(COMP_DIR / "pump_control" / "pump_control.c")
    assert_test(re.search(r"bool\s+pump_control_set_water_guard\s*\(", hdr) is not None,
                "pump_control.h declares pump_control_set_water_guard")
    assert_test(re.search(r"bool\s+pump_control_water_test\s*\(", hdr) is not None,
                "pump_control.h declares pump_control_water_test")
    assert_test("water_guard_initial" in hdr, "pump_control_init_context_t has water_guard_initial")
    apply = function_body(src, "apply_water_set_start_phase", r"static\s+bool\s+")
    assert_test(apply != "" and re.search(r"guard[\s\S]*set_relay1_energized", apply) is not None,
                "apply_water_set_start_phase checks the guard before set_relay1_energized")
    tick = function_body(src, "pump_tick_cb", r"static\s+void\s+")
    water = tick.split("/* Process Water System */")[-1].split("/* Process Wash System */")[0]
    assert_test(tick != "" and "water_inhibited_locked" in water,
                "pump_tick_cb water branch references the guard")
    assert_test("PUMP_WATER_TEST_MAX_MS" in tick and "s_test_active" in tick,
                "pump_tick_cb caps relay-1 test mode with PUMP_WATER_TEST_MAX_MS")
    start = function_body(src, "pump_control_start_water", r"bool\s+")
    assert_test(start != "" and re.search(r"water_inhibited_locked\(\)[\s\S]*return false", start) is not None,
                "pump_control_start_water returns false when inhibited or testing")
    assert_test("static bool water_inhibited_locked" in src, "water_inhibited_locked helper exists")


def test_guard():
    print("\n--- Task 5: NVS settings, dry_run_guard and boot wiring ---")
    app = read(MAIN_DIR / "app_main.c")
    guard = read(COMP_DIR / "dry_run_guard" / "dry_run_guard.c")
    nvs_h = read(COMP_DIR / "nvs_store" / "include" / "nvs_store.h")
    i_load = app.find("nvs_store_load_dry_run(")
    i_pump = app.find("pump_control_init(")
    i_pump_start = app.find("pump_control_start_task(&")
    i_alarm = app.find("alarm_engine_init(")
    i_guard = app.find("dry_run_guard_init(")
    pump_idx = i_pump_start if i_pump_start >= 0 else i_pump
    assert_test(i_load >= 0 and pump_idx >= 0 and i_load < pump_idx,
                "app_main loads dry-run settings before pump_control_init")
    assert_test(i_alarm >= 0 and i_guard > i_alarm, "app_main calls dry_run_guard_init after alarm_engine_init")
    assert_test("water_guard_initial" in app, "app_main passes water_guard_initial")
    assert_test("alarm_engine_system_register(ALARM_SYSTEM_ID_DRY_RUN" in guard,
                "dry_run_guard.c registers the dry-run system alarm")
    for fn in ("nvs_store_load_dry_run", "nvs_store_save_dry_run_config", "nvs_store_save_dry_run_locked"):
        assert_test(re.search(r"bool\s+" + fn + r"\s*\(", nvs_h) is not None, f"nvs_store.h declares {fn}")
    # every ESP_LOG in dry_run_guard.c must sit in a state-change logger or init
    bad = []
    for m in re.finditer(r"^(?:static\s+)?[\w\s\*]+?(\w+)\s*\([^;{]*\)\s*\{", guard, re.M):
        name = m.group(1)
        if name in ("if", "for", "while", "switch"):
            continue
        depth, i = 1, m.end()
        while i < len(guard) and depth:
            depth += (guard[i] == "{") - (guard[i] == "}")
            i += 1
        if "ESP_LOG" in guard[m.end():i] and not ("state_changed" in name or "log_transition" in name
                                                  or name == "dry_run_guard_init"):
            bad.append(name)
    assert_test(guard != "" and not bad, "ESP_LOG only in state-change loggers or init", str(bad))


def test_api():
    print("\n--- Task 6: web API ---")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from verify_alarm_system import routes_block, route_entry
    ws = read(MAIN_DIR / "web_server.c")
    routes = routes_block(ws)
    for method, uri, handler in (
        ("HTTP_GET", "/api/protection", "handle_api_protection_get"),
        ("HTTP_POST", "/api/protection", "handle_api_protection_post"),
        ("HTTP_GET", "/api/protection/measure", "handle_api_protection_measure_get"),
        ("HTTP_POST", "/api/protection/measure", "handle_api_protection_measure_post"),
    ):
        assert_test(route_entry(routes, uri, method, handler), f"{method} {uri} -> {handler}")
    for fn in ("handle_api_protection_post", "handle_api_protection_measure_post"):
        assert_test("api_begin_json_post(" in function_body(ws, fn), f"{fn} uses api_begin_json_post")
    for fn in ("handle_api_protection_get", "handle_api_protection_measure_get"):
        assert_test("require_auth(" in function_body(ws, fn), f"{fn} requires auth")
    for fn in ("handle_api_pump_start", "handle_api_pump_control_post"):
        body = function_body(ws, fn)
        i_blocked = body.find("pump_start_blocked_response")
        i_start = body.find("pump_control_start_water")
        assert_test(0 <= i_blocked < i_start,
                    f"{fn} checks the dry-run guard (helper) before pump_control_start_water")
        assert_test("pump_start_blocked_response" in body, f"{fn} calls pump_start_blocked_response")
    blocked = function_body(ws, "pump_start_blocked_response")
    assert_test("dry_run_guard_get_status" in blocked and "dry_run_guard_get_measure" in blocked,
                "pump_start_blocked_response reads guard and measure status")
    assert_test("pump_control_stop_water" not in blocked, "pump_start_blocked_response never stops the pump")
    for code in ("dry_run_locked", "dry_run_waiting", "measuring", "wait_remaining_sec"):
        assert_test(f'"{code}"' in blocked, f"pump_start_blocked_response emits {code}")
    assert_test("dry_run_add_status_object(" in function_body(ws, "api_pump_add_status_object")
                and '"dry_run"' in function_body(ws, "dry_run_add_status_object"),
                "api_pump_add_status_object adds dry_run")
    assert_test("dry_run_add_status_gen(" in function_body(ws, "handle_api_pump_status")
                and '"dry_run"' in function_body(ws, "dry_run_add_status_gen"),
                "GET /api/pump/status adds dry_run")
    assert_test("ALARM_SYSTEM_ID_DRY_RUN" in function_body(ws, "handle_api_alarms_status"),
                "alarms status adds dry_run detail to the system alarm")
    assert_test("ALARM_MAX_RULES + ALARM_ENGINE_MAX_SYSTEM" in function_body(ws, "handle_api_alarms_status"),
                "alarms status sizes active buffer for system alarms")
    hdr = read(COMP_DIR / "alarm_engine" / "include" / "alarm_engine.h")
    assert_test(re.search(r"alarm_engine_get_active[^\n]*system", hdr, re.I) is not None,
                "alarm_engine_get_active comment mentions system alarms")


def js_function_body(source, name):
    """Return the text of `function name(...) { ... }` in JS source, or ''."""
    m = re.search(r"\bfunction\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
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


DRY_RUN_UI_FUNCTIONS = (
    "dryRunStatusText", "dryRunApplyStatus", "dryRunLoad", "dryRunStartPolling", "dryRunStopPolling",
    "dryRunRenderWarning", "dryRunReadForm", "dryRunSave", "dryRunMeasureStart", "dryRunMeasurePoll",
    "dryRunRenderMeasure", "dryRunStartError", "initDryRunCard",
)


def test_ui():
    print("\n--- Task 7: dashboard web UI ---")
    html = read(MAIN_DIR / "static" / "index.html")
    js = read(MAIN_DIR / "static" / "app.js")
    m = re.search(r'<section id="view-dashboard"[\s\S]*?</section>\s*<section id="view-cooling"', html)
    assert_test(m is not None and 'id="dry-run-card"' in m.group(0),
                "index.html has id=\"dry-run-card\" inside view-dashboard")
    for token in ("/api/protection", "/api/protection/measure", "'dry_run_locked'", "'dry_run_waiting'",
                  "'measuring'", "a.dry_run"):
        assert_test(token in js, f"app.js contains {token}")
    for fn in DRY_RUN_UI_FUNCTIONS:
        body = js_function_body(js, fn)
        assert_test(body != "", f"app.js defines {fn}")
        assert_test(body != "" and "innerHTML" not in body and "setHtml(" not in body,
                    f"{fn} does not use innerHTML/setHtml")
    resume = js_function_body(js, "dryRunResumeMeasure")
    assert_test("dryRunRenderMeasure(" in resume and "!== 'running'" not in resume,
                "dryRunResumeMeasure re-renders every ok measure state, not only running")
    assert_test("dryRunSetMeasureText('', false, false)" in resume and "dryRunMeasureRunning = false" in resume,
                "dryRunResumeMeasure clears stale measure UI when the GET fails")
    assert_test("dryRunStopPolling" in js_function_body(js, "cleanupCurrentView"),
                "cleanupCurrentView stops dry-run polling")


def main():
    test_engine()
    test_pump_control()
    test_guard()
    test_api()
    test_ui()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
