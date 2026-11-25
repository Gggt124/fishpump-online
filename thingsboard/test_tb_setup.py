"""Offline tests for the shop-asset steps of tb_setup.py, against a fake ThingsBoard REST API (run by CI)."""
import json
import tempfile
import unittest
from pathlib import Path

import tb_setup
from tb_api import NULL_UUID, TbError

DEV_ID = {"entityType": "DEVICE", "id": "dev-1"}
PROFILE = {"id": {"entityType": "ASSET_PROFILE", "id": "ap-1"}, "name": "fishpump_shop"}


class FakeApi:
    """Just enough of TbApi for ensure_shop_asset / set_bench_direct. Records every post and delete."""

    def __init__(self):
        self.assets = {}       # name -> asset dict
        self.relations = set() # (from_id, to_id, type)
        self.attrs = {}        # (entity_type, id) -> {key: value}
        self.calls = []
        self._n = 0

    def get(self, path, params=None, ok404=False, **kw):
        params = params or {}
        if path == tb_setup.P_TENANT_ASSETS:
            a = self.assets.get(params["assetName"])
            if a is None and ok404:
                return None
            return a
        if path == tb_setup.P_RELATION:
            key = (params["fromId"], params["toId"], params["relationType"])
            if key in self.relations:
                return {"type": params["relationType"]}
            return None if ok404 else {}
        if path == getattr(tb_setup, "P_RELATIONS", None):
            return [{"from": {"entityType": "ASSET", "id": f}, "to": {"entityType": "DEVICE", "id": t}, "type": typ}
                    for f, t, typ in sorted(self.relations)
                    if t == params["toId"] and typ == params["relationType"]]
        if path.startswith("/api/plugins/telemetry/"):
            _, _, _, _, etype, eid = path.split("/")[:6]
            vals = self.attrs.get((etype, eid), {})
            keys = params.get("keys", "").split(",")
            return [{"key": k, "value": vals[k]} for k in keys if k in vals]
        raise AssertionError("unexpected GET %s" % path)

    def post(self, path, body=None, params=None, **kw):
        self.calls.append(("POST", path, body))
        if path == tb_setup.P_ASSET:
            self._n += 1
            a = dict(body)
            a["id"] = {"entityType": "ASSET", "id": "asset-%d" % self._n}
            a["customerId"] = {"entityType": "CUSTOMER", "id": NULL_UUID}
            self.assets[a["name"]] = a
            return a
        if path == tb_setup.P_RELATION:
            self.relations.add((body["from"]["id"], body["to"]["id"], body["type"]))
            return None
        if path.startswith("/api/plugins/telemetry/"):
            parts = path.split("/")
            self.attrs.setdefault((parts[4], parts[5]), {}).update(body)
            return None
        raise AssertionError("unexpected POST %s" % path)

    def request(self, method, path, body=None, params=None, **kw):
        if method == "DELETE" and path == tb_setup.P_RELATION:
            self.calls.append(("DELETE", path, dict(params)))
            self.relations.discard((params["fromId"], params["toId"], params["relationType"]))
            return None
        raise AssertionError("unexpected %s %s" % (method, path))

    def delete(self, path, **kw):
        self.calls.append(("DELETE", path, None))
        aid = path.rsplit("/", 1)[1]
        for a in self.assets.values():
            if a["id"]["id"] == aid:
                a["customerId"] = {"entityType": "CUSTOMER", "id": NULL_UUID}
        return None

    def posts_to(self, path):
        return [c for c in self.calls if c[0] == "POST" and c[1] == path]


def device(name="fishpump-x"):
    return {"id": dict(DEV_ID), "name": name}


class ShopAssetTest(unittest.TestCase):
    def test_shop_asset_name(self):
        self.assertEqual(tb_setup.shop_asset_name("fishpump-1de564"), "shop-fishpump-1de564")

    def test_shop_asset_created_once(self):
        api = FakeApi()
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        self.assertEqual(len(api.posts_to(tb_setup.P_ASSET)), 1)
        self.assertEqual(len(api.posts_to(tb_setup.P_RELATION)), 1)
        asset = api.assets["shop-fishpump-x"]
        self.assertEqual(asset["assetProfileId"], PROFILE["id"])
        rel = api.posts_to(tb_setup.P_RELATION)[0][2]
        self.assertEqual((rel["from"], rel["to"], rel["type"]), (asset["id"], DEV_ID, "fishpump_board"))
        self.assertEqual(api.attrs[("ASSET", asset["id"]["id"])], {"bench_allow_direct": False})

    def test_bench_flag_not_overwritten(self):
        api = FakeApi()
        a = tb_setup.ensure_shop_asset(api, device(), PROFILE)
        api.attrs[("ASSET", a["id"]["id"])]["bench_allow_direct"] = True
        api.calls.clear()
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        asset_attr_posts = [c for c in api.calls if c[1].startswith("/api/plugins/telemetry/ASSET/")]
        self.assertEqual(asset_attr_posts, [])
        self.assertIs(api.attrs[("ASSET", a["id"]["id"])]["bench_allow_direct"], True)

    def test_inactivity_timeout_set(self):
        api = FakeApi()
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        posts = api.posts_to("/api/plugins/telemetry/DEVICE/dev-1/attributes/SERVER_SCOPE")
        self.assertEqual([p[2] for p in posts], [{"inactivityTimeout": 180000}])

    def test_customer_asset_pulled_back(self):
        api = FakeApi()
        a = tb_setup.ensure_shop_asset(api, device(), PROFILE)
        a["customerId"] = {"entityType": "CUSTOMER", "id": "cust-9"}
        api.calls.clear()
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        self.assertIn(("DELETE", "/api/customer/asset/%s" % a["id"]["id"], None), api.calls)
        self.assertEqual(a["customerId"]["id"], NULL_UUID)

    def test_foreign_board_relation_removed(self):
        # an asset that is not shop-<device> (e.g. one a customer owns) must not stay related as fishpump_board:
        # the drop filter would read its bench_allow_direct
        api = FakeApi()
        a = tb_setup.ensure_shop_asset(api, device(), PROFILE)
        api.relations.add(("asset-foreign", "dev-1", "fishpump_board"))
        api.calls.clear()
        tb_setup.ensure_shop_asset(api, device(), PROFILE)
        self.assertNotIn(("asset-foreign", "dev-1", "fishpump_board"), api.relations)
        self.assertIn((a["id"]["id"], "dev-1", "fishpump_board"), api.relations)
        self.assertTrue(any(c[0] == "DELETE" and c[1] == tb_setup.P_RELATION for c in api.calls), api.calls)

    def test_set_bench_direct(self):
        api = FakeApi()
        a = tb_setup.ensure_shop_asset(api, device(), PROFILE)
        tb_setup.set_bench_direct(api, "fishpump-x", True)
        self.assertIs(api.attrs[("ASSET", a["id"]["id"])]["bench_allow_direct"], True)
        tb_setup.set_bench_direct(api, "fishpump-x", False)
        self.assertIs(api.attrs[("ASSET", a["id"]["id"])]["bench_allow_direct"], False)
        with self.assertRaises(TbError):
            tb_setup.set_bench_direct(api, "fishpump-none", True)


class PreflightTest(unittest.TestCase):
    def test_preflight_covers_shop_chain(self):
        doc = json.loads(tb_setup.SHOP_RULE_CHAIN_FILE.read_text(encoding="utf-8"))
        doc["ruleChain"]["additionalInfo"]["description"] = "a <b> c"
        saved = tb_setup.SHOP_RULE_CHAIN_FILE
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "shop.json"
            p.write_text(json.dumps(doc), encoding="utf-8")
            tb_setup.SHOP_RULE_CHAIN_FILE = p
            try:
                problems = tb_setup.preflight()
            finally:
                tb_setup.SHOP_RULE_CHAIN_FILE = saved
        self.assertTrue(any("shop" in pr for pr in problems), problems)
        self.assertEqual(tb_setup.preflight(), [])


if __name__ == "__main__":
    unittest.main()
