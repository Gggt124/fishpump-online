"""Parity test: an RPC method through ThingsBoard must answer like the board's HTTP endpoint.

    python thingsboard/parity_test.py --selftest
    python thingsboard/parity_test.py --user tenant@example.com --device fishpump-1de564 [--group alarms]
    python thingsboard/parity_test.py ... --writes --rule-id 3        # also the reversible write cases
    python thingsboard/parity_test.py ... --burst 30                  # RPC burst while polling /api/alarms/summary

Passwords: env TB_PASSWORD / BOARD_PASSWORD or a prompt. Cases live in parity_cases.json (format: README).
Exit code 0 only when no case FAILs.
"""
import argparse
import copy
import json
import re
import sys
import threading
import time
from pathlib import Path

import board_http
import tb_api

HERE = Path(__file__).resolve().parent
CASES_FILE = HERE / "parity_cases.json"
DEFAULT_DEVICE = "fishpump-1de564"
FORCE_PW_MSG = "board web password is still the default"


# ---------------------------------------------------------------- pure helpers (selftested)

def strip_keys(obj, ignore):
    """Deep copy without the ignored keys at any level."""
    if isinstance(obj, dict):
        return {k: strip_keys(v, ignore) for k, v in obj.items() if k not in ignore}
    if isinstance(obj, list):
        return [strip_keys(v, ignore) for v in obj]
    return obj


def compare_bodies(rpc, http, ignore=()):
    """None when equal after dropping the ignored keys, else a short reason."""
    a, b = strip_keys(rpc, set(ignore)), strip_keys(http, set(ignore))
    if a == b:
        return None
    return "bodies differ: rpc=%s http=%s" % (_short(a), _short(b))


def _short(obj, n=240):
    t = json.dumps(obj, ensure_ascii=False, sort_keys=True) if not isinstance(obj, str) else obj
    return t if len(t) <= n else t[:n] + "..."


def error_code(body):
    return body.get("error") if isinstance(body, dict) and body.get("ok") is False else None


_SEG = re.compile(r"^([A-Za-z0-9_]+)(?:\[([A-Za-z0-9_]+)=([^\]]+)\])?$")


def _segments(path, subst):
    for raw in path.split("."):
        raw = raw.replace("$RULE_ID", str(subst.get("RULE_ID", "")))
        m = _SEG.match(raw)
        if not m:
            raise KeyError("bad path segment %r" % raw)
        yield m.group(1), m.group(2), m.group(3)


def get_path(obj, path, subst=None):
    """Dotted path; a segment may be name or name[field=value] (picks the list element), digits index a list."""
    cur = obj
    for name, field, value in _segments(path, subst or {}):
        if isinstance(cur, list):
            cur = cur[int(name)]
        else:
            cur = cur[name]
        if field is not None:
            hits = [e for e in cur if str(e.get(field)) == value]
            if not hits:
                raise KeyError("no element with %s=%s" % (field, value))
            cur = hits[0]
    return cur


def set_path(obj, path, value, subst=None):
    """Return a deep copy of obj with the path set to value."""
    out = copy.deepcopy(obj)
    segs = list(_segments(path, subst or {}))
    cur = out
    for i, (name, field, val) in enumerate(segs):
        last = i == len(segs) - 1 and field is None
        key = int(name) if isinstance(cur, list) else name
        if last:
            cur[key] = value
            return out
        cur = cur[key]
        if field is not None:
            hits = [e for e in cur if str(e.get(field)) == val]
            if not hits:
                raise KeyError("no element with %s=%s" % (field, val))
            cur = hits[0]
            if i == len(segs) - 1:
                raise KeyError("path ends on a selector")
    raise KeyError("empty path")


def apply_change(old, change):
    if "add" in change:
        return old + change["add"]
    if "value" in change:
        return change["value"]
    raise ValueError("change needs add or value")


def write_body(get_result, case, subst):
    """Body for the set call: the GET result without ok, narrowed to rule_path, the path changed by `change`.
    A case with "wrap" nests the body under that key (the set method wants {"pump": {...}}).
    Returns (set_body, old_value, new_value)."""
    base = {k: v for k, v in get_result.items() if k != "ok"}
    if case.get("rule_path"):
        base = get_path(base, case["rule_path"], subst)
    old = get_path(base, case["path"], subst)
    new = apply_change(old, case["change"])
    body = set_path(base, case["path"], new, subst)
    if case.get("wrap"):
        body = {case["wrap"]: body}
    return body, old, new


def guard_ok(get_result, case, subst):
    g = case.get("guard")
    if not g:
        return True
    base = {k: v for k, v in get_result.items() if k != "ok"}
    if case.get("rule_path"):
        base = get_path(base, case["rule_path"], subst)
    return get_path(base, g["path"], subst) == g["equals"]


def burst_verdict(replies, poll_statuses):
    """replies: list of bodies (None = incomplete/failed). PASS when each is ok or exactly busy, and polls were all 200."""
    bad = []
    for i, r in enumerate(replies):
        if isinstance(r, dict) and r.get("ok") is True:
            continue
        if isinstance(r, dict) and r.get("ok") is False and r.get("error") == "busy":
            continue
        bad.append("reply %d: %s" % (i, _short(r, 80)))
    non200 = sorted(set(s for s in poll_statuses if s != 200))
    if non200:
        bad.append("poller saw HTTP %s" % non200)
    if not poll_statuses:
        bad.append("poller made no request")
    return bad



class _FakeBoard:
    """Board stand-in: holds one rule; options simulate the failures run_write must survive."""
    def __init__(self, rules, force_pw=False, raise_on_post=False, restore_status=200):
        self.rules, self.force_pw, self.raise_on_post, self.restore_status = rules, force_pw, raise_on_post, restore_status

    def request(self, verb, path, body=None):
        if self.force_pw:
            return 403, json.dumps({"ok": False, "error": "force_password_change"})
        if verb == "GET":
            return 200, json.dumps({"ok": True, "rules": copy.deepcopy(self.rules), "max": 8})
        if self.raise_on_post:
            raise board_http.BoardError("POST %s: connection reset" % path)
        if self.restore_status != 200:
            return self.restore_status, "{}"
        self.rules[0].update(body)
        return 200, json.dumps({"ok": True})


class _FakeTb:
    def __init__(self, rules, write_status=200, apply_on_fail=True):
        self.rules, self.write_status, self.apply_on_fail, self.writes = rules, write_status, apply_on_fail, 0

    def rpc_twoway(self, dev, method, params, timeout_ms=10000):
        if method == "alarms.rules.get":
            return 200, {"ok": True, "rules": copy.deepcopy(self.rules), "max": 8}
        self.writes += 1
        if self.write_status == 200 or self.apply_on_fail:
            self.rules[0].update(params)
        if self.write_status == 200:
            return 200, {"ok": True}
        return self.write_status, "timeout"


def _write_scenario(board_kw=None, tb_kw=None):
    rules = [{"id": 3, "enabled": False, "delay_sec": 4}]
    board, tb = _FakeBoard(rules, **(board_kw or {})), _FakeTb(rules, **(tb_kw or {}))
    r = Runner(tb, "dev", board)
    case = json.loads(CASES_FILE.read_text(encoding="utf-8"))["alarms"]["writes"][0]
    r.run_write("alarms", case, 3)
    return r, rules, tb


def _quiet(fn, *a, **kw):
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **kw)


def cases_for_selftest():
    return json.loads(CASES_FILE.read_text(encoding="utf-8"))


def selftest():
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    check("compare equal", compare_bodies({"a": 1}, {"a": 1}) is None)
    check("compare differ", compare_bodies({"a": 1}, {"a": 2}) is not None)
    check("ignore top", compare_bodies({"a": 1, "t": 5}, {"a": 1, "t": 9}, ["t"]) is None)
    check("ignore deep", compare_bodies({"l": [{"a": 1, "t": 1}]}, {"l": [{"a": 1, "t": 2}]}, ["t"]) is None)
    check("ignore keeps other", compare_bodies({"l": [{"a": 1, "t": 1}]}, {"l": [{"a": 2, "t": 1}]}, ["t"]) is not None)
    check("error_code", error_code({"ok": False, "error": "x"}) == "x" and error_code({"ok": True}) is None)
    sample = {"ok": True, "rules": [{"id": 3, "delay_sec": 4, "enabled": False}, {"id": 7, "delay_sec": 9}], "max": 8}
    sub = {"RULE_ID": 7}
    check("get_path id", get_path(sample, "rules[id=$RULE_ID].delay_sec", sub) == 9)
    check("get_path index", get_path(sample, "rules.0.id") == 3)
    changed = set_path(sample, "rules[id=$RULE_ID].delay_sec", 10, sub)
    check("set_path", changed["rules"][1]["delay_sec"] == 10 and sample["rules"][1]["delay_sec"] == 9)
    check("set_path index", set_path(sample, "rules.0.delay_sec", 0)["rules"][0]["delay_sec"] == 0)
    check("apply add", apply_change(4, {"add": 1}) == 5)
    check("apply value", apply_change(4, {"value": 0}) == 0)
    case = {"rule_path": "rules[id=$RULE_ID]", "path": "delay_sec", "change": {"add": 1}}
    body, old, new = write_body(sample, case, {"RULE_ID": 3})
    check("write_body", body == {"id": 3, "delay_sec": 5, "enabled": False} and (old, new) == (4, 5) and "ok" not in body)
    check("guard disabled", guard_ok(sample, dict(case, guard={"path": "enabled", "equals": False}), {"RULE_ID": 3}))
    check("guard enabled", not guard_ok({"rules": [{"id": 3, "enabled": True}]},
                                        dict(case, guard={"path": "enabled", "equals": False}), {"RULE_ID": 3}))
    pcase = cases_for_selftest()["protection"]["writes"][0]
    pget = {"ok": True, "pump": {"enabled": True, "prime_sec": 5, "max_retries": 3}, "pump_status": {"trips": 0}}
    pbody, pold, pnew = write_body(pget, pcase, {})
    check("write_body wrap", pbody == {"pump": {"enabled": True, "prime_sec": 6, "max_retries": 3}}
          and (pold, pnew) == (5, 6) and pcase["path"] == "prime_sec")
    check("no rule id needed", "$RULE_ID" not in json.dumps(pcase))
    check("burst ok", burst_verdict([{"ok": True}, {"ok": False, "error": "busy"}], [200, 200]) == [])
    check("burst bad reply", len(burst_verdict([{"ok": False, "error": "x"}, None], [200])) == 2)
    check("burst bad poll", len(burst_verdict([{"ok": True}], [200, 503])) == 1)
    cases = json.loads(CASES_FILE.read_text(encoding="utf-8"))
    check("cases file has alarms and protection", "alarms" in cases and "protection" in cases)
    # run_write must never leave the board changed (Runner is defined below; resolved at call time)
    r, rules, tb = _quiet(_write_scenario)
    check("write ok restores", r.counts["PASS"] == 1 and rules[0]["delay_sec"] == 4)
    r, rules, tb = _quiet(_write_scenario, None, {"write_status": 504})
    check("a: TB timeout, applied -> restored, FAIL", r.counts["FAIL"] == 1 and rules[0]["delay_sec"] == 4
          and not r.stop_writes)
    r, rules, tb = _quiet(_write_scenario, {"restore_status": 500}, {"write_status": 504})
    check("a: TB timeout, restore fails -> stop", r.counts["FAIL"] == 1 and r.stop_writes)
    r, rules, tb = _quiet(_write_scenario, {"force_pw": True})
    check("b: default password -> SKIP, no write", r.counts["SKIP"] == 1 and tb.writes == 0 and rules[0]["delay_sec"] == 4)
    r, rules, tb = _quiet(_write_scenario, {"raise_on_post": True})
    check("c: BoardError on restore -> stop", r.counts["FAIL"] == 1 and r.stop_writes)
    if fails:
        print("SELFTEST FAIL: " + ", ".join(fails))
        return 1
    print("SELFTEST PASS")
    return 0


# ---------------------------------------------------------------- live runner

class Runner:
    def __init__(self, tb, device_id, board):
        self.tb, self.device_id, self.board = tb, device_id, board
        self.fails = 0
        self.counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
        self.stop_writes = False

    def report(self, status, name, reason=""):
        self.counts[status] += 1
        print("%-5s %s%s" % (status, name, (" - " + reason) if reason else ""))

    def rpc(self, method, params):
        """(status, body); an HTTP error from ThingsBoard (408/504 on timeout) is returned as status with text body."""
        return self.tb.rpc_twoway(self.device_id, method, params)

    def http(self, verb, path, body=None):
        status, text = self.board.request(verb, path, body)
        try:
            return status, json.loads(text)
        except ValueError:
            return status, text

    def default_pw(self, status, body):
        return status == 403 and isinstance(body, dict) and body.get("error") == "force_password_change"

    def rpc_failed(self, status):
        return status != 200

    def run_read(self, group, c):
        name = "%s/read %s" % (group, c["method"])
        rs, rb = self.rpc(c["method"], {})
        if self.rpc_failed(rs):
            return self.report("FAIL", name, "rpc HTTP %s: %s" % (rs, _short(rb, 100)))
        hs, hb = self.http("GET", c["http"])
        if self.default_pw(hs, hb):
            return self.report("SKIP", name, FORCE_PW_MSG)
        if hs != 200:
            return self.report("FAIL", name, "board HTTP %s" % hs)
        diff = compare_bodies(rb, hb, c.get("ignore", []))
        self.report("FAIL" if diff else "PASS", name, diff or "")

    def run_error(self, group, c):
        name = "%s/error %s %s" % (group, c["method"], _short(c.get("params"), 40))
        rs, rb = self.rpc(c["method"], c.get("params"))
        if self.rpc_failed(rs):
            return self.report("FAIL", name, "rpc HTTP %s: %s" % (rs, _short(rb, 100)))
        code = error_code(rb)
        if code is None:
            return self.report("FAIL", name, "rpc did not return an error: %s" % _short(rb, 100))
        want = c.get("error")
        if want and code != want:
            return self.report("FAIL", name, "rpc error %r, expected %r" % (code, want))
        if not c.get("http"):
            return self.report("PASS", name, "error=%s (no HTTP side)" % code)
        hs, hb = self.http("POST", c["http"], c.get("body", c.get("params")))
        if self.default_pw(hs, hb):
            return self.report("SKIP", name, FORCE_PW_MSG)
        hcode = error_code(hb)
        if hcode != code:
            return self.report("FAIL", name, "rpc error %r but HTTP %s gave %r" % (code, hs, hcode))
        self.report("PASS", name, "error=%s" % code)

    def _value(self, body, c, subst):
        base = {k: v for k, v in body.items() if k != "ok"}
        return get_path(base, c["rule_path"] + "." + c["path"] if c.get("rule_path") else c["path"], subst)

    def _http_value(self, c, subst):
        """(value, error_text) read through the board's HTTP side; never raises."""
        try:
            hs, hb = self.http("GET", c["get_http"])
            if hs != 200 or not isinstance(hb, dict):
                return None, "HTTP %s" % hs
            return self._value(hb, c, subst), None
        except (board_http.BoardError, KeyError, IndexError, TypeError, AttributeError) as e:
            return None, str(e)

    def run_write(self, group, c, rule_id):
        name = "%s/write %s" % (group, c["name"])
        if self.stop_writes:
            return self.report("SKIP", name, "an earlier restore failed, writes stopped")
        if rule_id is None and "$RULE_ID" in json.dumps(c):
            return self.report("SKIP", name, "needs --rule-id (a disabled rule)")
        subst = {"RULE_ID": rule_id}
        rs, rb = self.rpc(c["get"], {})
        if self.rpc_failed(rs) or not isinstance(rb, dict) or rb.get("ok") is not True:
            return self.report("FAIL", name, "read via TB failed: %s %s" % (rs, _short(rb, 100)))
        try:
            if not guard_ok(rb, c, subst):
                return self.report("SKIP", name, "rule %s is not disabled (guard failed)" % rule_id)
            set_body, old, new = write_body(rb, c, subst)
            original_body = write_body(rb, dict(c, change={"value": old}), subst)[0]
        except (KeyError, IndexError, ValueError, TypeError) as e:
            return self.report("SKIP", name, "rule %s not usable: %s" % (rule_id, e))
        # preflight: the HTTP side must be usable, otherwise we could not read back or restore
        try:
            ps, pb = self.http("GET", c["get_http"])
        except board_http.BoardError as e:
            return self.report("SKIP", name, "board HTTP not reachable, no write done: %s" % e)
        if self.default_pw(ps, pb):
            return self.report("SKIP", name, FORCE_PW_MSG)
        if ps != 200:
            return self.report("SKIP", name, "board HTTP preflight gave %s, no write done" % ps)
        # the write; whatever happens next the value is read back and restored if it differs from the original
        ws, wb = self.rpc(c["set"], set_body)
        write_ok = not self.rpc_failed(ws) and isinstance(wb, dict) and wb.get("ok") is True
        problem = None if write_ok else "write via TB failed: %s %s" % (ws, _short(wb, 100))
        seen, err = self._http_value(c, subst)
        if err is None and write_ok and seen != new:
            problem = "HTTP readback shows %r, expected %r" % (seen, new)
        restored = err is None and seen == old
        rest_note = ""
        if not restored:
            try:
                rest_status, rest_body = self.http("POST", c["set_http"], original_body)
                rest_ok = rest_status == 200 and isinstance(rest_body, dict) and rest_body.get("ok") is True
                rest_note = "restore HTTP %s" % rest_status
            except board_http.BoardError as e:
                rest_ok, rest_note = False, "restore raised: %s" % e
            if rest_ok:
                back, berr = self._http_value(c, subst)
                restored = berr is None and back == old
                if not restored:
                    rest_note = "restore verify: %s" % (berr or "value %r" % (back,))
        if restored and write_ok:
            ts, tb_ = self.rpc(c["get"], {})
            try:
                restored = ts == 200 and self._value(tb_, c, subst) == old
            except (KeyError, IndexError, TypeError, AttributeError):
                restored = False
            if not restored:
                rest_note = "read via TB after restore does not show the original"
        if not restored:
            self.stop_writes = True
            print("      RESTORE FAILED: set %s.%s back to %r by hand (rule %s); remaining writes stopped"
                  % (c.get("rule_path", ""), c["path"], old, rule_id))
            return self.report("FAIL", name, "; ".join(x for x in (problem, rest_note) if x) or "restore failed")
        self.report("FAIL" if problem else "PASS", name, problem or "%r -> %r -> restored" % (old, new))

    def run_burst(self, n):
        name = "burst %d x alarms.rules.get" % n
        statuses, stop = [], threading.Event()

        def poll():
            while not stop.is_set():
                try:
                    s, _ = self.board.request("GET", "/api/alarms/summary")
                except board_http.BoardError:
                    s = 0
                statuses.append(s)
                time.sleep(0.2)

        t = threading.Thread(target=poll, daemon=True)
        t.start()
        replies = []
        try:
            for _ in range(n):
                s, b = self.rpc("alarms.rules.get", {})
                replies.append(b if s == 200 else {"rpc_http_status": s, "body": b})
        finally:
            stop.set()
            t.join(2)
        if statuses and all(s == 403 for s in statuses):
            return self.report("SKIP", name, FORCE_PW_MSG)
        bad = burst_verdict(replies, statuses)
        busy = sum(1 for r in replies if isinstance(r, dict) and r.get("error") == "busy")
        self.report("FAIL" if bad else "PASS", name,
                    "; ".join(bad[:3]) if bad else "%d replies (%d busy), %d polls all 200" % (n, busy, len(statuses)))


def main():
    tb_api.utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    tb_api.add_common_args(ap)
    board_http.add_board_args(ap)
    ap.add_argument("--device", default=DEFAULT_DEVICE, help="ThingsBoard device name (default %(default)s)")
    ap.add_argument("--group", action="append", help="only this group (repeatable); default all")
    ap.add_argument("--writes", action="store_true", help="also run the reversible write cases")
    ap.add_argument("--rule-id", type=int, help="id of a DISABLED alarm rule the alarms write case may edit")
    ap.add_argument("--burst", type=int, metavar="N", help="send N alarms.rules.get RPCs back to back while polling HTTP")
    ap.add_argument("--selftest", action="store_true", help="offline: check the compare/path/change helpers")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    cases = json.loads(CASES_FILE.read_text(encoding="utf-8"))
    groups = args.group or list(cases)
    unknown = [g for g in groups if g not in cases]
    if unknown:
        ap.error("unknown group(s): %s (have: %s)" % (", ".join(unknown), ", ".join(cases)))
    tb = tb_api.connect_from_args(args, ap)
    try:
        dev = tb.get("/api/tenant/devices", {"deviceName": args.device}, ok404=True)
    except tb_api.TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    if not dev:
        print("ERROR: device %r not found" % args.device, file=sys.stderr)
        return 1
    board = board_http.board_from_args(args)
    r = Runner(tb, dev["id"]["id"], board)
    try:
        for g in groups:
            for c in cases[g].get("reads", []):
                r.run_read(g, c)
            for c in cases[g].get("errors", []):
                r.run_error(g, c)
            for c in cases[g].get("writes", []):
                if args.writes:
                    r.run_write(g, c, args.rule_id)
                else:
                    r.report("SKIP", "%s/write %s" % (g, c["name"]), "needs --writes")
        if args.burst:
            r.run_burst(args.burst)
    except board_http.BoardError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        r.counts["FAIL"] += 1
    print("summary: %(PASS)d PASS, %(FAIL)d FAIL, %(SKIP)d SKIP" % r.counts)
    return 1 if r.counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
