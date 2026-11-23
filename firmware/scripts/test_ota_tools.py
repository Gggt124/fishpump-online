#!/usr/bin/env python3
"""Tests for the OTA tools: ota_crypto, encrypt_ota.py, write_secrets.py.

Run: python -m unittest firmware/scripts/test_ota_tools.py -v
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # noqa: E402

import ota_crypto  # noqa: E402

NIST_KEY = "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"
NIST_IV = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
NIST_PLAIN = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
NIST_CIPHER = bytes.fromhex("601ec313775789a5b7a7f504bbf3d228")
ZERO_KEY = "0" * 64


def run_script(name, *args, env=None):
    full_env = dict(os.environ)
    full_env.pop("OTA_TEST_KEY", None)
    if env:
        full_env.update(env)
    return subprocess.run(
        [sys.executable, str(SCRIPTS / name), *args],
        capture_output=True,
        text=True,
        env=full_env,
    )


class ScriptHygieneTests(unittest.TestCase):
    def test_scripts_compile_without_warnings(self):
        """A stray backslash in a docstring prints a SyntaxWarning on every run (found on the bench)."""
        import warnings
        for path in sorted(SCRIPTS.glob("*.py")):
            with self.subTest(script=path.name), warnings.catch_warnings():
                warnings.simplefilter("error")
                compile(path.read_text(encoding="utf-8"), str(path), "exec")


class OtaCryptoTests(unittest.TestCase):
    def test_nist_ctr_vector(self):
        out = ota_crypto.encrypt_bytes(NIST_PLAIN, NIST_KEY, iv=NIST_IV)
        self.assertEqual(out[:16], NIST_IV)
        self.assertEqual(out[16:], NIST_CIPHER)

    def test_zero_key_fingerprint(self):
        self.assertEqual(ota_crypto.key_fingerprint(ZERO_KEY), "66687aad")

    def test_uppercase_key_with_trailing_newline_is_accepted(self):
        messy = "  " + NIST_KEY.upper() + "\r\n"
        self.assertEqual(ota_crypto.normalize_key(messy), NIST_KEY)
        self.assertEqual(ota_crypto.key_fingerprint(messy), ota_crypto.key_fingerprint(NIST_KEY))

    def test_bad_keys_are_rejected(self):
        for bad in (NIST_KEY[:-1], NIST_KEY + "a", "zz" + NIST_KEY[2:], ""):
            with self.subTest(length=len(bad)):
                with self.assertRaisesRegex(ValueError, "64 hex characters"):
                    ota_crypto.normalize_key(bad)

    def test_empty_data_encrypts_to_iv_only(self):
        out = ota_crypto.encrypt_bytes(b"", NIST_KEY)
        self.assertEqual(len(out), 16)

    def test_image_exactly_at_limit_is_accepted_and_one_over_is_rejected(self):
        ota_crypto.encrypt_bytes(bytes(ota_crypto.OTA_MAX_IMAGE), NIST_KEY)
        with self.assertRaisesRegex(ValueError, "too large"):
            ota_crypto.encrypt_bytes(bytes(ota_crypto.OTA_MAX_IMAGE + 1), NIST_KEY)

    def test_iv_is_random_when_not_given(self):
        a = ota_crypto.encrypt_bytes(b"x" * 32, NIST_KEY)
        b = ota_crypto.encrypt_bytes(b"x" * 32, NIST_KEY)
        self.assertNotEqual(a[:16], b[:16])

    def test_round_trip_decrypts_to_original(self):
        data = os.urandom(5000)
        blob = ota_crypto.encrypt_bytes(data, NIST_KEY)
        dec = Cipher(algorithms.AES(bytes.fromhex(NIST_KEY)), modes.CTR(blob[:16])).decryptor()
        self.assertEqual(dec.update(blob[16:]) + dec.finalize(), data)

    def test_read_key_from_env_and_file(self):
        with tempfile.TemporaryDirectory() as d:
            key_file = Path(d) / "key.txt"
            key_file.write_text(NIST_KEY.upper() + "\n")
            os.environ["OTA_TEST_KEY"] = NIST_KEY + "\n"
            try:
                self.assertEqual(ota_crypto.read_key("OTA_TEST_KEY", None), NIST_KEY)
            finally:
                del os.environ["OTA_TEST_KEY"]
            self.assertEqual(ota_crypto.read_key(None, str(key_file)), NIST_KEY)
            with self.assertRaisesRegex(ValueError, "not set"):
                ota_crypto.read_key("OTA_TEST_KEY", None)


class EncryptCliTests(unittest.TestCase):
    def test_encrypts_a_file_using_env_key_without_echoing_it(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.bin", Path(d) / "out.bin"
            src.write_bytes(b"firmware-bytes" * 10)
            r = run_script("encrypt_ota.py", str(src), str(dst), "--key-env", "OTA_TEST_KEY", env={"OTA_TEST_KEY": NIST_KEY})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(len(dst.read_bytes()), 16 + len(src.read_bytes()))
            self.assertNotIn(NIST_KEY, r.stdout + r.stderr)
            self.assertIn(ota_crypto.key_fingerprint(NIST_KEY), r.stdout)

    def test_bad_key_exits_1_without_traceback(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "in.bin"
            src.write_bytes(b"x")
            r = run_script("encrypt_ota.py", str(src), str(Path(d) / "o.bin"), "--key-env", "OTA_TEST_KEY", env={"OTA_TEST_KEY": NIST_KEY[:-1]})
            self.assertEqual(r.returncode, 1)
            self.assertIn("64 hex characters", r.stderr)
            self.assertNotIn("Traceback", r.stderr)

    def test_oversized_image_exits_1_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.bin", Path(d) / "out.bin"
            src.write_bytes(bytes(ota_crypto.OTA_MAX_IMAGE + 1))
            r = run_script("encrypt_ota.py", str(src), str(dst), "--key-env", "OTA_TEST_KEY", env={"OTA_TEST_KEY": NIST_KEY})
            self.assertEqual(r.returncode, 1)
            self.assertIn("too large", r.stderr)
            self.assertFalse(dst.exists())

    def test_empty_file_is_encrypted_without_error(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "in.bin", Path(d) / "out.bin"
            src.write_bytes(b"")
            r = run_script("encrypt_ota.py", str(src), str(dst), "--key-env", "OTA_TEST_KEY", env={"OTA_TEST_KEY": NIST_KEY})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(len(dst.read_bytes()), 16)


class WriteSecretsTests(unittest.TestCase):
    def test_from_env_writes_the_key_and_prints_only_the_fingerprint(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "secrets.h"
            r = run_script("write_secrets.py", "--from-env", "OTA_TEST_KEY", "--output", str(out), env={"OTA_TEST_KEY": NIST_KEY.upper() + "\n"})
            self.assertEqual(r.returncode, 0, r.stderr)
            text = out.read_text(encoding="utf-8")
            self.assertIn(f'#define APP_CONFIG_OTA_ENCRYPTION_KEY "{NIST_KEY}"', text)
            self.assertFalse(out.read_bytes().startswith(b"\xef\xbb\xbf"))
            self.assertNotIn("\r", text)
            self.assertNotIn(NIST_KEY, r.stdout + r.stderr)
            self.assertNotIn(NIST_KEY.upper(), r.stdout + r.stderr)
            self.assertIn(ota_crypto.key_fingerprint(NIST_KEY), r.stdout)

    def test_refuses_to_overwrite_without_force(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "secrets.h"
            out.write_text("existing")
            r = run_script("write_secrets.py", "--from-env", "OTA_TEST_KEY", "--output", str(out), env={"OTA_TEST_KEY": NIST_KEY})
            self.assertEqual(r.returncode, 1)
            self.assertEqual(out.read_text(), "existing")
            r = run_script("write_secrets.py", "--from-env", "OTA_TEST_KEY", "--output", str(out), "--force", env={"OTA_TEST_KEY": NIST_KEY})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn(NIST_KEY, out.read_text())

    def test_refuses_the_zero_key(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "secrets.h"
            r = run_script("write_secrets.py", "--from-env", "OTA_TEST_KEY", "--output", str(out), env={"OTA_TEST_KEY": ZERO_KEY})
            self.assertEqual(r.returncode, 1)
            self.assertFalse(out.exists())

    def test_bad_key_exits_1_without_traceback(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "secrets.h"
            r = run_script("write_secrets.py", "--from-env", "OTA_TEST_KEY", "--output", str(out), env={"OTA_TEST_KEY": "abc"})
            self.assertEqual(r.returncode, 1)
            self.assertIn("64 hex characters", r.stderr)
            self.assertNotIn("Traceback", r.stderr)

    def test_generate_makes_a_random_valid_key(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d) / "a.h", Path(d) / "b.h"
            for path in (a, b):
                r = run_script("write_secrets.py", "--generate", "--output", str(path))
                self.assertEqual(r.returncode, 0, r.stderr)

            def key_of(path):
                text = path.read_text()
                start = text.index('KEY "') + 5
                return text[start:start + 64]

            ka, kb = key_of(a), key_of(b)
            self.assertNotEqual(ka, kb)
            self.assertEqual(ota_crypto.normalize_key(ka), ka)
            self.assertNotIn(ka, r.stdout)


CHECK_CMAKE = SCRIPTS.parent / "cmake" / "check_release_key.cmake"
SECRETS_LINE = '#define APP_CONFIG_OTA_ENCRYPTION_KEY "{key}"\n'


@unittest.skipIf(shutil.which("cmake") is None, "cmake not on PATH (activate ESP-IDF first)")
class ReleaseKeyCheckTests(unittest.TestCase):
    def check(self, content=None, raw=None):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "secrets.h"
            if raw is not None:
                f.write_bytes(raw)
            elif content is not None:
                f.write_text(content, encoding="utf-8", newline="")
            return subprocess.run(
                ["cmake", f"-DSECRETS_FILE={f.as_posix()}", "-P", str(CHECK_CMAKE)],
                capture_output=True,
                text=True,
            )

    def assert_fails_without_leaking_key(self, r, key):
        self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(key, r.stdout + r.stderr)

    def test_missing_file_fails(self):
        r = subprocess.run(
            ["cmake", "-DSECRETS_FILE=/nonexistent/secrets.h", "-P", str(CHECK_CMAKE)],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("write_secrets.py", r.stdout + r.stderr)

    def test_zero_key_fails(self):
        self.assert_fails_without_leaking_key(self.check(SECRETS_LINE.format(key=ZERO_KEY)), ZERO_KEY)

    def test_template_placeholder_fails(self):
        r = self.check(SECRETS_LINE.format(key="REPLACE_WITH_64_HEX_CHARS_FROM_generate_secrets_ps1"))
        self.assertNotEqual(r.returncode, 0)

    def test_short_key_fails(self):
        self.assert_fails_without_leaking_key(self.check(SECRETS_LINE.format(key=NIST_KEY[:-1])), NIST_KEY[:-1])

    def test_good_key_passes(self):
        r = self.check(SECRETS_LINE.format(key=NIST_KEY))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn(NIST_KEY, r.stdout + r.stderr)

    def test_bom_and_crlf_still_pass(self):
        body = "#pragma once\r\n" + SECRETS_LINE.format(key=NIST_KEY).replace("\n", "\r\n")
        r = self.check(raw=b"\xef\xbb\xbf" + body.encode())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
