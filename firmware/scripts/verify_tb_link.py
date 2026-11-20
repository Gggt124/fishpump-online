#!/usr/bin/env python3
"""
verify_tb_link.py

Source contract checks for the ThingsBoard link, milestone 1
(docs/superpowers/plans: 2026-11-18 thingsboard-link-m1).
"""

import json
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
    tick = function_body(src, "task_tick", rettype=r"static\s+void\s+")
    wifi_fast = re.search(r"CONNECT(ED|ING)[^;{]*(\|\||&&)?[^;{]*!\s*wifi_manager_is_sta_connected\(\)|"
                          r"!\s*wifi_manager_is_sta_connected\(\)[^;{]*CONNECT(ED|ING)", tick) is not None
    assert_test(wifi_fast, "task_tick drops a CONNECTING/CONNECTED link at once when Wi-Fi is lost",
                "no !wifi_manager_is_sta_connected() guard tied to the CONNECTING/CONNECTED states")
    conn_case = re.search(r"case\s+TB_STATE_CONNECTED\s*:(.*?)(?=case\s+TB_STATE_\w+\s*:|default\s*:)", tick, re.S)
    assert_test(conn_case is not None and "atomic_load(&s.connected)" in conn_case.group(1)
                and "task_fail(" in conn_case.group(1) and "lost_since_ms" in conn_case.group(1),
                "task_tick CONNECTED case watches the connected flag and fails the link when it stays down")
    assert_test(re.search(r"\besp_mqtt_client_reconnect\s*\(", src) is not None,
                "token client is kept and reconnected with esp_mqtt_client_reconnect")
    defaults_txt = read(FIRMWARE_DIR / "sdkconfig.defaults")
    m = re.search(r"^CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS=(\d+)\s*$", defaults_txt, re.M)
    assert_test(m is not None and int(m.group(1)) >= 300000
                and re.search(r"^CONFIG_MQTT_USE_CUSTOM_CONFIG=y\s*$", defaults_txt, re.M) is not None,
                "sdkconfig.defaults sets the esp-mqtt outbox expiry >= 300000 ms (custom config enabled)")
    fail_body = function_body(src, "task_fail", rettype=r"static\s+void\s+")
    assert_test(re.search(r"if\s*\([^)]*prov_mode[^)]*\)\s*\{\s*task_teardown\(", fail_body) is not None
                and len(re.findall(r"task_teardown\(", fail_body)) == 1,
                "task_fail tears the client down only for provisioning sessions (token client is kept)")
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
    assert_test("tb_backoff_after_fail_ms(" in src and "tb_provision_parse(" in src and "tb_provision_build_request(" in src
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


def test_web_card_and_api():
    print("\n--- Task 6: ThingsBoard card and /api/tb endpoints ---")
    ws = read(MAIN_DIR / "web_server.c")
    html = read(MAIN_DIR / "static" / "index.html")
    js = read(MAIN_DIR / "static" / "app.js")

    def route(uri, method):
        return re.search(r'\{\s*\.uri\s*=\s*"' + re.escape(uri) + r'"\s*,\s*\.method\s*=\s*' + method
                         + r'\s*,\s*\.handler\s*=\s*(\w+)\s*\}', ws)

    g, p, r = route("/api/tb", "HTTP_GET"), route("/api/tb", "HTTP_POST"), route("/api/tb/reprovision", "HTTP_POST")
    assert_test(bool(g) and bool(p) and bool(r), "routes GET /api/tb, POST /api/tb, POST /api/tb/reprovision registered")
    gb = function_body(ws, g.group(1)) if g else ""
    if gb:  # the GET handler delegates to these helpers; the secret-free rule covers them too
        gb += function_body(ws, "tb_link_send_view") + function_body(ws, "tb_link_build_view", rettype=r"static\s+cJSON\s*\*\s*")
    pb = function_body(ws, p.group(1)) if p else ""
    rb = function_body(ws, r.group(1)) if r else ""
    assert_test("api_begin_json_post(" in pb and "tb_link_config_from_json(" in pb and "tb_link_apply_config(" in pb
                and "invalid_field" in pb and "save_failed" in pb,
                "POST /api/tb uses api_begin_json_post, from_json, apply_config, invalid_field and save_failed")
    assert_test("require_auth(" in gb and "tb_link_config_to_json(" in gb and "tb_link_get_status(" in gb,
                "GET /api/tb requires auth and builds from config_to_json and get_status")
    assert_test(bool(gb) and not re.search(r"\.token\b|->token\b|provision_secret", gb),
                "GET /api/tb handler never references the token or provision secret")
    assert_test("tb_link_reprovision(" in rb and "no_provision_credentials" in rb and "409" in rb
                and "api_begin_json_post(" in rb,
                "POST /api/tb/reprovision checks origin/auth, 409 no_provision_credentials, calls tb_link_reprovision")
    leaks = [m for m in re.findall(r"ESP_LOG[EWIDV]\s*\([^;]*;", pb + gb + rb)
             if re.search(r"token|secret", m)]
    assert_test(bool(pb) and not leaks, "no log call in the /api/tb handlers mentions the token or secret", str(leaks))
    mcm = read(MAIN_DIR / "CMakeLists.txt")
    assert_test(re.search(r"^\s*tb_link_logic\s*$", mcm, re.M) is not None, "main/CMakeLists.txt REQUIRES tb_link_logic")
    a, b = html.find('id="tb-link-card"'), html.find('id="uplink-preview-card"')
    assert_test(a != -1 and b != -1 and a < b, "index.html has tb-link-card before uplink-preview-card")
    assert_test("ThingsBoard ลงทะเบียนชื่ออุปกรณ์ได้ครั้งเดียว ถ้าอุปกรณ์ชื่อนี้มีอยู่แล้วใน ThingsBoard "
                "ต้องลบออกก่อน ไม่อย่างนั้นการลงทะเบียนจะล้มเหลว ดำเนินการต่อ?" in js,
                "app.js has the exact reprovision confirm text")
    poll = function_body(js, "tbLinkPollTick", rettype=r"")
    assert_test("document.hidden" in poll and "tbLinkRefresh" in poll, "tb poll function checks document.hidden")
    cleanup = function_body(js, "cleanupCurrentView", rettype=r"")
    assert_test("tbLinkInterval" in cleanup, "cleanupCurrentView stops the tb poll timer")
    tbjs = "\n".join(function_body(js, n, rettype=r"") for n in
                      ("tbLinkRender", "tbLinkSave", "tbLinkReprovision", "tbLinkRefresh"))
    assert_test("/api/tb/reprovision" in tbjs and "/api/tb'" in tbjs and "innerHTML" not in tbjs,
                "tb card JS calls /api/tb and /api/tb/reprovision and never uses innerHTML")
    assert_test("window.confirm(" in function_body(js, "tbLinkReprovision", rettype=r""),
                "reprovision asks window.confirm before posting")


def test_rpc_transport():
    print("\n--- Milestone 2 Task 3: ThingsBoard RPC transport ---")
    src = read(COMP_DIR / "tb_link" / "tb_link.c")
    hdr = read(COMP_DIR / "tb_link" / "include" / "tb_link.h")
    cm = read(COMP_DIR / "tb_link" / "CMakeLists.txt")
    app = read(MAIN_DIR / "app_main.c")
    handle = function_body(src, "task_handle_cmd")
    connected = handle[handle.find("case CMD_CONNECTED:"):handle.find("case CMD_SUBSCRIBED:")]
    sub_at = connected.find("TB_RPC_SUB_TOPIC")
    assert_test(sub_at != -1 and sub_at < connected.find("} else {") and ", 1)" in connected[sub_at:sub_at + 60],
                "RPC subscribe (QoS 1) is in the token branch of task_handle_cmd CMD_CONNECTED")
    assert_test('#define TB_RPC_SUB_TOPIC TB_RPC_REQ_PREFIX "+"' in src,
                "subscribe topic is the request prefix + '+'")
    handler = function_body(src, "mqtt_event_handler")
    ondata = function_body(src, "rpc_on_data")
    assert_test(bool(ondata) and "device_api_call" not in ondata and "device_api_call" not in handler,
                "event handler RPC branch never calls device_api_call")
    assert_test(re.search(r"xQueueSend\s*\(\s*s\.rpc_q\s*,\s*&it\s*,\s*0\s*\)", ondata) is not None,
                "event handler queues the request with xQueueSend and zero wait")
    assert_test("rpc_on_data(" in handler, "MQTT_EVENT_DATA routes RPC topics to rpc_on_data")
    assert_test("TB_RPC_BODY_TOO_LARGE" in ondata and "TB_RPC_BODY_BUSY" in ondata
                and "current_data_offset" in ondata and "total_data_len > ev->data_len" in ondata
                and "TB_RPC_MAX_REQUEST" in ondata,
                "handler answers too_large (fragmented or over cap, first fragment only) and busy")
    assert_test("#define TB_RPC_QUEUE_LEN 4" in src and "#define TB_RPC_TASK_STACK 6144" in src
                and "#define TB_RPC_TASK_PRIORITY 2" in src and '"tb_rpc"' in src,
                "tb_rpc task: 6144 B stack, priority 2, queue depth 4")
    bodies = {
        "too_large": r'"{\"ok\":false,\"error\":\"too_large\"}"',
        "busy": r'"{\"ok\":false,\"error\":\"busy\"}"',
        "invalid_json": r'"{\"ok\":false,\"error\":\"invalid_json\",\"message\":\"Expected a JSON object\"}"',
        "unknown_method": r'"{\"ok\":false,\"error\":\"unknown_method\"}"',
        "memory": r'"{\"ok\":false,\"error\":\"memory\",\"message\":\"JSON allocation failed\"}"',
    }
    for name, body in bodies.items():
        assert_test(body in src, f"fixed reply body present: {name}")
    pub = function_body(src, "rpc_publish")
    take = pub.find("xSemaphoreTake(s.client_lock")
    assert_test(take != -1 and take < pub.find("s.client_pub != NULL") and "gen == atomic_load(&s.gen)" in pub
                and take < pub.find("esp_mqtt_client_enqueue(s.client_pub")
                and pub.find("esp_mqtt_client_enqueue") < pub.find("xSemaphoreGive(s.client_lock)"),
                "tb_rpc publishes under client_lock, checking client_pub and the generation inside the lock")
    proc = function_body(src, "rpc_process")
    assert_test("device_api_call(method, params, DAPI_SRC_TB, &out)" in proc and "rpc_publish(" in proc
                and "esp_mqtt_client_enqueue" not in proc and "client_pub" not in proc,
                "rpc_process calls device_api_call and publishes only through rpc_publish")
    assert_test("TB_RPC_BODY_MEMORY" in proc and 'ESP_LOGI(TAG, "rpc %s id=%lu -> %s' in proc,
                "rpc_process has the memory fallback body and logs 'rpc <method> id=<id> -> <status>'")
    assert_test(not re.search(r"ESP_LOG[EWIDV]\s*\([^;]*(params|payload)", proc + ondata),
                "RPC path never logs params or payload")
    td = function_body(src, "task_teardown")
    assert_test("rpc_queue_drain()" in td and td.find("esp_mqtt_client_destroy") < td.find("rpc_queue_drain()"),
                "task_teardown drains rpc_q after destroying the client")
    drain = function_body(src, "rpc_queue_drain")
    assert_test("xQueueReceive(s.rpc_q" in drain and "rpc_item_free(" in drain,
                "rpc_queue_drain frees every queued item")
    assert_test("tb_rpc_task" in function_body(src, "tb_link_init", rettype=r"esp_err_t\s+"),
                "tb_link_init creates the tb_rpc task")
    assert_test("tb_link_rpc_stack_hwm" in hdr and "tb_link_rpc_stack_hwm" in src,
                "tb_link exports tb_link_rpc_stack_hwm")
    assert_test("device_api" in cm, "tb_link CMakeLists REQUIRES device_api")
    assert_test("tb_hwm=%ld tb_rpc_hwm=%ld" in app and "tb_link_rpc_stack_hwm()" in app,
                "BOARD_DIAG prints tb_rpc_hwm= right after tb_hwm=")


def _take_give_spans(body):
    """Text between each xSemaphoreTake and its next xSemaphoreGive."""
    spans = []
    for m in re.finditer(r"xSemaphoreTake\s*\(", body):
        g = body.find("xSemaphoreGive", m.end())
        spans.append(body[m.start():g if g != -1 else len(body)])
    return spans


def test_uplink_state():
    print("\n--- Milestone 3 Task 3: uplink sends state attributes and status telemetry ---")
    hdr = read(COMP_DIR / "uplink" / "include" / "uplink.h")
    src = read(COMP_DIR / "uplink" / "uplink.c")
    cm = read(COMP_DIR / "uplink" / "CMakeLists.txt")
    assert_test("uplink_set_state_source" in hdr and "uplink_note_settings_changed" in hdr,
                "uplink.h declares uplink_set_state_source and uplink_note_settings_changed")
    assert_test("#define UPLINK_TASK_STACK 6144" in src, "UPLINK_TASK_STACK is 6144")
    for name in ("uplink_attr_cache_reset", "uplink_state_rebuild_due", "uplink_status_poll_due",
                 "uplink_status_send_due", "UPLINK_STATE_ATTR_MAX_BYTES"):
        assert_test(name in src, f"uplink.c uses {name}")
    for fn, builder in (("do_state", "build_attributes("), ("do_status", "build_status(")):
        body = function_body(src, fn)
        spans = _take_give_spans(body)
        assert_test(bool(body) and builder in body and not any(builder in sp for sp in spans),
                    f"{fn} calls {builder[:-1]} and never between a Take and its Give")
    ds = function_body(src, "do_status")
    i_pa = ds.find("status_polled_ever = true")
    i_bs = ds.find("build_status(")
    assert_test(i_pa != -1 and i_bs != -1 and i_pa < i_bs,
                "do_status sets status_polled_ever before calling build_status (offline poll backs off)")
    assert_test('"attribute %s sent (%u B)"' in src, "uplink.c logs each sent attribute")
    assert_test('\\"last_status\\":' in src, "preview format string has last_status")
    m = re.search(r"REQUIRES([^)]*)\)", cm)
    assert_test(m is not None and "cjson" in m.group(1).split(), "uplink CMake REQUIRES contain cjson")


def test_tb_state():
    print("\n--- Milestone 3 Task 4: tb_state builders, wiring and the HTTP-only triggers ---")
    cm = read(MAIN_DIR / "CMakeLists.txt")
    app = read(MAIN_DIR / "app_main.c")
    web = read(MAIN_DIR / "web_server.c")
    src = read(MAIN_DIR / "tb_state.c")
    assert_test('"tb_state.c"' in cm, "main CMake SRCS contain tb_state.c")
    i_dapi = app.find("device_api_init()")
    i_state = app.find("tb_state_init()")
    assert_test(i_dapi != -1 and i_state > i_dapi, "app_main.c calls tb_state_init() after device_api_init()")
    assert_test('esp_log_level_set("tb_state", ESP_LOG_INFO)' in app, "app_main.c sets the tb_state log level")
    assert_test("uplink_set_state_source" in src and "device_api_set_write_hook" in src,
                "tb_state.c registers the state source and the write hook")
    assert_test("== DAPI_OK" in src, "tb_state.c compares each read with == DAPI_OK")
    low = src.lower()
    for word in ("password", "token", "secret", "provision"):
        assert_test(word not in low, f"tb_state.c does not contain '{word}'")
    for fn in ("handle_api_change_password", "handle_api_auth_credentials", "handle_api_confirm_post",
               "handle_api_confirm_delete", "handle_api_ota", "handle_api_tb_post"):
        assert_test("uplink_note_settings_changed()" in function_body(web, fn),
                    f"{fn} calls uplink_note_settings_changed()")
    assert_test("uplink_json_materialize(" in function_body(src, "read_method"),
                "tb_state.c read_method calls uplink_json_materialize (Raw replies become objects)")
    af = function_body(src, "add_filtered")
    assert_test("uplink_json_without" not in af and "cJSON_DeleteItemFromObjectCaseSensitive" in af,
                "tb_state.c add_filtered drops keys in place, no deep copy")
    for key in ("cfg_pump", "cfg_cooling", "cfg_protection", "hardware_map", "wifi", "tb_link"):
        assert_test(f'"{key}"' in src, f'tb_state.c names the attribute key "{key}"')


def test_state_cases_drift():
    print("\n--- Milestone 3 Task 5: state_cases.json status_keys follow the firmware ---")
    fmt = read(COMP_DIR / "uplink_format" / "uplink_format.c")
    table = re.findall(r'\{UPLINK_SRC_\w+\s*,\s*"[^"]+"\s*,\s*"([^"]+)"\s*,\s*(?:true|false)\s*\}', fmt)
    state = read(MAIN_DIR / "tb_state.c")
    body = function_body(state, "build_status", rettype=r"static\s+cJSON\s*\*\s*")
    extra = re.findall(r'(?:add_hwm|cJSON_AddNumberToObject)\s*\(\s*out\s*,\s*"(\w+)"', body)
    cases_path = FIRMWARE_DIR.parent / "thingsboard" / "state_cases.json"
    try:
        keys = json.loads(read(cases_path)).get("status_keys")
    except ValueError:
        keys = None
    assert_test(len(table) >= 20 and len(extra) == 7,
                "parsed the uplink_status_fields table and the tb_state keys", f"table={len(table)} extra={len(extra)}")
    assert_test(keys == table + extra, "state_cases.json status_keys = table keys in order + tb_state keys",
                f"expected {table + extra}, got {keys}")


def test_remote_wifi_link():
    print("\n--- Milestone 4 Task 2: tb_link defers an action until the reply is acked ---")
    hdr = read(COMP_DIR / "tb_link" / "include" / "tb_link.h")
    src = read(COMP_DIR / "tb_link" / "tb_link.c")
    assert_test("tb_link_defer_after_reply(void (*action)(void))" in hdr, "tb_link.h declares tb_link_defer_after_reply")
    assert_test("tb_link_set_connected_hook(void (*hook)(void))" in hdr, "tb_link.h declares tb_link_set_connected_hook")
    assert_test("case MQTT_EVENT_PUBLISHED" in src, "event handler has a case MQTT_EVENT_PUBLISHED")
    i_case = src.find("case CMD_CONNECTED:")
    i_hook = src.find("connected_hook", i_case)
    i_sub = src.find("TB_RPC_SUB_TOPIC", i_case)
    assert_test(i_case != -1 and i_hook != -1 and i_sub != -1 and i_hook < i_sub,
                "CMD_CONNECTED calls the connected hook before subscribing to TB_RPC_SUB_TOPIC")
    tf = function_body(src, "task_fail")
    assert_test("tb_backoff_after_fail_ms" in tf and "nvs_store_get_staging_type" in tf and "tb_backoff_next_ms" not in tf,
                "task_fail uses tb_backoff_after_fail_ms and nvs_store_get_staging_type, not tb_backoff_next_ms")
    assert_test("tb_after_reply_ready" in src, "the tb_rpc code calls tb_after_reply_ready")


def test_wifi_remote():
    print("\n--- Milestone 4 Task 3: wifi_remote commits the staged network on the ThingsBoard connect ---")
    cm = read(MAIN_DIR / "CMakeLists.txt")
    app = read(MAIN_DIR / "app_main.c")
    src = read(MAIN_DIR / "wifi_remote.c")
    assert_test('"wifi_remote.c"' in cm, "wifi_remote.c is in main SRCS")
    i_init = app.find("tb_link_init()")
    i_hook = app.find("tb_link_set_connected_hook(wifi_remote_on_tb_connected)")
    assert_test(i_init != -1 and i_hook != -1 and i_hook < i_init, "app_main registers the hook before tb_link_init")
    assert_test('esp_log_level_set("wifi_remote", ESP_LOG_INFO)' in app, "app_main raises the wifi_remote log tag")
    body = function_body(src, "wifi_remote_on_tb_connected", r"void\s+")
    order = ["nvs_store_get_staging_type", "nvs_store_save_wifi_profile", "nvs_store_set_wifi_auto_connect",
             "nvs_store_commit_staging", "g_cancel_rollback_timer", "uplink_note_settings_changed"]
    pos = [body.find(n) for n in order]
    assert_test(body != "" and all(x != -1 for x in pos) and pos == sorted(pos),
                "wifi_remote_on_tb_connected saves the profile and sets auto-connect before clearing the staging, then cancels the timer",
                f"positions={pos}")
    i_cur = body.find("esp_wifi_sta_get_ap_info")
    i_cmp = body.find("strcmp", i_cur) if i_cur != -1 else -1
    i_com = body.find("nvs_store_save_wifi_profile")
    assert_test(i_cur != -1 and i_cmp != -1 and i_com != -1 and i_cur < i_cmp < i_com,
                "wifi_remote_on_tb_connected compares the current STA SSID with the staged one before saving",
                f"cur={i_cur} cmp={i_cmp} save={i_com}")
    assert_test("esp_wifi_get_config" in src and "active_password(" in body and body.find("active_password(") < i_com,
                "wifi_remote_on_tb_connected also compares the active station password before saving")
    assert_test("nvs_store_get_staged_wifi" in body and "nvs_open" not in src,
                "wifi_remote.c reads the staged credentials through nvs_store, not raw NVS keys")
    assert_test("memset" in body, "wifi_remote_on_tb_connected clears its password buffers")
    ns = read(FIRMWARE_DIR / "components" / "nvs_store" / "nvs_store.c")
    assert_test("prof_load_stored" in ns, "nvs_store has a private overlay-free profile loader")
    for fn in ["nvs_store_save_wifi_profile", "nvs_store_forget_wifi_profile", "nvs_store_set_wifi_auto_connect",
               "nvs_store_migrate_legacy_wifi"]:
        fb = function_body(ns, fn, r"bool\s+")
        assert_test(fb != "" and "prof_load_stored" in fb and "nvs_store_load_wifi_profiles(" not in fb,
                    f"{fn} reads the real list, not the staged overlay")
    cb = function_body(ns, "nvs_store_commit_staging", r"uint8_t\s+")
    assert_test("nvs_store_save_wifi_profile(" in cb and "return 0" in cb[cb.find("nvs_store_save_wifi_profile("):],
                "nvs_store_commit_staging returns 0 and keeps the staging when the profile write fails")
    da = read(MAIN_DIR / "dapi_wifi.c")
    assert_test("stg_type = 0xFF" in da, "dapi_wifi_connect treats an unreadable staging type as active (fail closed)")
    assert_test("could not clear the staging" in da, "dapi_wifi_connect warns when the rollback of the staging fails")
    rb = function_body(src, "wifi_remote_restart_for_stage", r"void\s+")
    assert_test('"WIFI_REMOTE_STAGE"' in rb and "esp_restart" in rb and "session_invalidate_all" in rb,
                "wifi_remote_restart_for_stage sets the detail, invalidates sessions and restarts")
    bad = [l for l in src.splitlines() if "ESP_LOG" in l and "pass" in l.lower()]
    assert_test(src != "" and not bad, "wifi_remote.c has no ESP_LOG line mentioning pass", str(bad))


def test_wifi_connect_remote():
    print("\n--- Milestone 4 Task 4: wifi.connect and wifi.profiles.save from ThingsBoard ---")
    src = read(MAIN_DIR / "dapi_wifi.c")
    body = function_body(src, "dapi_wifi_connect")
    i_conn = body.find("already_connected")
    i_tb = body.find("DAPI_SRC_TB", i_conn)
    i_sta = body.find("wifi_manager_connect_sta")
    branch = body[i_tb:i_sta] if i_tb != -1 and i_sta != -1 else ""
    names = ["dapi_wifi_connect_refusal", "nvs_store_stage_wifi", "tb_link_defer_after_reply(wifi_remote_restart_for_stage)",
             "nvs_store_rollback_staging", 'staged\\"', "dry_run_guard_get_measure", "stage_failed"]
    miss = [n for n in names if n not in branch]
    assert_test(branch != "" and not miss, "the TB branch of dapi_wifi_connect (after already_connected, before the HTTP connect) has the stage steps", f"missing={miss}")
    pos = [branch.find(n) for n in ("dapi_wifi_connect_refusal", "nvs_store_stage_wifi", "tb_link_defer_after_reply")]
    assert_test(all(x != -1 for x in pos) and pos == sorted(pos), "refusal, then stage, then defer", f"{pos}")
    save = function_body(src, "dapi_wifi_profiles_save")
    assert_test("dapi_wifi_save_empty_blocked(" in save, "dapi_wifi_profiles_save calls dapi_wifi_save_empty_blocked")
    bad = [l for l in src.splitlines() if "ESP_LOG" in l and re.search(r",\s*password", l)]
    assert_test(src != "" and not bad, "no ESP_LOG line in dapi_wifi.c prints the password variable", str(bad))


def test_ota_link():
    print("\n--- Milestone 5 Task 2: tb_link data/session hooks, publish, OTA subscriptions, version ---")
    hdr = read(COMP_DIR / "tb_link" / "include" / "tb_link.h")
    src = read(COMP_DIR / "tb_link" / "tb_link.c")
    cmake = read(FIRMWARE_DIR / "CMakeLists.txt")
    gitignore = read(FIRMWARE_DIR.parent / ".gitignore")
    assert_test("tb_link_data_t" in hdr and "const char *topic; int topic_len;" in hdr and
                "int offset;" in hdr and "int total;" in hdr, "tb_link.h declares tb_link_data_t")
    assert_test("tb_link_set_data_hook(void (*hook)(const tb_link_data_t *d))" in hdr, "tb_link.h declares tb_link_set_data_hook")
    assert_test("tb_link_set_session_hook(void (*hook)(void))" in hdr, "tb_link.h declares tb_link_set_session_hook")
    assert_test("tb_link_publish(const char *topic, const char *data, int len, int qos)" in hdr, "tb_link.h declares tb_link_publish")
    assert_test("tb_link_is_connected(void)" in hdr, "tb_link.h declares tb_link_is_connected")
    i_case = src.find("case CMD_CONNECTED:")
    order = ["connected_hook", "TB_RPC_SUB_TOPIC", "TB_FW_ATTR_TOPIC", "TB_FW_ATTR_RESP_PREFIX", "TB_FW_RESP_PREFIX", "session_hook"]
    i_end = src.find("    case ", i_case + 1)  # the next case label ends the CMD_CONNECTED body
    case_body = src[i_case:i_end] if i_case != -1 and i_end != -1 else ""
    pos, last = [], 0
    for tok in order:
        last = case_body.find(tok, last + 1) if last != -1 else -1
        pos.append(last)
    assert_test(case_body != "" and all(p != -1 for p in pos),
                "CMD_CONNECTED: connected hook, RPC, fw attr, attr response, fw chunk subscribes, session hook in order", str(pos))
    pub = function_body(src, "tb_link_publish", r"int\s+")
    i_take = pub.find("xSemaphoreTake(s.client_lock")
    i_enq = pub.find("esp_mqtt_client_enqueue")
    i_give = pub.find("xSemaphoreGive(s.client_lock")
    assert_test(i_take != -1 and i_take < i_enq < i_give, "tb_link_publish enqueues while holding client_lock")
    assert_test("TB_RPC_LOCK_MS" in pub and "s.client_pub" in pub and "s.connected" in pub,
                "tb_link_publish uses the rpc_publish guard")
    assert_test("mc.buffer.size = 2048" in src, "esp-mqtt buffer.size stays 2048")
    ok, depth = True, 0
    for line in src.splitlines():
        t = line.strip()
        if t.startswith("#if"):
            depth += 1
            if "TB_LINK_TEST_BLOCK_CONNECT" in t and not t.startswith("#ifdef"):
                ok = False
        elif t.startswith("#endif"):
            depth -= 1
        elif "TB_LINK_TEST_BLOCK_CONNECT" in t and not t.startswith("#ifdef"):
            ok = False
    assert_test("#ifdef TB_LINK_TEST_BLOCK_CONNECT" in src and ok, "every TB_LINK_TEST_BLOCK_CONNECT is inside an #ifdef")
    assert_test("version.txt" in cmake and "PROJECT_VER" in cmake, "CMakeLists reads version.txt")
    assert_test(re.search(r'option\(TB_TEST_BLOCK_CONNECT.*OFF\)', cmake) is not None,
                "CMakeLists declares TB_TEST_BLOCK_CONNECT default OFF")
    i_proj = cmake.find("project(fishpump_firmware")
    i_def = cmake.find('idf_build_set_property(COMPILE_DEFINITIONS "TB_LINK_TEST_BLOCK_CONNECT=1" APPEND)')
    assert_test(i_def != -1 and i_proj != -1 and i_def < i_proj and "add_compile_definitions(TB_LINK" not in cmake,
                "the test-block define is set with idf_build_set_property before project()")
    mh = function_body(src, "mqtt_event_handler")
    assert_test("ev->topic == NULL" in mh and "ROUTE_PROV" in mh,
                "a provision continuation fragment (no topic) returns without queueing a command")
    assert_test("VERSION 0." not in cmake, "CMakeLists has no literal VERSION 0.")
    assert_test("firmware/build_block/" in gitignore.splitlines(), ".gitignore lists firmware/build_block/")


def test_ota_lock():
    print("\n--- Milestone 5 Task 3: OTA writer lock and pending version in NVS ---")
    cmake = read(MAIN_DIR / "CMakeLists.txt")
    web = read(MAIN_DIR / "web_server.c")
    nvs = read(COMP_DIR / "nvs_store" / "nvs_store.c")
    lock_c = read(MAIN_DIR / "ota_lock.c")
    lock_h = read(MAIN_DIR / "ota_lock.h")
    assert_test('"ota_lock.c"' in cmake, "ota_lock.c is in SRCS")
    assert_test("atomic_compare_exchange_strong" in lock_c and "atomic_bool" in lock_c,
                "ota_lock.c uses an atomic_bool compare-exchange")
    assert_test("ota_lock_claim(void)" in lock_h and "ota_lock_release(void)" in lock_h and "ota_lock_held(void)" in lock_h,
                "ota_lock.h declares claim, release, held")
    body = function_body(web, "handle_api_ota")
    i_claim = body.find("ota_lock_claim()")
    i_begin = body.find("esp_ota_begin(")
    assert_test(i_claim != -1 and i_begin != -1 and i_claim < i_begin, "handle_api_ota claims the lock before esp_ota_begin")
    i_sb = body.find("esp_ota_set_boot_partition(")
    i_clr = body.find("nvs_store_clear_tb_ota_pending(")
    i_rb = body.find("schedule_delayed_reboot(")
    assert_test(-1 not in (i_sb, i_clr, i_rb) and i_sb < i_clr < i_rb,
                "handle_api_ota clears the ThingsBoard pending version between set boot partition and the reboot")
    i_busy = body.find('ota_busy')
    assert_test(i_busy != -1 and "409 Conflict" in body[i_busy:i_busy + 120], "ota_busy comes with 409 Conflict")
    i_after = body.find("409 Conflict", i_busy) if i_busy != -1 else -1
    tail = body[i_after:] if i_after != -1 else ""
    n_ret = len(re.findall(r"\breturn\b", tail))
    n_rel = len(re.findall(r"\bota_lock_release\(\)", tail))
    assert_test(n_ret > 1 and n_rel == n_ret - 1, "every error return after the claim releases the lock, the success return does not", f"{n_rel} releases, {n_ret} returns")
    i_boot = body.find("esp_ota_set_boot_partition(")
    i_ok = body.rfind(":true}")
    between = body[body.find("OTA successful", i_boot):i_ok] if i_boot != -1 and i_ok != -1 else "ota_lock_release()"
    assert_test("ota_lock_release" not in between, "the lock stays held on the success path until the reboot")
    assert_test('"tb_ota_ver"' in nvs, 'nvs_store.c uses the key "tb_ota_ver"')
    hdr = read(COMP_DIR / "nvs_store" / "include" / "nvs_store.h")
    assert_test("nvs_store_set_tb_ota_pending(" in hdr and "nvs_store_get_tb_ota_pending(" in hdr and
                "nvs_store_clear_tb_ota_pending(" in hdr, "nvs_store.h declares the pending-OTA functions")


def test_tb_ota():
    print("\n--- Milestone 5 Task 4: tb_ota download, verify, write, reboot ---")
    cmake = read(MAIN_DIR / "CMakeLists.txt")
    c = read(MAIN_DIR / "tb_ota.c")
    h = read(MAIN_DIR / "tb_ota.h")
    assert_test('"tb_ota.c"' in cmake, "tb_ota.c is in SRCS")
    assert_test("tb_ota_on_data(const tb_link_data_t" in h and "tb_ota_busy(void)" in h, "tb_ota.h declares tb_ota_on_data and tb_ota_busy")
    req_pubs = [c[m.start():m.start() + 90] for m in re.finditer(r"tb_link_publish\(\s*req_topic", c)]
    assert_test(len(req_pubs) >= 1 and all("req_topic, TB_FW_CHUNK_SIZE_STR" in p for p in req_pubs),
                "every chunk request publish carries TB_FW_CHUNK_SIZE_STR", f"{len(req_pubs)} request publishes")
    assert_test("tb_fw_request_topic(" in c, "the request topic comes from tb_fw_request_topic")
    for fn in ("psa_hash_setup", "psa_hash_update", "psa_hash_finish", "psa_hash_abort", "tb_fw_digest_matches"):
        assert_test(fn + "(" in c, f"tb_ota.c uses {fn}")
    assert_test("PSA_ALG_SHA_256" in c, "the hash is SHA-256")
    assert_test("esp_ota_abort(" in c and "ota_lock_release(" in c, "tb_ota.c aborts the OTA and releases the lock on failure")
    hook = function_body(c, "on_attributes")
    top = function_body(c, "tb_ota_on_data", r"void\s+")
    chunk = function_body(c, "on_chunk")
    fetch = function_body(c, "fetch_chunk")
    i_claim = hook.find("ota_lock_claim()")
    i_create = hook.find("xTaskCreate(")
    assert_test(i_claim != -1 and i_create != -1 and i_claim < i_create, "the attribute handler claims the lock before xTaskCreate")
    assert_test(re.search(r"heap_caps_malloc\(\s*TB_FW_CHUNK_SIZE[^;]*MALLOC_CAP_SPIRAM", c) is not None,
                "the chunk buffer is heap_caps_malloc(TB_FW_CHUNK_SIZE, MALLOC_CAP_SPIRAM ...)")
    assert_test("xTaskCreateWithCaps" not in c, "no xTaskCreateWithCaps (the task stack stays in internal RAM)")
    assert_test(re.search(r"xTaskCreate\([^;]*6144", hook) is not None, "the task is xTaskCreate with a 6144-byte stack")
    task = function_body(c, "tb_ota_task", r"static\s+void\s+")
    i_dry = task.find("dry_run_guard_get_measure(")
    i_pend = task.find("nvs_store_set_tb_ota_pending(")
    i_boot = task.find("esp_ota_set_boot_partition(")
    i_rest = task.find("esp_restart(")
    assert_test(-1 not in (i_dry, i_pend, i_boot, i_rest) and i_dry < i_pend < i_boot < i_rest,
                "task order: dry-run check, set pending, set boot partition, esp_restart")
    tail = task[i_rest:] if i_rest != -1 else ""
    after_boot = task[i_boot:i_rest] if i_boot != -1 and i_rest != -1 else ""
    assert_test("nvs_store_clear_tb_ota_pending(" in after_boot, "set_boot_failed clears the pending version")
    assert_test("ota_lock_release" not in task[task.find("UPDATING"):i_rest] if "UPDATING" in task and i_rest != -1 else False,
                "the lock stays held on the success path until esp_restart")
    i_end = task.find("esp_ota_end(")
    i_desc = task.find("esp_ota_get_partition_description(")
    assert_test(-1 not in (i_end, i_desc, i_pend) and i_end < i_desc < i_pend and "version_mismatch" in task and
                "tb_fw_version_equal(" in task[i_desc:i_pend],
                "the task compares the image's own version with the package version after esp_ota_end and before set pending")
    assert_test('"TB_OTA_REBOOT"' in c, 'the restart detail "TB_OTA_REBOOT" is set')
    assert_test("session_invalidate_all(" in task and "uplink_note_settings_changed(" in task,
                "the success path invalidates sessions and notes the settings change")
    assert_test("while (attempts < TB_FW_CHUNK_TRIES)" in fetch, "the chunk loop references TB_FW_CHUNK_TRIES")
    assert_test("tb_link_is_connected(" in fetch and "offline_ms" in fetch, "the chunk loop waits while disconnected")
    assert_test("esp_https_ota" not in c, "no esp_https_ota")
    assert_test("atomic_" in c and "portMUX_TYPE" in c, "shared state uses atomics and a spinlock")
    assert_test("xTaskNotify(" in chunk, "the data hook wakes the task with xTaskNotify")
    assert_test("TB_FW_ERROR_TOPIC" in top, "the data hook handles TB_FW_ERROR_TOPIC")
    assert_test("tb_fw_chunk_len(" in chunk, "the data hook checks the chunk total against tb_fw_chunk_len")
    assert_test("vTaskDelete(NULL)" in task, "the task deletes itself")
    notifies = re.findall(r"xTaskNotify\(\s*(\w+)", chunk + top)
    assert_test(len(notifies) >= 3 and "s_task" not in notifies, "the hook notifies a handle captured under s_mux, never xTaskNotify(s_task",
                str(notifies))
    for name, fbody in (("on_chunk", chunk), ("tb_ota_on_data", top)):
        for m in re.finditer(r"xTaskNotify\(", fbody):
            before = fbody[:m.start()]
            assert_test(before.rfind("portENTER_CRITICAL") > before.rfind("portEXIT_CRITICAL"),
                        f"{name}: xTaskNotify runs inside the s_mux critical section")
    wait = function_body(c, "end_wait_timeout")
    assert_test("portENTER_CRITICAL(&s_mux)" in wait and "atomic_store(&s_wait_idx, IDX_NONE)" in wait and
                wait.find("portENTER_CRITICAL") < wait.find('return "chunk_timeout"'),
                "end_wait_timeout disarms s_wait_idx under s_mux before returning a timeout")
    assert_test('return "chunk_timeout"' not in fetch and fetch.count("end_wait_timeout(idx)") >= 2,
                "fetch_chunk only leaves through end_wait_timeout on a timeout")
    i_task = task.find("s_task = xTaskGetCurrentTaskHandle()")
    assert_test(i_task != -1 and i_task < task.find("report_state(") and i_task < task.find("fetch_chunk("),
                "s_task is set before any request is armed")
    fail_i = task.find("fail:")
    fail = task[fail_i:] if fail_i != -1 else ""
    assert_test(fail.find("atomic_store(&s_wait_idx, IDX_NONE)") != -1 and
                fail.find("atomic_store(&s_wait_idx, IDX_NONE)") < fail.find("s_task = NULL") and
                fail.find("portENTER_CRITICAL") < fail.find("s_task = NULL") < fail.find("portEXIT_CRITICAL"),
                "the fail path clears s_task only after disarming the wait, under s_mux")
    assert_test(re.search(r"psa_hash_abort\(&op\)", fail) is not None and "if (hash_open" not in c,
                "psa_hash_abort runs unconditionally on the fail path")


def test_ota_boot():
    print("\n--- Milestone 5 Task 5: boot gating, 5 min rollback, boot report ---")
    a = read(MAIN_DIR / "app_main.c")
    c = read(MAIN_DIR / "tb_ota.c")
    h = read(MAIN_DIR / "tb_ota.h")
    assert_test("tb_ota_boot_init(void)" in h and "tb_ota_boot_holds_valid(void)" in h and "tb_ota_on_session(void)" in h,
                "tb_ota.h declares boot_init, boot_holds_valid and on_session")
    marks = [m.start() for m in re.finditer(r"esp_ota_mark_app_valid_cancel_rollback\(\)", a)]
    guarded = 0
    for i in marks:
        g = a.rfind("if (!tb_ota_boot_holds_valid())", 0, i)
        # the guard's block must still be open at the call: no closing brace at guard depth before the call
        if g != -1:
            depth = 0
            for ch in a[a.find("{", g):i]:
                depth += (ch == "{") - (ch == "}")
            guarded += depth > 0
    assert_test(len(marks) == 2 and guarded == 2, "every mark-valid call in app_main.c is inside a tb_ota_boot_holds_valid guard",
                f"{guarded}/{len(marks)} guarded")
    assert_test("OTA from ThingsBoard: waiting for the ThingsBoard connect to mark valid" in a, "app_main logs the wait for the ThingsBoard connect")
    i_init, i_web = a.find("tb_ota_boot_init()"), a.find("web_server_start()")
    i_tl = a.find("tb_link_init()")
    assert_test(i_init != -1 and i_init < i_web and i_init < i_tl,
                "tb_ota_boot_init runs before tb_link_init and the first web_server_start")
    i_d, i_s = a.find("tb_link_set_data_hook(tb_ota_on_data)"), a.find("tb_link_set_session_hook(tb_ota_on_session)")
    assert_test(i_d != -1 and i_s != -1 and i_d < i_tl and i_s < i_tl, "both OTA hooks are registered before tb_link_init")
    assert_test('esp_log_level_set("tb_ota", ESP_LOG_INFO)' in a, "the tb_ota tag is raised to INFO")
    assert_test("#ifdef TB_LINK_TEST_BLOCK_CONNECT" in a and "TEST BUILD: ThingsBoard connect is blocked" in a,
                "the block-connect test build warns at boot")
    assert_test("esp_ota_mark_app_invalid_rollback_and_reboot" in c and "TB_FW_VERIFY_TIMEOUT_MS" in c,
                "tb_ota.c arms the rollback timer with TB_FW_VERIFY_TIMEOUT_MS")
    assert_test("no ThingsBoard connect within 5 min, rolling back" in c, "the timer logs before rolling back")
    sess = function_body(c, "tb_ota_on_session", r"void\s+")
    i_m = sess.find("esp_ota_mark_app_valid_cancel_rollback")
    i_r = sess.find('report_state("UPDATED"')
    i_c = sess.find("nvs_store_clear_tb_ota_pending")
    assert_test(i_m != -1 and i_m < i_r < i_c, "on_session: mark valid, then report UPDATED, then clear the pending version")
    i_cl = sess.find("nvs_store_clear_tb_ota_pending")
    assert_test("static bool report_state" in c and "bool ok" in sess and "bool ok = " in sess and 0 < sess.find("if (ok)") < i_cl,
                "on_session clears the pending version only when the boot report was enqueued")
    assert_test("rolled_back" in sess and "esp_timer_stop" in sess and "TB_FW_ATTR_REQ_PREFIX" in sess and
                "TB_FW_ATTR_REQUEST_BODY" in sess and "uplink_note_settings_changed()" in sess,
                "on_session reports rolled_back, stops the timer, requests the attributes and notes the change")
    init = function_body(c, "tb_ota_boot_init", r"void\s+")
    assert_test(init != "" and "nvs_store_clear_tb_ota_pending" not in init and "tb_fw_boot_state(" in init and
                "ESP_OTA_IMG_PENDING_VERIFY" in init,
                "tb_ota_boot_init resolves the state and never clears the pending version")
    i_rb = init.find("TB_FW_BOOT_ROLLED_BACK")
    assert_test(i_rb != -1 and "remember_failed(" in init[i_rb:],
                "tb_ota_boot_init remembers the pending version on the ROLLED_BACK path")


def test_bench6_fixes():
    print("\n--- Bench round 6 fixes: invalid image code, real firmware version ---")
    c = read(MAIN_DIR / "tb_ota.c")
    task = function_body(c, "tb_ota_task", r"static\s+void\s+")
    i_w = task.find("esp_ota_write(")
    i_end = task.find("psa_hash_finish(")
    loop = task[i_w:i_end] if i_w != -1 and i_end != -1 else ""
    assert_test(re.search(r'ESP_ERR_OTA_VALIDATE_FAILED\s*\)?\s*\?\s*"invalid_image"\s*:\s*"write_failed"', loop) is not None,
                "esp_ota_write ESP_ERR_OTA_VALIDATE_FAILED reports invalid_image; other write errors stay write_failed")
    cmake = read(FIRMWARE_DIR / "CMakeLists.txt")
    i_proj = cmake.find("project(fishpump_firmware")
    m = re.search(r'idf_build_set_property\(COMPILE_DEFINITIONS\s+"APP_TEMPLATE_FIRMWARE_VERSION=[^\n]*?\$\{FISHPUMP_VERSION\}', cmake)
    assert_test(m is not None and i_proj != -1 and m.start() < i_proj,
                "APP_TEMPLATE_FIRMWARE_VERSION comes from version.txt via idf_build_set_property before project()")
    assert_test("add_compile_definitions(APP_TEMPLATE_FIRMWARE_VERSION" not in cmake,
                "no add_compile_definitions(APP_TEMPLATE_FIRMWARE_VERSION (it never reaches the components)")
    cfg = read(COMP_DIR / "app_config" / "include" / "app_config.h")
    assert_test(re.search(r'#define\s+APP_TEMPLATE_FIRMWARE_VERSION\s+"', cfg) is None and "#error" in cfg,
                "app_config.h has no hard-coded version fallback and stops the build when the define is missing")


def main():
    test_nvs_storage()
    test_time_sync()
    test_tb_link_component()
    test_web_card_and_api()
    test_rpc_transport()
    test_uplink_state()
    test_tb_state()
    test_state_cases_drift()
    test_remote_wifi_link()
    test_wifi_remote()
    test_wifi_connect_remote()
    test_ota_link()
    test_ota_lock()
    test_tb_ota()
    test_ota_boot()
    test_bench6_fixes()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
