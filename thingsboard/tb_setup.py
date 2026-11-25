"""Idempotent ThingsBoard setup for fishpump: rule chains, device and asset profiles, customer, shop assets.

    python thingsboard/tb_setup.py --user tenant@example.com
    python thingsboard/tb_setup.py --user tenant@example.com --assign fishpump-a1b2c3
    python thingsboard/tb_setup.py --user tenant@example.com --allow-direct fishpump-1de564   # bench board
    python thingsboard/tb_setup.py --user tenant@example.com --block-direct fishpump-1de564

Every fishpump board gets a tenant-owned shop asset `shop-<device>` (relation fishpump_board to the board) and
the server attribute inactivityTimeout. --allow-direct lets dangerous RPCs reach that board straight over REST
(parity_test.py, the test dashboard); without it they are dropped and only the shop path (apply_request) works.
"""
import argparse
import json
import secrets
import string
import sys
from pathlib import Path

import rpc_policy
import tb_dashboard
from tb_api import NULL_UUID, TbError, add_common_args, connect_from_args, utf8_stdout

HERE = Path(__file__).resolve().parent
RULE_CHAIN_FILE = HERE / "rule_chains" / "fishpump.json"
SHOP_RULE_CHAIN_FILE = HERE / "rule_chains" / "fishpump_shop.json"
PROVISION_FILE = HERE / ".local" / "provision.json"
PROFILE_NAME = "fishpump"
QUEUE_NAME = "SequentialByOriginator"  # system queue, strategy SEQUENTIAL_BY_ORIGINATOR
RULE_CHAIN_NAME = "fishpump"
CUSTOMER_TITLE = "ลูกค้าทดสอบ"
POLICY = rpc_policy.load()
SHOP = POLICY["shop"]

# REST paths kept in one place so a live-run fix is a one-line change.
P_RULE_CHAINS = "/api/ruleChains"
P_RULE_CHAIN = "/api/ruleChain"
P_RULE_CHAIN_META = "/api/ruleChain/metadata"
P_PROFILES = "/api/deviceProfiles"
P_PROFILE = "/api/deviceProfile"
P_CUSTOMERS = "/api/customers"
P_CUSTOMER = "/api/customer"
P_ASSET_PROFILES = "/api/assetProfiles"
P_ASSET_PROFILE = "/api/assetProfile"
P_TENANT_ASSETS = "/api/tenant/assets"          # ?assetName= -> one asset or 404
P_ASSET = "/api/asset"
P_UNASSIGN_ASSET = "/api/customer/asset/%s"     # DELETE: back to the tenant
P_RELATION = "/api/relation"                    # GET ?fromId&fromType&relationType&toId&toType (404 if none), POST, DELETE
P_RELATIONS = "/api/relations"                  # GET ?toId&toType&relationType -> list
P_DEVICE_INFOS = "/api/tenant/deviceInfos"      # ?deviceProfileId= paged
P_SERVER_ATTRS = "/api/plugins/telemetry/%s/%s/attributes/SERVER_SCOPE"
P_SERVER_ATTR_VALUES = "/api/plugins/telemetry/%s/%s/values/attributes/SERVER_SCOPE"


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
    shop = json.loads(SHOP_RULE_CHAIN_FILE.read_text(encoding="utf-8"))
    return check_text_fields(("ruleChain", doc["ruleChain"]), ("metadata", doc["metadata"]),
                             ("shop.ruleChain", shop["ruleChain"]), ("shop.metadata", shop["metadata"]),
                             ("profile", profile_body(PROFILE_NAME, "k", "s", None)),
                             ("assetProfile", asset_profile_body(None)))


def rand_token(n=20):
    alphabet = string.ascii_letters + string.digits
    return "".join(secrets.choice(alphabet) for _ in range(n))


def ensure_rule_chain(api, name, path):
    doc = json.loads(path.read_text(encoding="utf-8"))
    chain, meta = doc["ruleChain"], doc["metadata"]
    existing = api.find_by_name(P_RULE_CHAINS, name)
    version = None
    if existing is None:
        created = api.post(P_RULE_CHAIN, {
            "name": name,
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
    print("rule chain  %-13s %s (id %s)" % (name, state, rc_id["id"]))
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


def asset_profile_body(rc_id):
    return {
        "name": SHOP["asset_profile"],
        "description": "fishpump shop asset, one per board; tenant only, never assigned to a customer",
        "defaultRuleChainId": rc_id,
        "defaultQueueName": QUEUE_NAME,
    }


def ensure_asset_profile(api, rc_id):
    body = asset_profile_body(rc_id)
    existing = api.find_by_name(P_ASSET_PROFILES, body["name"])
    if existing is None:
        saved, state = api.post(P_ASSET_PROFILE, body), "created"
    else:
        merged = dict(existing)
        merged.update({k: body[k] for k in ("defaultRuleChainId", "defaultQueueName")})
        saved, state = api.post(P_ASSET_PROFILE, merged), "exists, updated"
    print("asset prof. %-13s %s (id %s)" % (body["name"], state, saved["id"]["id"]))
    return saved


def shop_asset_name(device_name):
    return SHOP["asset_name_prefix"] + device_name


def _server_attrs(api, entity_id, keys):
    rows = api.get(P_SERVER_ATTR_VALUES % (entity_id["entityType"], entity_id["id"]), {"keys": ",".join(keys)})
    return {r["key"]: r.get("value") for r in (rows or []) if "key" in r}


def _write_server_attrs(api, entity_id, values):
    api.post(P_SERVER_ATTRS % (entity_id["entityType"], entity_id["id"]), values)


def ensure_shop_asset(api, device, profile):
    """Shop asset of one board: tenant-owned, related to the board, bench_allow_direct present.
    Also sets the board's inactivityTimeout. Returns the asset."""
    name = shop_asset_name(device["name"])
    notes = []
    asset = api.get(P_TENANT_ASSETS, {"assetName": name}, ok404=True)
    if not asset:
        asset = api.post(P_ASSET, {"name": name, "label": device["name"], "assetProfileId": profile["id"]})
        notes.append("created")
    cust = (asset.get("customerId") or {}).get("id")
    if cust not in (None, NULL_UUID):
        print("WARN        %s was assigned to customer %s; taking it back to the tenant" % (name, cust))
        api.delete(P_UNASSIGN_ASSET % asset["id"]["id"])
        notes.append("unassigned")
    rel = api.get(P_RELATION, {"fromId": asset["id"]["id"], "fromType": "ASSET", "relationType": SHOP["relation_type"],
                               "toId": device["id"]["id"], "toType": "DEVICE"}, ok404=True)
    if not rel:
        api.post(P_RELATION, {"from": asset["id"], "to": device["id"], "type": SHOP["relation_type"],
                              "typeGroup": "COMMON"})
        notes.append("related")
    # Only the shop asset may point at the board: the drop filter reads bench_allow_direct from whatever is
    # related, and a customer who owns some asset could otherwise relate it to their own board.
    for other in api.get(P_RELATIONS, {"toId": device["id"]["id"], "toType": "DEVICE",
                                       "relationType": SHOP["relation_type"]}) or []:
        src = other.get("from") or {}
        if src.get("id") == asset["id"]["id"]:
            continue
        print("WARN        %s: removing foreign %s relation from %s %s" % (
            device["name"], SHOP["relation_type"], src.get("entityType"), src.get("id")))
        api.request("DELETE", P_RELATION, params={
            "fromId": src.get("id"), "fromType": src.get("entityType"), "relationType": SHOP["relation_type"],
            "toId": device["id"]["id"], "toType": "DEVICE"})
        notes.append("foreign relation removed")
    bench = _server_attrs(api, asset["id"], [SHOP["bench_key"]])
    if SHOP["bench_key"] not in bench:
        _write_server_attrs(api, asset["id"], {SHOP["bench_key"]: False})
        bench[SHOP["bench_key"]] = False
    _write_server_attrs(api, device["id"], {"inactivityTimeout": POLICY["inactivity_timeout_ms"]})
    print("shop asset  %s %s (direct dangerous RPC %s, inactivityTimeout %d ms)" % (
        name, ", ".join(notes) or "ok", "ALLOWED" if str(bench[SHOP["bench_key"]]).lower() == "true" else "dropped",
        POLICY["inactivity_timeout_ms"]))
    return asset


def ensure_all_shop_assets(api, profile_id, asset_profile):
    """ensure_shop_asset for every device of the fishpump device profile."""
    out, page = [], 0
    while True:
        res = api.get(P_DEVICE_INFOS, {"pageSize": 100, "page": page, "deviceProfileId": profile_id["id"]})
        for dev in res.get("data", []):
            out.append(ensure_shop_asset(api, dev, asset_profile))
        if not res.get("hasNext"):
            break
        page += 1
    if not out:
        print("shop asset  no fishpump board yet (run again after a board provisions)")
    return out


def set_bench_direct(api, device_name, allow):
    name = shop_asset_name(device_name)
    asset = api.get(P_TENANT_ASSETS, {"assetName": name}, ok404=True)
    if not asset:
        raise TbError("shop asset %r not found - run tb_setup.py after %s has provisioned" % (name, device_name))
    _write_server_attrs(api, asset["id"], {SHOP["bench_key"]: bool(allow)})
    print("shop asset  %s %s = %s (%s)" % (name, SHOP["bench_key"], "true" if allow else "false",
                                           "direct dangerous RPC reaches the board" if allow else
                                           "direct dangerous RPC dropped"))


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--check", action="store_true", help="offline: validate text fields and the dashboard files, no login")
    ap.add_argument("--device", default=tb_dashboard.DEFAULT_DEVICE,
                    help="device name the test dashboard targets (default %(default)s)")
    ap.add_argument("--assign", metavar="DEVICE_NAME", help="assign this device to the test customer (after provisioning)")
    bench = ap.add_mutually_exclusive_group()
    bench.add_argument("--allow-direct", metavar="DEVICE_NAME",
                       help="bench board: let dangerous RPCs reach it straight over REST (sets bench_allow_direct)")
    bench.add_argument("--block-direct", metavar="DEVICE_NAME",
                       help="drop dangerous direct RPCs to this board again (the default)")
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
        rc_id = ensure_rule_chain(api, RULE_CHAIN_NAME, RULE_CHAIN_FILE)
        shop_rc_id = ensure_rule_chain(api, SHOP["rule_chain"], SHOP_RULE_CHAIN_FILE)
        profile = ensure_profile(api, rc_id)
        asset_profile = ensure_asset_profile(api, shop_rc_id)
        customer = ensure_customer(api)
        tb_dashboard.ensure_all(api, customer, args.device)
        ensure_all_shop_assets(api, profile["id"], asset_profile)
        if args.assign:
            assign(api, customer, args.assign)
        if args.allow_direct:
            set_bench_direct(api, args.allow_direct, True)
        if args.block_direct:
            set_bench_direct(api, args.block_direct, False)
    except TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
