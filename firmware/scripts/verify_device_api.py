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


if __name__ == "__main__":
    test_component()
    test_logging_and_lock()
    test_app_main()
    print(f"\nPassed: {passed_tests}  Failed: {failed_tests}")
    sys.exit(1 if failed_tests else 0)
