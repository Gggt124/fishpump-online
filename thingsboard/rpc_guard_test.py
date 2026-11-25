"""Live test of the dangerous-RPC guard and the shop path, with a fake board over MQTT (no real board needed).

    python thingsboard/rpc_guard_test.py --selftest
    python thingsboard/rpc_guard_test.py --user tenant@example.com [--customer-user customer@example.com] [--keep]

Run tb_setup.py first (it installs both rule chains and the profiles). This creates device fishpump-guardtest
(profile fishpump) and its shop asset, connects as that device over MQTT, answers every RPC with
{"ok": true, "echo": <method>} and records what arrived. Cases G1-G7 are in
docs/superpowers/plans/2026-11-25-thingsboard-shop-path-guard.md (Task 6).

Passwords: tenant from env TB_PASSWORD or a prompt; customer (G7) from env TB_CUSTOMER_PASSWORD or a prompt.
The test device and asset are deleted at the end unless --keep, or a case failed (kept for inspection; the
next run replaces them).
"""
import argparse
import getpass
import json
import os
import secrets
import sys
import threading
import time
import urllib.parse

import rpc_policy
import tb_setup
from mqtt_mini import MqttClient, MqttError
from tb_api import TbApi, TbError, add_common_args, connect_from_args, utf8_stdout

DEVICE_NAME = "fishpump-guardtest"
DIRECT_TIMEOUT_MS = 5000
RESULT_WAIT_S = 30
TOPIC_REQ = "v1/devices/me/rpc/request/"
TOPIC_RESP = "v1/devices/me/rpc/response/"
POLICY = rpc_policy.load()
SHOP = POLICY["shop"]


# ---------- pure helpers (selftest) ----------

def result_matches(attr_value, request_id):
    """True when an apply_result attribute value (object or JSON string) answers request_id."""
    v = attr_value
    if isinstance(v, str):
        try:
            v = json.loads(v)
        except ValueError:
            return False
    return isinstance(v, dict) and v.get("request_id") == request_id


def rpc_reached(log, method, since_index):
    return any(m == method for m, _ in log[since_index:])


def request_id_of(topic):
    """The RPC id at the end of v1/devices/me/rpc/request/<id>, or None for another topic."""
    if not topic.startswith(TOPIC_REQ):
        return None
    rid = topic[len(TOPIC_REQ):]
    return rid if rid.isdigit() else None


def selftest():
    failures = []

    def check(name, got, want):
        ok = got == want
        print("%s %s" % ("ok  " if ok else "FAIL", name))
        if not ok:
            print("     got  %r\n     want %r" % (got, want))
            failures.append(name)

    check("result object matches", result_matches({"request_id": "a", "ok": True}, "a"), True)
    check("result JSON string matches", result_matches('{"request_id": "a", "ok": false}', "a"), True)
    check("result other id", result_matches({"request_id": "b"}, "a"), False)
    check("result none", result_matches(None, "a"), False)
    check("result garbage string", result_matches("{nope", "a"), False)
    log = [("protection.get", {}), ("protection.set", {})]
    check("rpc reached after index", rpc_reached(log, "protection.set", 1), True)
    check("rpc not reached after index", rpc_reached(log, "protection.get", 1), False)
    check("request id", request_id_of("v1/devices/me/rpc/request/42"), "42")
    check("request id other topic", request_id_of("v1/devices/me/attributes"), None)
    print("SELFTEST %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 1 if failures else 0


# ---------- fake board ----------

class FakeBoard:
    """A device session that answers every RPC request with ok and records (method, params)."""

    def __init__(self, host, port, token):
        self.host, self.port, self.token = host, port, token
        self.log, self._lock = [], threading.Lock()
        self._stop, self._thread, self._client, self.error = None, None, None, None

    def start(self):
        c = MqttClient(self.host, self.port, client_id=DEVICE_NAME, username=self.token, keepalive=600)
        c.connect()
        c.subscribe(TOPIC_REQ + "+")
        self._client, self._stop = c, threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        time.sleep(1.0)  # let ThingsBoard register the subscription

    def _run(self):
        try:
            while not self._stop.is_set():
                got = self._client.recv_publish(0.5)
                if got is None:
                    continue
                topic, payload = got
                rid = request_id_of(topic)
                if rid is None:
                    continue
                try:
                    req = json.loads(payload.decode("utf-8"))
                except ValueError:
                    req = {}
                method = req.get("method")
                with self._lock:
                    self.log.append((method, req.get("params")))
                self._client.publish(TOPIC_RESP + rid, json.dumps({"ok": True, "echo": method}))
        except (MqttError, OSError) as e:
            if not self._stop.is_set():
                self.error = str(e)

    def mark(self):
        with self._lock:
            return len(self.log)

    def reached(self, method, since):
        with self._lock:
            return rpc_reached(self.log, method, since)

    def stop(self):
        if self._stop is not None:
            self._stop.set()
            self._thread.join(timeout=3)
            self._client.disconnect()
            self._stop = None


# ---------- cases ----------

class Runner:
    def __init__(self, api, device, asset, board):
        self.api, self.device, self.asset, self.board = api, device, asset, board
        self.did = device["id"]["id"]
        self.counts = {"PASS": 0, "FAIL": 0, "SKIP": 0}

    def report(self, verdict, name, detail=""):
        self.counts[verdict] += 1
        print("%-4s  %-58s %s" % (verdict, name, detail))

    def direct(self, method, params, api=None):
        """Send a direct two-way RPC; returns (http_status, reached_fake_board)."""
        since = self.board.mark()
        status, _ = (api or self.api).rpc_twoway(self.did, method, params, timeout_ms=DIRECT_TIMEOUT_MS)
        time.sleep(0.5)
        return status, self.board.reached(method, since)

    def expect_direct(self, name, method, params, should_reach, api=None):
        status, reached = self.direct(method, params, api)
        ok = (status == 200 and reached) if should_reach else (status != 200 and not reached)
        self.report("PASS" if ok else "FAIL", name,
                    "HTTP %s, %s the board" % (status, "reached" if reached else "did not reach"))

    def apply(self, request):
        """Write apply_request on the shop asset and wait for the matching apply_result. Returns it or None."""
        rid = request.get("request_id")
        tb_setup._write_server_attrs(self.api, self.asset["id"], {SHOP["request_key"]: request})
        deadline = time.time() + RESULT_WAIT_S
        while time.time() < deadline:
            val = tb_setup._server_attrs(self.api, self.asset["id"], [SHOP["result_key"]]).get(SHOP["result_key"])
            if rid is None:
                if isinstance(val, dict) and val.get("error") == "bad_request" and val.get("request_id") is None:
                    return val
            elif result_matches(val, rid):
                return json.loads(val) if isinstance(val, str) else val
            time.sleep(0.5)
        return None


def new_id(prefix):
    return "%s-%s" % (prefix, secrets.token_hex(4))


def run_cases(r, args, customer_title):
    api = r.api
    # G1
    r.expect_direct("G1 protection.get direct reaches the board", "protection.get", {}, True)
    # G2a
    for m in POLICY["dangerous_methods"]:
        r.expect_direct("G2a %s direct is dropped" % m, m, {}, False)
    # G2b
    rel_q = {"fromId": r.asset["id"]["id"], "fromType": "ASSET", "relationType": SHOP["relation_type"],
             "toId": r.did, "toType": "DEVICE"}
    api.request("DELETE", tb_setup.P_RELATION, params=rel_q)
    try:
        r.expect_direct("G2b no shop asset: protection.set direct is dropped", "protection.set", {}, False)
    finally:
        tb_setup.ensure_shop_asset(api, r.device, {"id": r.asset["assetProfileId"]})
    # G2c / G2d
    for mode in POLICY["dangerous_cooling_modes"]:
        r.expect_direct("G2c cooling.mode.set %s direct is dropped" % mode, "cooling.mode.set", {"mode": mode}, False)
    r.expect_direct("G2d cooling.mode.set auto direct reaches the board", "cooling.mode.set", {"mode": "auto"}, True)
    # G3
    rid = new_id("g3")
    since = r.board.mark()
    res = r.apply({"request_id": rid, "method": "protection.set", "params": {"pump": {"enabled": True}}})
    ok = bool(res) and res.get("ok") is True and r.board.reached("protection.set", since)
    r.report("PASS" if ok else "FAIL", "G3 shop path protection.set reaches the board", json.dumps(res))
    # G4
    r.board.stop()
    try:
        rid = new_id("g4")
        res = r.apply({"request_id": rid, "method": "protection.get", "params": {}})
        ok = bool(res) and res.get("ok") is False and res.get("error") in ("board_offline", "no_reply")
        r.report("PASS" if ok else "FAIL", "G4 board offline: apply_result says so", json.dumps(res))
    finally:
        r.board.start()
    # G5
    res = r.apply({"request_id": "g5-" + secrets.token_hex(4), "params": {}})
    ok = bool(res) and res.get("error") == "bad_request" and res.get("ok") is False
    r.report("PASS" if ok else "FAIL", "G5 apply_request without method gives bad_request", json.dumps(res))
    # G6
    tb_setup.set_bench_direct(api, DEVICE_NAME, True)
    try:
        r.expect_direct("G6 bench_allow_direct: protection.set direct reaches", "protection.set", {}, True)
    finally:
        tb_setup.set_bench_direct(api, DEVICE_NAME, False)
    # G7
    if not args.customer_user:
        r.report("SKIP", "G7 customer checks", "needs --customer-user")
        return
    customer = api.find_by_name(tb_setup.P_CUSTOMERS, customer_title, key="title")
    if customer is None:
        r.report("FAIL", "G7 customer checks", "customer %r not found - run tb_setup.py" % customer_title)
        return
    cid = customer["id"]["id"]
    api.post("/api/customer/%s/device/%s" % (cid, r.did))
    try:
        pw = os.environ.get("TB_CUSTOMER_PASSWORD") or getpass.getpass(
            "ThingsBoard password for customer %s: " % args.customer_user)
        capi = TbApi(args.url, args.customer_user, pw)
        pw = None
        capi.login()
        aid = r.asset["id"]["id"]
        try:
            capi.get("/api/asset/%s" % aid)
            r.report("FAIL", "G7 customer cannot read the shop asset", "GET succeeded")
        except TbError as e:
            ok = "HTTP 403" in str(e) or "HTTP 404" in str(e)
            r.report("PASS" if ok else "FAIL", "G7 customer cannot read the shop asset", str(e)[:80])
        try:
            capi.post(tb_setup.P_SERVER_ATTRS % ("ASSET", aid), {SHOP["bench_key"]: True})
            wrote = True
        except TbError:
            wrote = False
        bench = tb_setup._server_attrs(api, r.asset["id"], [SHOP["bench_key"]]).get(SHOP["bench_key"])
        ok = not wrote and str(bench).lower() != "true"
        r.report("PASS" if ok else "FAIL", "G7 customer cannot set bench_allow_direct",
                 "write %s, value now %s" % ("accepted" if wrote else "refused", bench))
        r.expect_direct("G7 customer protection.set direct is dropped", "protection.set", {}, False, api=capi)
        r.expect_direct("G7 customer pump.start direct reaches the board", "pump.start", {}, True, api=capi)
    finally:
        tb_setup.set_bench_direct(api, DEVICE_NAME, False)
        api.delete("/api/customer/device/%s" % r.did)


def get_or_replace_device(api, profile):
    old = api.get("/api/tenant/devices", {"deviceName": DEVICE_NAME}, ok404=True)
    if old:
        api.delete("/api/device/%s" % old["id"]["id"])
    return api.post("/api/device", {"name": DEVICE_NAME, "type": tb_setup.PROFILE_NAME,
                                    "deviceProfileId": profile["id"]})


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--mqtt-port", type=int, default=1883, help="MQTT port (default %(default)s)")
    ap.add_argument("--customer-user", help="e-mail of a user of the test customer (enables G7)")
    ap.add_argument("--keep", action="store_true", help="keep the test device and asset")
    ap.add_argument("--selftest", action="store_true", help="offline: check the helpers")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    api = connect_from_args(args, ap)
    host = urllib.parse.urlparse(args.url).hostname
    device = asset = board = r = None
    try:
        profile = api.find_by_name(tb_setup.P_PROFILES, tb_setup.PROFILE_NAME)
        aprofile = api.find_by_name(tb_setup.P_ASSET_PROFILES, SHOP["asset_profile"])
        if profile is None or aprofile is None:
            print("ERROR: run tb_setup.py first (profile %r or asset profile %r missing)"
                  % (tb_setup.PROFILE_NAME, SHOP["asset_profile"]), file=sys.stderr)
            return 1
        device = get_or_replace_device(api, profile)
        asset = tb_setup.ensure_shop_asset(api, device, aprofile)
        tb_setup.set_bench_direct(api, DEVICE_NAME, False)
        cred = api.get("/api/device/%s/credentials" % device["id"]["id"])
        if cred.get("credentialsType") != "ACCESS_TOKEN" or not cred.get("credentialsId"):
            raise TbError("device credentials are not an ACCESS_TOKEN")
        board = FakeBoard(host, args.mqtt_port, cred["credentialsId"])
        board.start()
        print()
        r = Runner(api, device, asset, board)
        run_cases(r, args, tb_setup.CUSTOMER_TITLE)
    except (TbError, MqttError, OSError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        if r is not None:
            r.counts["FAIL"] += 1
        else:
            return 1
    finally:
        if board is not None:
            board.stop()
            if board.error:
                print("fake board error: %s" % board.error, file=sys.stderr)
    print("summary: %(PASS)d PASS, %(FAIL)d FAIL, %(SKIP)d SKIP" % r.counts)
    if r.counts["FAIL"]:
        print("test device %s and asset %s KEPT for inspection (the next run replaces them)"
              % (DEVICE_NAME, tb_setup.shop_asset_name(DEVICE_NAME)))
    elif not args.keep:
        for path in ("/api/device/%s" % device["id"]["id"], "/api/asset/%s" % asset["id"]["id"]):
            try:
                api.delete(path)
            except TbError as e:
                print("warning: could not delete %s: %s" % (path, e), file=sys.stderr)
    return 1 if r.counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
