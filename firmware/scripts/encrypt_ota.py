#!/usr/bin/env python3
"""Encrypt a firmware image for web OTA upload.

    python encrypt_ota.py IN.bin OUT.bin --key-env OTA_ENCRYPTION_KEY
    python encrypt_ota.py IN.bin OUT.bin --key-file path/to/key.txt

The key is never accepted on the command line (it would show up in process
lists and CI logs) and is never printed; only its fingerprint is.
"""

import argparse
import sys
from pathlib import Path

import ota_crypto


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--key-env", metavar="NAME", help="environment variable holding the 64-hex key")
    group.add_argument("--key-file", metavar="PATH", help="file holding the 64-hex key")
    args = ap.parse_args(argv)

    try:
        key = ota_crypto.read_key(args.key_env, args.key_file)
        blob = ota_crypto.encrypt_bytes(Path(args.input).read_bytes(), key)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    Path(args.output).write_bytes(blob)
    print(f"encrypted {args.input} -> {args.output} ({len(blob)} bytes), key fingerprint {ota_crypto.key_fingerprint(key)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
