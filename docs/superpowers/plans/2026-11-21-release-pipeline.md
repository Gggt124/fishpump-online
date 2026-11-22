# Release pipeline, คีย์ OTA และแพ็กเกจติดตั้ง — แผนลงมือ

> **For agentic workers:** REQUIRED SUB-SKILL: ใช้ superpowers:subagent-driven-development (แนะนำ) หรือ superpowers:executing-plans ทำทีละ task ขั้นตอนใช้ checkbox (`- [ ]`) ติดตามงาน

**Goal:** ทำให้ firmware รุ่น release build ซ้ำได้บน CI, คีย์ OTA ตรวจสอบได้ด้วยลายนิ้วมือและไม่ตกไปเป็นคีย์ศูนย์เงียบๆ, และติดตั้ง/อัปเดตบอร์ดได้ง่ายเหมือนโปรเจกต์เก่า

**Architecture:** ตรรกะบริสุทธิ์ฝั่งบอร์ดอยู่ในคอมโพเนนต์เล็ก `ota_key` (ทดสอบใน QEMU selftest เดิม) ฝั่งเครื่องมืออยู่ใน Python ใต้ `firmware/scripts/` (ทดสอบด้วย `unittest`) ใช้ทั้งบนเครื่องผู้ใช้และใน CI การห้าม build release โดยไม่มีคีย์ทำใน CMake script แยกที่ทดสอบได้ด้วย `cmake -P` CI (GitHub Actions) เรียกสคริปต์เหล่านี้ ไม่มีตรรกะซ้ำใน YAML

**Tech Stack:** ESP-IDF 6.0.1 (ESP32-S3), C, CMake, Python 3.13 + `cryptography`, `esptool`, GitHub Actions, PowerShell/batch สำหรับชุดลูกค้า

**Spec:** `docs/superpowers/specs/2026-11-21-release-pipeline-design.md`

## Global Constraints

- web OTA: ไฟล์ = IV 16 ไบต์ + AES-256-CTR ใช้คีย์ 64 ตัวฐานสิบหก (`APP_CONFIG_OTA_ENCRYPTION_KEY`) ไม่เปลี่ยนรูปแบบ
- ลายนิ้วมือคีย์ = 8 ตัวอักษรฐานสิบหกตัวเล็กแรกของ SHA-256 ของคีย์ดิบ 32 ไบต์ (คีย์ศูนย์ → `66687aad`)
- ขีดจำกัดอัปโหลด web OTA = ขนาด partition (0x380000) + 16 ไบต์ IV
- ห้ามพิมพ์ตัวคีย์ในหน้าจอ/log/เอกสาร/commit และห้ามส่งคีย์เป็นอาร์กิวเมนต์บรรทัดคำสั่ง (ใช้ตัวแปรสภาพแวดล้อมหรือไฟล์)
- ห้ามเขียนรหัส AP จริงของบอร์ดใดลงเอกสาร อธิบายวิธีคำนวณเท่านั้น
- เป้าเดียว: esp32s3 (ESP-IDF v6.0.1, ตามที่ล็อกใน `firmware/dependencies.lock`)
- ห้ามรัน `git stash`, `git reset`, `git checkout --`; ห้าม stage `firmware/.vscode/settings.json`; push เมื่อผู้ใช้สั่งเท่านั้น
- Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`
- ข้อความสำหรับผู้ใช้ (README ลูกค้า, docs/release) เป็นภาษาไทย (README ลูกค้ามีฉบับอังกฤษด้วย); identifier และ log เป็นอังกฤษ

## Review Focus

1. คีย์จาก GitHub secret มีช่องว่าง/ขึ้นบรรทัดใหม่ท้าย หรือเป็นตัวพิมพ์ใหญ่ → ต้องรับได้และได้ลายนิ้วมือเดียวกัน (Task 3)
2. คีย์ยาว 63/65 ตัวหรือมีตัวอักษรที่ไม่ใช่ฐานสิบหก → ปฏิเสธพร้อมข้อความชัดเจน ไม่ใช่ traceback (Task 3)
3. ไฟล์ firmware ใหญ่เกิน partition → เครื่องมือเข้ารหัสต้องปฏิเสธ จะได้ไม่ผลิตไฟล์ที่บอร์ดตีกลับ; ไฟล์ว่างต้องไม่พัง (Task 3)
4. `secrets.h` ที่ผู้ใช้สร้างบน Windows มี BOM/CRLF ต้องไม่ทำให้การตรวจคีย์ผิดพลาด (Task 4)
5. tag ไม่ตรงกับ `firmware/version.txt` → release ต้องล้มก่อน build (Task 7)

---

### Task 1: ล็อกอินพุตของ build และกัน secrets.h

**Files:**
- Create: `firmware/IDF_VERSION`, `firmware/scripts/verify_release_pipeline.py`
- Modify: `.gitignore` (บรรทัด `firmware/dependencies.lock` ที่ 46), `firmware/main/idf_component.yml`
- Add to git: `firmware/dependencies.lock`

**Interfaces:**
- Produces: `verify_release_pipeline.py` — สคริปต์ source-contract รูปแบบเดียวกับ `verify_tb_link.py` (ฟังก์ชัน `assert_test(cond, name, details="")`, exit code 1 เมื่อมี FAIL) Task 4, 5, 6, 7 เพิ่มการตรวจต่อท้ายไฟล์เดียวกัน

- [ ] **Step 1:** เขียน `verify_release_pipeline.py` ให้ตรวจ: (a) `.gitignore` มีรูปแบบที่ครอบ `firmware/components/app_config/secrets.h` และ **ไม่มี** `firmware/dependencies.lock`; (b) `firmware/IDF_VERSION` เป็น `6.0.1` และตรงกับเวอร์ชัน `idf` ใน `dependencies.lock`; (c) `firmware/main/idf_component.yml` ไม่มี `'*'`
- [ ] **Step 2:** รัน `python firmware/scripts/verify_release_pipeline.py` — คาดว่า FAIL ทั้งสามกลุ่ม
- [ ] **Step 3:** แก้: เพิ่ม `firmware/components/app_config/secrets.h` และ `**/secrets.h` ใน `.gitignore`; ลบบรรทัด `firmware/dependencies.lock`; สร้าง `firmware/IDF_VERSION` (`6.0.1` บรรทัดเดียว); เปลี่ยน `espressif/led_strip: '*'` เป็น `'^3.1.1'` และ `esp-idf-lib/max31865: '*'` เป็น `'^1.0.8'` (เวอร์ชันที่ล็อกไว้แล้ว); `git add firmware/dependencies.lock`
- [ ] **Step 4:** รัน verify ซ้ำ ต้อง PASS แล้วรัน `idf.py -C firmware build` ต้องผ่านโดยไม่ดึงเวอร์ชันคอมโพเนนต์ใหม่ (เทียบ `git diff firmware/dependencies.lock` ต้องไม่เปลี่ยน)
- [ ] **Step 5:** Commit `build: pin IDF and component versions, ignore secrets.h`

### Task 2: คอมโพเนนต์ `ota_key` (ตรรกะบริสุทธิ์) + selftest

**Files:**
- Create: `firmware/components/ota_key/CMakeLists.txt`, `firmware/components/ota_key/include/ota_key.h`, `firmware/components/ota_key/ota_key.c`, `firmware/test/sensor_selftest/main/test_ota_key.c`
- Modify: `firmware/test/sensor_selftest/main/selftest_main.c` (เรียก `test_ota_key()` ต่อจาก `test_uplink_format()` ที่บรรทัด 18), `firmware/test/sensor_selftest/main/selftest.h` (ประกาศ), `firmware/test/sensor_selftest/main/CMakeLists.txt` (เพิ่มไฟล์และ `ota_key` ใน REQUIRES)

**Interfaces:**
- Produces (`ota_key.h`):
  - `#define OTA_KEY_HEX_LEN 64` / `#define OTA_KEY_FP_LEN 8`
  - `bool ota_key_parse_hex(const char *hex, uint8_t out[32])` — false ถ้ายาวไม่ใช่ 64 หรือมีอักขระนอกฐานสิบหก (รับตัวพิมพ์ใหญ่/เล็ก)
  - `bool ota_key_is_dev(const char *hex)` — true ถ้าคีย์เป็นศูนย์ทั้งหมดหรือ parse ไม่ได้
  - `bool ota_key_fingerprint(const char *hex, char out[OTA_KEY_FP_LEN + 1])` — ตัวเล็ก; false ถ้า parse ไม่ได้
  - `size_t ota_upload_limit(size_t partition_size)` — `partition_size + 16`

- [ ] **Step 1:** เขียน `test_ota_key.c` ตามรูปแบบ `test_uplink_format.c`: คีย์ศูนย์ → `is_dev` true และ fingerprint `"66687aad"`; คีย์ NIST `603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4` → `is_dev` false และ fingerprint ตัวพิมพ์ใหญ่/เล็กของคีย์เดียวกันได้ค่าเท่ากัน; 63 ตัว, 65 ตัว, `"zz…"` 64 ตัว → `parse_hex` false และ `is_dev` true; `ota_upload_limit(0x380000) == 0x380010`
- [ ] **Step 2:** `run_selftest.ps1` — คาดว่า build ล้ม (ไม่มีคอมโพเนนต์)
- [ ] **Step 3:** เขียนคอมโพเนนต์ ใช้ PSA SHA-256 แบบเดียวกับ `firmware/main/tb_ota.c:147-194` (`psa_crypto_init`, `psa_hash_compute`); `REQUIRES mbedtls`
- [ ] **Step 4:** `pwsh firmware/test/sensor_selftest/run_selftest.ps1` — ต้องจบด้วย `SELFTEST PASS` และจำนวนตรวจเพิ่มจาก 2089
- [ ] **Step 5:** Commit `feat(ota_key): key fingerprint, dev-key detection and upload limit`

### Task 3: เครื่องมือเข้ารหัสและสร้าง secrets (Python)

**Files:**
- Create: `firmware/scripts/ota_crypto.py`, `firmware/scripts/encrypt_ota.py`, `firmware/scripts/write_secrets.py`, `firmware/scripts/generate_secrets.ps1`, `firmware/scripts/test_ota_tools.py`
- อ้างอิงต้นฉบับ: `C:\Users\Copter\OneDrive\Desktop\fish-pump-RelayTimerControl\scripts\encrypt_ota.py` และ `generate_secrets.ps1` (อ่านแล้วปรับ ไม่คัดลอกตรงๆ)

**Interfaces:**
- Produces (`ota_crypto.py`):
  - `OTA_MAX_IMAGE = 0x380000`
  - `normalize_key(text: str) -> str` — strip ช่องว่าง/ขึ้นบรรทัด, ตัวพิมพ์เล็ก; ยก `ValueError("key must be 64 hex characters")` ถ้าไม่ถูกต้อง
  - `read_key(env: str | None, file: str | None) -> str`
  - `key_fingerprint(key_hex: str) -> str` (8 ตัว)
  - `encrypt_bytes(data: bytes, key_hex: str, iv: bytes | None = None) -> bytes` — `iv(16) + AES-256-CTR(data)`; iv สุ่มถ้าไม่ระบุ; ยก `ValueError` ถ้า `len(data) > OTA_MAX_IMAGE`
- CLI: `encrypt_ota.py IN OUT (--key-env NAME | --key-file PATH)`; `write_secrets.py (--from-env NAME | --generate) [--force]` เขียน `firmware/components/app_config/secrets.h` (UTF-8 ไม่มี BOM, LF) พิมพ์เฉพาะ path และลายนิ้วมือ ไม่พิมพ์ตัวคีย์ ปฏิเสธเขียนทับถ้าไม่ใส่ `--force` และปฏิเสธคีย์ศูนย์; `generate_secrets.ps1` เรียก `python write_secrets.py --generate` เท่านั้น

- [ ] **Step 1:** เขียน `test_ota_tools.py` (`unittest`): เวกเตอร์ NIST — คีย์ `603deb10…dff4`, IV `f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff`, plaintext `6bc1bee22e409f96e93d7e117393172a` → ส่วน ciphertext `601ec313775789a5b7a7f504bbf3d228`; คีย์ศูนย์ → fingerprint `66687aad`; คีย์ตัวพิมพ์ใหญ่ + ขึ้นบรรทัดใหม่ท้าย → fingerprint เท่ากับตัวพิมพ์เล็ก (Review Focus 1); คีย์ 63/65/อักขระผิด → `ValueError` ข้อความตามที่ระบุ และ CLI ออก exit 1 โดยไม่มี traceback (Review Focus 2); ข้อมูลว่างเข้ารหัสได้ได้เฉพาะ IV 16 ไบต์; ข้อมูลยาว `OTA_MAX_IMAGE + 1` ถูกปฏิเสธ (Review Focus 3); round-trip ถอดรหัสด้วย `cryptography` ได้ต้นฉบับ; `write_secrets.py --from-env` ใน temp dir ได้ไฟล์ที่มีคีย์ตรงและ **stdout ไม่มีตัวคีย์**; เขียนทับโดยไม่ `--force` ล้ม; คีย์ศูนย์ล้ม
- [ ] **Step 2:** `python -m unittest firmware/scripts/test_ota_tools.py -v` — คาดว่า FAIL (ไม่มีโมดูล)
- [ ] **Step 3:** เขียนโมดูลและ CLI ตาม Interfaces ใช้ `cryptography` (`Cipher(AES, CTR)`)
- [ ] **Step 4:** รัน unittest ซ้ำ ต้อง PASS ทั้งหมด
- [ ] **Step 5:** Commit `feat(scripts): OTA encrypt tool and secrets writer`

### Task 4: กติกา release ล้มเมื่อไม่มีคีย์ (CMake)

**Files:**
- Create: `firmware/cmake/check_release_key.cmake`
- Modify: `firmware/CMakeLists.txt` (เพิ่ม `option(FISHPUMP_RELEASE ...)` ก่อน `include(project.cmake)` และ include สคริปต์ตรวจเมื่อเปิดโหมด), `firmware/scripts/test_ota_tools.py` (เพิ่ม test case), `firmware/scripts/verify_release_pipeline.py`

**Interfaces:**
- Consumes: รูปแบบบรรทัดในไฟล์ที่ `write_secrets.py` เขียน: `#define APP_CONFIG_OTA_ENCRYPTION_KEY "<64 hex>"`
- Produces: `cmake -DSECRETS_FILE=<path> -P firmware/cmake/check_release_key.cmake` — exit 0 เมื่อไฟล์มีอยู่และมีคีย์ 64 ตัวฐานสิบหกที่ไม่ใช่ศูนย์ทั้งหมดและไม่ใช่ค่า `REPLACE_WITH…` ของ template; ไม่งั้น `FATAL_ERROR` ข้อความบอกวิธีแก้และไม่แสดงตัวคีย์

- [ ] **Step 1:** เพิ่ม test (subprocess เรียก `cmake -P`): ไม่มีไฟล์ → exit ≠ 0; คีย์ศูนย์ → ≠ 0; ค่า `REPLACE_WITH_…` → ≠ 0; คีย์ 63 ตัว → ≠ 0; คีย์ดีแบบ LF → 0; คีย์ดีไฟล์ที่ขึ้นต้น BOM + CRLF → 0 (Review Focus 4); stderr ของทุกกรณีไม่มีตัวคีย์
- [ ] **Step 2:** รัน test ต้อง FAIL (ไม่มีสคริปต์)
- [ ] **Step 3:** เขียน `check_release_key.cmake` (`file(READ)` + `string(REGEX MATCH)`) แล้วใน `firmware/CMakeLists.txt` เรียกเมื่อ `FISHPUMP_RELEASE` เปิด โดยใช้ `components/app_config/secrets.h`; เพิ่มใน verify ตรวจว่า CMakeLists มี option และ include นี้
- [ ] **Step 4:** รัน test ต้อง PASS; จากนั้น `idf.py -C firmware -DFISHPUMP_RELEASE=ON reconfigure` ในสภาพไม่มี `secrets.h` ต้องล้มด้วยข้อความที่กำหนด และ build ปกติ (ไม่ตั้ง flag) ยังผ่านด้วยคีย์ศูนย์
- [ ] **Step 5:** Commit `feat(build): release builds fail without a real OTA key`

### Task 5: ใช้ลายนิ้วมือและขีดจำกัดใหม่ในเฟิร์มแวร์

**Files:**
- Modify: `firmware/main/web_server.c` (ขีดจำกัดที่บรรทัด ~2130 และ handler ของ `/api/status` ที่บรรทัด ~2029), `firmware/main/tb_state.c` (ใกล้บรรทัด 150), ไฟล์ start-up ที่พิมพ์เวอร์ชันตอนบูต (หาด้วย grep `APP_TEMPLATE_FIRMWARE_VERSION` ใน `firmware/main/`), `firmware/main/CMakeLists.txt` (REQUIRES `ota_key`), `docs/firmware/device-api.md`, `docs/firmware/reference.md`, `firmware/scripts/verify_release_pipeline.py`

**Interfaces:**
- Consumes: ฟังก์ชันทั้งหมดของ `ota_key.h` (Task 2)
- Produces: (1) web OTA จำกัดที่ `ota_upload_limit(next_partition->size)` โดย `next_partition = esp_ota_get_next_update_partition(NULL)` แทน `0x1E0000`; ถ้าหา partition ไม่ได้ตอบ 500 `no_ota_partition`; (2) `/api/status` มีฟิลด์ `"ota_key": {"fp": "<8 hex>", "dev": <bool>}`; (3) attribute ThingsBoard `ota_key_fp` และ `ota_key_dev` ในออบเจ็กต์ `tb_link` ข้าง `fw_version`; (4) บรรทัดบูต `OTA key fp=<8 hex> (dev|release)`

- [ ] **Step 1:** เพิ่มใน verify: `web_server.c` ไม่มีค่าคงที่ `0x1E0000` อีก และเรียก `ota_upload_limit`; `tb_state.c` มี `ota_key_fp`; มี `ota_key` ใน REQUIRES; `docs/firmware/device-api.md` อธิบายฟิลด์ใหม่ รัน → FAIL
- [ ] **Step 2:** แก้ตาม Produces (1)-(4) และอัปเดตเอกสารสองไฟล์ (ระบุว่า web OTA รับไฟล์เข้ารหัสที่ขนาดสูงสุด partition+16 และ TB OTA รับไฟล์ธรรมดา)
- [ ] **Step 3:** รัน verify ทั้งหมด `for f in firmware/scripts/verify_*.py; do python $f; done` ต้อง PASS และ `idf.py -C firmware build` ผ่าน; ตรวจขนาด image เทียบ partition (`idf.py size`) และบันทึกค่าไว้ใน commit message
- [ ] **Step 4:** Commit `fix(ota): web upload limit follows the partition; report key fingerprint`

### Task 6: แพ็กเกจติดตั้งสำหรับลูกค้า

**Files:**
- Create: `firmware/scripts/package_release.py`, `firmware/scripts/customer/FLASH.bat`, `ERASE_FLASH.bat`, `SHOW_PASSWORD.bat`, `README_customer.txt`, `README_ลูกค้า.txt`, `firmware/scripts/customer/tools/flash_and_show.ps1`, `erase_flash.ps1`, `show_ap_password.ps1`; Modify: `firmware/scripts/test_ota_tools.py`, `firmware/scripts/verify_release_pipeline.py`
- ต้นฉบับ: `C:\Users\Copter\OneDrive\Desktop\fish-pump-RelayTimerControl\flash-package\` และ `scripts\` (อ่านแล้วปรับ: เหลือ esp32s3 ตัวเดียว ลบตัวเลือกเลือกบอร์ด ไฟล์ฝัง `COM5`)

**Interfaces:**
- Consumes: `ota_crypto.encrypt_bytes`, `key_fingerprint`, `read_key` (Task 3); `firmware/build/flasher_args.json` ของ ESP-IDF
- Produces: `package_release.py --build-dir B --version V --out-dir O --key-env NAME` สร้างใน `O/`: `fishpump-V.bin` (ธรรมดา), `fishpump-V-<fp>.ota.bin` (เข้ารหัส), `fishpump-V-factory.bin` (รวมทุกไฟล์ตาม `flasher_args.json` ด้วย `python -m esptool --chip esp32s3 merge-bin`), `flash-package.zip` (`FLASH.bat`, `ERASE_FLASH.bat`, `SHOW_PASSWORD.bat`, README สองภาษา, `firmware/factory.bin`, `tools/*.ps1`, `tools/esptool.exe`), `SHA256SUMS`; ฟังก์ชัน `get_ap_password(mac: bytes) -> str` สำหรับเทสต์ และ `show_ap_password.ps1 -SelfTest` ตรวจเวกเตอร์เดียวกัน
- เวกเตอร์รหัส AP: MAC `00:11:22:33:44:55` → ไบต์ 2-5 XOR `0x5A` → `78691E0F` (MAC สมมติ ไม่ใช่ของบอร์ดจริง)

- [ ] **Step 1:** เขียน test: สร้าง build dir ปลอม (`flasher_args.json` + ไบนารีสั้นๆ) แล้วรัน package; ต้องได้ไฟล์ครบตาม Produces, `SHA256SUMS` ตรงกับไฟล์, `.ota.bin` ถอดรหัสได้ต้นฉบับ, ชื่อ `.ota.bin` มี fingerprint, zip มี `FLASH.bat` ที่อ้าง `firmware\factory.bin` ที่ offset `0x0`, และไม่มีตัวคีย์ในไฟล์ใดๆ; `get_ap_password(bytes.fromhex("001122334455")) == "78691E0F"`; verify เพิ่มตรวจว่า `wifi_manager.c` ยังใช้ salt `0x5A` กับ `mac[2..5]` (ถ้าแก้สูตรใน firmware ต้องแก้ทั้งสองที่)
- [ ] **Step 2:** รัน test → FAIL
- [ ] **Step 3:** เขียน `package_release.py` และพอร์ตไฟล์ลูกค้าตามต้นฉบับ ดาวน์โหลด `esptool.exe` จากรุ่นที่ตรึง (ต้นฉบับใช้ v5.3.1) พร้อมตรวจ SHA-256 ที่บันทึกไว้ใน `package_release.py`; ถ้าดาวน์โหลดไม่ได้ ให้ข้ามพร้อมคำเตือนเมื่อใส่ `--no-esptool-exe` (สำหรับเทสต์)
- [ ] **Step 4:** รัน test → PASS; รัน `powershell -File firmware/scripts/customer/tools/show_ap_password.ps1 -SelfTest` ได้ `OK`; แพ็กเกจจริงจาก `firmware/build` และให้ผู้ใช้ตรวจไฟล์ใน zip ด้วยตา
- [ ] **Step 5:** Commit `feat(scripts): release packaging and customer flash tools`

### Task 7: GitHub Actions (ci.yml, release.yml)

**Files:**
- Create: `.github/workflows/ci.yml`, `.github/workflows/release.yml`; Modify: `firmware/scripts/verify_release_pipeline.py`

**Interfaces:**
- Consumes: ทุกสคริปต์จาก Task 1-6; secret `OTA_ENCRYPTION_KEY` (ผู้ใช้ตั้งเอง)

- [ ] **Step 1:** เพิ่มใน verify (ใช้ `yaml.safe_load`): `ci.yml` ทำงานบน `pull_request` และ push ไป `main`, ใช้ image `espressif/idf:v<IDF_VERSION>`, รัน `verify_*.py` และ unittest ของสคริปต์, build dev; `release.yml` ทำงานบน tag `v*.*.*` เท่านั้น มี `permissions: contents: write`, step ตรวจ tag เทียบ `firmware/version.txt` ก่อน build (Review Focus 5), เรียก `write_secrets.py --from-env`, build ด้วย `-DFISHPUMP_RELEASE=ON`, เรียก `package_release.py`, สร้าง Release จากไฟล์ใน `out-dir`; ไม่มี `echo`/`set -x`/`printenv` ที่ใกล้ตัวแปร `OTA_ENCRYPTION_KEY`; รัน → FAIL
- [ ] **Step 2:** เขียน workflow ตามข้อ 1; ตรวจว่า image `espressif/idf:v6.0.1` มีจริงและมี `qemu-xtensa` สำหรับ selftest (ถ้าไม่มี ติดตั้งผ่าน `idf_tools.py install qemu-xtensa` ใน job; ถ้ายังรันไม่ได้ ให้ตัด job selftest ออกแล้วบันทึกเป็นช่องว่างใน `docs/release/handover.md` ใน Task 8 พร้อมเหตุผล ห้ามปล่อยเงียบ)
- [ ] **Step 3:** รัน verify → PASS; ถ้ามี `actionlint` ให้รันด้วย
- [ ] **Step 4:** Commit `ci: GitHub Actions for PR checks and tagged releases`

### Task 8: เอกสารส่งมอบและแผนทดสอบบนบอร์ด

**Files:**
- Create: `docs/release/README.md`, `keys.md`, `release.md`, `install.md`, `handover.md`, `docs/bench-test-release.md`; Modify: `docs/summary.md` (แผนผังเอกสาร), `docs/backlog/after-frontend.md` (เพิ่มงานที่แยกไว้: secure boot, TLS, ของจากโปรเจกต์เก่า, ล้าง branch/โฟลเดอร์ว่าง), `docs/bench-test-pending.md`, `firmware/scripts/verify_release_pipeline.py`

- [ ] **Step 1:** เพิ่มใน verify: มีไฟล์ docs/release ทั้งห้า, `docs/summary.md` ลิงก์ไปยัง `docs/release/README.md`, และ `docs/release` **ไม่มี** สตริงฐานสิบหกยาว 64 ตัว รัน → FAIL
- [ ] **Step 2:** เขียนเอกสารตามหัวข้อ 6.7 ของสเปก: `keys.md` ต้องมีขั้นตอนสร้าง/สำรอง (สองที่: GitHub secret และที่สำรองนอก GitHub; ย้ายออกจากโฟลเดอร์ OneDrive)/กู้/ย้ายจากคีย์ศูนย์ (6.6) และเปลี่ยนคีย์ในอนาคต; `release.md` มีขั้นตอน version.txt → PR → merge แบบ merge commit → tag; `handover.md` มีรายการสิทธิ์/secret/เวอร์ชันที่คนรับช่วงต้องได้ และช่องว่างที่ยังค้าง; `bench-test-release.md` เป็นเช็กลิสต์ให้ผู้ใช้ทำเองตามหัวข้อ 7 ของสเปก (ย้ายคีย์, OTA ทางเว็บไม่ติด 413, `FLASH.bat` กับบอร์ดเปล่า, `SHOW_PASSWORD.bat` ตรงกับ AP, ลายนิ้วมือใน `/api/status` ตรงกับ Release)
- [ ] **Step 3:** รัน verify → PASS; อ่านเอกสารทั้งชุดตามลำดับเหมือนคนรับช่วงใหม่ และแก้จุดที่ต้องเดา
- [ ] **Step 4:** Commit `docs: release, key and handover guides`

### Task 9: ตรวจทั้ง branch ก่อนส่ง PR

**Files:** ไม่มี (ตรวจอย่างเดียว) แก้ตามที่พบแล้ว commit แยก

- [ ] **Step 1:** รันทั้งหมด: `python -m unittest firmware/scripts/test_ota_tools.py`, `for f in firmware/scripts/verify_*.py; do python $f; done`, `run_selftest.ps1` (ต้อง `SELFTEST PASS`), `idf.py -C firmware build`, `idf.py -C firmware -DFISHPUMP_RELEASE=ON reconfigure` โดยไม่มีคีย์ (ต้องล้ม) แล้วมีคีย์ทดสอบชั่วคราวใน `secrets.h` (ต้องผ่าน แล้วลบไฟล์ทดสอบนั้น ห้าม commit)
- [ ] **Step 2:** `git status` ต้องไม่มี `secrets.h` หรือ `settings.json` ใน stage; `git log -p` ค้นหาสตริงฐานสิบหก 64 ตัวต้องไม่พบ
- [ ] **Step 3:** รายงานผู้ใช้: สิ่งที่ผ่าน สิ่งที่ยังไม่ได้ตรวจ (CI ยังไม่เคยรันจริง, บอร์ดยังไม่ได้แฟลช) และเช็กลิสต์ `docs/bench-test-release.md`; ถามก่อน push และก่อนเปิด PR (merge commit, เนื้อหาไทย)
