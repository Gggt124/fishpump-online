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


def test_packaging():
    print(chr(10) + "[packaging and customer tools]")
    wifi = read(FIRMWARE_DIR / "components" / "wifi_manager" / "wifi_manager.c")
    ps = read(FIRMWARE_DIR / "scripts" / "customer" / "tools" / "ap_password.ps1")
    assert_test("SALT = 0x5A" in wifi and "mac[2] ^ SALT" in wifi and "mac[5] ^ SALT" in wifi,
                "firmware AP password formula is MAC[2..5] XOR 0x5A",
                "if the firmware formula changes, update ap_password.ps1 and its test vector")
    assert_test("$salt = 0x5A" in ps and "$Mac[2]" in ps and "$Mac[5]" in ps,
                "ap_password.ps1 uses the same formula")
    flash = read(FIRMWARE_DIR / "scripts" / "customer" / "tools" / "flash_and_show.ps1")
    assert_test("0x0" in flash and "factory.bin" in flash, "FLASH installs factory.bin at 0x0")
    assert_test((FIRMWARE_DIR / "scripts" / "package_release.py").exists(), "package_release.py exists")


def _load_workflow(name):
    import yaml  # PyYAML: pip install pyyaml
    path = REPO_DIR / ".github" / "workflows" / name
    text = read(path)
    doc = yaml.safe_load(text) if text else {}
    triggers = doc.get("on", doc.get(True, {})) if isinstance(doc, dict) else {}
    return text, doc, triggers


def test_workflows():
    print(chr(10) + "[GitHub Actions]")
    idf = read(FIRMWARE_DIR / "IDF_VERSION").strip()
    ci_text, ci, ci_on = _load_workflow("ci.yml")
    assert_test(bool(ci_text), "ci.yml exists")
    assert_test("pull_request" in ci_on and "push" in ci_on, "ci.yml runs on pull requests and pushes to main")
    assert_test("main" in (ci_on.get("push") or {}).get("branches", []), "ci.yml push trigger is limited to main")
    assert_test("espressif/idf:v" + idf in ci_text, "ci.yml uses the pinned ESP-IDF image", "expected espressif/idf:v" + idf)
    assert_test("verify_" in ci_text and "unittest" in ci_text, "ci.yml runs verify scripts and unit tests")
    assert_test("idf.py" in ci_text and "build" in ci_text, "ci.yml builds the firmware")
    assert_test("run_selftest.sh" in ci_text, "ci.yml runs the QEMU selftest")

    rel_text, rel, rel_on = _load_workflow("release.yml")
    assert_test(bool(rel_text), "release.yml exists")
    push = rel_on.get("push") or {}
    assert_test(push.get("tags") == ["v*.*.*"] and "branches" not in push and "pull_request" not in rel_on,
                "release.yml runs only on version tags")
    assert_test((rel.get("permissions") or {}).get("contents") == "write" or
                any((j.get("permissions") or {}).get("contents") == "write" for j in rel.get("jobs", {}).values()),
                "release.yml can create a release (contents: write)")
    i_tag = rel_text.find("check_release_tag.py")
    i_secrets = rel_text.find("write_secrets.py --from-env")
    i_build = rel_text.find("-DFISHPUMP_RELEASE=ON")
    i_pack = rel_text.find("package_release.py")
    assert_test(0 <= i_tag < i_secrets < i_build < i_pack,
                "release.yml order: check tag, write secrets, build in release mode, package",
                f"positions {i_tag} {i_secrets} {i_build} {i_pack}")
    assert_test("secrets.OTA_ENCRYPTION_KEY" in rel_text, "release.yml takes the key from the OTA_ENCRYPTION_KEY secret")
    jobs = rel.get("jobs", {})
    key_steps = [st for j in jobs.values() for st in j.get("steps", []) if "OTA_ENCRYPTION_KEY" in (st.get("env") or {})]
    wide_env = [rel.get("env") or {}] + [j.get("env") or {} for j in jobs.values()]
    assert_test(rel_text.count("secrets.OTA_ENCRYPTION_KEY") == len(key_steps) == 2 and
                not any("OTA_ENCRYPTION_KEY" in e for e in wide_env),
                "the key secret is only set in the env of the two steps that need it (never workflow or job wide)")
    leaky = [l for l in rel_text.splitlines() if "OTA_ENCRYPTION_KEY" in l and ("echo" in l or "printenv" in l)]
    assert_test(not leaky and "set -x" not in rel_text and "printenv" not in rel_text and "env |" not in rel_text,
                "release.yml never echoes or dumps the key", str(leaky))
    assert_test("gh release create" in rel_text, "release.yml publishes the files with gh release create")


def main():
    test_build_inputs()
    test_release_gate()
    test_firmware_reports_key()
    test_packaging()
    test_workflows()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
