# Release, คีย์ OTA และการติดตั้ง: ภาพรวม

เอกสารชุดนี้ให้คนที่รับช่วงดูแลเฟิร์มแวร์ ออก release ติดตั้งบอร์ด และอัปเดตบอร์ดได้เอง ออกแบบไว้ใน
[`docs/superpowers/specs/2026-11-21-release-pipeline-design.md`](../superpowers/specs/2026-11-21-release-pipeline-design.md)

| อ่านเรื่อง | ไฟล์ |
|---|---|
| สร้าง สำรอง ตรวจ และเปลี่ยนคีย์ OTA | [`keys.md`](keys.md) |
| ขั้นตอนออก release (tag แล้ว CI ทำให้) | [`release.md`](release.md) |
| ติดตั้งบอร์ดใหม่ อ่านรหัส Wi-Fi อัปเดตผ่านเว็บหรือ ThingsBoard | [`install.md`](install.md) |
| สิ่งที่ผู้รับช่วงต้องได้ และช่องว่างที่ยังค้าง | [`handover.md`](handover.md) |
| เช็กลิสต์ทดสอบบนบอร์ดของส่วนนี้ (เจ้าของทำเอง) | [`../bench-test-release.md`](../bench-test-release.md) |

## อะไรอยู่ที่ไหน

| ส่วน | ที่อยู่ |
|---|---|
| เวอร์ชันเฟิร์มแวร์ (เลขล้วน เช่น `0.4.0`) | `firmware/version.txt` |
| เวอร์ชัน ESP-IDF ที่ใช้ | `firmware/IDF_VERSION` (และ tag ของ container ใน `.github/workflows/*.yml`) |
| เวอร์ชันไลบรารีที่ล็อกไว้ | `firmware/dependencies.lock` (commit ไว้ ห้าม gitignore) |
| คีย์ OTA (ไม่อยู่ใน git) | `firmware/components/app_config/secrets.h` |
| ตรรกะลายนิ้วมือคีย์และเพดานอัปโหลดฝั่งบอร์ด | `firmware/components/ota_key/` |
| กติกา "build release ต้องมีคีย์จริง" | `firmware/cmake/check_release_key.cmake` (เปิดด้วย `-DFISHPUMP_RELEASE=ON`) |
| เครื่องมือ Python | `firmware/scripts/`: `ota_crypto.py`, `encrypt_ota.py`, `write_secrets.py`, `check_release_tag.py`, `package_release.py` |
| ชุดติดตั้ง Windows ของลูกค้า (ต้นฉบับ) | `firmware/scripts/customer/` (`FLASH.bat`, `SHOW_PASSWORD.bat`, `ERASE_FLASH.bat`, `tools/`, README ไทย/อังกฤษ) |
| CI | `.github/workflows/ci.yml` (ทุก PR และ push ไป main), `.github/workflows/release.yml` (เมื่อ push tag `v*.*.*`) |
| ตัวตรวจสัญญาของส่วนนี้ | `firmware/scripts/verify_release_pipeline.py` |

## หลักที่ต้องจำ

1. ไฟล์ release จริงมาจาก CI เท่านั้น เครื่องใครก็ build แบบ dev ได้ แต่ห้ามเอาไฟล์ dev ไปติดตั้งลูกค้า
2. build dev ใช้คีย์ศูนย์ (ลายนิ้วมือ `66687aad`) ส่วน build release ล้มทันทีถ้าไม่มีคีย์จริง
3. คีย์หาย = บอร์ดที่ใช้คีย์นั้นรับ OTA แบบเข้ารหัสไม่ได้ ต้องแฟลชด้วยสาย
4. web OTA รับไฟล์เข้ารหัส ส่วน ThingsBoard OTA รับไฟล์ธรรมดา
