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
               "handle_api_confirm_delete", "handle_api_ota"):
        assert_test("uplink_note_settings_changed()" in function_body(web, fn),
                    f"{fn} calls uplink_note_settings_changed()")
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


def main():
    test_nvs_storage()
    test_time_sync()
    test_tb_link_component()
    test_web_card_and_api()
    test_rpc_transport()
    test_uplink_state()
    test_tb_state()
    test_state_cases_drift()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
