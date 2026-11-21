#!/usr/bin/env python3
"""
verify_release_pipeline.py

Source contract checks for the release pipeline
(docs/superpowers/specs/2026-11-21-release-pipeline-design.md).
Each task of docs/superpowers/plans/2026-11-21-release-pipeline.md appends its
own test_* function below and calls it from main().
"""

import re
import sys
from pathlib import Path

FIRMWARE_DIR = Path(__file__).resolve().parent.parent
REPO_DIR = FIRMWARE_DIR.parent

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

passed_tests = 0
failed_tests = 0


def assert_test(condition, name, details=""):
    global passed_tests, failed_tests
    if condition:
        passed_tests += 1
        print(f"  [PASS] {name}")
    else:
        failed_tests += 1
        print(f"  [FAIL] {name} - {details}")


def read(path):
    p = Path(path)
    return p.read_text(encoding="utf-8") if p.exists() else ""


def test_build_inputs():
    print("\n[build inputs]")
    gitignore = read(REPO_DIR / ".gitignore").splitlines()
    ignored = {line.strip() for line in gitignore}
    assert_test(
        "firmware/components/app_config/secrets.h" in ignored and "**/secrets.h" in ignored,
        "secrets.h is gitignored",
        "add firmware/components/app_config/secrets.h and **/secrets.h to .gitignore",
    )
    assert_test(
        "firmware/dependencies.lock" not in ignored,
        "dependencies.lock is not gitignored",
        "the lock file must be committed so builds are reproducible",
    )
    idf_version = read(FIRMWARE_DIR / "IDF_VERSION").strip()
    assert_test(idf_version == "6.0.1", "IDF_VERSION is 6.0.1", f"got {idf_version!r}")
    lock = read(FIRMWARE_DIR / "dependencies.lock")
    m = re.search(r"^  idf:\n(?:    .*\n)*?    version: (\S+)", lock, re.M)
    assert_test(
        m is not None and m.group(1) == idf_version,
        "IDF_VERSION matches dependencies.lock",
        f"lock idf version {m.group(1) if m else None!r}",
    )
    yml = read(FIRMWARE_DIR / "main" / "idf_component.yml")
    assert_test("'*'" not in yml and '"*"' not in yml, "no wildcard component versions", "pin every dependency")


def test_release_gate():
    print("\n[release key gate]")
    cmake = read(FIRMWARE_DIR / "CMakeLists.txt")
    assert_test("option(FISHPUMP_RELEASE" in cmake, "CMakeLists has the FISHPUMP_RELEASE option")
    assert_test("cmake/check_release_key.cmake" in cmake, "CMakeLists includes the release key check")
    check = read(FIRMWARE_DIR / "cmake" / "check_release_key.cmake")
    assert_test("FATAL_ERROR" in check, "check script fails the build", "expected message(FATAL_ERROR ...)")


def test_firmware_reports_key():
    print(chr(10) + "[firmware: upload limit and key fingerprint]")
    web = read(FIRMWARE_DIR / "main" / "web_server.c")
    assert_test("0x1E0000" not in web, "web OTA no longer uses the fixed 0x1E0000 limit")
    assert_test("ota_upload_limit(" in web, "web OTA limit comes from ota_upload_limit()")
    assert_test("ota_key_fingerprint(" in read(FIRMWARE_DIR / "main" / "dapi_status.c"), "status.get reports the key fingerprint")
    assert_test("ota_key_fp" in read(FIRMWARE_DIR / "main" / "dapi_status.c"), "status.get has ota_key_fp")
    assert_test("ota_key_dev" in read(FIRMWARE_DIR / "main" / "dapi_status.c"), "status.get has ota_key_dev")
    assert_test("ota_key_fp" in read(FIRMWARE_DIR / "main" / "tb_state.c"), "ThingsBoard attributes carry ota_key_fp")
    assert_test("ota_key_dev" in read(FIRMWARE_DIR / "main" / "tb_state.c"), "ThingsBoard attributes carry ota_key_dev")
    assert_test("OTA key fp=" in read(FIRMWARE_DIR / "main" / "app_main.c"), "boot log prints the key fingerprint")
    cm = read(FIRMWARE_DIR / "main" / "CMakeLists.txt")
    assert_test(re.search(r"REQUIRES[\s\S]*\bota_key\b", cm) is not None, "main requires ota_key")
    doc = read(REPO_DIR / "docs" / "firmware" / "device-api.md")
    assert_test("ota_key_fp" in doc, "device-api.md documents ota_key_fp")


def main():
    test_build_inputs()
    test_release_gate()
    test_firmware_reports_key()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
