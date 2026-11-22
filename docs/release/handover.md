# ส่งมอบ: สิ่งที่ผู้รับช่วงต้องได้ และช่องว่างที่ยังค้าง

## ต้องได้ก่อนเริ่มงาน

| สิ่งที่ต้องได้ | หมายเหตุ |
|---|---|
| สิทธิ์ GitHub ของ repo `Gggt124/fishpump-online` | ต้องแก้ secret และ push tag ได้ |
| GitHub secret `OTA_ENCRYPTION_KEY` และที่สำรองคีย์นอก GitHub | ดู [`keys.md`](keys.md) ถ้าไม่มีที่สำรอง อย่าเปลี่ยน secret |
| ESP-IDF 6.0.1 บน Windows | เครื่องเดิมเปิดด้วย `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` (`export.ps1` ใช้ไม่ได้บนเครื่องนั้น) |
| Python 3.13 กับ `pip install cryptography pyyaml` | สคริปต์ release และตัวตรวจสัญญา |
| ThingsBoard CE 4.3.1 ที่ 192.168.1.51:8080 (LAN, MQTT ธรรมดาพอร์ต 1883) | เจ้าของเป็นผู้ถือล็อกอิน รหัสผ่านไม่อยู่ใน repo |
| โปรเจกต์เก่า `fish-pump-RelayTimerControl` (ถ้ายังมี) | ชุดติดตั้งลูกค้าพอร์ตมาจากที่นั่น ไม่จำเป็นต้องใช้อีก |
| เอกสารระบบ | `docs/summary.md`, `docs/firmware/reference.md`, `docs/firmware/device-api.md`, `docs/backlog/` |

## คำสั่งตรวจก่อน merge หรือ release

ทุกคำสั่งรันที่ root ของ repo ในเชลล์ที่เปิด ESP-IDF แล้ว (บางเทสต์ใช้ `cmake` และข้ามเองถ้าไม่มี)

```powershell
foreach ($f in Get-ChildItem firmware/scripts/verify_*.py) { python $f.FullName }
python -m unittest discover -s firmware/scripts -p "test_*.py"
firmware/test/sensor_selftest/run_selftest.ps1
idf.py -C firmware build
```

selftest ตอนนี้ 2108 ข้อ ต้องจบด้วย `SELFTEST PASS` การ build ที่ต้องการทดสอบกติกาคีย์: `idf.py -C firmware -B <โฟลเดอร์อื่น> -DFISHPUMP_RELEASE=ON reconfigure` ต้องล้มเมื่อไม่มี `secrets.h` (ใช้โฟลเดอร์ build แยก ไม่งั้นตัวเลือกจะค้างใน cache ของ `firmware/build`)

## ช่องว่างที่ยังค้าง (ตรงไปตรงมา)

1. **CI ยังไม่เคยรันจริงบน GitHub** ไฟล์ workflow ตรวจแค่ในเครื่อง (YAML ถูกต้อง, `verify_release_pipeline.py`, และรันสคริปต์ตรวจใน clean clone) การรันครั้งแรกบน PR คือการทดสอบจริง คาดว่าอาจต้องแก้เล็กน้อย
2. **QEMU selftest ใน CI:** `firmware/test/sensor_selftest/run_selftest.sh` เป็นการพอร์ต `run_selftest.ps1` เป็น bash ที่เขียนโดยไม่เคยรันบน Linux และ job ติดตั้ง `qemu-xtensa` ด้วย `idf_tools.py` ถ้ารันใน container ไม่ได้ ให้ตัด step นั้นออกจาก `ci.yml` (และแก้ตัวตรวจใน `verify_release_pipeline.py` ที่เช็กว่ามี `run_selftest.sh`) แล้วรัน `run_selftest.ps1` เองก่อนทุก release
3. **`esptool.exe` ในชุดติดตั้ง:** `package_release.py` ดาวน์โหลดจาก URL ที่ตรึงไว้ตอน release แล้วตรวจ SHA-256 ที่ตรึงไว้ (ค่าที่ตรึงมาจากไฟล์ v5.3.1 ของโปรเจกต์เก่า) การดาวน์โหลดจริงครั้งแรกเกิดใน CI ถ้า hash ไม่ตรง job ล้มดังๆ
4. **ยังไม่เคยแฟลชบอร์ดด้วยของชุดนี้:** เช็กลิสต์ของเจ้าของอยู่ที่ [`../bench-test-release.md`](../bench-test-release.md)
5. **ไม่มี secure boot / ลายเซ็น OTA** และ ThingsBoard OTA ส่งไฟล์ธรรมดาผ่าน MQTT ธรรมดา ต้องทำ TLS ก่อนขึ้น VPS (ดู [`../backlog/thingsboard-data-and-deploy.md`](../backlog/thingsboard-data-and-deploy.md))
6. **สูตรรหัส AP อยู่สองที่** (เฟิร์มแวร์กับ `ap_password.ps1`) ตัวตรวจจับได้ถ้าสูตรในเฟิร์มแวร์เปลี่ยนรูป แต่ต้องแก้ทั้งสองที่เอง
7. **ไฟล์ release ที่ปล่อยแล้วแก้ไม่ได้:** ถ้า tag ผิด ให้ลบ Release และ tag แล้วทำใหม่ ห้ามเขียนทับไฟล์ที่ลูกค้าโหลดไปแล้วโดยไม่บอก

## ถ้าเจออะไรไม่ตรงกับเอกสาร

เชื่อโค้ดและ `verify_release_pipeline.py` ก่อน แล้วแก้เอกสาร บันทึกการตัดสินใจที่ต่างจากสเปกใน `docs/superpowers/specs/2026-11-21-release-pipeline-design.md` หัวข้อ 9
