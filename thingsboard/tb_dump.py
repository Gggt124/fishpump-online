"""Save the server's JSON for a widget type or a dashboard (the 4.3.1 format reference for hand-written widgets).

    python thingsboard/tb_dump.py --user tenant@example.com --widget-type control_widgets.rpcbutton
    python thingsboard/tb_dump.py --user tenant@example.com --dashboard Thermostats

Files go to thingsboard/.local/dump/ (git-ignored).
"""
import argparse
import json
import sys
from pathlib import Path

from tb_api import TbError, add_common_args, connect_from_args, utf8_stdout

DUMP_DIR = Path(__file__).resolve().parent / ".local" / "dump"


def save(name, obj):
    DUMP_DIR.mkdir(parents=True, exist_ok=True)
    path = DUMP_DIR / name
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")
    print("saved %s" % path)


def main():
    utf8_stdout()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--widget-type", metavar="FQN", help="widget type fqn, e.g. control_widgets.rpcbutton")
    ap.add_argument("--dashboard", metavar="TITLE", help="dashboard title")
    args = ap.parse_args()
    if not args.widget_type and not args.dashboard:
        ap.error("give --widget-type or --dashboard")
    api = connect_from_args(args, ap)
    try:
        if args.widget_type:
            doc = api.get("/api/widgetType", {"fqn": args.widget_type}, ok404=True)
            if doc is None:
                raise TbError("widget type %r not found" % args.widget_type)
            save("widget_type_%s.json" % args.widget_type.replace("/", "_"), doc)
        if args.dashboard:
            info = api.find_by_name("/api/tenant/dashboards", args.dashboard, key="title")
            if info is None:
                raise TbError("dashboard %r not found" % args.dashboard)
            doc = api.get("/api/dashboard/%s" % info["id"]["id"])
            save("dashboard_%s.json" % info["id"]["id"], doc)
    except TbError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
