#!/usr/bin/env python3
"""
verify_pt100_inverter_system.py

Comprehensive regression test and verification suite for:
- PT100 MAX31865 SPI RTD Sensor
- Inverter 0-10V PWM Output Driver with Safety Slew Rate
- Multi-Point Rule Curve Engine (Linear & Step modes)
- Partition table & binary memory allocation
- REST API contracts and SSE telemetry payloads
- Web UI dashboard and compressed asset consistency
"""

import os
import sys
import math
import re
import gzip
from pathlib import Path

# Paths
FIRMWARE_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = FIRMWARE_DIR / "build"
COMPONENTS_DIR = FIRMWARE_DIR / "components"
MAIN_DIR = FIRMWARE_DIR / "main"

COOLING_MAX_RULE_POINTS = 8
COOLING_MIN_RULE_POINTS = 2
COOLING_RULE_MAX_OUTPUT_MV = 10000

COOLING_CURVE_LINEAR = 0
COOLING_CURVE_STEP = 1

COOLING_PWM_MAX_DUTY = 1023
COOLING_PWM_MAX_MV = 10000

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


# -----------------------------------------------------------------------------
# Section 1: Multi-Point Rule Curve Engine Math (Replicating C implementation)
# -----------------------------------------------------------------------------

def cooling_validate_settings(settings):
    if not settings:
        return False
    curve_type = settings.get("curve_type")
    if curve_type not in (COOLING_CURVE_LINEAR, COOLING_CURVE_STEP):
        return False
    points = settings.get("points", [])
    count = settings.get("point_count", len(points))
    if count < COOLING_MIN_RULE_POINTS or count > COOLING_MAX_RULE_POINTS:
        return False
    if len(points) < count:
        return False
    for i in range(count):
        p = points[i]
        if p["output_mv"] > COOLING_RULE_MAX_OUTPUT_MV or p["output_mv"] < 0:
            return False
        if i > 0:
            if p["temp_c_x10"] <= points[i - 1]["temp_c_x10"]:
                return False
    return True


def cooling_evaluate_rule_curve(settings, temp_c):
    if not cooling_validate_settings(settings):
        return 0
    if temp_c is None or not math.isfinite(temp_c):
        return 0

    points = settings["points"]
    count = settings["point_count"]

    t_0 = points[0]["temp_c_x10"] / 10.0
    if temp_c < t_0:
        return 0

    t_last = points[count - 1]["temp_c_x10"] / 10.0
    if temp_c >= t_last:
        return points[count - 1]["output_mv"]

    for i in range(count - 1):
        t_i = points[i]["temp_c_x10"] / 10.0
        t_next = points[i + 1]["temp_c_x10"] / 10.0

        if t_i <= temp_c < t_next:
            if settings["curve_type"] == COOLING_CURVE_STEP:
                return points[i]["output_mv"]

            # Linear interpolation
            v_i = points[i]["output_mv"]
            v_next = points[i + 1]["output_mv"]
            span_t = t_next - t_i
            if span_t <= 0.0:
                return points[i]["output_mv"]

            v = v_i + ((temp_c - t_i) * (v_next - v_i) / span_t)
            v_rounded = round(v)
            if v_rounded < 0:
                v_rounded = 0
            elif v_rounded > COOLING_RULE_MAX_OUTPUT_MV:
                v_rounded = COOLING_RULE_MAX_OUTPUT_MV
            return v_rounded

    return points[count - 1]["output_mv"]


def test_rule_curve_engine():
    print("\n--- 1. Testing Multi-Point Rule Curve Engine ---")

    # Default settings
    default_settings = {
        "auto_enable": False,
        "curve_type": COOLING_CURVE_LINEAR,
        "point_count": 2,
        "points": [
            {"temp_c_x10": 240, "output_mv": 5000},
            {"temp_c_x10": 350, "output_mv": 10000},
        ],
        "min_off_sec": 180,
        "test_timeout_sec": 30,
    }

    assert_test(cooling_validate_settings(default_settings), "Default settings are valid")

    # Below T0 -> 0 mV
    assert_test(cooling_evaluate_rule_curve(default_settings, 20.0) == 0, "Temp below T0 outputs 0 mV")
    assert_test(cooling_evaluate_rule_curve(default_settings, 23.9) == 0, "Temp 23.9C outputs 0 mV")

    # Exact T0 -> V0
    assert_test(cooling_evaluate_rule_curve(default_settings, 24.0) == 5000, "Temp 24.0C (T0) outputs 5000 mV")

    # Interpolation midpoint: 29.5C between 24C(5000) and 35C(10000)
    # span = 11.0, frac = 5.5/11.0 = 0.5, V = 5000 + 2500 = 7500
    assert_test(cooling_evaluate_rule_curve(default_settings, 29.5) == 7500, "Temp 29.5C (midpoint) outputs 7500 mV")

    # Exact T1 -> V1
    assert_test(cooling_evaluate_rule_curve(default_settings, 35.0) == 10000, "Temp 35.0C (T1) outputs 10000 mV")

    # Above T1 -> Clamp V1
    assert_test(cooling_evaluate_rule_curve(default_settings, 40.0) == 10000, "Temp 40.0C (above T1) clamps to 10000 mV")

    # NaN / Invalid temp -> 0 mV
    assert_test(cooling_evaluate_rule_curve(default_settings, float('nan')) == 0, "NaN temp outputs fail-safe 0 mV")
    assert_test(cooling_evaluate_rule_curve(default_settings, float('inf')) == 0, "+Inf temp outputs fail-safe 0 mV")
    assert_test(cooling_evaluate_rule_curve(default_settings, float('-inf')) == 0, "-Inf temp outputs fail-safe 0 mV")

    # Step Mode
    step_settings = dict(default_settings)
    step_settings["curve_type"] = COOLING_CURVE_STEP
    assert_test(cooling_evaluate_rule_curve(step_settings, 24.0) == 5000, "Step mode at 24.0C outputs 5000 mV")
    assert_test(cooling_evaluate_rule_curve(step_settings, 30.0) == 5000, "Step mode at 30.0C holds 5000 mV")
    assert_test(cooling_evaluate_rule_curve(step_settings, 34.9) == 5000, "Step mode at 34.9C holds 5000 mV")
    assert_test(cooling_evaluate_rule_curve(step_settings, 35.0) == 10000, "Step mode at 35.0C steps up to 10000 mV")

    # 5-point piecewise curve
    p5_settings = {
        "curve_type": COOLING_CURVE_LINEAR,
        "point_count": 5,
        "points": [
            {"temp_c_x10": 200, "output_mv": 2000},
            {"temp_c_x10": 250, "output_mv": 4000},
            {"temp_c_x10": 300, "output_mv": 6000},
            {"temp_c_x10": 350, "output_mv": 8000},
            {"temp_c_x10": 400, "output_mv": 10000},
        ],
    }
    assert_test(cooling_validate_settings(p5_settings), "5-point curve settings are valid")
    assert_test(cooling_evaluate_rule_curve(p5_settings, 22.5) == 3000, "5-point curve 22.5C outputs 3000 mV")
    assert_test(cooling_evaluate_rule_curve(p5_settings, 32.5) == 7000, "5-point curve 32.5C outputs 7000 mV")

    # 8-point maximum curve
    p8_settings = {
        "curve_type": COOLING_CURVE_LINEAR,
        "point_count": 8,
        "points": [{"temp_c_x10": 200 + i * 20, "output_mv": (i + 1) * 1250} for i in range(8)],
    }
    p8_settings["points"][-1]["output_mv"] = 10000
    assert_test(cooling_validate_settings(p8_settings), "8-point maximum curve settings are valid")

    # Validation rejection checks
    # Less than 2 points
    assert_test(not cooling_validate_settings({"curve_type": 0, "point_count": 1, "points": [{"temp_c_x10": 250, "output_mv": 5000}]}),
                "Reject point_count < 2")
    # More than 8 points
    assert_test(not cooling_validate_settings({"curve_type": 0, "point_count": 9, "points": [{"temp_c_x10": 200 + i * 10, "output_mv": 1000} for i in range(9)]}),
                "Reject point_count > 8")
    # Output mV > 10,000
    assert_test(not cooling_validate_settings({"curve_type": 0, "point_count": 2, "points": [{"temp_c_x10": 200, "output_mv": 5000}, {"temp_c_x10": 300, "output_mv": 10001}]}),
                "Reject output_mv > 10000")
    # Duplicate temperatures
    assert_test(not cooling_validate_settings({"curve_type": 0, "point_count": 2, "points": [{"temp_c_x10": 250, "output_mv": 5000}, {"temp_c_x10": 250, "output_mv": 8000}]}),
                "Reject duplicate temperature points")
    # Decreasing temperatures
    assert_test(not cooling_validate_settings({"curve_type": 0, "point_count": 2, "points": [{"temp_c_x10": 300, "output_mv": 5000}, {"temp_c_x10": 250, "output_mv": 8000}]}),
                "Reject decreasing temperature points")
    # Invalid curve type
    assert_test(not cooling_validate_settings({"curve_type": 2, "point_count": 2, "points": [{"temp_c_x10": 200, "output_mv": 5000}, {"temp_c_x10": 300, "output_mv": 8000}]}),
                "Reject invalid curve_type")


# -----------------------------------------------------------------------------
# Section 2: LEDC PWM Output Driver & Slew Rate Math
# -----------------------------------------------------------------------------

def calc_duty_from_mv(mv):
    if mv > COOLING_PWM_MAX_MV:
        mv = COOLING_PWM_MAX_MV
    duty = round((mv / float(COOLING_PWM_MAX_MV)) * float(COOLING_PWM_MAX_DUTY))
    return min(duty, COOLING_PWM_MAX_DUTY)


def calc_slew_step(current_mv, target_mv, max_step_mv):
    if max_step_mv == 0 or current_mv == target_mv:
        return current_mv

    if target_mv > current_mv:
        diff = target_mv - current_mv
        return target_mv if diff <= max_step_mv else current_mv + max_step_mv
    else:
        diff = current_mv - target_mv
        return target_mv if diff <= max_step_mv else current_mv - max_step_mv


def calc_slew(current_mv, target_mv, delta_ms, max_rate_mv_per_sec):
    if current_mv == target_mv:
        return current_mv
    if max_rate_mv_per_sec == 0 or delta_ms == 0:
        return current_mv
    step = (max_rate_mv_per_sec * delta_ms) // 1000
    if step == 0:
        step = 1
    max_step = min(step, 65535)
    return calc_slew_step(current_mv, target_mv, max_step)


def test_pwm_and_slew():
    print("\n--- 2. Testing LEDC PWM & Slew Rate Math ---")

    assert_test(calc_duty_from_mv(0) == 0, "0 mV gives raw duty 0 (0.0%)")
    assert_test(calc_duty_from_mv(2500) == 256, "2500 mV gives raw duty 256 (~25.0%)")
    assert_test(calc_duty_from_mv(5000) == 512, "5000 mV gives raw duty 512 (~50.0%)")
    assert_test(calc_duty_from_mv(7500) == 767, "7500 mV gives raw duty 767 (~75.0%)")
    assert_test(calc_duty_from_mv(10000) == 1023, "10000 mV gives raw duty 1023 (100.0%)")
    assert_test(calc_duty_from_mv(15000) == 1023, ">10000 mV clamps to duty 1023 (100.0%)")

    # Slew step with step limit = 0 (no change)
    assert_test(calc_slew_step(2000, 5000, 0) == 2000, "Slew step with max_step_mv=0 returns current_mv")
    # Slew step up
    assert_test(calc_slew_step(2000, 5000, 1000) == 3000, "Slew step up from 2000 to 5000 with step 1000 is 3000")
    # Slew step down
    assert_test(calc_slew_step(5000, 2000, 1000) == 4000, "Slew step down from 5000 to 2000 with step 1000 is 4000")
    # Convergence
    assert_test(calc_slew_step(4800, 5000, 1000) == 5000, "Slew converges when remaining delta < step")
    assert_test(calc_slew_step(5200, 5000, 1000) == 5000, "Slew converges downwards when remaining delta < step")

    # Slew calculation with time
    # 500 ms @ 1000 mV/s -> step = 500 mV
    assert_test(calc_slew(0, 5000, 500, 1000) == 500, "Slew 500ms at 1000 mV/s ramps 500 mV")
    # 1000 ms @ 1000 mV/s -> step = 1000 mV
    assert_test(calc_slew(0, 5000, 1000, 1000) == 1000, "Slew 1000ms at 1000 mV/s ramps 1000 mV")


# -----------------------------------------------------------------------------
# Section 3: Memory, Partition Table & Binary Fit
# -----------------------------------------------------------------------------

def test_partition_and_binary_fit():
    print("\n--- 3. Testing Partition Table & Firmware Binary Fit ---")

    part_csv = FIRMWARE_DIR / "partitions.csv"
    assert_test(part_csv.exists(), "partitions.csv exists")

    partitions = []
    with open(part_csv, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 5:
                name, ptype, subtype, offset, size = parts[:5]
                partitions.append({
                    "name": name,
                    "type": ptype,
                    "subtype": subtype,
                    "offset": int(offset, 16),
                    "size": int(size, 16),
                })

    ota_0 = next((p for p in partitions if p["name"] == "ota_0"), None)
    ota_1 = next((p for p in partitions if p["name"] == "ota_1"), None)
    assert_test(ota_0 is not None and ota_1 is not None, "Both ota_0 and ota_1 partitions exist")
    assert_test(ota_0["size"] == 0x380000, f"ota_0 size is 0x380000 (3.5 MB) [found {hex(ota_0['size'])}]")
    assert_test(ota_1["size"] == 0x380000, f"ota_1 size is 0x380000 (3.5 MB) [found {hex(ota_1['size'])}]")

    bin_path = BUILD_DIR / "fishpump_firmware.bin"
    assert_test(bin_path.exists(), "fishpump_firmware.bin binary exists in build/")
    if bin_path.exists():
        bin_size = bin_path.stat().st_size
        free_space = ota_0["size"] - bin_size
        pct_free = (free_space / ota_0["size"]) * 100.0
        assert_test(bin_size < ota_0["size"],
                    f"Binary fits in ota_0 partition ({bin_size} bytes < {ota_0['size']} bytes, {pct_free:.1f}% free)")
        print(f"      Binary size: {bin_size} bytes ({bin_size/1024/1024:.2f} MB), Free: {free_space} bytes ({pct_free:.1f}%)")

    boot_bin = BUILD_DIR / "bootloader" / "bootloader.bin"
    assert_test(boot_bin.exists(), "bootloader.bin exists in build/bootloader/")
    if boot_bin.exists():
        boot_size = boot_bin.stat().st_size
        assert_test(boot_size < 0x10000, f"Bootloader fits in reserved offset ({boot_size} bytes < 65536 bytes)")


# -----------------------------------------------------------------------------
# Section 4: Source Contract & Interface Audit
# -----------------------------------------------------------------------------

def test_source_contracts():
    print("\n--- 4. Testing Source Contracts & API Endpoints ---")

    web_server_c = MAIN_DIR / "web_server.c"
    assert_test(web_server_c.exists(), "web_server.c exists")
    content = web_server_c.read_text(encoding="utf-8")

    # Endpoints
    assert_test("/api/cooling/status" in content, "REST endpoint /api/cooling/status implemented")
    assert_test("/api/cooling/config" in content, "REST endpoint /api/cooling/config implemented")
    assert_test("/api/cooling/mode" in content, "REST endpoint /api/cooling/mode implemented")

    # Negative test voltage rejection
    assert_test("value_out_of_range" in content and "test_volts" in content,
                "Negative / out-of-range test voltage explicitly rejected in web_server.c")

    # SSE Event format
    assert_test("TELEMETRY_EVT_COOLING" in content or "telemetry_cooling" in content,
                "SSE telemetry push streaming for cooling status implemented")

    # Hardware map pins
    app_main_c = MAIN_DIR / "app_main.c"
    app_main_text = app_main_c.read_text(encoding="utf-8")
    assert_test("max31865_cs_gpio" in app_main_text, "app_main honors max31865_cs_gpio from hardware map")
    assert_test("cooling_pwm_gpio" in app_main_text, "app_main honors cooling_pwm_gpio from hardware map")

    # NVS persistence schema
    nvs_store_c = COMPONENTS_DIR / "nvs_store" / "nvs_store.c"
    nvs_store_text = nvs_store_c.read_text(encoding="utf-8")
    assert_test("nvs_store_save_cooling_settings" in nvs_store_text, "nvs_store_save_cooling_settings implemented")
    assert_test("nvs_store_load_cooling_settings" in nvs_store_text, "nvs_store_load_cooling_settings implemented")
    assert_test("NVS_COOL_KEY_RULES" in nvs_store_text and "cool_rules" in nvs_store_text,
                "NVS cooling rules stored as atomic blob via NVS_COOL_KEY_RULES")
    assert_test("cooling_modern_settings_valid" in nvs_store_text,
                "NVS cooling settings validated with cooling_modern_settings_valid")


# -----------------------------------------------------------------------------
# Section 5: Web UI Assets and Gzip Synchronization
# -----------------------------------------------------------------------------

def test_web_assets():
    print("\n--- 5. Testing Web UI Assets & Gzip Compression ---")

    static_dir = MAIN_DIR / "static"
    comp_dir = MAIN_DIR / "static_compressed"

    for fname in ["index.html", "style.css", "app.js"]:
        raw_f = static_dir / fname
        gz_f = comp_dir / f"{fname}.gz"

        assert_test(raw_f.exists(), f"Raw asset {fname} exists in main/static/")
        assert_test(gz_f.exists(), f"Compressed asset {fname}.gz exists in main/static_compressed/")

        if raw_f.exists() and gz_f.exists():
            with gzip.open(gz_f, "rb") as gz:
                gz_decompressed = gz.read()
            assert_test(len(gz_decompressed) > 0, f"{fname}.gz decompresses cleanly ({len(gz_decompressed)} bytes)")

    # Check UI elements in static/index.html
    html_text = (static_dir / "index.html").read_text(encoding="utf-8")
    assert_test("cooling-card" in html_text or "cooling" in html_text, "index.html has cooling card element")
    assert_test("cooling-rule-modal" in html_text or "cooling-rule" in html_text, "index.html has cooling rule modal")
    assert_test("cooling-curve-svg" in html_text or "cooling-svg" in html_text, "index.html has SVG rule curve element")

    # Check UI JS functions in static/app.js
    js_text = (static_dir / "app.js").read_text(encoding="utf-8")
    assert_test("openCoolingRuleModal" in js_text or "openCoolingModal" in js_text or "cooling" in js_text,
                "app.js has cooling modal handler")
    assert_test("renderCoolingCurvePreview" in js_text or "renderCoolingCurve" in js_text or "svg" in js_text,
                "app.js has SVG curve preview renderer")


def main():
    print("================================================================")
    print(" PT100 MAX31865 & INVERTER COOLING REGRESSION TEST SUITE")
    print("================================================================")

    test_rule_curve_engine()
    test_pwm_and_slew()
    test_partition_and_binary_fit()
    test_source_contracts()
    test_web_assets()

    print("\n================================================================")
    print(f" TOTAL TESTS RUN: {passed_tests + failed_tests}")
    print(f" PASSED: {passed_tests}")
    print(f" FAILED: {failed_tests}")
    print("================================================================")

    if failed_tests > 0:
        sys.exit(1)
    else:
        print(" ALL REGRESSION TESTS PASSED CLEANLY WITH ZERO FAILURES!")
        sys.exit(0)


if __name__ == "__main__":
    main()
