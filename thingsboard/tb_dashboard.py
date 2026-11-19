"""ThingsBoard test dashboard for fishpump: widget bundle, RPC panel widget type, dashboard.

    python thingsboard/tb_dashboard.py --check                 # offline: validate dashboard_pages.json + parity_cases.json
    python thingsboard/tb_dashboard.py --user tenant@example.com [--device fishpump-1de564]

tb_setup.py calls ensure_all() after the customer step. Idempotent by name. The widget and dashboard JSON are
hand-written against the 4.3.1 dumps from tb_dump.py; the REST paths are kept in one place (P_*) so a live fix is
a one-line change.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

from tb_api import TbError, add_common_args, connect_from_args, utf8_stdout

HERE = Path(__file__).resolve().parent
PAGES_FILE = HERE / "dashboard_pages.json"
CASES_FILE = HERE / "parity_cases.json"
WIDGET_FILE = HERE / "widgets" / "fishpump_rpc_panel.json"

BUNDLE_TITLE = "fishpump"
WIDGET_FQN = "fishpump_rpc_panel"
WIDGET_FULL_FQN = "tenant." + WIDGET_FQN  # confirmed live (4.3.1): typeFullFqn and the ?fqn= query are scope-qualified; the stored fqn stays bare
LATEST_FQN = "system.cards.entities_table"  # latest-values table, as used in the Firmware demo dashboard
DASHBOARD_TITLE = "Fishpump — ทดสอบ"
DEFAULT_DEVICE = "fishpump-1de564"
DEVICE_TYPE = "fishpump"  # device profile name = device type

# actions with no parity case: they move the pump or touch the pH sensor (calibration cancel), so their success is a guided bench step (Task 13), never automatic
NON_PARITY_METHODS = {"pump.start", "pump.stop", "water.calibrate.cancel"}

UNSAFE_CHARS = "<>&\"'"  # ThingsBoard NoXss validator rejects these in human-text fields

P_BUNDLES = "/api/widgetsBundles"
P_BUNDLE = "/api/widgetsBundle"
P_WIDGET_TYPE = "/api/widgetType"
P_DASHBOARDS = "/api/tenant/dashboards"
P_DASHBOARD = "/api/dashboard"


# ---------------------------------------------------------------- offline checks

def has_thai(text):
    return any("฀" <= c <= "๿" for c in text)


def parity_methods(cases):
    """Every method that some parity case exercises."""
    out = set()
    for group in cases.values():
        for r in group.get("reads", []):
            out.add(r["method"])
        for e in group.get("errors", []):
            out.add(e["method"])
        for w in group.get("writes", []):
            out.add(w["get"])
            out.add(w["set"])
    return out


def check_pages(pages, cases):
    """Return a list of problems (empty = OK)."""
    problems = []
    if not isinstance(pages, list) or not pages:
        return ["pages: must be a non-empty list"]
    known = parity_methods(cases) | NON_PARITY_METHODS
    seen = set()
    for p in pages:
        pid = p.get("id") if isinstance(p, dict) else None
        if not pid or not isinstance(pid, str):
            problems.append("page without a string id: %r" % (p,))
            continue
        if pid in seen:
            problems.append("duplicate page id %r" % pid)
        seen.add(pid)
        if not p.get("title"):
            problems.append("page %s: empty title" % pid)
        if not isinstance(p.get("telemetry"), list):
            problems.append("page %s: telemetry must be a list" % pid)
        if not isinstance(p.get("actions"), list):
            problems.append("page %s: actions must be a list" % pid)
            continue
        for i, a in enumerate(p["actions"]):
            where = "page %s action %d" % (pid, i)
            for k in ("label", "method"):
                if not a.get(k):
                    problems.append("%s: missing %s" % (where, k))
            m = a.get("method")
            if m and m not in known:
                problems.append("%s: method %r is in neither parity_cases.json nor the non-parity allow-list" % (where, m))
            if a.get("params") is not None and not isinstance(a["params"], (dict, list)):
                problems.append("%s: params must be an object, a list or null" % where)
            if a.get("load") and not a.get("edit"):
                problems.append("%s: load without edit" % where)
            if a.get("load") and a["load"] not in known:
                problems.append("%s: load method %r unknown" % (where, a["load"]))
            c = a.get("confirm")
            if c is not None and (not isinstance(c, str) or not c.strip() or not has_thai(c)):
                problems.append("%s: confirm must be non-empty Thai text" % where)
    return problems


def _walk_strings(obj, path, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            _walk_strings(v, "%s.%s" % (path, k), out)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _walk_strings(v, "%s[%d]" % (path, i), out)
    elif isinstance(obj, str):
        bad = sorted(set(c for c in obj if c in UNSAFE_CHARS))
        if bad:
            out.append("%s contains %s" % (path, " ".join(bad)))


def check_unsafe(pages, widget_doc):
    """NoXss: the dashboard is built from the pages, so every string in them ends up in a stored field. In the widget
    type only name/description/tags are human text; templateHtml/templateCss/controllerScript are code and skipped."""
    out = []
    _walk_strings(pages, "pages", out)
    _walk_strings({k: widget_doc.get(k) for k in ("name", "description", "tags")}, "widget", out)
    _walk_strings(DASHBOARD_TITLE, "dashboard title", out)
    return out


def node_syntax_problem(script):
    """node --check on the widget controller script; None when fine or when node is not installed."""
    node = shutil.which("node")
    if not node:
        return None
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "panel.js"
        f.write_text(script, encoding="utf-8")
        r = subprocess.run([node, "--check", str(f)], capture_output=True, text=True)
    return None if r.returncode == 0 else r.stderr.strip()[:400]


BROKEN_FIXTURE = [
    {"id": "dup", "title": "A", "telemetry": [], "actions": [
        {"label": "x", "method": "no.such.method", "params": None, "edit": False, "load": None, "confirm": "english only"}]},
    {"id": "dup", "title": "B", "telemetry": [], "actions": []},
]


class _RecordingApi:
    """Fake api for the offline lookup test: records GET params, answers 404 (type missing)."""
    def __init__(self):
        self.gets = []

    def get(self, path, params=None, **kw):
        self.gets.append((path, dict(params or {})))
        return None

    def post(self, path, body=None, params=None, **kw):
        return {"id": {"id": "fake"}}


def widget_lookup_problem():
    import contextlib
    import io
    api = _RecordingApi()
    with contextlib.redirect_stdout(io.StringIO()):
        ensure_widget_type(api)
    want = [(P_WIDGET_TYPE, {"fqn": WIDGET_FULL_FQN})]
    if api.gets != want or WIDGET_FULL_FQN != "tenant.fishpump_rpc_panel":
        return "widget type lookup must GET %s?fqn=tenant.fishpump_rpc_panel (TB rejects a bare fqn), got %r" % (
            P_WIDGET_TYPE, api.gets)
    return None


def run_check(pages_file=PAGES_FILE, cases_file=CASES_FILE):
    """Returns the problem list for the real files (plus the self-test of the checker itself)."""
    problems = []
    lookup = widget_lookup_problem()
    if lookup:
        problems.append(lookup)
    if not check_pages(BROKEN_FIXTURE, {}):
        problems.append("internal: the checker accepted a deliberately broken fixture")
    try:
        pages = json.loads(Path(pages_file).read_text(encoding="utf-8"))
        cases = json.loads(Path(cases_file).read_text(encoding="utf-8"))
        widget = json.loads(WIDGET_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return problems + ["cannot read input files: %s" % e]
    problems += check_pages(pages, cases)
    problems += check_unsafe(pages, widget)
    if widget.get("fqn") != WIDGET_FQN or widget.get("descriptor", {}).get("type") != "rpc":
        problems.append("widget: fqn must be %s and descriptor.type rpc" % WIDGET_FQN)
    js = node_syntax_problem(widget.get("descriptor", {}).get("controllerScript", ""))
    if js:
        problems.append("widget controllerScript does not parse: %s" % js)
    # building must work offline and produce unique widget ids
    try:
        dash = build_dashboard(pages, DEFAULT_DEVICE)
        ids = list(dash["configuration"]["widgets"])
        if len(ids) != len(set(ids)):
            problems.append("dashboard: duplicate widget ids")
        for pid in [p["id"] for p in pages]:
            if pid not in dash["configuration"]["states"]:
                problems.append("dashboard: no state for page %s" % pid)
        problems += check_unsafe(dash, {})
    except Exception as e:  # noqa: BLE001 - report, do not crash the checker
        problems.append("dashboard build failed: %r" % e)
    return problems


# ---------------------------------------------------------------- builders

def _uid(*parts):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "fishpump-dashboard/" + "/".join(parts)))


def _timewindow():
    return {"displayValue": "", "selectedTab": 0,
            "realtime": {"realtimeType": 1, "interval": 1000, "timewindowMs": 86400000, "quickInterval": "CURRENT_DAY"},
            "history": {"historyType": 0, "interval": 1000, "timewindowMs": 60000,
                        "fixedTimewindow": {"startTimeMs": 1694085363133, "endTimeMs": 1694171763133},
                        "quickInterval": "CURRENT_DAY"},
            "aggregation": {"type": "NONE", "limit": 200}}


def build_dashboard(pages, device_name, existing=None):
    alias_id = _uid("alias", "device")
    aliases = {alias_id: {"id": alias_id, "alias": "Fishpump",
                          "filter": {"type": "deviceType", "resolveMultiple": False,
                                     "deviceNameFilter": device_name, "deviceTypes": [DEVICE_TYPE]}}}
    widgets, states = {}, {}
    for idx, page in enumerate(pages):
        nav = [{"id": p["id"], "title": p["title"], "current": p["id"] == page["id"]} for p in pages]
        rpc_id = _uid(page["id"], "rpc")
        tel_id = _uid(page["id"], "telemetry")
        widgets[rpc_id] = {
            "typeFullFqn": WIDGET_FULL_FQN, "type": "rpc", "sizeX": 14, "sizeY": 16, "row": 0, "col": 0, "id": rpc_id,
            "config": {
                "showTitle": True, "title": page["title"], "backgroundColor": "#ffffff", "color": "rgba(0, 0, 0, 0.87)",
                "padding": "4px", "dropShadow": True, "enableFullscreen": True, "widgetStyle": {}, "actions": {},
                "titleStyle": {"fontSize": "16px", "fontWeight": 400}, "showLegend": False,
                "targetDevice": {"type": "entity", "entityAliasId": alias_id},
                "targetDeviceAliasIds": [alias_id],
                "settings": {"actions": page["actions"], "nav": nav},
                "datasources": [], "useDashboardTimewindow": True,
            },
        }
        keys = [{"name": k, "type": "timeseries", "label": k, "color": "#2196f3",
                 "settings": {"columnWidth": "0px", "useCellStyleFunction": False, "cellStyleFunction": "",
                              "useCellContentFunction": False, "defaultColumnVisibility": "visible",
                              "columnSelectionToDisplay": "enabled"},
                 "units": None, "decimals": None, "funcBody": None, "usePostProcessing": None, "postFuncBody": None}
                for k in page["telemetry"]]
        widgets[tel_id] = {
            "typeFullFqn": LATEST_FQN, "type": "latest", "sizeX": 10, "sizeY": 8, "row": 0, "col": 14, "id": tel_id,
            "config": {
                "timewindow": _timewindow(), "showTitle": True, "title": "ค่าล่าสุด",
                "backgroundColor": "rgb(255, 255, 255)", "color": "rgba(0, 0, 0, 0.87)", "padding": "4px",
                "dropShadow": True, "enableFullscreen": False, "useDashboardTimewindow": False, "showLegend": False,
                "settings": {"enableSearch": False, "displayPagination": False, "defaultPageSize": 10,
                             "defaultSortOrder": "entityName", "displayEntityName": True, "displayEntityType": False,
                             "enableSelectColumnDisplay": False, "enableStickyHeader": True, "enableStickyAction": False,
                             "entitiesTitle": "ค่าล่าสุด", "displayEntityLabel": False, "entityNameColumnTitle": "อุปกรณ์"},
                "titleStyle": {"fontSize": "16px", "fontWeight": 400, "padding": "5px 10px 5px 10px"},
                "actions": {},
                "datasources": [{"type": "entity", "name": None, "entityAliasId": alias_id, "filterId": None,
                                 "dataKeys": keys}],
            },
        }
        states[page["id"]] = {
            "name": page["title"], "root": idx == 0,
            "layouts": {"main": {
                "widgets": {rpc_id: {"sizeX": 14, "sizeY": 16, "row": 0, "col": 0},
                            tel_id: {"sizeX": 10, "sizeY": 8, "row": 0, "col": 14}},
                "gridSettings": {"backgroundColor": "#eeeeee", "color": "rgba(0,0,0,0.870588)", "columns": 24,
                                 "margin": 10, "backgroundSizeMode": "100%", "autoFillHeight": False,
                                 "mobileAutoFillHeight": False, "mobileRowHeight": 70, "outerMargin": True}}},
        }
    configuration = {
        "widgets": widgets, "states": states, "entityAliases": aliases, "filters": {},
        "timewindow": {"displayValue": "", "selectedTab": 0, "hideAggregation": False, "hideAggInterval": False,
                       "realtime": {"interval": 1000, "timewindowMs": 60000},
                       "aggregation": {"type": "AVG", "limit": 25000}},
        "settings": {"stateControllerId": "default", "showTitle": False, "showDashboardsSelect": False,
                     "showEntitiesSelect": False, "showDashboardTimewindow": False, "showDashboardExport": True,
                     "toolbarAlwaysOpen": False, "titleColor": "rgba(0,0,0,0.870588)", "showDashboardLogo": False,
                     "dashboardLogoUrl": None, "hideToolbar": False, "showFilters": False,
                     "showUpdateDashboardImage": False, "dashboardCss": ""},
    }
    body = dict(existing) if existing else {}
    body.update({"title": DASHBOARD_TITLE, "name": DASHBOARD_TITLE, "configuration": configuration})
    return body


# ---------------------------------------------------------------- ThingsBoard calls

def ensure_widget_type(api):
    doc = json.loads(WIDGET_FILE.read_text(encoding="utf-8"))
    existing = api.get(P_WIDGET_TYPE, {"fqn": WIDGET_FULL_FQN}, ok404=True)
    if existing and existing.get("fqn") == WIDGET_FQN and (existing.get("tenantId") or {}).get("id") != \
            "13814000-1dd2-11b2-8080-808080808080":
        body = dict(existing)
        body.update({k: doc[k] for k in ("name", "descriptor", "description", "tags", "deprecated", "scada")})
        state = "updated"
    else:
        body, state = dict(doc), "created"
    saved = api.post(P_WIDGET_TYPE, body)
    print("widget type %-18s %s (id %s)" % (WIDGET_FQN, state, saved["id"]["id"]))
    return saved


def find_bundle(api):
    """Bundle by title. /api/widgetsBundles may answer a bare list or a paged {"data":...} object."""
    page = 0
    while True:
        res = api.get(P_BUNDLES, {"pageSize": 100, "page": page})
        items = res if isinstance(res, list) else (res or {}).get("data", [])
        for item in items:
            if item.get("title") == BUNDLE_TITLE:
                return item
        if isinstance(res, list) or not (res or {}).get("hasNext"):
            return None
        page += 1


def ensure_bundle(api, widget_type):
    existing = find_bundle(api)
    if existing is None:
        existing = api.post(P_BUNDLE, {"title": BUNDLE_TITLE})
        state = "created"
    else:
        state = "exists"
    # guess: the bundle's member list is replaced by a list of widget type ids
    api.post("%s/%s/widgetTypes" % (P_BUNDLE, existing["id"]["id"]), [widget_type["id"]["id"]])
    print("bundle      %-18s %s, widget type attached" % (BUNDLE_TITLE, state))
    return existing


def ensure_dashboard(api, customer, device_name=DEFAULT_DEVICE):
    pages = json.loads(PAGES_FILE.read_text(encoding="utf-8"))
    existing = api.find_by_name(P_DASHBOARDS, DASHBOARD_TITLE, key="title")
    body = build_dashboard(pages, device_name, existing)
    saved = api.post(P_DASHBOARD, body)
    did = saved["id"]["id"]
    cid = customer["id"]["id"]
    assigned = [c.get("customerId", {}).get("id") for c in (saved.get("assignedCustomers") or [])]
    if cid not in assigned:
        api.post("/api/customer/%s/dashboard/%s" % (cid, did))
        assign_state = "assigned to the test customer"
    else:
        assign_state = "already assigned"
    print("dashboard   %s %s, %d pages, %s (id %s)" % (DASHBOARD_TITLE, "updated" if existing else "created",
                                                       len(pages), assign_state, did))
    return saved


def ensure_all(api, customer, device_name=DEFAULT_DEVICE):
    wt = ensure_widget_type(api)
    ensure_bundle(api, wt)
    return ensure_dashboard(api, customer, device_name)


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--check", action="store_true", help="offline: validate the pages and parity case files, no login")
    ap.add_argument("--pages", default=str(PAGES_FILE), help="pages file for --check (default %(default)s)")
    ap.add_argument("--cases", default=str(CASES_FILE), help="parity cases file for --check (default %(default)s)")
    ap.add_argument("--device", default=DEFAULT_DEVICE, help="device name the dashboard targets (default %(default)s)")
    args = ap.parse_args()
    if args.check:
        problems = run_check(args.pages, args.cases)
        if problems:
            print("ERROR: dashboard check failed:", file=sys.stderr)
            for p in problems:
                print("  " + p, file=sys.stderr)
            return 1
        print("check OK: dashboard pages, parity cases and widget type are consistent")
        return 0
    from tb_setup import CUSTOMER_TITLE  # late import: tb_setup imports this module
    api = connect_from_args(args, ap)
    try:
        customer = api.find_by_name("/api/customers", CUSTOMER_TITLE, key="title")
        if customer is None:
            raise TbError("customer %r not found - run tb_setup.py first" % CUSTOMER_TITLE)
        ensure_all(api, customer, args.device)
    except TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
