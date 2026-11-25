"""Which RPC methods a customer may not send straight to a board (spec 6.2), and the shop-path names.

rpc_policy.json is the single source: the rule chains, tb_setup.py, the tests and the frontend app read it.
is_dangerous() is the Python twin of the "Direct RPC allowed?" TBEL filter in rule_chains/fishpump.json.
"""
import json
import re
from pathlib import Path

POLICY_FILE = Path(__file__).resolve().parent / "rpc_policy.json"


def load():
    return json.loads(POLICY_FILE.read_text(encoding="utf-8"))


def is_dangerous(method, params, policy=None):
    policy = policy or load()
    if method in policy["dangerous_methods"]:
        return True
    if method == "cooling.mode.set" and isinstance(params, dict):
        return params.get("mode") in policy["dangerous_cooling_modes"]
    return False


def device_api_methods(text):
    """Method names from the code block under '## 9.' of docs/firmware/device-api.md."""
    m = re.search(r"^## 9\..*?^```\s*\n(.*?)^```", text, re.S | re.M)
    if not m:
        return set()
    return set(m.group(1).split())
