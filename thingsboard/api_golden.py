"""Golden-response harness for the fishpump board's web API (stdlib only).

capture: log in, run every case in golden_cases.json, save status + raw body per case.
compare: capture again, then compare with a saved capture; one line per case.

Used to prove that a firmware refactor leaves the HTTP replies unchanged.
Captures live in thingsboard/.local/golden/<label>.json (git-ignored).

Case format (golden_cases.json is a list of these):
  name, method (GET|POST), path, body (object/list or null), mode (exact|shape),
  ignore (keys skipped by shape mode, at any depth)
  optional "needs": "get:<case>"  -- an idempotent write, run only with --with-writes.
      The POST body is the JSON body that GET case returned in the same run, with the
      top-level "ok" removed, then edited by these optional steps, in this order:
        "when_path": dotted path that must be truthy, else the case is skipped
        "from_path": dotted path (list indexes are numbers) to pick a sub-value
        "pick":      list of keys to keep (when the value is an object)
        "wrap":      key to wrap the value in, e.g. {"pump": <value>}
      If a step cannot be applied the case is recorded as skipped.

Board password: env BOARD_PASSWORD or a getpass prompt (see board_http.py).
"""
import argparse
import json
import os
import re
import sys
import threading
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CASES = os.path.join(HERE, "golden_cases.json")
GOLDEN_DIR = os.path.join(HERE, ".local", "golden")

# Never allowed in golden_cases.json: they start things or touch auth/ThingsBoard/OTA.
FORBIDDEN_PREFIXES = (
    "/api/pump/start", "/api/pump/stop", "/api/tb", "/api/login", "/api/logout",
    "/api/auth", "/api/change-password", "/api/confirm", "/api/ota", "/api/events",
    "/events", "/api/wifi/disconnect",
)


def utf8_stdout():
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8")
        except Exception:
            pass


# ---------------------------------------------------------------- comparison

def _kind(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    return "object"


def shape_diff(a, b, ignore, path="$"):
    """None when a and b have the same keys and JSON types, else a reason string.

    Values are ignored. Keys in `ignore` are skipped at any depth. Arrays match
    when either is empty, otherwise their first elements must match.
    """
    ka, kb = _kind(a), _kind(b)
    if ka != kb:
        return "%s: type %s vs %s" % (path, ka, kb)
    if ka == "object":
        keys_a = {k for k in a if k not in ignore}
        keys_b = {k for k in b if k not in ignore}
        if keys_a != keys_b:
            miss = sorted(keys_a - keys_b)
            extra = sorted(keys_b - keys_a)
            return "%s: keys differ (missing %s, extra %s)" % (path, miss, extra)
        for k in sorted(keys_a):
            d = shape_diff(a[k], b[k], ignore, "%s.%s" % (path, k))
            if d:
                return d
    elif ka == "array" and a and b:
        return shape_diff(a[0], b[0], ignore, path + "[0]")
    return None


def compare_case(case, base, new):
    """(ok, reason) for one case. base/new are {"status","text"} or {"skipped"}."""
    if "skipped" in base or "skipped" in new:
        if "skipped" in base and "skipped" in new:
            return True, "skipped in both"
        return False, "skipped in only one run (%s)" % (base.get("skipped") or new.get("skipped"))
    if base["status"] != new["status"]:
        return False, "status %s vs %s" % (base["status"], new["status"])
    if case.get("mode", "exact") == "exact":
        if base["text"] != new["text"]:
            return False, "body differs: %s vs %s" % (_clip(base["text"]), _clip(new["text"]))
        return True, ""
    try:
        ja, jb = json.loads(base["text"]), json.loads(new["text"])
    except ValueError:
        if base["text"] == new["text"]:
            return True, ""
        return False, "body is not JSON and differs"
    d = shape_diff(ja, jb, set(case.get("ignore") or []))
    return (d is None), (d or "")


def _clip(s, n=90):
    s = s.replace("\n", " ")
    return s if len(s) <= n else s[:n] + "..."


# ------------------------------------------------------------------- cases

def load_cases(path):
    with open(path, "r", encoding="utf-8") as f:
        cases = json.load(f)
    validate_cases(cases)
    return cases


def validate_cases(cases):
    if not isinstance(cases, list) or not cases:
        raise ValueError("cases must be a non-empty list")
    seen = {}
    for c in cases:
        name = c.get("name")
        if not name or name in seen:
            raise ValueError("bad or duplicate case name: %r" % name)
        if c.get("method") not in ("GET", "POST"):
            raise ValueError("%s: method must be GET or POST" % name)
        if not str(c.get("path", "")).startswith("/api/"):
            raise ValueError("%s: path must start with /api/" % name)
        if c.get("mode", "exact") not in ("exact", "shape"):
            raise ValueError("%s: mode must be exact or shape" % name)
        if c["method"] == "GET" and c.get("body") is not None:
            raise ValueError("%s: a GET has no body" % name)
        for p in FORBIDDEN_PREFIXES:
            if c["path"] == p or c["path"].startswith(p + "/"):
                raise ValueError("%s: %s is not allowed in golden cases" % (name, c["path"]))
        needs = c.get("needs")
        if needs is not None:
            if not needs.startswith("get:") or needs[4:] not in seen:
                raise ValueError("%s: needs must name an earlier case as get:<case>" % name)
            if seen[needs[4:]]["method"] != "GET" or c["method"] != "POST":
                raise ValueError("%s: needs links a POST to an earlier GET" % name)
            if c.get("body") is not None:
                raise ValueError("%s: a case with needs has no fixed body" % name)
        seen[name] = c


def ordered(cases):
    """File order, except the Wi-Fi scan (slow, disturbs the radio) goes last."""
    return [c for c in cases if c["path"] != "/api/wifi/scan"] + \
           [c for c in cases if c["path"] == "/api/wifi/scan"]


def get_path(obj, dotted):
    for part in dotted.split("."):
        if isinstance(obj, list):
            obj = obj[int(part)]
        else:
            obj = obj[part]
    return obj


def build_body(case, results):
    """(body, None) or (None, reason-to-skip) for a case with "needs"."""
    src = results.get(case["needs"][4:])
    if not src or "status" not in src or src["status"] != 200:
        return None, "source GET %s did not return 200" % case["needs"][4:]
    try:
        value = json.loads(src["text"])
        if isinstance(value, dict):
            value = {k: v for k, v in value.items() if k != "ok"}
        if "when_path" in case and not get_path(value, case["when_path"]):
            return None, "%s is not set on the board" % case["when_path"]
        if "from_path" in case:
            value = get_path(value, case["from_path"])
        if "pick" in case:
            value = {k: value[k] for k in case["pick"]}
        if "wrap" in case:
            value = {case["wrap"]: value}
    except (ValueError, KeyError, IndexError, TypeError) as e:
        return None, "cannot build body (%s: %s)" % (type(e).__name__, e)
    if "from_path" in case and "wrap" in case and value[case["wrap"]] is None:
        return None, "%s is null on the board" % case["from_path"]
    return value, None


def run_cases(session, cases, with_writes, log=None):
    """Runs the cases in order; returns {name: {"status","text"} | {"skipped"}}."""
    results = {}
    for c in ordered(cases):
        if c.get("needs") and not with_writes:
            continue
        body = c.get("body")
        if c.get("needs"):
            body, why = build_body(c, results)
            if why:
                results[c["name"]] = {"skipped": why}
                continue
        try:
            status, text = session.request(c["method"], c["path"], body)
            results[c["name"]] = {"status": status, "text": text}
        except Exception as e:  # keep going: one dead request must not hide the rest
            results[c["name"]] = {"status": 0, "text": "request failed: %s" % e}
        if log:
            log("  %-34s %s" % (c["name"], results[c["name"]].get("status", "skip")))
    return results


def compare_all(cases, base, new):
    """Returns (lines, all_ok). One line per case that ran in `new`."""
    by_name = {c["name"]: c for c in cases}
    lines, all_ok = [], True
    for c in ordered(cases):
        n = c["name"]
        if n not in new:
            if n in base:
                lines.append("SKIP  %s (not run now; pass --with-writes)" % n)
            continue
        if n not in base:
            lines.append("FAIL  %s: not in the base capture" % n)
            all_ok = False
            continue
        ok, why = compare_case(by_name[n], base[n], new[n])
        if ok:
            lines.append("PASS  %s%s" % (n, " (" + why + ")" if why else ""))
        else:
            lines.append("FAIL  %s: %s" % (n, why))
        all_ok = all_ok and ok
    return lines, all_ok


# ------------------------------------------------------------------ capture

def capture_path(label):
    if not re.match(r"^[A-Za-z0-9._-]+$", label):
        raise ValueError("label may only use letters, digits, . _ -")
    return os.path.join(GOLDEN_DIR, label + ".json")


def save_capture(label, results):
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    path = capture_path(label)
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"label": label, "cases": results}, f, ensure_ascii=False, indent=1)
    return path


def load_capture(label):
    with open(capture_path(label), "r", encoding="utf-8") as f:
        return json.load(f)["cases"]


# ----------------------------------------------------------------- selftest

def _selftest_compare():
    ok_ = lambda r: r[0]
    ex = {"mode": "exact"}
    sh = {"mode": "shape", "ignore": ["gone"]}
    a = {"status": 200, "text": '{"ok":true,"n":1}'}
    checks = [
        ("exact match", ok_(compare_case(ex, a, dict(a))), True),
        ("exact one byte differs", ok_(compare_case(ex, a, {"status": 200, "text": '{"ok":true,"n":2}'})), False),
        ("shape changed number", ok_(compare_case(sh, a, {"status": 200, "text": '{"ok":true,"n":99}'})), True),
        ("shape missing key", ok_(compare_case(sh, a, {"status": 200, "text": '{"ok":true}'})), False),
        ("shape number vs string", ok_(compare_case(sh, a, {"status": 200, "text": '{"ok":true,"n":"1"}'})), False),
        ("shape ignored key missing", ok_(compare_case(
            sh, {"status": 200, "text": '{"n":1,"gone":5}'}, {"status": 200, "text": '{"n":2}'})), True),
        ("status differs", ok_(compare_case(ex, a, {"status": 500, "text": a["text"]})), False),
        ("shape bool is not number", ok_(compare_case(sh, {"status": 200, "text": '{"n":1}'},
                                                       {"status": 200, "text": '{"n":true}'})), False),
        ("shape arrays of different length", ok_(compare_case(
            sh, {"status": 200, "text": '{"l":[{"a":1}]}'}, {"status": 200, "text": '{"l":[{"a":2},{"a":3}]}'})), True),
        ("shape array element key missing", ok_(compare_case(
            sh, {"status": 200, "text": '{"l":[{"a":1}]}'}, {"status": 200, "text": '{"l":[{"b":1}]}'})), False),
        ("skipped in both", ok_(compare_case(ex, {"skipped": "x"}, {"skipped": "y"})), True),
        ("skipped in one", ok_(compare_case(ex, {"skipped": "x"}, a)), False),
    ]
    bad = 0
    for name, got, want in checks:
        if got != want:
            bad += 1
            print("  FAIL selftest: %s (got %s, want %s)" % (name, got, want))
    return bad


def _selftest_flow():
    """Fake board on localhost: login with Origin + cookie, capture, needs, compare."""
    import http.server
    import board_http

    state = {"n": 1}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, obj, cookie=False):
            data = json.dumps(obj, separators=(",", ":")).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            if cookie:
                self.send_header("Set-Cookie", "session=tok; Path=/; HttpOnly; SameSite=Lax")
            self.end_headers()
            self.wfile.write(data)

        def _guard(self, post):
            if "session=tok" not in (self.headers.get("Cookie") or ""):
                self._send(401, {"ok": False, "error": "unauthorized"})
                return False
            if post and self.headers.get("Origin") != "http://127.0.0.1:%d" % self.server.server_port:
                self._send(403, {"ok": False, "error": "forbidden"})
                return False
            return True

        def do_GET(self):
            if self.path == "/api/cfg" and self._guard(False):
                self._send(200, {"ok": True, "n": state["n"], "nested": {"x": [{"v": 2}], "k": 7}})

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if self.path == "/api/login":
                if self.headers.get("Origin") != "http://127.0.0.1:%d" % self.server.server_port:
                    return self._send(403, {"ok": False, "error": "forbidden"})
                d = json.loads(raw or b"{}")
                if d.get("username") == "admin" and d.get("password") == "pw":
                    return self._send(200, {"ok": True, "username": "admin"}, cookie=True)
                return self._send(401, {"ok": False, "error": "invalid_credentials"})
            if self.path == "/api/echo" and self._guard(True):
                self._send(200, {"ok": True, "got": json.loads(raw) if raw else None})

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_port
    bad = 0

    def expect(name, cond):
        nonlocal bad
        if not cond:
            bad += 1
            print("  FAIL selftest: %s" % name)

    try:
        wrong = board_http.BoardSession(base, "admin", "nope")
        try:
            wrong.login()
            expect("bad password is refused", False)
        except board_http.BoardError:
            pass
        s = board_http.BoardSession(base, "admin", "pw")
        s.login()
        expect("cookie kept after login", s.request("GET", "/api/cfg")[0] == 200)
        expect("no cookie -> 401", board_http.BoardSession(base, "a", "b").request("GET", "/api/cfg")[0] == 401)
        expect("empty POST body is sent", s.request("POST", "/api/echo")[1] == '{"ok":true,"got":null}')

        cases = [
            {"name": "cfg", "method": "GET", "path": "/api/cfg", "body": None, "mode": "exact"},
            {"name": "echo_all", "method": "POST", "path": "/api/echo", "mode": "exact", "needs": "get:cfg"},
            {"name": "echo_sub", "method": "POST", "path": "/api/echo", "mode": "exact", "needs": "get:cfg",
             "from_path": "nested.x.0", "wrap": "w"},
            {"name": "echo_pick", "method": "POST", "path": "/api/echo", "mode": "exact", "needs": "get:cfg",
             "from_path": "nested", "pick": ["k"]},
            {"name": "echo_missing", "method": "POST", "path": "/api/echo", "mode": "exact", "needs": "get:cfg",
             "from_path": "nested.nope"},
            {"name": "echo_when", "method": "POST", "path": "/api/echo", "mode": "exact", "needs": "get:cfg",
             "when_path": "nested.zero"},
        ]
        validate_cases(cases)
        no_writes = run_cases(s, cases, with_writes=False)
        expect("writes are skipped without --with-writes", list(no_writes) == ["cfg"])
        r1 = run_cases(s, cases, with_writes=True)
        expect("needs: body is the GET minus ok",
               r1["echo_all"]["text"] == '{"ok":true,"got":{"n":1,"nested":{"x":[{"v":2}],"k":7}}}')
        expect("needs: from_path + wrap", r1["echo_sub"]["text"] == '{"ok":true,"got":{"w":{"v":2}}}')
        expect("needs: pick", r1["echo_pick"]["text"] == '{"ok":true,"got":{"k":7}}')
        expect("needs: unresolved path is skipped", "skipped" in r1["echo_missing"])
        expect("needs: when_path falsy is skipped", "skipped" in r1["echo_when"])
        r2 = run_cases(s, cases, with_writes=True)
        lines, ok = compare_all(cases, r1, r2)
        expect("identical runs compare equal", ok and len(lines) == 6)
        state["n"] = 2
        r3 = run_cases(s, cases, with_writes=True)
        lines, ok = compare_all(cases, r1, r3)
        expect("a changed reply is caught", (not ok) and lines[0].startswith("FAIL  cfg"))
        lines, ok = compare_all(cases, r1, no_writes)
        expect("cases not run are SKIP, not FAIL", any(l.startswith("SKIP") for l in lines))
    finally:
        srv.shutdown()
    return bad


def _selftest_cases_file():
    bad = 0
    try:
        cases = load_cases(DEFAULT_CASES)
    except Exception as e:
        print("  FAIL selftest: golden_cases.json: %s" % e)
        return 1
    paths = {(c["method"], c["path"]) for c in cases}
    for need in (("GET", "/api/status"), ("GET", "/api/alarms/rules"), ("POST", "/api/protection"),
                 ("POST", "/api/water/offset"), ("GET", "/api/wifi/scan")):
        if need not in paths:
            bad += 1
            print("  FAIL selftest: golden_cases.json has no case for %s %s" % need)
    return bad


def selftest():
    bad = _selftest_compare() + _selftest_flow() + _selftest_cases_file()
    print("SELFTEST PASS" if bad == 0 else "SELFTEST FAIL (%d)" % bad)
    return 0 if bad == 0 else 1


# --------------------------------------------------------------------- main

def main(argv=None):
    utf8_stdout()
    ap = argparse.ArgumentParser(description="Golden-response harness for the board web API")
    ap.add_argument("--selftest", action="store_true", help="run the offline self-check and exit")
    sub = ap.add_subparsers(dest="cmd")
    for name in ("capture", "compare"):
        p = sub.add_parser(name)
        import board_http
        board_http.add_board_args(p)
        p.add_argument("--label", required=True, help="capture name, saved as .local/golden/<label>.json")
        p.add_argument("--with-writes", action="store_true",
                       help="also run the idempotent writes (post back the value just read)")
        p.add_argument("--cases", default=DEFAULT_CASES)
        if name == "compare":
            p.add_argument("--base", required=True, help="label of the capture to compare against")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.cmd:
        ap.error("give capture, compare or --selftest")

    import board_http
    cases = load_cases(args.cases)
    base = load_capture(args.base) if args.cmd == "compare" else None
    session = board_http.board_from_args(args)
    print("capturing %d cases%s" % (len(cases), " (with writes)" if args.with_writes else ""))
    results = run_cases(session, cases, args.with_writes, log=print)
    print("saved", save_capture(args.label, results))
    if args.cmd == "capture":
        return 0
    lines, ok = compare_all(cases, base, results)
    print("\n".join(lines))
    print("ALL PASS" if ok else "SOME CASES FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    sys.exit(main())
