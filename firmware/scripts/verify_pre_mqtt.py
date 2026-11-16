#!/usr/bin/env python3
"""
verify_pre_mqtt.py

Source contract checks for the pre-MQTT cleanup round
(docs/superpowers/specs/2026-11-16-pre-mqtt-cleanup-design.md).
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


def test_task1():
    print("\n--- Task 1: alarm hold reset, per-severity unacked, LED level ---")
    hdr = read(COMP_DIR / "alarm_rules" / "include" / "alarm_rules.h")
    ev = read(COMP_DIR / "alarm_rules" / "alarm_eval.c")
    assert_test(re.search(r"#define\s+ALARM_HOLD_RESET_MS\s+5000u", hdr) is not None,
                "ALARM_HOLD_RESET_MS is defined as 5000")
    assert_test("alarm_led_level_t alarm_counts_led_level(" in hdr, "alarm_counts_led_level is declared")
    counts = re.search(r"typedef struct \{[^}]*\} alarm_counts_t;", hdr)
    ctext = counts.group(0) if counts else ""
    assert_test("warning_unacked" in ctext and "info_unacked" in ctext,
                "alarm_counts_t has warning_unacked and info_unacked")
    assert_test("holding" in hdr and "hold_since_ms" in hdr, "alarm_runtime_t has holding and hold_since_ms")
    step = function_body(ev, "alarm_rule_step", r"bool\s+")
    assert_test("ALARM_HOLD_RESET_MS" in step, "alarm_rule_step applies ALARM_HOLD_RESET_MS on hold")


def test_task2():
    print("\n--- Task 2: rule blob v2 with record size ---")
    hdr = read(COMP_DIR / "alarm_rules" / "include" / "alarm_rules.h")
    blob = read(COMP_DIR / "alarm_rules" / "alarm_blob.c")
    nvs = read(COMP_DIR / "nvs_store" / "nvs_store.c")
    assert_test(re.search(r"#define\s+ALARM_BLOB_VERSION\s+2$", hdr, re.M) is not None, "ALARM_BLOB_VERSION is 2")
    assert_test("ALARM_BLOB_READ_MAX_BYTES" in hdr, "ALARM_BLOB_READ_MAX_BYTES is defined")
    dec = function_body(blob, "alarm_blob_decode", r"bool\s+")
    assert_test("ALARM_BLOB_V1" in dec, "alarm_blob_decode still reads v1 blobs")
    assert_test(re.search(r"#define\s+ALARM_BLOB_V1_REC_SIZE\s+\d+u?\b", hdr) is not None
                and "_Static_assert(sizeof(alarm_rule_t) >= ALARM_BLOB_V1_REC_SIZE" in hdr,
                "ALARM_BLOB_V1_REC_SIZE is a literal with a size guard")
    assert_test("ALARM_BLOB_V1_REC_SIZE" in dec, "v1 decode uses ALARM_BLOB_V1_REC_SIZE")
    assert_test("ALARM_BLOB_READ_MAX_BYTES" in nvs, "nvs_store.c loads with ALARM_BLOB_READ_MAX_BYTES")


def test_task3():
    print("\n--- Task 3: uplink send lock and clock hold ---")
    up = read(COMP_DIR / "uplink" / "uplink.c")
    fmt = read(COMP_DIR / "uplink_format" / "include" / "uplink_format.h")
    setter = function_body(up, "uplink_set_sink", r"esp_err_t\s+")
    assert_test("xSemaphoreTake(s.send_lock" in setter, "uplink_set_sink takes send_lock")
    assert_test(setter.find("s.send_lock") < setter.find("xSemaphoreTake(s.lock"),
                "uplink_set_sink takes send_lock before lock (lock order)")
    # every function that touches s.sink., sink_ready() or send_attribute() must take send_lock
    bad, empty, scanned = [], [], 0
    for m in re.finditer(r"^(?:static\s+)?[\w\s\*]+?\b(\w+)\s*\((?:[^;{()]|\([^()]*\))*\)\s*\{", up, re.M):
        name = m.group(1)
        if name in ("if", "for", "while", "switch"):
            continue
        body = function_body(up, name, r"(?:static\s+)?[\w\s\*]+?")
        if body == "":
            empty.append(name)  # a header was found but its body was not: the scan would be blind
            continue
        scanned += 1
        if "s.sink." not in body and "sink_ready()" not in body and "send_attribute(" not in body:
            continue
        if name in ("sink_ready", "send_attribute"):
            continue
        if "xSemaphoreTake(s.send_lock" not in body:
            bad.append(name)
    assert_test(not empty and scanned >= 8, "the s.sink. scan parsed every function body", ", ".join(empty))
    assert_test(not bad, "every user of s.sink. / sink_ready / send_attribute holds send_lock", ", ".join(bad))
    attrs = function_body(up, "do_attributes")
    assert_test("send_attribute(" in attrs and "xSemaphoreTake(s.send_lock" in attrs,
                "do_attributes (the send_attribute caller) holds send_lock")
    prev = function_body(up, "uplink_preview_json", r"char\s*\*\s*")
    assert_test(prev != "" and "send_lock" not in prev and "sink_ready" not in prev,
                "the preview neither takes send_lock nor calls sink_ready")
    assert_test("connected_cache" in prev and "s.connected_cache = connected" in attrs,
                "the preview reads connected_cache, which the task refreshes")
    assert_test("UPLINK_CLOCK_HOLD_MAX_MS" in fmt, "the clock hold has a time cap")
    assert_test("UPLINK_CLOCK_HOLD_MARGIN" in fmt and "uplink_forward_hold_for_clock" in fmt,
                "uplink_forward_hold_for_clock is declared")
    assert_test("uplink_clock_hold_step(" in function_body(up, "do_events"),
                "do_events calls uplink_clock_hold_step")
    fmtc = read(COMP_DIR / "uplink_format" / "uplink_format.c")
    assert_test("uplink_forward_hold_for_clock(" in function_body(fmtc, "uplink_clock_hold_step", r"uplink_clock_decision_t\s+"),
                "the hold step uses uplink_forward_hold_for_clock")
    assert_test("released = false" not in up and "active = false" not in up,
                "uplink.c does not touch the hold latch (it survives sink drops)")
    assert_test("waiting_for_clock" in function_body(up, "uplink_preview_json", r"char\s*\*\s*"),
                "the status JSON contains waiting_for_clock")
    init = function_body(up, "uplink_init", r"esp_err_t\s+")
    assert_test("vSemaphoreDelete(s.send_lock)" in init, "uplink_init deletes send_lock on failure")


def test_task4():
    print("\n--- Task 4: LED colour by severity, last_critical_seq ---")
    app = read(MAIN_DIR / "app_main.c")
    hdr = read(MAIN_DIR / "app_main.h")
    web = read(MAIN_DIR / "web_server.c")
    eng = read(COMP_DIR / "alarm_engine" / "alarm_engine.c")
    eng_h = read(COMP_DIR / "alarm_engine" / "include" / "alarm_engine.h")
    assert_test("alarm_counts_led_level(" in app, "the app_main alarm branch calls alarm_counts_led_level")
    assert_test("LED_STATE_ALARM_WARNING" in hdr and "LED_STATE_ALARM_INFO" in hdr,
                "LED_STATE_ALARM_WARNING and LED_STATE_ALARM_INFO are declared")
    leds = function_body(app, "update_status_leds")
    assert_test("LED_STATE_ALARM_WARNING" in leds and "LED_STATE_ALARM_INFO" in leds,
                "update_status_leds handles the warning and info states")
    names = function_body(app, "app_led_state_name", r"const\s+char\s*\*\s*")
    assert_test('"alarm_warning"' in names and '"alarm_info"' in names, "app_led_state_name names the new states")
    stg = app.find("LED_STATE_STAGING_PENDING;")
    alm = app.find("alarm_counts_led_level(")
    assert_test(0 <= stg < alm, "the staging branch comes before the alarm branch in the LED loop")
    assert_test(app.find("LED_STATE_BTN_HOLD_SHORT;") < alm and app.find("LED_STATE_FACTORY_RESET;") < alm,
                "button hold and factory reset come before the alarm branch")
    counts = function_body(web, "alarms_add_counts")
    assert_test('"warning_unacked"' in counts and '"info_unacked"' in counts,
                "alarms_add_counts emits warning_unacked and info_unacked")
    summ = function_body(web, "handle_api_alarms_summary")
    assert_test('"last_critical_seq"' in summ, "the summary handler emits last_critical_seq")
    assert_test("last_critical_seq" in eng_h, "alarm_engine_summary_t has last_critical_seq")
    rec = function_body(eng, "record_event_for")
    assert_test("s.last_critical_seq" in rec and "ALARM_SEV_CRITICAL" in rec,
                "record_event_for sets last_critical_seq for critical raised events")
    assert_test("s.last_critical_seq" in function_body(eng, "alarm_engine_get_summary", r"bool\s+"),
                "alarm_engine_get_summary copies last_critical_seq")


def test_task5():
    print("\n--- Task 5: logs and diagnostics ---")
    cool = read(COMP_DIR / "cooling_control" / "cooling_control.c")
    app = read(MAIN_DIR / "app_main.c")
    m = re.search(r"(.{0,300})ESP_LOGW\(TAG,\s*\"PT100 sensor fault:", cool, re.S)
    ctx = m.group(1) if m else ""
    guarded = re.search(r"if\s*\(\s*(?:!s_fault|entering_fault|s_failed_reads\s*==[^)]*)\)\s*\{\s*$", ctx)
    entering = "entering_fault" not in ctx or "entering_fault = !s_fault" in cool
    assert_test(m is not None and guarded is not None and entering,
                "the PT100 sensor fault warning is guarded to the transition into fault")
    assert_test("PT100 sensor fault cleared" in cool, "the fault cleared log stays")
    i = app.find("#if CONFIG_ESP_TASK_WDT_INIT")
    j = app.find("#else", i) if i >= 0 else -1
    k = app.find("#endif", j) if j >= 0 else -1
    assert_test(i >= 0 and j > i and k > j, "app_main has #if CONFIG_ESP_TASK_WDT_INIT ... #else ... #endif")
    if i >= 0 and j > i:
        cfg_part = app[i:j]
        assert_test("esp_task_wdt_reconfigure" in cfg_part and "esp_task_wdt_init" not in cfg_part,
                    "the configured-TWDT path reconfigures and never calls esp_task_wdt_init")
        assert_test("esp_task_wdt_init" in app[j:k], "the #else fallback still initialises TWDT")
    assert_test('"TWDT reconfigured (' in app, "the TWDT reconfigured INFO line exists")
    for f in ("hwm_alarm=", "hwm_uplink=", "hwm_dry_run="):
        assert_test(f in app, f"BOARD_DIAG contains {f}")
    for comp, fn in (("alarm_engine", "alarm_engine_stack_hwm"), ("uplink", "uplink_stack_hwm"),
                     ("dry_run_guard", "dry_run_guard_stack_hwm")):
        hdr = read(COMP_DIR / comp / "include" / f"{comp}.h")
        src = read(COMP_DIR / comp / f"{comp}.c")
        assert_test(f"int32_t {fn}(void);" in hdr, f"{comp}.h declares {fn}")
        body = function_body(src, fn, r"int32_t\s+")
        assert_test("uxTaskGetStackHighWaterMark" in body and "-1" in body,
                    f"{fn} reads the stored handle and returns -1 when absent")
        assert_test(re.search(r"xTaskCreatePinnedToCore\([^;]*,\s*&\w+(\.\w+)?,\s*\w+\)", src, re.S) is not None,
                    f"{comp} stores the task handle at creation")
    assert_test("alarm_engine_stack_hwm()" in app and "uplink_stack_hwm()" in app
                and "dry_run_guard_stack_hwm()" in app, "BOARD_DIAG calls all three getters")


def test_task6():
    print("\n--- Task 6: dry-run guard minors ---")
    g = read(COMP_DIR / "dry_run_guard" / "dry_run_guard.c")
    ws = read(MAIN_DIR / "web_server.c")
    js = read(MAIN_DIR / "static" / "app.js")
    ms = function_body(g, "dry_run_guard_measure_start", r"esp_err_t\s+")
    m = re.search(r"if\s*\(\s*!\s*flow_switch_get_status\(&fs\)\s*\)\s*\{[^}]*ESP_ERR_NOT_FOUND", ms, re.S)
    assert_test(m is not None, "measure_start returns ESP_ERR_NOT_FOUND when the flow switch cannot be read")
    assert_test(m is not None and "xSemaphoreGive(s_apply)" in m.group(0),
                "the NOT_FOUND path releases s_apply")
    h = function_body(ws, "handle_api_protection_measure_post")
    assert_test(re.search(r"ESP_ERR_NOT_FOUND[^;]*\{?[^}]*\"flow_unavailable\"[^;]*503", h, re.S) is not None,
                "web handler maps ESP_ERR_NOT_FOUND to 503 flow_unavailable")
    assert_test("flow_already_on" in h and "409" in h and "measure_unavailable" in h,
                "web handler keeps the 409 flow_already_on / measure_unavailable mapping")
    assert_test(re.search(r"is_start\s*\?\s*send_api_error\(req,\s*\"measure_unavailable\"", h) is None
                or "ESP_ERR_INVALID_STATE" in h, "unknown start errors are not mapped to measure_unavailable")
    assert_test(re.search(r"\"unavailable\"[^;]*503", h, re.S) is not None, "other start errors map to 503 unavailable")
    assert_test("flow_unavailable" in js and "อ่านสถานะ flow switch ไม่ได้ ตรวจสายแล้วลองใหม่" in js,
                "app.js shows the Thai flow_unavailable message")
    ac = function_body(g, "apply_change")
    assert_test("(void)alarm_engine_system_raise" not in ac and "(void)alarm_engine_system_clear" not in ac,
                "apply_change does not discard the alarm raise/clear results")
    assert_test("(void)alarm_engine_system_raise" not in g and "(void)alarm_engine_system_clear" not in g,
                "no alarm raise/clear result is discarded anywhere in the guard")
    tick = function_body(g, "guard_tick")
    assert_test("alarm_resync" in tick, "guard_tick retries a failed alarm raise/clear")
    rs = function_body(g, "alarm_resync")
    assert_test("alarm_engine_system_raise" in rs and "alarm_engine_system_clear" in rs
                and "s_lock" not in rs, "alarm_resync calls the engine without taking s_lock")
    assert_test("ESP_LOGW" in rs and "ESP_LOGI" in rs, "alarm_resync logs one WARN on failure and one INFO on recovery")


def main():
    test_task1()
    test_task2()
    test_task3()
    test_task4()
    test_task5()
    test_task6()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
