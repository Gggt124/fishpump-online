"""ThingsBoard firmware (OTA) packages for the fishpump device profile (stdlib only).

  python thingsboard/tb_ota.py upload firmware/build/fishpump_firmware.bin --version 0.3.3 --user <email>
  python thingsboard/tb_ota.py assign fishpump-1de564 0.3.3 --user <email>
  python thingsboard/tb_ota.py status fishpump-1de564 --user <email>
  python thingsboard/tb_ota.py unassign fishpump-1de564 --user <email>
  python thingsboard/tb_ota.py delete 0.3.3 --user <email>
  python thingsboard/tb_ota.py --selftest          # offline

Uses the tenant-admin session only. It never uses a device access token: ThingsBoard keeps one MQTT session per
device, so a second connection with the token would kick the real board off.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_api  # noqa: E402

PROFILE = "fishpump"
DEFAULT_TITLE = "fishpump"
STATUS_TELEMETRY = ("fw_state", "fw_error", "current_fw_title", "current_fw_version",
                    "target_fw_title", "target_fw_version", "target_fw_ts")


# esp_app_desc_t sits after the 24-byte image header and the 8-byte first segment header; its magic word
# (0xABCD5432, little-endian) is the first field and the 32-byte NUL-padded version string starts 16 bytes in.
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_OFFSET = 24 + 8
IMAGE_VERSION_OFFSET = APP_DESC_OFFSET + 16
IMAGE_VERSION_LEN = 32


def read_image_version(data):
    """The version string built into an ESP-IDF app image; ValueError when it cannot be read."""
    if len(data) < IMAGE_VERSION_OFFSET + IMAGE_VERSION_LEN:
        raise ValueError("file is too short to be an ESP-IDF app image (%d bytes)" % len(data))
    if int.from_bytes(data[APP_DESC_OFFSET:APP_DESC_OFFSET + 4], "little") != APP_DESC_MAGIC:
        raise ValueError("no app descriptor magic at byte %d: not a plain ESP-IDF app .bin" % APP_DESC_OFFSET)
    raw = data[IMAGE_VERSION_OFFSET:IMAGE_VERSION_OFFSET + IMAGE_VERSION_LEN].split(b"\x00", 1)[0]
    return raw.decode("ascii", "replace")


def versions_equal(a, b):
    """Equal when they differ only by one leading v/V (the same rule as the firmware)."""
    def strip(v):
        return v[1:] if v[:1] in ("v", "V") else v
    return strip(a) == strip(b)


def build_multipart(filename, data, boundary):
    """multipart/form-data body with one file part named "file"."""
    head = ('--%s\r\nContent-Disposition: form-data; name="file"; filename="%s"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n" % (boundary, filename)).encode("utf-8")
    return head + data + ("\r\n--%s--\r\n" % boundary).encode("utf-8")


def list_packages(api):
    page = 0
    while True:
        res = api.get("/api/otaPackages", {"pageSize": 100, "page": page})
        for item in res.get("data", []):
            yield item
        if not res.get("hasNext"):
            return
        page += 1


def find_package(api, title, version):
    for p in list_packages(api):
        if p.get("title") == title and p.get("version") == version:
            return p
    return None


def need_package(api, version):
    pkg = find_package(api, DEFAULT_TITLE, version)
    if not pkg:
        raise tb_api.TbError("no package %s %s" % (DEFAULT_TITLE, version))
    return pkg


def need_device(api, name):
    dev = api.get("/api/tenant/devices", {"deviceName": name}, ok404=True)
    if not dev:
        raise tb_api.TbError("no device %s" % name)
    return dev


def cmd_upload(api, args):
    with open(args.image, "rb") as f:
        data = f.read()
    if not args.no_version_check:
        try:
            built_in = read_image_version(data)
        except ValueError as e:
            raise tb_api.TbError("%s (use --no-version-check to upload anyway)" % e) from None
        if not versions_equal(built_in, args.version):
            raise tb_api.TbError("the image's built-in version is %r but --version is %r; the board would refuse it "
                                 "(version_mismatch). Fix --version or use --no-version-check" % (built_in, args.version))
    profile = api.find_by_name("/api/deviceProfiles", PROFILE)
    if not profile:
        raise tb_api.TbError("device profile %s not found (run tb_setup.py)" % PROFILE)
    info = api.post("/api/otaPackage", {"title": args.title, "version": args.version, "type": "FIRMWARE",
                                         "deviceProfileId": profile["id"]})
    pid = info["id"]["id"]
    boundary = uuid.uuid4().hex
    url = "%s/api/otaPackage/%s?checksumAlgorithm=SHA256" % (api.base, pid)
    if args.checksum:
        url += "&checksum=" + args.checksum
    req = urllib.request.Request(url, data=build_multipart(os.path.basename(args.image), data, boundary),
                                 method="POST", headers={
        "X-Authorization": "Bearer " + api.token, "Accept": "application/json",
        "Content-Type": "multipart/form-data; boundary=" + boundary})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            info = json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise tb_api.TbError("upload -> HTTP %d: %s (the empty package %s was created; delete it with 'delete %s')" % (
            e.code, e.read().decode("utf-8", "replace")[:300], args.version, args.version)) from None
    except (urllib.error.URLError, OSError) as e:
        raise tb_api.TbError("upload failed: %s" % e) from None
    print("package id %s" % pid)
    print("size %s (local file %d bytes)" % (info.get("dataSize"), len(data)))
    print("checksum %s %s" % (info.get("checksumAlgorithm"), info.get("checksum")))
    print("local sha256 %s" % hashlib.sha256(data).hexdigest())


def set_firmware(api, device, pkg):
    dev = need_device(api, device)
    full = api.get("/api/device/%s" % dev["id"]["id"])
    full["firmwareId"] = {"entityType": "OTA_PACKAGE", "id": pkg["id"]["id"]} if pkg else None
    api.post("/api/device", full)


def cmd_assign(api, args):
    pkg = need_package(api, args.version)
    set_firmware(api, args.device, pkg)
    print("assigned %s %s to %s" % (pkg["title"], pkg["version"], args.device))


def cmd_unassign(api, args):
    set_firmware(api, args.device, None)
    print("unassigned firmware from %s" % args.device)


def cmd_status(api, args):
    did = need_device(api, args.device)["id"]["id"]
    tel = api.latest_telemetry(did, STATUS_TELEMETRY)
    print("telemetry:")
    for k in STATUS_TELEMETRY:
        print("  %-18s %s" % (k, tel[k][1] if k in tel else "-"))
    rows = api.get("/api/plugins/telemetry/DEVICE/%s/values/attributes/SHARED_SCOPE" % did) or []
    print("shared attributes (fw_*):")
    shown = False
    for row in rows:
        if str(row.get("key", "")).startswith("fw_"):
            print("  %-18s %s" % (row["key"], row.get("value")))
            shown = True
    if not shown:
        print("  (none: no firmware assigned)")


def cmd_delete(api, args):
    pkg = need_package(api, args.version)
    api.delete("/api/otaPackage/%s" % pkg["id"]["id"])
    print("deleted package %s %s" % (pkg["title"], pkg["version"]))


def build_parser():
    ap = argparse.ArgumentParser(description="ThingsBoard OTA packages for fishpump")
    ap.add_argument("--selftest", action="store_true", help="offline checks, no password or network")
    tb_api.add_common_args(ap)
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("upload", help="create a package and upload the image")
    s.add_argument("image")
    s.add_argument("--version", required=True)
    s.add_argument("--title", default=DEFAULT_TITLE)
    s.add_argument("--no-version-check", action="store_true",
                   help="do not compare the image's built-in version with --version (for test files)")
    s.add_argument("--checksum", help="expected SHA-256 hex (ThingsBoard computes it when omitted)")
    s = sub.add_parser("assign", help="assign a package version to a device")
    s.add_argument("device")
    s.add_argument("version")
    s = sub.add_parser("unassign", help="clear a device's firmware assignment")
    s.add_argument("device")
    s = sub.add_parser("status", help="show OTA telemetry and shared fw_* attributes")
    s.add_argument("device")
    s = sub.add_parser("delete", help="delete the fishpump package with this version")
    s.add_argument("version")
    return ap


COMMANDS = {"upload": cmd_upload, "assign": cmd_assign, "unassign": cmd_unassign,
            "status": cmd_status, "delete": cmd_delete}


def selftest():
    fails = []

    def check(ok, what):
        if not ok:
            fails.append(what)

    data = bytes(range(256)) + b"\r\n--b--\r\n" + b"\x00\xff"
    body = build_multipart("fishpump.bin", data, "BND")
    check(body.startswith(b"--BND\r\n"), "starts with the boundary")
    check(body.endswith(b"\r\n--BND--\r\n"), "ends with the closing boundary")
    check(b'name="file"; filename="fishpump.bin"' in body, "file part named file")
    check(b"\r\n\r\n" + data + b"\r\n--BND--\r\n" in body, "exact file bytes before the closing boundary")
    check(body.count(b"--BND\r\n") == 1, "one part")
    check(build_multipart("a.bin", b"", "X").endswith(b"\r\n\r\n\r\n--X--\r\n"), "empty file")
    ap = build_parser()
    a = ap.parse_args(["upload", "x.bin", "--version", "1.2.3"])
    check((a.cmd, a.image, a.version, a.title, a.checksum) == ("upload", "x.bin", "1.2.3", "fishpump", None), "upload args")
    a = ap.parse_args(["upload", "x.bin", "--version", "1", "--title", "t", "--checksum", "ab"])
    check((a.title, a.checksum) == ("t", "ab"), "upload options")
    a = ap.parse_args(["assign", "dev", "1.2.3"])
    check((a.cmd, a.device, a.version) == ("assign", "dev", "1.2.3"), "assign args")
    a = ap.parse_args(["unassign", "dev"])
    check((a.cmd, a.device) == ("unassign", "dev"), "unassign args")
    a = ap.parse_args(["status", "dev"])
    check((a.cmd, a.device) == ("status", "dev"), "status args")
    a = ap.parse_args(["delete", "1.2.3"])
    check((a.cmd, a.version) == ("delete", "1.2.3"), "delete args")
    check(set(COMMANDS) == {"upload", "assign", "unassign", "status", "delete"}, "all subcommands handled")
    def synth(magic=APP_DESC_MAGIC, ver=b"0.3.2"):
        buf = bytearray(80)
        buf[24 + 8:24 + 8 + 4] = magic.to_bytes(4, "little")
        buf[IMAGE_VERSION_OFFSET:IMAGE_VERSION_OFFSET + len(ver)] = ver
        return bytes(buf)

    check(read_image_version(synth()) == "0.3.2", "image version read from a synthetic image")
    check(read_image_version(synth(ver=b"v1.0")) == "v1.0", "image version keeps the leading v")
    check(IMAGE_VERSION_OFFSET == 48, "version offset is 48")
    for what, blob in (("wrong magic", synth(magic=0x12345678)), ("short file", synth()[:60]), ("empty", b"")):
        try:
            read_image_version(blob)
            check(False, "read_image_version must refuse " + what)
        except ValueError:
            pass
    check(versions_equal("v0.3.2", "0.3.2") and versions_equal("0.3.2", "0.3.2") and not versions_equal("0.3.2", "0.3.3"),
          "versions_equal ignores one leading v")
    a = ap.parse_args(["upload", "x.bin", "--version", "1", "--no-version-check"])
    check(a.no_version_check is True, "--no-version-check parsed")
    check(ap.parse_args(["upload", "x.bin", "--version", "1"]).no_version_check is False, "version check on by default")
    import contextlib
    import io
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            ap.parse_args(["upload", "x.bin"])
        check(False, "upload without --version must fail")
    except SystemExit:
        pass
    if fails:
        for f in fails:
            print("  FAIL: " + f)
        print("SELFTEST FAIL (%d)" % len(fails))
        return 1
    print("SELFTEST PASS")
    return 0


def main(argv=None):
    tb_api.utf8_stdout()
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.cmd:
        ap.error("a subcommand is required")
    api = tb_api.connect_from_args(args, ap)
    try:
        COMMANDS[args.cmd](api, args)
    except (tb_api.TbError, OSError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
