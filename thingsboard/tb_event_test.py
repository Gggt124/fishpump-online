"""Rule-chain test: publish alarm events as a test device and check the alarms over REST.

    python thingsboard/tb_event_test.py --user tenant@example.com [--keep]

Creates (fresh) device `fishpump-ruletest` with the `fishpump` profile, reads its
access token, publishes over MQTT with mqtt_mini, prints a PASS/FAIL table and
exits with the number of failures. Run tb_setup.py first.
"""
import argparse
import sys
import time
import urllib.parse

from mqtt_mini import MqttClient, MqttError
from tb_api import TbError, add_common_args, connect_from_args, utf8_stdout

DEVICE_NAME = "fishpump-ruletest"
PROFILE_NAME = "fishpump"
TOPIC_TELEMETRY = "v1/devices/me/telemetry"
POLL_TIMEOUT = 10.0
SETTLE = 3.0  # wait before a "nothing must change" check
ALARM_PATHS = ("/api/alarm/DEVICE/%s", "/api/v2/alarm/DEVICE/%s")  # tried in order


class Ctx:
    def __init__(self, api, host, mqtt_port, token, device_id):
        self.api, self.host, self.mqtt_port, self.token, self.device_id = api, host, mqtt_port, token, device_id
        self._alarm_path = None

    def publish(self, obj):
        import json
        c = MqttClient(self.host, self.mqtt_port, client_id=DEVICE_NAME, username=self.token)
        c.connect()
        c.publish(TOPIC_TELEMETRY, json.dumps(obj))
        time.sleep(0.2)  # let the broker read the PUBLISH before DISCONNECT
        c.disconnect()

    def alarms(self, atype=None):
        params = {"pageSize": 100, "page": 0}
        res = None
        for path in ([self._alarm_path] if self._alarm_path else ALARM_PATHS):
            res = self.api.get(path % self.device_id, params, ok404=True)
            if res is not None:
                self._alarm_path = path
                break
        if res is None:
            raise TbError("no alarm list endpoint answered (tried %s)" % ", ".join(ALARM_PATHS))
        data = res.get("data", []) if isinstance(res, dict) else res
        return [a for a in data if atype is None or a.get("type") == atype]


def is_active(a):
    st = a.get("status")
    return st.startswith("ACTIVE") if st else not a.get("cleared", False)


def is_cleared(a):
    st = a.get("status")
    return st.startswith("CLEARED") if st else bool(a.get("cleared", False))


def wait_for(pred, timeout=POLL_TIMEOUT, every=0.5):
    end = time.time() + timeout
    last = None
    while True:
        last = pred()
        if last[0]:
            return last
        if time.time() >= end:
            return last
        time.sleep(every)


def describe(alarms):
    return "[%s]" % ", ".join("%s/%s/%s" % (a.get("type"), a.get("severity"), a.get("status")) for a in alarms) or "[]"


def ev(boot, seq, typ, rule, sev, name="test rule", value=1.0):
    return {"seq": seq, "boot_id": boot, "type": typ, "rule_id": rule, "name": name,
            "signal": "water_ph", "severity": sev, "value": value, "reason": "condition"}


def run_cases(ctx):
    rows = []
    boot1 = int(time.time()) % 2000000000
    boot2 = boot1 + 1

    def record(name, ok, detail):
        rows.append((name, ok, detail))

    # 1 raised warning -> ACTIVE rule_901 WARNING
    ctx.publish({"alarm_event": ev(boot1, 1, "raised", 901, "warning")})

    def c1():
        al = ctx.alarms("rule_901")
        ok = len(al) == 1 and is_active(al[0]) and al[0].get("severity") == "WARNING"
        return ok, describe(al)
    ok, d = wait_for(c1)
    record("1 raised -> ACTIVE rule_901 WARNING", ok, d)

    # 2 duplicate (same boot, seq) -> still one alarm
    ctx.publish({"alarm_event": ev(boot1, 1, "raised", 901, "warning")})
    time.sleep(SETTLE)
    al = ctx.alarms("rule_901")
    record("2 duplicate (boot,seq) -> still 1 alarm", len(al) == 1 and is_active(al[0]), describe(al))

    # 3 cleared seq 2 -> CLEARED
    ctx.publish({"alarm_event": ev(boot1, 2, "cleared", 901, "warning")})

    def c3():
        al = ctx.alarms("rule_901")
        return len(al) == 1 and is_cleared(al[0]), describe(al)
    ok, d = wait_for(c3)
    record("3 cleared seq+1 -> CLEARED", ok, d)

    # 4 old raised seq 1 re-sent -> not re-opened
    ctx.publish({"alarm_event": ev(boot1, 1, "raised", 901, "warning")})
    time.sleep(SETTLE)
    al = ctx.alarms("rule_901")
    record("4 stale raised after clear -> stays CLEARED", len(al) == 1 and is_cleared(al[0]), describe(al))

    # 5 new boot_id, seq 1 -> new ACTIVE alarm
    ctx.publish({"alarm_event": ev(boot2, 1, "raised", 901, "warning")})

    def c5():
        al = ctx.alarms("rule_901")
        return len(al) == 2 and sum(1 for a in al if is_active(a)) == 1, describe(al)
    ok, d = wait_for(c5)
    record("5 new boot_id seq 1 -> new ACTIVE alarm", ok, d)

    # 6 {"ts","values"} form handled like the plain form
    ctx.publish({"ts": int(time.time() * 1000), "values": {"alarm_event": ev(boot2, 2, "raised", 902, "critical")}})

    def c6():
        al = ctx.alarms("rule_902")
        ok = len(al) == 1 and is_active(al[0]) and al[0].get("severity") == "CRITICAL"
        return ok, describe(al)
    ok, d = wait_for(c6)
    record('6 {"ts","values"} form -> ACTIVE rule_902 CRITICAL', ok, d)

    # 7 plain telemetry saved and readable
    ctx.publish({"water_ph": 7.1})

    def c7():
        path = "/api/plugins/telemetry/DEVICE/%s/values/timeseries" % ctx.device_id
        res = ctx.api.get(path, {"keys": "water_ph"})
        try:
            v = float(res["water_ph"][0]["value"])
        except (KeyError, IndexError, TypeError, ValueError):
            return False, "latest = %r" % (res,)
        return abs(v - 7.1) < 1e-6, "latest water_ph = %s" % v
    ok, d = wait_for(c7)
    record("7 telemetry water_ph=7.1 saved", ok, d)
    return rows


def get_or_replace_device(api, profile):
    old = api.get("/api/tenant/devices", {"deviceName": DEVICE_NAME}, ok404=True)
    if old:
        api.delete("/api/device/%s" % old["id"]["id"])  # fresh alarms/attributes for a deterministic run
    return api.post("/api/device", {"name": DEVICE_NAME, "type": PROFILE_NAME,
                                    "deviceProfileId": profile["id"]})


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--mqtt-port", type=int, default=1883, help="MQTT port (default %(default)s)")
    ap.add_argument("--keep", action="store_true", help="do not delete the test device at the end")
    args = ap.parse_args()
    api = connect_from_args(args, ap)
    host = urllib.parse.urlparse(args.url).hostname
    device = None
    rows = []
    try:
        profile = api.find_by_name("/api/deviceProfiles", PROFILE_NAME)
        if profile is None:
            print("ERROR: device profile %r not found - run tb_setup.py first" % PROFILE_NAME, file=sys.stderr)
            return 1
        device = get_or_replace_device(api, profile)
        did = device["id"]["id"]
        cred = api.get("/api/device/%s/credentials" % did)
        token = cred.get("credentialsId")
        if cred.get("credentialsType") != "ACCESS_TOKEN" or not token:
            raise TbError("device credentials are not an ACCESS_TOKEN: %r" % cred.get("credentialsType"))
        rows = run_cases(Ctx(api, host, args.mqtt_port, token, did))
    except (TbError, MqttError, OSError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    finally:
        if device is not None and not args.keep:
            try:
                api.delete("/api/device/%s" % device["id"]["id"])
            except TbError as e:
                print("warning: could not delete %s: %s" % (DEVICE_NAME, e), file=sys.stderr)
    fails = 0
    print()
    for name, ok, detail in rows:
        print("%-4s %-52s %s" % ("PASS" if ok else "FAIL", name, detail))
        fails += 0 if ok else 1
    print("\n%d/%d PASS" % (len(rows) - fails, len(rows)))
    return min(fails, 255)


if __name__ == "__main__":
    sys.exit(main())
