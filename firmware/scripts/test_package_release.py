#!/usr/bin/env python3
"""Tests for package_release.py and the customer flash tools.

Run: python -m unittest firmware/scripts/test_package_release.py -v
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # noqa: E402

import ota_crypto  # noqa: E402
import package_release  # noqa: E402

KEY = "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"
VERSION = "0.4.1"

FLASHER_ARGS = {
    "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
    "flash_files": {
        "0x0": "bootloader/bootloader.bin",
        "0x10000": "partition_table/partition-table.bin",
        "0x17000": "ota_data_initial.bin",
        "0x20000": "fishpump_firmware.bin",
    },
    "app": {"offset": "0x20000", "file": "fishpump_firmware.bin", "encrypted": "false"},
    "extra_esptool_args": {"chip": "esp32s3"},
}


def make_build_dir(root: Path) -> dict:
    files = {
        "bootloader/bootloader.bin": b"\xaa" * 100,
        "partition_table/partition-table.bin": b"\xbb" * 50,
        "ota_data_initial.bin": b"\xcc" * 20,
        "fishpump_firmware.bin": os.urandom(3000),
    }
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    (root / "flasher_args.json").write_text(json.dumps(FLASHER_ARGS))
    return files


class MergeFactoryTests(unittest.TestCase):
    def test_images_land_at_their_offsets_with_ff_padding(self):
        with tempfile.TemporaryDirectory() as d:
            build = Path(d) / "build"
            files = make_build_dir(build)
            blob = package_release.merge_factory(build)
            self.assertEqual(blob[0:100], files["bootloader/bootloader.bin"])
            self.assertEqual(blob[100:0x10000], b"\xff" * (0x10000 - 100))
            self.assertEqual(blob[0x10000:0x10000 + 50], files["partition_table/partition-table.bin"])
            self.assertEqual(blob[0x17000:0x17000 + 20], files["ota_data_initial.bin"])
            app = files["fishpump_firmware.bin"]
            self.assertEqual(blob[0x20000:], app)
            self.assertEqual(len(blob), 0x20000 + len(app))

    def test_overlapping_images_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            build = Path(d) / "build"
            make_build_dir(build)
            (build / "bootloader/bootloader.bin").write_bytes(b"\xaa" * 0x10001)
            with self.assertRaisesRegex(ValueError, "overlap"):
                package_release.merge_factory(build)

    def test_missing_file_is_reported_by_name(self):
        with tempfile.TemporaryDirectory() as d:
            build = Path(d) / "build"
            make_build_dir(build)
            (build / "ota_data_initial.bin").unlink()
            with self.assertRaisesRegex(ValueError, "ota_data_initial.bin"):
                package_release.merge_factory(build)


class PackageCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.build = root / "build"
        cls.files = make_build_dir(cls.build)
        cls.out = root / "out"
        env = dict(os.environ, OTA_TEST_KEY=KEY)
        cls.result = subprocess.run(
            [sys.executable, str(SCRIPTS / "package_release.py"), "--build-dir", str(cls.build), "--version", VERSION,
             "--out-dir", str(cls.out), "--key-env", "OTA_TEST_KEY", "--no-esptool-exe"],
            capture_output=True, text=True, env=env,
        )
        cls.fp = ota_crypto.key_fingerprint(KEY)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_run_succeeded(self):
        self.assertEqual(self.result.returncode, 0, self.result.stderr)

    def test_expected_files_exist(self):
        names = {p.name for p in self.out.iterdir()}
        self.assertEqual(
            names,
            {f"fishpump-{VERSION}.bin", f"fishpump-{VERSION}-{self.fp}.ota.bin", f"fishpump-{VERSION}-factory.bin",
             "flash-package.zip", "SHA256SUMS"},
        )

    def test_plain_image_is_the_app_binary(self):
        self.assertEqual((self.out / f"fishpump-{VERSION}.bin").read_bytes(), self.files["fishpump_firmware.bin"])

    def test_ota_file_decrypts_to_the_app_binary(self):
        blob = (self.out / f"fishpump-{VERSION}-{self.fp}.ota.bin").read_bytes()
        dec = Cipher(algorithms.AES(bytes.fromhex(KEY)), modes.CTR(blob[:16])).decryptor()
        self.assertEqual(dec.update(blob[16:]) + dec.finalize(), self.files["fishpump_firmware.bin"])

    def test_sha256sums_match_the_files(self):
        lines = (self.out / "SHA256SUMS").read_text().strip().splitlines()
        listed = {}
        for line in lines:
            digest, name = line.split("  ", 1)
            listed[name] = digest
        self.assertEqual(set(listed), {p.name for p in self.out.iterdir() if p.name != "SHA256SUMS"})
        for name, digest in listed.items():
            self.assertEqual(hashlib.sha256((self.out / name).read_bytes()).hexdigest(), digest, name)

    def test_zip_contains_the_install_kit(self):
        with zipfile.ZipFile(self.out / "flash-package.zip") as z:
            names = set(z.namelist())
            for required in ("FLASH.bat", "ERASE_FLASH.bat", "SHOW_PASSWORD.bat", "README_customer.txt",
                             "README_ลูกค้า.txt", "firmware/factory.bin", "tools/flash_and_show.ps1",
                             "tools/erase_flash.ps1", "tools/show_ap_password.ps1", "tools/ap_password.ps1"):
                self.assertIn(required, names)
            self.assertEqual(z.read("firmware/factory.bin"), (self.out / f"fishpump-{VERSION}-factory.bin").read_bytes())
            flash = z.read("tools/flash_and_show.ps1").decode("utf-8-sig")
            self.assertIn("factory.bin", flash)
            self.assertIn("0x0", flash)
            self.assertNotIn("esp32 ", flash.lower().replace("esp32s3", ""))

    def test_no_output_contains_the_key(self):
        needles = [KEY.encode(), KEY.upper().encode(), bytes.fromhex(KEY)]
        blobs = [p.read_bytes() for p in self.out.iterdir()]
        with zipfile.ZipFile(self.out / "flash-package.zip") as z:
            blobs += [z.read(n) for n in z.namelist()]
        blobs.append((self.result.stdout + self.result.stderr).encode())
        for blob in blobs:
            for needle in needles:
                self.assertNotIn(needle, blob)

    def test_fingerprint_is_reported_in_the_output(self):
        self.assertIn(self.fp, self.result.stdout)


@unittest.skipIf(shutil.which("powershell") is None, "powershell not available")
class CustomerToolsTests(unittest.TestCase):
    def test_esptool_calls_use_v5_command_names(self):
        """The bundled esptool v5 prints a deprecation warning for write_flash/erase_flash/read_mac."""
        import re
        old = re.compile(r"(?<![\w.\\/-])(write_flash|erase_flash|read_mac)(?![\w.])")
        for path in sorted((SCRIPTS / "customer" / "tools").glob("*.ps1")):
            for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "& $" in line:
                    with self.subTest(file=path.name, line=n):
                        self.assertIsNone(old.search(line), line.strip())


class ApPasswordTests(unittest.TestCase):
    def run_ps(self, *args):
        return subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", *args],
            capture_output=True, text=True,
        )

    def test_selftest_vector_passes(self):
        r = self.run_ps(str(SCRIPTS / "customer" / "tools" / "show_ap_password.ps1"), "-SelfTest")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("OK", r.stdout)


class ReleaseTagTests(unittest.TestCase):
    """check_release_tag.py: a release tag must match firmware/version.txt before anything is built."""

    def run_check(self, tag, version="0.4.1\n"):
        with tempfile.TemporaryDirectory() as d:
            vf = Path(d) / "version.txt"
            vf.write_text(version)
            return subprocess.run(
                [sys.executable, str(SCRIPTS / "check_release_tag.py"), tag, "--version-file", str(vf)],
                capture_output=True, text=True,
            )

    def test_matching_tag_passes_and_prints_the_package_version(self):
        r = self.run_check("v0.4.1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "0.4.1 release")

    def test_release_candidate_suffix_is_a_prerelease(self):
        r = self.run_check("v0.4.1-rc1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "0.4.1-rc1 prerelease")

    def test_version_file_with_crlf_still_matches(self):
        self.assertEqual(self.run_check("v0.4.1", version="0.4.1\r\n").returncode, 0)

    def test_mismatched_tag_fails_before_anything_is_built(self):
        r = self.run_check("v0.4.2")
        self.assertEqual(r.returncode, 1)
        self.assertIn("version.txt", r.stderr)
        self.assertNotIn("Traceback", r.stderr)

    def test_malformed_tags_fail(self):
        for tag in ("0.4.1", "v0.4", "v0.4.1.2", "vX.Y.Z", ""):
            with self.subTest(tag=tag):
                self.assertEqual(self.run_check(tag).returncode, 1)


if __name__ == "__main__":
    unittest.main()
