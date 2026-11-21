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


def main():
    test_build_inputs()
    print(f"\nPASSED: {passed_tests}  FAILED: {failed_tests}")
    sys.exit(1 if failed_tests else 0)


if __name__ == "__main__":
    main()
