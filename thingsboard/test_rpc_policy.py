"""Offline checks for rpc_policy.json (run by CI: python -m unittest discover -s thingsboard)."""
import unittest
from pathlib import Path

import rpc_policy

DEVICE_API = Path(__file__).resolve().parent.parent / "docs" / "firmware" / "device-api.md"


def _api_text():
    return DEVICE_API.read_text(encoding="utf-8")


class RpcPolicyTest(unittest.TestCase):
    def setUp(self):
        self.policy = rpc_policy.load()

    def test_every_dangerous_method_exists_in_device_api(self):
        methods = rpc_policy.device_api_methods(_api_text())
        for m in self.policy["dangerous_methods"] + ["cooling.mode.set"]:
            self.assertIn(m, methods)

    def test_device_api_has_38_methods(self):
        self.assertEqual(len(rpc_policy.device_api_methods(_api_text())), 38)

    def test_is_dangerous(self):
        self.assertTrue(rpc_policy.is_dangerous("protection.set", {}))
        self.assertTrue(rpc_policy.is_dangerous("cooling.mode.set", {"mode": "test_on"}))
        self.assertTrue(rpc_policy.is_dangerous("cooling.mode.set", {"mode": "test"}))
        self.assertFalse(rpc_policy.is_dangerous("cooling.mode.set", {"mode": "auto"}))
        self.assertFalse(rpc_policy.is_dangerous("cooling.mode.set", None))
        self.assertFalse(rpc_policy.is_dangerous("protection.get", {}))
        self.assertFalse(rpc_policy.is_dangerous("pump.start", {}))

    def test_shop_names(self):
        shop = self.policy["shop"]
        self.assertEqual(shop["asset_name_prefix"], "shop-")
        self.assertEqual(shop["bench_key"], "bench_allow_direct")
        self.assertEqual(self.policy["inactivity_timeout_ms"], 180000)


if __name__ == "__main__":
    unittest.main()
