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


def test_time_sync():
    print("\n--- Task 3: SNTP wall clock ---")
    defaults = read(FIRMWARE_DIR / "sdkconfig.defaults")
    assert_test("CONFIG_LWIP_SNTP_MAX_SERVERS=2" in defaults,
                "sdkconfig.defaults sets CONFIG_LWIP_SNTP_MAX_SERVERS=2")
    src = read(COMP_DIR / "time_sync" / "time_sync.c")
    assert_test("pool.ntp.org" in src and "time.google.com" in src,
                "time_sync.c names both pool.ntp.org and time.google.com")
    assert_test("uplink_set_wall_clock(" in src, "time_sync.c calls uplink_set_wall_clock")
    app = read(MAIN_DIR / "app_main.c")
    w = app.find("wifi_manager_init(")
    t = app.find("time_sync_init()")
    assert_test(w != -1 and t != -1 and t > w,
                "app_main.c calls time_sync_init() after wifi_manager_init(")
    assert_test(re.search(r'esp_log_level_set\(\s*"time_sync"\s*,\s*ESP_LOG_INFO\s*\)', app) is not None,
                'app_main.c raises "time_sync" to ESP_LOG_INFO')


def test_tb_link_component():
    print("\n--- Task 4: tb_link component and wiring ---")
    src = read(COMP_DIR / "tb_link" / "tb_link.c")
    hdr = read(COMP_DIR / "tb_link" / "include" / "tb_link.h")
    assert_test(all(n in hdr for n in ("tb_link_init(", "tb_link_apply_config(", "tb_link_reprovision(",
                                       "tb_link_get_config(", "tb_link_get_status(", "tb_link_state_name(",
                                       "tb_link_stack_hwm(")),
                "tb_link.h declares the full public API")
    init = function_body(src, "tb_link_init", rettype=r"esp_err_t\s+")
    task = function_body(src, "tb_link_task", rettype=r"static\s+void\s+")
    handler = function_body(src, "mqtt_event_handler", rettype=r"static\s+void\s+")
    send = function_body(src, "sink_send", rettype=r"static\s+esp_err_t\s+")
    assert_test(bool(init) and bool(task) and bool(handler) and bool(send),
                "tb_link.c defines tb_link_init, tb_link_task, mqtt_event_handler, sink_send")
    assert_test(len(re.findall(r"\buplink_set_sink\s*\(", src)) == 1
                and re.search(r"\buplink_set_sink\s*\(", init) is not None,
                "uplink_set_sink( appears exactly once in tb_link.c, inside tb_link_init")
    # The task function plus the static task_* helpers (which must be called only from the task side).
    helper_names = re.findall(r"static\s+[\w\s\*]+?\b(task_\w+)\s*\(", src)
    task_side = task + "".join(function_body(src, h) for h in helper_names)
    assert_test(len(helper_names) >= 5 and "task_" in task, "task-side helpers task_* exist and the task uses them",
                str(helper_names))
    for fn in ("esp_mqtt_client_init", "esp_mqtt_client_destroy", "esp_mqtt_client_start"):
        total = len(re.findall(r"\b" + fn + r"\s*\(", src))
        in_task = len(re.findall(r"\b" + fn + r"\s*\(", task_side))
        assert_test(total >= 1 and total == in_task,
                    f"{fn} appears only inside tb_link_task and its task_* helpers",
                    f"total={total} in_task_side={in_task}")
    for h in helper_names:
        calls = len(re.findall(r"\b" + h + r"\s*\(", src)) - 1  # minus the definition
        calls_task_side = len(re.findall(r"\b" + h + r"\s*\(", task_side)) - 1
        assert_test(calls >= 1 and calls == calls_task_side, f"{h}() is called only from the task side",
                    f"calls={calls} task_side={calls_task_side}")
    assert_test(bool(handler) and not any(x in handler for x in ("uplink_", "esp_mqtt_client_destroy", "nvs_store_",
                                                                "esp_mqtt_client_stop", "esp_mqtt_client_start",
                                                                "esp_mqtt_client_init")),
                "event handler never calls uplink, client lifecycle or NVS functions")
    assert_test("esp_mqtt_client_enqueue" in send and "esp_mqtt_client_publish" not in send
                and "esp_mqtt_client_publish" not in src,
                "sink send uses esp_mqtt_client_enqueue, not esp_mqtt_client_publish")
    bad = [m for m in re.findall(r"ESP_LOG[EWIDV]\s*\([^;]*;", src)
           if re.search(r"\.token\b|->token\b|provision_secret", m)]
    assert_test("ESP_LOGI" in src and not bad, "no ESP_LOG call in tb_link.c takes the token or provision secret",
                str(bad))
    assert_test(re.search(r"disable_auto_reconnect\s*=\s*true", src) is not None,
                "client config disables esp-mqtt auto reconnect")
    assert_test("tb_backoff_next_ms(" in src and "tb_provision_parse(" in src and "tb_provision_build_request(" in src
                and "nvs_store_load_tb_link(" in src and "nvs_store_save_tb_link(" in src,
                "tb_link.c uses the logic and NVS APIs")
    assert_test("/provision/response" in src and "/provision/request" in src and "UPLINK_TOPIC_TELEMETRY" in src
                and "UPLINK_TOPIC_ATTRIBUTES" in src, "tb_link.c uses the provisioning and uplink topics")
    assert_test("wifi_manager_is_sta_connected" in task_side, "task gates on wifi_manager_is_sta_connected")
    assert_test("esp_crt_bundle_attach" in src and "mqtts" in src, "TLS option uses mqtts + crt bundle")
    yml = read(COMP_DIR / "tb_link" / "idf_component.yml")
    assert_test("espressif/mqtt" in yml, "tb_link/idf_component.yml depends on espressif/mqtt")
    cm = read(COMP_DIR / "tb_link" / "CMakeLists.txt")
    assert_test("tb_link_logic" in cm and "nvs_store" in cm and "uplink" in cm, "tb_link CMakeLists REQUIRES the deps")
    mcm = read(MAIN_DIR / "CMakeLists.txt")
    assert_test(re.search(r"^\s*tb_link\s*$", mcm, re.M) is not None, "main/CMakeLists.txt REQUIRES tb_link")
    app = read(MAIN_DIR / "app_main.c")
    assert_test(re.search(r'esp_log_level_set\(\s*"tb_link"\s*,\s*ESP_LOG_INFO\s*\)', app) is not None,
                'app_main.c raises "tb_link" to ESP_LOG_INFO')
    u = app.find("uplink_init()")
    t = app.find("tb_link_init()")
    w = app.find("wifi_manager_init(")
    ts = app.find("time_sync_init()")
    assert_test(u != -1 and t > u and t > w and t > ts,
                "app_main.c calls tb_link_init() after uplink_init(), wifi_manager_init() and time_sync_init()")
    assert_test("tb_hwm=%ld" in app and "tb_link_stack_hwm()" in app, "BOARD_DIAG has tb_hwm= fed by tb_link_stack_hwm()")


def main():
    test_nvs_storage()
    test_time_sync()
    test_tb_link_component()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
