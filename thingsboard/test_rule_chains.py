"""Offline structure checks for the rule chains in rule_chains/ (run by CI)."""
import json
import re
import unittest
from pathlib import Path

import rpc_policy
import tb_setup

CHAINS = Path(__file__).resolve().parent / "rule_chains"


def chain(name):
    return json.loads((CHAINS / (name + ".json")).read_text(encoding="utf-8"))


def node_index(meta, name):
    hits = [i for i, n in enumerate(meta["nodes"]) if n["name"] == name]
    if len(hits) != 1:
        raise AssertionError("expected one node named %r, found %d" % (name, len(hits)))
    return hits[0]


def of_type(meta, short_type):
    return [i for i, n in enumerate(meta["nodes"]) if n["type"].endswith("." + short_type)]


def paths_to(meta, start, short_type, first_type=None):
    """Every simple path from node `start` to a node of `short_type`, as lists of (from, to, conn_type).
    With first_type, only paths whose first connection has that type."""
    targets = set(of_type(meta, short_type))
    out = []

    def walk(i, path, seen):
        if i in targets and path:
            out.append(path)
            return
        for c in meta["connections"]:
            if c["fromIndex"] != i or c["toIndex"] in seen:
                continue
            if not path and first_type is not None and c["type"] != first_type:
                continue
            step = (c["fromIndex"], c["toIndex"], c["type"])
            walk(c["toIndex"], path + [step], seen | {c["toIndex"]})

    walk(start, [], {start})
    return out


def script(meta, name):
    return meta["nodes"][node_index(meta, name)]["configuration"]["tbelScript"]


class DeviceChainTest(unittest.TestCase):
    def setUp(self):
        self.meta = chain("fishpump")["metadata"]
        self.policy = rpc_policy.load()

    def test_device_rpc_passes_filter(self):
        f = node_index(self.meta, "Direct RPC allowed?")
        paths = paths_to(self.meta, 0, "TbSendRPCRequestNode", first_type="RPC Request to Device")
        self.assertTrue(paths, "no path from the switch to the RPC node")
        for p in paths:
            self.assertIn((f, p[-1][1], "True"), p, p)
        rpc = of_type(self.meta, "TbSendRPCRequestNode")
        incoming = [(c["fromIndex"], c["type"]) for c in self.meta["connections"] if c["toIndex"] in rpc]
        self.assertEqual(incoming, [(f, "True")])

    def test_filter_list_matches_policy(self):
        s = script(self.meta, "Direct RPC allowed?")
        m = re.search(r"var dangerous = \[(.*?)\];", s, re.S)
        self.assertIsNotNone(m, "no `var dangerous = [...]` in the filter")
        self.assertEqual(set(re.findall(r'"([^"]+)"', m.group(1))), set(self.policy["dangerous_methods"]))
        t = re.search(r"var testModes = \[(.*?)\];", s, re.S)
        self.assertIsNotNone(t, "no `var testModes = [...]` in the filter")
        self.assertEqual(set(re.findall(r'"([^"]+)"', t.group(1))), set(self.policy["dangerous_cooling_modes"]))
        self.assertIn(self.policy["shop"]["bench_key"], json.dumps(self.meta["nodes"]))

    def test_filter_reads_params_as_text(self):
        # params is a JSON string on the REST path and a map elsewhere; reading p.mode breaks on a string
        s = script(self.meta, "Direct RPC allowed?")
        self.assertIn('("" + msg.params)', s)
        self.assertNotRegex(s, r"params\.mode|\bp\.mode")

    def test_fetch_failure_goes_to_filter(self):
        fetch = node_index(self.meta, "Fetch bench_allow_direct")
        f = node_index(self.meta, "Direct RPC allowed?")
        types = {c["type"] for c in self.meta["connections"] if c["fromIndex"] == fetch and c["toIndex"] == f}
        self.assertEqual(types, {"Success", "Failure"})

    def test_filter_false_and_failure_do_not_reach_rpc(self):
        f = node_index(self.meta, "Direct RPC allowed?")
        rpc = set(of_type(self.meta, "TbSendRPCRequestNode"))
        for c in self.meta["connections"]:
            if c["fromIndex"] == f and c["type"] != "True":
                self.assertNotIn(c["toIndex"], rpc)

    def test_fetch_uses_policy_relation(self):
        cfg = self.meta["nodes"][node_index(self.meta, "Fetch bench_allow_direct")]["configuration"]
        q = cfg["relationsQuery"]
        self.assertEqual(q["direction"], "TO")
        self.assertEqual([f["relationType"] for f in q["filters"]], [self.policy["shop"]["relation_type"]])
        self.assertEqual(q["filters"][0]["entityTypes"], ["ASSET"])

    def test_preflight_clean(self):
        self.assertEqual(tb_setup.preflight(), [])


class ShopChainTest(unittest.TestCase):
    def setUp(self):
        self.doc = chain("fishpump_shop")
        self.meta = self.doc["metadata"]
        self.policy = rpc_policy.load()
        self.shop = self.policy["shop"]

    def test_shop_chain_name(self):
        self.assertEqual(self.doc["ruleChain"]["name"], self.shop["rule_chain"])

    def test_shop_rpc_only_after_valid(self):
        check = node_index(self.meta, "Check apply_request")
        to_board = node_index(self.meta, "To board")
        paths = paths_to(self.meta, 0, "TbSendRPCRequestNode")
        self.assertTrue(paths)
        for p in paths:
            self.assertIn("valid", [t for f, _, t in p if f == check], p)
            self.assertIn(to_board, [to for _, to, _ in p], p)

    def test_shop_every_outcome_saves_result(self):
        save = node_index(self.meta, "Save apply_result")
        for name in ("Result from reply", "Result from RPC failure", "Result bad_request", "Result no_board",
                     "Result bad_request (unreadable)"):
            self.assertTrue(paths_to(self.meta, node_index(self.meta, name), "TbMsgAttributesNode"), name)
            self.assertEqual(of_type(self.meta, "TbMsgAttributesNode"), [save])
        check = node_index(self.meta, "Check apply_request")
        bad = node_index(self.meta, "Result bad_request")
        out = {(c["toIndex"], c["type"]) for c in self.meta["connections"] if c["fromIndex"] == check}
        self.assertIn((bad, "invalid"), out)
        self.assertIn((bad, "Failure"), out)
        rpc = of_type(self.meta, "TbSendRPCRequestNode")[0]
        rpc_out = {c["type"]: c["toIndex"] for c in self.meta["connections"] if c["fromIndex"] == rpc}
        self.assertEqual(rpc_out, {"Success": node_index(self.meta, "Result from reply"),
                                   "Failure": node_index(self.meta, "Result from RPC failure")})
        to_board = node_index(self.meta, "To board")
        self.assertIn((node_index(self.meta, "Result no_board"), "Failure"),
                      {(c["toIndex"], c["type"]) for c in self.meta["connections"] if c["fromIndex"] == to_board})

    def test_shop_rpc_not_marked_as_rest_call(self):
        # RPC_CALL_FROM_SERVER_TO_DEVICE makes TbSendRPCRequestNode treat it as a REST call and route the reply
        # to a core service (originServiceId), which this message does not have
        self.assertNotIn("RPC_CALL_FROM_SERVER_TO_DEVICE", script(self.meta, "Build RPC"))
        self.assertIn("msgType: msgType", script(self.meta, "Build RPC"))

    def test_shop_save_does_not_loop(self):
        cfg = self.meta["nodes"][node_index(self.meta, "Save apply_result")]["configuration"]
        self.assertEqual(cfg["scope"], "SERVER_SCOPE")
        self.assertIs(cfg["sendAttributesUpdatedNotification"], False)

    def test_shop_relation_names(self):
        for name, direction, etype in (("To board", "FROM", "DEVICE"), ("Back to shop asset", "TO", "ASSET")):
            cfg = self.meta["nodes"][node_index(self.meta, name)]["configuration"]
            self.assertEqual(cfg["originatorSource"], "RELATED", name)
            q = cfg["relationsQuery"]
            self.assertEqual(q["direction"], direction, name)
            self.assertEqual(q["maxLevel"], 1, name)
            self.assertEqual([(f["relationType"], f["entityTypes"]) for f in q["filters"]],
                             [(self.shop["relation_type"], [etype])], name)

    def test_shop_keys_match_policy(self):
        self.assertIn('"%s"' % self.shop["request_key"], script(self.meta, "Check apply_request"))
        for name in ("Result from reply", "Result from RPC failure", "Result bad_request", "Result no_board"):
            self.assertIn('"%s"' % self.shop["result_key"], script(self.meta, name), name)

    def test_shop_error_codes(self):
        fail = script(self.meta, "Result from RPC failure")
        for code in ("TIMEOUT", "NO_ACTIVE_CONNECTION", "no_reply", "board_offline", "rpc_failed"):
            self.assertIn('"%s"' % code, fail)
        self.assertIn('"bad_request"', script(self.meta, "Result bad_request"))
        self.assertIn('"no_board"', script(self.meta, "Result no_board"))

    def test_shop_preflight_clean(self):
        self.assertEqual(tb_setup.check_text_fields(("ruleChain", self.doc["ruleChain"]),
                                                    ("metadata", self.meta)), [])


if __name__ == "__main__":
    unittest.main()
