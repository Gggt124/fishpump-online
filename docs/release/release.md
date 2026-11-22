# ออก release

ไฟล์ release ทุกตัวมาจาก CI (`.github/workflows/release.yml`) เท่านั้น

## ก่อนครั้งแรก (ทำครั้งเดียว)

1. ตั้ง GitHub secret `OTA_ENCRYPTION_KEY` ตาม [`keys.md`](keys.md) และสำรองคีย์นอก GitHub แล้ว
2. ตรวจว่า CI รันเขียวบน PR จริงอย่างน้อยหนึ่งครั้ง (ดูช่องว่างที่ค้างใน [`handover.md`](handover.md))
3. ซ้อมด้วย tag ทดสอบ `vX.Y.Z-rc1` (ดูด้านล่าง) ก่อน release จริงครั้งแรก

## ขั้นตอน

1. แก้ `firmware/version.txt` เป็นเลขเวอร์ชันล้วน `X.Y.Z` (บรรทัดเดียว)
2. เปิด branch แล้ว PR เข้า `main` ให้ CI เขียว แล้ว merge ด้วย **"Create a merge commit"** (ไม่ fast-forward)
3. สลับไป `main` ล่าสุดแล้ว push tag:

   ```powershell
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

4. `release.yml` ทำตามลำดับ: เช็กว่า tag ตรงกับ `version.txt` (`firmware/scripts/check_release_tag.py`) > เขียน `secrets.h` จาก secret > build ด้วย `-DFISHPUMP_RELEASE=ON` (ล้มถ้าไม่มีคีย์จริง) > `package_release.py` > สร้าง GitHub Release
5. tag ที่ลงท้าย `-rc1` (เช่น `v0.4.1-rc1`) เป็นการซ้อม: เลขหน้า `-` ต้องเท่ากับ `version.txt` และ Release จะถูกทำเป็น prerelease
6. ถ้า tag ไม่ตรงกับ `version.txt` workflow ล้มก่อน build ลบ tag (`git push origin :refs/tags/vX.Y.Z`) แก้แล้ว tag ใหม่

## ไฟล์ที่แนบใน Release

| ไฟล์ | ใช้ทำอะไร |
|---|---|
| `fishpump-<ver>.bin` | image ธรรมดา สำหรับ ThingsBoard OTA |
| `fishpump-<ver>-<fp>.ota.bin` | image เข้ารหัส สำหรับ web OTA (`<fp>` คือลายนิ้วมือคีย์) |
| `fishpump-<ver>-factory.bin` | flash image ทั้งก้อน เขียนที่ address 0x0 ด้วยสาย |
| `flash-package.zip` | ชุดติดตั้ง Windows (`FLASH.bat` ฯลฯ) |
| `SHA256SUMS` | ผลรวมตรวจไฟล์ |

## เมื่อต้องเปลี่ยนเวอร์ชันของเครื่องมือ

- **ESP-IDF:** แก้ `firmware/IDF_VERSION` และ tag `espressif/idf:v<เวอร์ชัน>` ใน `ci.yml` กับ `release.yml` ให้ตรงกัน (`verify_release_pipeline.py` ตรวจ) แล้ว build ใหม่ commit `firmware/dependencies.lock` ที่เปลี่ยน และรัน selftest
- **ไลบรารี (managed component):** แก้ช่วงเวอร์ชันใน `idf_component.yml` build แล้ว commit `dependencies.lock` ห้ามใช้ `'*'`
- **esptool.exe ในชุดติดตั้ง:** ตรึงใน `package_release.py` (`ESPTOOL_URL`, `ESPTOOL_EXE_SHA256`) เปลี่ยนต้องใส่ SHA-256 ของไฟล์ใหม่

## build ในเครื่อง (เฉพาะทดสอบ ไม่ใช่ไฟล์ release)

เครื่องนี้เปิด ESP-IDF 6.0.1 ด้วย `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` (ใน PowerShell: `. <path>`) แล้ว

```powershell
idf.py -C firmware build
```

ได้ build แบบ dev (คีย์ศูนย์ ถ้าไม่มี `secrets.h`) ผลอยู่ใน `firmware/build/`
