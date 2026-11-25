"""Runs the offline selftest of rpc_guard_test.py under unittest (CI)."""
import unittest

import rpc_guard_test


class RpcGuardSelftest(unittest.TestCase):
    def test_selftest(self):
        self.assertEqual(rpc_guard_test.selftest(), 0)


if __name__ == "__main__":
    unittest.main()
