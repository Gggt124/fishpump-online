"""Parity test: an RPC method through ThingsBoard must answer like the board's HTTP endpoint.

    python thingsboard/parity_test.py --selftest
    python thingsboard/parity_test.py --user tenant@example.com --device fishpump-1de564 [--group alarms]
    python thingsboard/parity_test.py ... --writes --rule-id 3        # also the reversible write cases
    python thingsboard/parity_test.py ... --burst 30                  # RPC burst while polling /api/alarms/summary

Besides the RPC cases it checks the client attributes and the status telemetry keys (state_cases.json; --group state
runs only those). With --writes the write cases that have an "attr" also check that the attribute follows the change.

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
STATE_FILE = HERE / "state_cases.json"
FOLLOW_POLL_S = 0.25
# the clocks are module-level so the selftest can run the follow polling instantly
_sleep = time.sleep
_now = time.monotonic
_wall = time.time


# ---------------------------------------------------------------- pure helpers (selftested)

def strip_keys(obj, ignore, path=""):
    """Deep copy without the ignored keys. A plain name is ignored at any level; a dotted entry
    ("status.output_mv") only at that path from the reply root (lists do not add a path segment)."""
    if isinstance(obj, dict):
        return {k: strip_keys(v, ignore, path + k + ".") for k, v in obj.items()
                if k not in ignore and (path + k) not in ignore}
    if isinstance(obj, list):
        return [strip_keys(v, ignore, path) for v in obj]
    return obj


def compare_bodies(rpc, http, ignore=()):
    """None when equal after dropping the ignored keys, else a short reason."""
    a, b = strip_keys(rpc, set(ignore)), strip_keys(http, set(ignore))
    if a == b:
        return None
    return "bodies differ: rpc=%s http=%s" % (_short(a), _short(b))


def diff_path(a, b, path=""):
    """Dotted path of the first difference between two JSON values ('' at the root), or None when equal."""
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            sub = path + ("." if path else "") + k
            if k not in a or k not in b:
                return sub
            d = diff_path(a[k], b[k], sub)
            if d is not None:
                return d
        return None
    if isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            d = diff_path(x, y, path + ("." if path else "") + str(i))
            if d is not None:
                return d
        return None
    return None if a == b else (path or "(root)")


def ignore_for(cases, http):
    """The read ignore list of the parity case that reads this HTTP path ([] when there is none)."""
    for group in cases.values():
        for c in group.get("reads", []):
            if c.get("http") == http:
                return c.get("ignore", [])
    return []


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
    if change.get("same"):
        return old
    raise ValueError("change needs add, value or same")


def write_body(get_result, case, subst):
    """Body for the set call: the GET result without ok, narrowed to rule_path, the path changed by `change`.
    A case with "wrap" nests the body under that key (the set method wants {"pump": {...}}).
    A case with "omit" drops those top-level keys first (read-only fields the set method rejects).
    A case with "send_as" sends only {send_as: new value} (the set method takes one differently named key).
    Returns (set_body, old_value, new_value)."""
    omit = set(case.get("omit") or ())
    base = {k: v for k, v in get_result.items() if k != "ok" and k not in omit}
    if case.get("rule_path"):
        base = get_path(base, case["rule_path"], subst)
    old = get_path(base, case["path"], subst)
    new = apply_change(old, case["change"])
    body = set_path(base, case["path"], new, subst)
    if case.get("send_as"):
        body = {case["send_as"]: new}
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


def shape_of(obj):
    """Structure of a reply without its values: dict -> {key: shape}, list -> [shape of the first element], scalars -> type name."""
    if isinstance(obj, dict):
        return {k: shape_of(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [shape_of(obj[0])] if obj else []
    if isinstance(obj, bool):
        return "bool"
    if isinstance(obj, (int, float)):
        return "number"
    return type(obj).__name__


def shapes_match(a, b):
    """True when two shape_of() results agree. An empty list matches any list (a scan may find no network)."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(shapes_match(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return not a or not b or shapes_match(a[0], b[0])
    return a == b


def fill_param(params, spec, reply):
    """params with spec["key"] set to the non-empty string found in the reply at spec["path"]; None when there is none.
    Lets an error case name something only the live board knows (the SSID it is connected to)."""
    try:
        value = get_path(reply, spec["path"])
    except (KeyError, IndexError, TypeError, AttributeError):
        return None
    if not isinstance(value, str) or not value:
        return None
    out = dict(params or {})
    out[spec["key"]] = value
    return out


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


class _FakeTbUnknown:
    def rpc_twoway(self, dev, method, params, timeout_ms=10000):
        return 200, {"ok": False, "error": "unknown_method"}


class _FakeTbProfiles:
    """TB stand-in for the in_use case: wifi.profiles.get returns the given reply, forget refuses the connected SSID."""
    def __init__(self, profiles_reply):
        self.reply = profiles_reply

    def rpc_twoway(self, dev, method, params, timeout_ms=10000):
        if method == "wifi.profiles.get":
            return 200, copy.deepcopy(self.reply)
        connected = [p["ssid"] for p in self.reply.get("profiles", []) if p.get("connected")]
        if method == "wifi.profiles.forget" and params.get("ssid") in connected:
            return 200, {"ok": False, "error": "in_use", "message": "Cannot forget the network in use"}
        return 200, {"ok": True}


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


class _FakeBoardMap:
    """Board stand-in that answers GET <path> from a dict of replies."""
    def __init__(self, replies):
        self.replies = replies

    def request(self, verb, path, body=None):
        return 200, json.dumps(self.replies[path])


class _FakeTbState:
    """TB stand-in with a client-attribute store and a latest-telemetry store."""
    def __init__(self, attrs=None, tele=None):
        self.attrs, self.tele = attrs or {}, tele or {}

    def client_attributes(self, dev, keys):
        return {k: copy.deepcopy(v) for k, v in self.attrs.items() if k in keys}

    def latest_telemetry(self, dev, keys):
        return {k: v for k, v in self.tele.items() if k in keys}


class _FakeTbAttr(_FakeTb):
    """Writes land in the shared rule list at once, but the attribute TB shows follows only `lag` polls later
    (never = it never follows)."""
    def __init__(self, rules, lag=2, never=False):
        super().__init__(rules)
        self.lag, self.never, self.shown, self.stale = lag, never, rules[0]["delay_sec"], 0

    def client_attributes(self, dev, keys):
        cur = self.rules[0]["delay_sec"]
        if cur != self.shown and not self.never:
            self.stale += 1
            if self.stale > self.lag:
                self.shown, self.stale = cur, 0
        return {"cfg_t": {"x": {"v": self.shown}}}


class _GetOnlyTb(tb_api.TbApi):
    """TbApi whose GET returns canned data, to test the attribute/telemetry parsing offline."""
    def __init__(self, reply):
        self.reply, self.calls = reply, []

    def get(self, path, params=None, **kw):
        self.calls.append((path, params))
        return self.reply


class _FakeTbBoom(_FakeTbAttr):
    def client_attributes(self, dev, keys):
        raise RuntimeError("boom")


def _follow_scenario(lag=2, never=False, boom=False):
    rules = [{"id": 3, "enabled": False, "delay_sec": 4}]
    tb = (_FakeTbBoom if boom else _FakeTbAttr)(rules, lag, never)
    r = Runner(tb, "dev", _FakeBoard(rules), state={"follow_timeout_s": 5, "follow_pass_s": 3})
    case = dict(json.loads(CASES_FILE.read_text(encoding="utf-8"))["alarms"]["writes"][0],
                attr={"key": "cfg_t", "path": "x.v"})
    _quiet(r.run_write, "alarms", case, 3)
    return r, rules


def selftest_state(check):
    global _sleep, _now, _wall
    saved = (_sleep, _now, _wall)
    clock = [0.0]
    _now = lambda: clock[0]
    _sleep = lambda s: clock.__setitem__(0, clock[0] + s)
    _wall = lambda: 1000.0
    try:
        case = {"key": "cfg_pump", "http": "/api/pump/config", "drop": ["ok"]}
        board = _FakeBoardMap({"/api/pump/config": {"ok": True, "a": {"b": 1}, "c": 2}})
        r = Runner(_FakeTbState({"cfg_pump": {"a": {"b": 1}, "c": 2}}), "dev", board)
        _quiet(r.run_attr, case, [])
        check("attr equal to the HTTP reply minus drop is PASS", r.counts["PASS"] == 1 and r.counts["FAIL"] == 0
              and r.rows[0][1] == "attr cfg_pump")
        r = Runner(_FakeTbState({"cfg_pump": {"a": {"b": 9}, "c": 2}}), "dev", board)
        _quiet(r.run_attr, case, [])
        check("attr with a different value is FAIL naming the path",
              r.counts["FAIL"] == 1 and "a.b" in r.rows[0][2])
        r = Runner(_FakeTbState({}), "dev", board)
        _quiet(r.run_attr, case, [])
        check("a missing attribute is FAIL", r.counts["FAIL"] == 1)
        r = Runner(_FakeTbState({"cfg_pump": {"a": {"b": 1}, "c": 3}}), "dev", board)
        _quiet(r.run_attr, case, ["c"])
        check("attr honours the read ignore list", r.counts["PASS"] == 1)
        r = Runner(_FakeTbState({"wifi": {"x": 1}}), "dev", None)
        _quiet(r.run_attr_present, "wifi")
        _quiet(r.run_attr_present, "tb_link")
        check("attr present: PASS when there, FAIL when not", r.counts["PASS"] == 1 and r.counts["FAIL"] == 1
              and r.rows[0][1] == "attr wifi present")
        tele = {"pump_phase": (1000000 - 5000, "idle"), "old_key": (1000000 - 60000, 1)}
        r = Runner(_FakeTbState(tele=tele), "dev", None)
        _quiet(r.run_status, ["pump_phase", "old_key", "gone_key"], 45)
        by = {row[1]: row[0] for row in r.rows}
        check("status: fresh PASS, 60 s old FAIL, missing FAIL",
              by == {"status pump_phase": "PASS", "status old_key": "FAIL", "status gone_key": "FAIL"})
        g = _GetOnlyTb([{"key": "cfg_x", "value": '{"a": 1}', "lastUpdateTs": 1}, {"key": "n", "value": 5},
                        {"key": "s", "value": "hello"}])
        got = g.client_attributes("dev", ["cfg_x", "n", "s"])
        check("client_attributes decodes a string-encoded object and keeps other values",
              got == {"cfg_x": {"a": 1}, "n": 5, "s": "hello"}
              and g.calls[0][0].endswith("/DEVICE/dev/values/attributes/CLIENT_SCOPE")
              and g.calls[0][1] == {"keys": "cfg_x,n,s"})
        g = _GetOnlyTb({"k": [{"ts": 7, "value": "3"}], "e": []})
        check("latest_telemetry gives key -> (ts, value) and leaves empty keys out",
              g.latest_telemetry("dev", ["k", "e"]) == {"k": (7, "3")}
              and g.calls[0][0].endswith("/DEVICE/dev/values/timeseries"))
        clock[0] = 0.0
        r, rules = _follow_scenario(lag=2)
        fr = [row for row in r.rows if row[1].endswith("follow")]
        check("follow: the attribute updating after 2 polls is PASS with both delays",
              len(fr) == 1 and fr[0][0] == "PASS" and "follow" in fr[0][2] and " s / " in fr[0][2]
              and rules[0]["delay_sec"] == 4)
        class _Down:
            def request(self, verb, path, body=None):
                raise board_http.BoardError("GET %s: connection refused" % path)
        r = Runner(_FakeTbState({"cfg_pump": {"a": 1}, "wifi": {"x": 1}}), "dev", _Down())
        _quiet(r.run_attr, case, [])
        _quiet(r.run_attr_present, "wifi")
        check("attr with an unreachable board is a FAIL row and the run goes on",
              r.rows[0][0] == "FAIL" and "refused" in r.rows[0][2] and r.rows[1][0] == "PASS")
        clock[0] = 0.0
        r, rules = _follow_scenario(boom=True)
        fr = [row for row in r.rows if row[1].endswith("follow")]
        check("follow: an unexpected exception is FAIL and the restore still happened",
              len(fr) == 1 and fr[0][0] == "FAIL" and "boom" in fr[0][2] and rules[0]["delay_sec"] == 4
              and not r.stop_writes)
        clock[0] = 0.0
        r, rules = _follow_scenario(never=True)
        fr = [row for row in r.rows if row[1].endswith("follow")]
        check("follow: an attribute that never updates is FAIL after the timeout, showing the last value",
              len(fr) == 1 and fr[0][0] == "FAIL" and "4" in fr[0][2] and clock[0] >= 5.0
              and rules[0]["delay_sec"] == 4)
    finally:
        _sleep, _now, _wall = saved


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
    check("ignore dotted path only at its path",
          compare_bodies({"s": {"t": 1}, "t": 5}, {"s": {"t": 2}, "t": 5}, ["s.t"]) is None
          and compare_bodies({"s": {"t": 1}, "t": 5}, {"s": {"t": 1}, "t": 6}, ["s.t"]) is not None
          and compare_bodies({"s": {"t": 1}, "l": [{"t": 1}]}, {"s": {"t": 1}, "l": [{"t": 2}]}, ["s.t"]) is not None)
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
    ocase = {"path": "b", "change": {"add": 60}, "omit": ["ro"]}
    obody, oold, onew = write_body({"ok": True, "a": 1, "b": 5, "ro": 9}, ocase, {})
    check("write_body omit", obody == {"a": 1, "b": 65} and (oold, onew) == (5, 65))
    check("apply_change same keeps the value", apply_change("auto", {"same": True}) == "auto")
    mcase = [w for w in cases_for_selftest()["cooling"]["writes"] if w["set"] == "cooling.mode.set"][0]
    mget = {"ok": True, "mode": "auto", "auto_enable": True, "points": [], "status": {"mode": "auto"}}
    mbody, mold, mnew = write_body(mget, mcase, {})
    check("cooling mode write sends only the current mode", mbody == {"mode": "auto"} and mold == mnew == "auto")
    ocase2 = cases_for_selftest()["water_power"]["writes"][0]
    oget = {"ok": True, "water": {"offsets": {"known": True, "ph": 0.05, "temp": -0.5}, "ph": 7.0}}
    obody2, oold2, onew2 = write_body(oget, ocase2, {})
    check("water offset write sends only the current ph offset",
          obody2 == {"ph_offset": 0.05} and oold2 == onew2 == 0.05 and guard_ok(oget, ocase2, {}))
    check("water offset guard needs known offsets",
          not guard_ok({"water": {"offsets": {"known": False, "ph": 0, "temp": 0}}}, ocase2, {}))
    wcase = cases_for_selftest()["pump"]["writes"][0]
    wget = {"ok": True, "water_set1_off": 30, "float_gpio": 4, "relay_gpio": 5, "debounce_ms": 2000,
            "hardware_reboot_required": False, "pump_relay1_gpio": 5, "pump_relay2_gpio": 6, "ds18b20_gpio": 7,
            "cooling_relay_gpio": 8, "cooling_relay_polarity": "active_low", "relay1_polarity": "active_low"}
    wbody, wold, wnew = write_body(wget, wcase, {})
    check("pump write drops read-only fields", wnew == wold + 60 and "float_gpio" not in wbody and "ok" not in wbody
          and wbody["relay1_polarity"] == "active_low")
    real_get = json.loads('{"ok":true,"water_set1_on":50,"water_set1_off":60,"water_set1_start_phase":"on","water_set2_on":10,"water_set2_off":180,"water_set2_start_phase":"on","wash_on":10,"wash_off":60,"wash_start_phase":"ON","water_auto_start":true,"wash_auto_start":false,"timer1_on_sec":50,"timer1_off_sec":60,"timer2_on_sec":10,"timer2_off_sec":180,"auto_start":true,"relay_polarity":"active_low","float_gpio":16,"relay_gpio":5,"debounce_ms":2000,"min_dwell_sec":30,"pump_relay1_gpio":5,"pump_relay2_gpio":6,"ds18b20_gpio":10,"cooling_relay_gpio":8,"relay1_polarity":"active_low","relay2_polarity":"active_low","timer1_start_phase":"on","timer2_start_phase":"on","cooling_relay_polarity":"active_low","hardware_reboot_required":false,"hardware_fields_read_only":true,"hardware_map_status":"defaults_missing","settings_status":"loaded"}')
    rbody, _, _ = write_body(real_get, wcase, {})
    dropped = ("hardware_fields_read_only", "hardware_map_status", "settings_status", "timer1_on_sec", "timer1_off_sec",
               "timer2_on_sec", "timer2_off_sec", "auto_start", "relay_polarity", "timer1_start_phase", "timer2_start_phase")
    kept = ("water_set1_on", "water_set1_off", "water_set1_start_phase", "water_set2_on", "water_set2_off",
            "water_set2_start_phase", "wash_on", "wash_off", "wash_start_phase", "water_auto_start", "wash_auto_start",
            "relay1_polarity", "relay2_polarity", "min_dwell_sec")
    check("pump write body from the real get reply is under 512 bytes",
          len(json.dumps(rbody, separators=(",", ":"))) < 512)
    check("pump write body drops status keys and alias duplicates", not any(k in rbody for k in dropped))
    check("pump write body keeps every editable primary key and changes water_set1_off to 120",
          all(k in rbody for k in kept) and rbody.get("water_set1_off") == 120)
    r = Runner(_FakeTbUnknown(), "dev", None)
    _quiet(r.run_write, "water_power", ocase2, None)
    check("write on a missing get method SKIPs with the method name", r.counts["SKIP"] == 1 and r.counts["FAIL"] == 0
          and r.skip_reasons == ["needs sensors.get"])
    sh = shape_of({"ok": True, "networks": [{"ssid": "a", "rssi": -50, "auth": "WPA2", "channel": 6, "connected": False}]})
    check("shape ignores values and list length",
          shapes_match(sh, shape_of({"ok": True, "networks": [{"ssid": "b", "rssi": -70, "auth": "OPEN", "channel": 1, "connected": True},
                                                                {"ssid": "c", "rssi": -80, "auth": "WPA", "channel": 11, "connected": False}]}))
          and shapes_match(sh, shape_of({"ok": True, "networks": []})))
    check("shape catches a missing key, a changed type and an error reply",
          not shapes_match(sh, shape_of({"ok": True}))
          and not shapes_match(sh, shape_of({"ok": True, "networks": [{"ssid": 5, "rssi": -50, "auth": "x", "channel": 6, "connected": False}]}))
          and not shapes_match(sh, shape_of({"ok": False, "error": "scan_busy"})))
    fspec = cases_for_selftest()["wifi"]["errors"][2]["params_from"]
    prof = {"ok": True, "auto_index": 0, "profiles": [{"ssid": "farm", "auto": True, "connected": False},
                                                       {"ssid": "shed", "auto": False, "connected": True}]}
    check("fill_param takes the connected SSID from the reply", fill_param({}, fspec, prof) == {"ssid": "shed"})
    check("fill_param gives None when nothing is connected or the reply is an error",
          fill_param({}, fspec, {"ok": True, "profiles": [{"ssid": "farm", "connected": False}]}) is None
          and fill_param({}, fspec, {"ok": False, "error": "x"}) is None)
    r = Runner(_FakeTbProfiles(prof), "dev", None)
    _quiet(r.run_error, "wifi", cases_for_selftest()["wifi"]["errors"][2])
    check("in_use case sends the connected SSID and expects in_use", r.counts["PASS"] == 1 and r.counts["FAIL"] == 0)
    r = Runner(_FakeTbProfiles({"ok": True, "profiles": []}), "dev", None)
    _quiet(r.run_error, "wifi", cases_for_selftest()["wifi"]["errors"][2])
    check("in_use case SKIPs when no network is connected", r.counts["SKIP"] == 1 and r.counts["FAIL"] == 0)
    wifi = cases_for_selftest()["wifi"]
    check("wifi case file: connect and disconnect have no HTTP side, setauto write is a no-op",
          all(e["http"] is None for e in wifi["errors"] if e["method"] in ("wifi.connect", "wifi.disconnect"))
          and wifi["writes"][0]["change"] == {"same": True} and wifi["writes"][0]["send_as"] == "index")
    sa = [(e["params"], e["http"], e["error"]) for e in wifi["errors"] if e["method"] == "wifi.profiles.setauto"]
    check("wifi setauto: TB-only in_use cases for index 99 and -1, no setauto_failed from ThingsBoard",
          ({"index": 99}, None, "in_use") in sa and ({"index": -1}, None, "in_use") in sa
          and not any(e.get("error") == "setauto_failed" for e in wifi["errors"]))
    check("burst ok", burst_verdict([{"ok": True}, {"ok": False, "error": "busy"}], [200, 200]) == [])
    check("burst bad reply", len(burst_verdict([{"ok": False, "error": "x"}, None], [200])) == 2)
    check("burst bad poll", len(burst_verdict([{"ok": True}], [200, 503])) == 1)
    cases = json.loads(CASES_FILE.read_text(encoding="utf-8"))
    check("cases file has alarms, protection and pump", "alarms" in cases and "protection" in cases and "pump" in cases)
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
    selftest_state(check)
    if fails:
        print("SELFTEST FAIL: " + ", ".join(fails))
        return 1
    print("SELFTEST PASS")
    return 0


# ---------------------------------------------------------------- live runner

class Runner:
    def __init__(self, tb, device_id, board, state=None):
        self.tb, self.device_id, self.board = tb, device_id, board
        self.state = state or {}  # state_cases.json (follow timing)
        self.fails = 0
        self.counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}
        self.stop_writes = False
        self.skip_reasons = []
        self.rows = []

    def report(self, status, name, reason=""):
        self.counts[status] += 1
        self.rows.append((status, name, reason))
        if status == "SKIP":
            self.skip_reasons.append(reason)
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
        if c.get("shape"):  # the values change from scan to scan: only the structure must agree
            diff = None if shapes_match(shape_of(rb), shape_of(hb)) else "shapes differ: rpc=%s http=%s" % (
                _short(shape_of(rb)), _short(shape_of(hb)))
        self.report("FAIL" if diff else "PASS", name, diff or "")

    def run_error(self, group, c):
        name = "%s/error %s %s" % (group, c["method"], _short(c.get("params"), 40))
        params = c.get("params")
        if c.get("params_from"):  # a value only the live board knows, read through TB first
            spec = c["params_from"]
            ps, pb = self.rpc(spec["method"], {})
            if self.rpc_failed(ps) or not isinstance(pb, dict) or pb.get("ok") is not True:
                return self.report("FAIL", name, "read %s via TB failed: %s %s" % (spec["method"], ps, _short(pb, 100)))
            params = fill_param(params, spec, pb)
            if params is None:
                return self.report("SKIP", name, "no value for %s in %s" % (spec["key"], spec["method"]))
        rs, rb = self.rpc(c["method"], params)
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
        # valid only until the `get` method ships (sensors.get, Task 12)
        if not self.rpc_failed(rs) and error_code(rb) == "unknown_method":
            return self.report("SKIP", name, "needs %s" % c["get"])
        if self.rpc_failed(rs) or not isinstance(rb, dict) or rb.get("ok") is not True:
            return self.report("FAIL", name, "read via TB failed: %s %s" % (rs, _short(rb, 100)))
        try:
            if not guard_ok(rb, c, subst):
                return self.report("SKIP", name, "guard failed (%s must be %r)" % (c["guard"]["path"], c["guard"]["equals"]))
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
        attr = c.get("attr")
        follow_new = self._follow(attr, new) if attr and write_ok else None
        seen, err = self._http_value(c, subst)
        if err is None and write_ok and seen != new:
            problem = "HTTP readback shows %r, expected %r" % (seen, new)
        restored = err is None and seen == old
        rest_note = ""
        did_restore = False
        if not restored:
            did_restore = True
            try:
                rest_status, rest_body = self.http("POST", c["set_http"], original_body)
                rest_ok = rest_status in (200, 202) and isinstance(rest_body, dict) and rest_body.get("ok") is True
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
        if attr and write_ok and did_restore:
            self._report_follow("%s/write %s follow" % (group, c["name"]), follow_new, self._follow(attr, old))

    # ---- client attributes and status telemetry (state_cases.json)

    def _follow(self, attr, want):
        """Poll the client attribute until attr["path"] equals want. Returns (seconds until seen or None, last value seen)."""
        timeout = self.state.get("follow_timeout_s", 5)
        start, last = _now(), "(nothing read)"
        while True:
            try:
                value = self.tb.client_attributes(self.device_id, [attr["key"]]).get(attr["key"])
                last = "(attribute missing)" if value is None else get_path(value, attr["path"])
            except Exception as e:  # nothing here may skip the restore that follows
                last = "(error: %s)" % e
            if last == want:
                return _now() - start, last
            if _now() - start >= timeout:
                return None, last
            _sleep(FOLLOW_POLL_S)

    def _report_follow(self, name, new_res, old_res):
        limit = self.state.get("follow_pass_s", 3)
        (d_new, last_new), (d_old, last_old) = new_res, old_res
        if d_new is not None and d_old is not None and d_new <= limit and d_old <= limit:
            return self.report("PASS", name, "(follow %.1f s / %.1f s)" % (d_new, d_old))
        bad = []
        if d_new is None:
            bad.append("new value never seen, last %r" % (last_new,))
        elif d_new > limit:
            bad.append("new value seen after %.1f s (limit %s s)" % (d_new, limit))
        if d_old is None:
            bad.append("old value never seen, last %r" % (last_old,))
        elif d_old > limit:
            bad.append("old value seen after %.1f s (limit %s s)" % (d_old, limit))
        self.report("FAIL", name, "; ".join(bad))

    def run_attr(self, c, ignore):
        name = "attr %s" % c["key"]
        try:
            got = self.tb.client_attributes(self.device_id, [c["key"]])
        except tb_api.TbError as e:
            return self.report("FAIL", name, str(e))
        if c["key"] not in got:
            return self.report("FAIL", name, "attribute missing in ThingsBoard")
        try:
            hs, hb = self.http("GET", c["http"])
        except board_http.BoardError as e:
            return self.report("FAIL", name, "board not reachable: %s" % e)
        if self.default_pw(hs, hb):
            return self.report("SKIP", name, FORCE_PW_MSG)
        if hs != 200 or not isinstance(hb, dict):
            return self.report("FAIL", name, "board HTTP %s" % hs)
        want = {k: v for k, v in hb.items() if k not in c.get("drop", [])}
        if compare_bodies(got[c["key"]], want, ignore) is None:
            return self.report("PASS", name)
        a, b = strip_keys(got[c["key"]], set(ignore)), strip_keys(want, set(ignore))
        p = diff_path(a, b)
        try:
            tb_v, http_v = get_path(a, p), get_path(b, p)
        except (KeyError, IndexError, TypeError, ValueError):
            tb_v = http_v = "?"
        self.report("FAIL", name, "differs at %s: tb=%s http=%s" % (p, _short(tb_v, 80), _short(http_v, 80)))

    def run_attr_present(self, key):
        name = "attr %s present" % key
        try:
            got = self.tb.client_attributes(self.device_id, [key])
        except tb_api.TbError as e:
            return self.report("FAIL", name, str(e))
        self.report("PASS" if got.get(key) is not None else "FAIL", name,
                    "" if got.get(key) is not None else "attribute missing in ThingsBoard")

    def run_status(self, keys, max_age_s):
        try:
            got = self.tb.latest_telemetry(self.device_id, keys)
        except tb_api.TbError as e:
            return self.report("FAIL", "status keys", str(e))
        now_ms = _wall() * 1000.0
        for key in keys:
            name = "status %s" % key
            if key not in got:
                self.report("FAIL", name, "no telemetry value")
                continue
            age = (now_ms - got[key][0]) / 1000.0
            if age > max_age_s:
                self.report("FAIL", name, "last value is %.0f s old (limit %s s)" % (age, max_age_s))
            else:
                self.report("PASS", name)

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
    state = json.loads(STATE_FILE.read_text(encoding="utf-8"))
    run_state = not args.group or "state" in args.group  # the attribute and status rows
    groups = [g for g in (args.group or list(cases)) if g != "state"]
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
    r = Runner(tb, dev["id"]["id"], board, state)
    try:
        if run_state:
            for a in state["attributes"]:
                r.run_attr(a, ignore_for(cases, a["http"]))
            for key in state["attr_present"]:
                r.run_attr_present(key)
            r.run_status(state["status_keys"], state["status_max_age_s"])
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
