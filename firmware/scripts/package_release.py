#!/usr/bin/env python3
"""Turn an ESP-IDF build into the release files.

    python package_release.py --build-dir firmware/build --version 0.4.1 \\
        --out-dir release --key-env OTA_ENCRYPTION_KEY

Writes into --out-dir:
    fishpump-<ver>.bin                  plain app image (ThingsBoard OTA)
    fishpump-<ver>-<keyfp>.ota.bin      encrypted app image (web OTA)
    fishpump-<ver>-factory.bin          whole flash image, written at address 0x0
    flash-package.zip                   customer install kit (FLASH.bat, tools, READMEs, factory.bin)
    SHA256SUMS

The key is read from the environment, never printed. Offsets, file names and
flash settings come from <build-dir>/flasher_args.json, nothing is hard-coded.
"""

import argparse
import hashlib
import io
import json
import sys
import urllib.request
import zipfile
from pathlib import Path

import ota_crypto

SCRIPTS = Path(__file__).resolve().parent
CUSTOMER_DIR = SCRIPTS / "customer"

# Pinned esptool for the customer kit. The zip is downloaded at packaging time;
# the extracted exe must match this SHA-256 or the package is refused.
ESPTOOL_URL = "https://github.com/espressif/esptool/releases/download/v5.3.1/esptool-v5.3.1-windows-amd64.zip"
ESPTOOL_ZIP_MEMBER = "esptool-windows-amd64/esptool.exe"
ESPTOOL_EXE_SHA256 = "a939e28fe2fbb53e65e46f0246a4d71b9fd6c052ef0e046182df4a4fe8bec946"


def _flasher_args(build_dir: Path) -> dict:
    path = build_dir / "flasher_args.json"
    if not path.exists():
        raise ValueError(f"{path} not found: run the build first")
    return json.loads(path.read_text(encoding="utf-8"))


def flash_images(build_dir: Path) -> "list[tuple[int, Path]]":
    """(offset, file) of every image in flasher_args.json, sorted by offset."""
    images = []
    for offset, rel in _flasher_args(build_dir)["flash_files"].items():
        path = build_dir / rel
        if not path.exists():
            raise ValueError(f"missing build output: {path}")
        images.append((int(offset, 16), path))
    return sorted(images)


def app_image(build_dir: Path) -> Path:
    path = build_dir / _flasher_args(build_dir)["app"]["file"]
    if not path.exists():
        raise ValueError(f"missing build output: {path}")
    return path


def merge_factory(build_dir: Path) -> bytes:
    """One flash image starting at address 0x0, gaps filled with 0xFF."""
    out = bytearray()
    end_of_previous = 0
    for offset, path in flash_images(build_dir):
        data = path.read_bytes()
        if offset < end_of_previous:
            raise ValueError(f"images overlap at 0x{offset:x} ({path.name})")
        out.extend(b"\xff" * (offset - len(out)))
        out.extend(data)
        end_of_previous = offset + len(data)
    return bytes(out)


def get_esptool_exe(local: "str | None") -> bytes:
    """Return the pinned esptool.exe bytes, from a local copy or by download. Verified either way."""
    if local:
        data = Path(local).read_bytes()
    else:
        with urllib.request.urlopen(ESPTOOL_URL, timeout=120) as resp:
            blob = resp.read()
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            data = z.read(ESPTOOL_ZIP_MEMBER)
    if hashlib.sha256(data).hexdigest() != ESPTOOL_EXE_SHA256:
        raise ValueError("esptool.exe does not match the pinned SHA-256; refusing to package it")
    return data


def build_kit(zip_path: Path, factory: bytes, version: str, fingerprint: str, esptool_exe: "bytes | None") -> None:
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for src in sorted(CUSTOMER_DIR.rglob("*")):
            if src.is_file():
                z.write(src, src.relative_to(CUSTOMER_DIR).as_posix())
        z.writestr("firmware/factory.bin", factory)
        z.writestr("firmware/VERSION.txt", f"fishpump {version}\nOTA key fingerprint {fingerprint}\n")
        if esptool_exe is not None:
            z.writestr("tools/esptool.exe", esptool_exe)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--key-env", required=True, metavar="NAME")
    ap.add_argument("--esptool-exe", metavar="PATH", help="use this esptool.exe instead of downloading (still hash-checked)")
    ap.add_argument("--no-esptool-exe", action="store_true", help="leave esptool.exe out of the kit (tests only)")
    args = ap.parse_args(argv)

    build_dir, out_dir, version = Path(args.build_dir), Path(args.out_dir), args.version
    try:
        key = ota_crypto.read_key(args.key_env, None)
        fp = ota_crypto.key_fingerprint(key)
        app = app_image(build_dir).read_bytes()
        factory = merge_factory(build_dir)
        encrypted = ota_crypto.encrypt_bytes(app, key)
        esptool_exe = None if args.no_esptool_exe else get_esptool_exe(args.esptool_exe)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"fishpump-{version}.bin").write_bytes(app)
    (out_dir / f"fishpump-{version}-{fp}.ota.bin").write_bytes(encrypted)
    (out_dir / f"fishpump-{version}-factory.bin").write_bytes(factory)
    build_kit(out_dir / "flash-package.zip", factory, version, fp, esptool_exe)

    sums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}" for p in sorted(out_dir.iterdir()) if p.name != "SHA256SUMS"]
    (out_dir / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8", newline="\n")

    print(f"packaged fishpump {version} into {out_dir}; OTA key fingerprint {fp}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
