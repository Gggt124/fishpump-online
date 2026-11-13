#!/usr/bin/env python3
"""
verify_sensor_integration.py

Source and asset contract checks for the PZEM-004T x2 + SEN0708 pH sensor
integration (docs/superpowers/specs/2026-11-13-pzem-ph-sensor-integration-design.md):
- REST routes and their auth / same-origin guards
- SSE power / water events and their worst-case payload size
- Sensors page markup, SPA routing, SSE reconnect and compressed assets
"""

import gzip
import re
import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
MAIN_DIR = FIRMWARE_DIR / "main"

passed_tests = 0
failed_tests = 0

# Must match web_server_telemetry_cb exactly (checked below).
SSE_POWER_FMT = ('{"ch":%u,"valid":%s,"fault":%s,"v":%.1f,"a":%.3f,"w":%.1f,'
                 '"wh":%.0f,"hz":%.1f,"pf":%.2f}')
SSE_WATER_FMT = '{"valid":%s,"fault":%s,"stable":%s,"ph":%.2f,"temp_c":%.1f}'
SSE_JSON_BUF = 256
SSE_PACKET_BUF = 384

POST_HANDLERS = {
    "/api/power/reset-energy": "handle_api_power_reset_energy",
    "/api/water/calibrate": "handle_api_water_calibrate",
    "/api/water/calibrate/cancel": "handle_api_water_calibrate_cancel",
    "/api/water/offset": "handle_api_water_offset",
}
GET_HANDLERS = {
    "/sensors": "handle_get_sensors",
    "/api/sensors/status": "handle_api_sensors_status",
}


def assert_test(condition, name, details=""):
    global passed_tests, failed_tests
    if condition:
        passed_tests += 1
        print(f"  [PASS] {name}")
    else:
        failed_tests += 1
        print(f"  [FAIL] {name} - {details}")


def c_to_py_format(fmt):
    return fmt.replace("%u", "%d")


def function_body(source, name):
    """Return the text of `static esp_err_t name(...) { ... }`, or ''."""
    m = re.search(r"static\s+esp_err_t\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
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


def test_routes_and_guards(ws):
    print("\n--- 1. Routes and guards ---")
    routes = routes_block(ws)
    assert_test(bool(routes), "s_routes[] table found")
    for uri, handler in {**GET_HANDLERS, **POST_HANDLERS}.items():
        entry = re.search(r'\.uri\s*=\s*"' + re.escape(uri) + r'"[^}]*?' + re.escape(handler), routes, re.S)
        assert_test(entry is not None, f"route {uri} -> {handler} registered")
    # Write handlers share one guard helper; it must hold both checks.
    guard = re.search(r"static\s+cJSON\s*\*\s*sensors_begin_post\s*\([^)]*\)\s*\{(.*?)\n\}", ws, re.S)
    guard_body = guard.group(1) if guard else ""
    assert_test("require_auth(req)" in guard_body, "sensors_begin_post requires auth")
    assert_test("is_same_origin(req, false)" in guard_body, "sensors_begin_post checks same origin")
    for uri, handler in POST_HANDLERS.items():
        body = function_body(ws, handler)
        assert_test(bool(body), f"{handler} defined")
        guarded = ("sensors_begin_post(req" in body) or (
            "require_auth(req)" in body and "is_same_origin(req" in body)
        assert_test(guarded, f"{handler} is guarded by auth + same-origin")
    body = function_body(ws, "handle_api_sensors_status")
    assert_test("require_auth(req)" in body, "handle_api_sensors_status requires auth")
    hw = function_body(ws, "api_hardware_map_send")
    assert_test("fixed_bus_pins" in hw, "/api/hardware/map exposes fixed_bus_pins")


def test_sse(ws):
    print("\n--- 2. SSE events ---")
    cb = function_body(ws, "web_server_telemetry_cb") or ws[ws.find("web_server_telemetry_cb"):]
    assert_test("case TELEMETRY_EVT_POWER" in cb and '"power"' in cb, "SSE power event emitted")
    assert_test("case TELEMETRY_EVT_WATER" in cb and '"water"' in cb, "SSE water event emitted")
    flat = re.sub(r'"\s*\n\s*"', "", cb)  # join C string literal continuations
    c_power = SSE_POWER_FMT.replace('"', '\\"')
    c_water = SSE_WATER_FMT.replace('"', '\\"')
    assert_test(c_power in flat, "SSE power format matches the contract")
    assert_test(c_water in flat, "SSE water format matches the contract")

    worst_power = c_to_py_format(SSE_POWER_FMT) % (
        1, "false", "false", 9999.9, 99.999, 99999.9, 9999999999.0, 99.9, -1.00)
    worst_water = c_to_py_format(SSE_WATER_FMT) % ("false", "false", "false", 14.00, -20.0)
    for name, payload in (("power", worst_power), ("water", worst_water)):
        n = len(payload.encode())
        assert_test(n < SSE_JSON_BUF, f"worst-case {name} JSON fits {SSE_JSON_BUF} B ({n} B)")
        packet = f"event: {name}\ndata: {payload}\n\n"
        assert_test(len(packet.encode()) < SSE_PACKET_BUF,
                    f"worst-case {name} SSE packet fits {SSE_PACKET_BUF} B ({len(packet)} B)")


def test_console_guard():
    print("\n--- 4. UART0 console guard ---")
    pm = (FIRMWARE_DIR / "components" / "power_monitor" / "power_monitor.c").read_text(encoding="utf-8")
    m = re.search(r"#if\s+CONFIG_ESP_CONSOLE_UART\b[^\n]*CONFIG_ESP_CONSOLE_UART_NUM\s*==\s*0[^\n]*\n\s*#error", pm)
    assert_test(m is not None and '#include "sdkconfig.h"' in pm,
                "power_monitor.c refuses to build when the console is on UART0")


def test_ui_poll_guard(js):
    print("\n--- 5. Sensors poll guard ---")
    m = re.search(r"function syncSensorsStatus\(force\) \{(.*?)\n\}", js, re.S)
    body = m.group(1) if m else ""
    assert_test("document.hidden && !force" in body, "syncSensorsStatus skips hidden tabs")
    assert_test("sensorsStatusRequestInFlight" in body, "syncSensorsStatus has an in-flight guard")


def test_ui():
    print("\n--- 3. Sensors page UI ---")
    html = (MAIN_DIR / "static" / "index.html").read_text(encoding="utf-8")
    js = (MAIN_DIR / "static" / "app.js").read_text(encoding="utf-8")
    for el in ('id="view-sensors"', 'id="nav-sensors"', 'id="m-nav-sensors"',
               'id="power-card-0"', 'id="power-card-1"', 'id="hw-fixed-bus-pins"'):
        assert_test(el in html, f"index.html has {el}")
    for fn in ("syncSensorsStatus", "handlePowerSseEvent", "handleWaterSseEvent", "renderSensorsStatus"):
        assert_test(("function " + fn) in js, f"app.js defines {fn}")
    assert_test("addEventListener('power'" in js and "addEventListener('water'" in js,
                "app.js listens to power and water SSE events")
    m = re.search(r"sseReconnectTimer\s*=\s*setTimeout\(function \(\) \{(.*?)\}, 5000\)", js, re.S)
    assert_test(m is not None and "'/sensors'" in m.group(1),
                "SSE reconnect also fires on /sensors")
    for name, marker in (("index.html", "view-sensors"), ("app.js", "/sensors")):
        gz = MAIN_DIR / "static_compressed" / f"{name}.gz"
        content = gzip.open(gz, "rb").read().decode("utf-8", "replace") if gz.exists() else ""
        assert_test(marker in content, f"static_compressed/{name}.gz contains {marker}")


def main():
    ws = (MAIN_DIR / "web_server.c").read_text(encoding="utf-8")
    test_routes_and_guards(ws)
    test_sse(ws)
    test_ui()
    test_console_guard()
    test_ui_poll_guard((MAIN_DIR / "static" / "app.js").read_text(encoding="utf-8"))
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
