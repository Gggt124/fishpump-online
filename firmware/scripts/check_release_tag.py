#!/usr/bin/env python3
"""Check that a release tag matches firmware/version.txt before anything is built.

    python check_release_tag.py v0.4.1                -> prints "0.4.1 release"
    python check_release_tag.py v0.4.1-rc1            -> prints "0.4.1-rc1 prerelease"

Exit 1 with a message when the tag is malformed or its number differs from
firmware/version.txt (project() needs the plain number, so a suffix such as
-rc1 only marks a dry-run release).
"""

import argparse
import re
import sys
from pathlib import Path

DEFAULT_VERSION_FILE = Path(__file__).resolve().parent.parent / "version.txt"
TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)(-[0-9A-Za-z.]+)?$")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag")
    ap.add_argument("--version-file", default=str(DEFAULT_VERSION_FILE))
    args = ap.parse_args(argv)

    m = TAG_RE.match(args.tag)
    if not m:
        print(f"error: tag {args.tag!r} must look like v1.2.3 or v1.2.3-rc1", file=sys.stderr)
        return 1
    try:
        version = Path(args.version_file).read_text(encoding="utf-8").strip().splitlines()[0].strip()
    except (OSError, IndexError) as e:
        print(f"error: cannot read {args.version_file}: {e}", file=sys.stderr)
        return 1
    if m.group(1) != version:
        print(f"error: tag {args.tag} does not match firmware/version.txt ({version}); change one of them", file=sys.stderr)
        return 1
    suffix = m.group(2) or ""
    print(f"{version}{suffix} {'prerelease' if suffix else 'release'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
