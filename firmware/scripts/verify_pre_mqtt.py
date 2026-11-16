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


def main():
    test_task1()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
