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


def main():
    test_engine()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
