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


def main():
    test_engine()
    test_pump_control()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
