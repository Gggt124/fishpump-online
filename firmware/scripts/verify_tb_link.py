#!/usr/bin/env python3
"""
verify_tb_link.py

Source contract checks for the ThingsBoard link, milestone 1
(docs/superpowers/plans: 2026-11-18 thingsboard-link-m1).
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


def test_nvs_storage():
    print("\n--- Task 2: NVS storage of the link config ---")
    hdr = read(COMP_DIR / "nvs_store" / "include" / "nvs_store.h")
    src = read(COMP_DIR / "nvs_store" / "nvs_store.c")
    assert_test("nvs_store_load_tb_link(" in hdr and "nvs_store_save_tb_link(" in hdr,
                "nvs_store.h declares nvs_store_load_tb_link and nvs_store_save_tb_link")
    assert_test(re.search(r'#define\s+\w+\s+"tb_cfg"', src) is not None,
                'nvs_store.c uses the namespace "tb_cfg"')
    reset = function_body(src, "nvs_store_factory_reset", rettype=r"bool\s+")
    assert_test("tb_cfg" in reset, "factory reset erase list contains tb_cfg")
    save = function_body(src, "nvs_store_save_tb_link", rettype=r"bool\s+")
    assert_test("tb_link_config_valid(" in save, "nvs_store_save_tb_link calls tb_link_config_valid")


def main():
    test_nvs_storage()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
