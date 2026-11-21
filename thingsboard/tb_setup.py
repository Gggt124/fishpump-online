"""Idempotent ThingsBoard setup for fishpump: rule chain, device profile, customer.

    python thingsboard/tb_setup.py --user tenant@example.com
    python thingsboard/tb_setup.py --user tenant@example.com --assign fishpump-a1b2c3
"""
import argparse
import json
import secrets
import string
import sys
from pathlib import Path

import tb_dashboard
from tb_api import NULL_UUID, TbError, add_common_args, connect_from_args, utf8_stdout

HERE = Path(__file__).resolve().parent
RULE_CHAIN_FILE = HERE / "rule_chains" / "fishpump.json"
PROVISION_FILE = HERE / ".local" / "provision.json"
PROFILE_NAME = "fishpump"
QUEUE_NAME = "SequentialByOriginator"  # system queue, strategy SEQUENTIAL_BY_ORIGINATOR
RULE_CHAIN_NAME = "fishpump"
CUSTOMER_TITLE = "ลูกค้าทดสอบ"

# REST paths kept in one place so a live-run fix is a one-line change.
P_RULE_CHAINS = "/api/ruleChains"
P_RULE_CHAIN = "/api/ruleChain"
P_RULE_CHAIN_META = "/api/ruleChain/metadata"
P_PROFILES = "/api/deviceProfiles"
P_PROFILE = "/api/deviceProfile"
P_CUSTOMERS = "/api/customers"
P_CUSTOMER = "/api/customer"


UNSAFE_CHARS = "<>&\"'"  # ThingsBoard NoXss validator rejects these in human-text fields


def _walk_text(obj, path, in_text, out):
    if isinstance(obj, dict):
        for k, v in obj.items():
            _walk_text(v, "%s.%s" % (path, k), in_text or k in ("additionalInfo", "description"), out)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            _walk_text(v, "%s[%d]" % (path, i), in_text, out)
    elif isinstance(obj, str) and in_text:
        bad = sorted(set(c for c in obj if c in UNSAFE_CHARS))
        if bad:
            out.append("%s contains %s" % (path, " ".join(bad)))


def check_text_fields(*named_docs):
    """Return problems for every additionalInfo/description string holding < > & quote chars."""
    out = []
    for name, doc in named_docs:
        _walk_text(doc, name, False, out)
    return out


def preflight():
    doc = json.loads(RULE_CHAIN_FILE.read_text(encoding="utf-8"))
    return check_text_fields(("ruleChain", doc["ruleChain"]), ("metadata", doc["metadata"]),
                             ("profile", profile_body(PROFILE_NAME, "k", "s", None)))


def rand_token(n=20):
    alphabet = string.ascii_letters + string.digits
    return "".join(secrets.choice(alphabet) for _ in range(n))


def ensure_rule_chain(api):
    doc = json.loads(RULE_CHAIN_FILE.read_text(encoding="utf-8"))
    chain, meta = doc["ruleChain"], doc["metadata"]
    existing = api.find_by_name(P_RULE_CHAINS, RULE_CHAIN_NAME)
    version = None
    if existing is None:
        created = api.post(P_RULE_CHAIN, {
            "name": RULE_CHAIN_NAME,
            "type": chain.get("type", "CORE"),
            "root": False,
            "debugMode": chain.get("debugMode", False),
            "additionalInfo": chain.get("additionalInfo"),
        })
        rc_id, state = created["id"], "created"
    else:
        rc_id, state = existing["id"], "exists, metadata updated"
        if chain.get("additionalInfo") != existing.get("additionalInfo"):
            existing["additionalInfo"] = chain.get("additionalInfo")
            api.post(P_RULE_CHAIN, existing)
        old = api.get("%s/%s/metadata" % (P_RULE_CHAIN, rc_id["id"]), ok404=True) or {}
        version = old.get("version")
    body = {
        "ruleChainId": rc_id,
        "version": version,
        "firstNodeIndex": meta.get("firstNodeIndex", 0),
        "nodes": meta["nodes"],
        "connections": meta["connections"],
        "ruleChainConnections": meta.get("ruleChainConnections"),
    }
    api.post(P_RULE_CHAIN_META, body)
    print("rule chain  %-10s %s (id %s)" % (RULE_CHAIN_NAME, state, rc_id["id"]))
    return rc_id


def load_local_provision():
    try:
        d = json.loads(PROVISION_FILE.read_text(encoding="utf-8"))
        if d.get("provision_key") and d.get("provision_secret"):
            return d["provision_key"], d["provision_secret"]
    except (OSError, ValueError):
        pass
    return None, None


def profile_body(name, key, secret, rc_id):
    return {
        "name": name,
        "type": "DEFAULT",
        "transportType": "DEFAULT",
        "provisionType": "ALLOW_CREATE_NEW_DEVICES",
        "provisionDeviceKey": key,
        "defaultRuleChainId": rc_id,
        # one message of a device finishes the whole chain before the next starts: the alarm dedupe is a
        # read-modify-write across async nodes and a backfilled raised/cleared pair must not run in parallel
        "defaultQueueName": QUEUE_NAME,
        "description": "fishpump boards (MQTT default topics, device provisioning)",
        "profileData": {
            "configuration": {"type": "DEFAULT"},
            "transportConfiguration": {"type": "DEFAULT"},
            "provisionConfiguration": {"type": "ALLOW_CREATE_NEW_DEVICES", "provisionDeviceSecret": secret},
            "alarms": None,
        },
    }


def ensure_profile(api, rc_id):
    existing = api.find_by_name(P_PROFILES, PROFILE_NAME)
    key = secret = None
    if existing is not None:
        key = existing.get("provisionDeviceKey")
        pc = (existing.get("profileData") or {}).get("provisionConfiguration") or {}
        secret = pc.get("provisionDeviceSecret")
    if not (key and secret):
        key, secret = load_local_provision()
    if not (key and secret):
        key, secret = rand_token(), rand_token()
    body = profile_body(PROFILE_NAME, key, secret, rc_id)
    if existing is None:
        saved = api.post(P_PROFILE, body)
        state = "created"
    else:
        # keep everything an operator set in the UI (alarms, dashboard...); the queue is overridden below, override only ours
        merged = dict(existing)
        for k in ("transportType", "provisionType", "provisionDeviceKey", "defaultRuleChainId",
                  "defaultQueueName"):
            merged[k] = body[k]
        pd = dict(existing.get("profileData") or {})
        pd["provisionConfiguration"] = body["profileData"]["provisionConfiguration"]
        pd.setdefault("configuration", body["profileData"]["configuration"])
        pd.setdefault("transportConfiguration", body["profileData"]["transportConfiguration"])
        merged["profileData"] = pd
        saved = api.post(P_PROFILE, merged)
        state = "exists, updated"
    PROVISION_FILE.parent.mkdir(parents=True, exist_ok=True)
    PROVISION_FILE.write_text(json.dumps({"provision_key": key, "provision_secret": secret}, indent=2),
                              encoding="utf-8")
    print("profile     %-10s %s (id %s)" % (PROFILE_NAME, state, saved["id"]["id"]))
    print("            provision key   : %s" % key)
    print("            provision secret: %s   (also in %s)" % (secret, PROVISION_FILE))
    return saved


def ensure_customer(api):
    existing = api.find_by_name(P_CUSTOMERS, CUSTOMER_TITLE, key="title")
    if existing is None:
        existing = api.post(P_CUSTOMER, {"title": CUSTOMER_TITLE})
        state = "created"
    else:
        state = "exists"
    print("customer    %s %s (id %s)" % (CUSTOMER_TITLE, state, existing["id"]["id"]))
    return existing


def assign(api, customer, device_name):
    dev = api.get("/api/tenant/devices", {"deviceName": device_name}, ok404=True)
    if not dev:
        raise TbError("device %r not found - provision it first (the board must connect once)" % device_name)
    cid = customer["id"]["id"]
    cur = (dev.get("customerId") or {}).get("id")
    if cur == cid:
        print("device      %s already assigned to %s" % (device_name, CUSTOMER_TITLE))
        return
    api.post("/api/customer/%s/device/%s" % (cid, dev["id"]["id"]))
    print("device      %s assigned to %s%s" % (device_name, CUSTOMER_TITLE,
                                                "" if cur in (None, NULL_UUID) else " (was another customer)"))


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--check", action="store_true", help="offline: validate text fields and the dashboard files, no login")
    ap.add_argument("--device", default=tb_dashboard.DEFAULT_DEVICE,
                    help="device name the test dashboard targets (default %(default)s)")
    ap.add_argument("--assign", metavar="DEVICE_NAME", help="assign this device to the test customer (after provisioning)")
    args = ap.parse_args()
    problems = preflight()
    if problems:
        print("ERROR: unsafe characters in ThingsBoard text fields (NoXss validator would reject):", file=sys.stderr)
        for pr in problems:
            print("  " + pr, file=sys.stderr)
        return 1
    if args.check:
        dash_problems = tb_dashboard.run_check()
        if dash_problems:
            print("ERROR: dashboard check failed:", file=sys.stderr)
            for pr in dash_problems:
                print("  " + pr, file=sys.stderr)
            return 1
        print("check OK: no unsafe characters in additionalInfo/description strings; dashboard files consistent")
        return 0
    api = connect_from_args(args, ap)
    try:
        rc_id = ensure_rule_chain(api)
        ensure_profile(api, rc_id)
        customer = ensure_customer(api)
        tb_dashboard.ensure_all(api, customer, args.device)
        if args.assign:
            assign(api, customer, args.assign)
    except TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
