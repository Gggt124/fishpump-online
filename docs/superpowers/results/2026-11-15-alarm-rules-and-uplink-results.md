# ผลการดำเนินงาน: Flow switch + ระบบแจ้งเตือน (Alarm) + Uplink ที่พร้อมต่อ ThingsBoard

วันที่: 2026-11-15
Branch: `feature/alarm-rules` (ตัดจาก `firmware/esp32s3-dev`) — **ยังไม่ merge ยังไม่ push**
Plan: `docs/superpowers/plans/2026-11-15-alarm-rules-and-uplink.md`
Spec: `docs/superpowers/specs/2026-11-15-alarm-rules-and-uplink-design.md`
คู่มือทดสอบบนบอร์ด (ให้คุณรัน): `docs/bench-test-alarms.md`

---

## 1. สรุป

ทำครบทั้ง **12 tasks** ตาม plan ในส่วนที่ทำได้โดยไม่ต้องมีบอร์ด: โค้ด, self-test บน QEMU, script ตรวจ contract, build, และเขียนคู่มือทดสอบ
**ที่ยังไม่ได้ทำ:** การ flash ลงบอร์ดและดู serial log (ไม่มีบอร์ดต่อกับ PC เครื่องนี้ — ดูข้อ 4) และการลองหน้าเว็บจริงบนบอร์ด

ผลของ gate อัตโนมัติ ณ commit สุดท้าย:

| Gate | ผล |
|---|---|
| Self-test (QEMU) `run_selftest.ps1` | `SELFTEST PASS (913 checks)` |
| `python firmware/scripts/verify_alarm_system.py` | `PASSED: 87  FAILED: 0` |
| `python firmware/scripts/verify_sensor_integration.py` | `PASSED: 45  FAILED: 0` |
| `idf.py -C firmware build` (`-Werror`) | สำเร็จ (ไฟล์ firmware ~1.78 MB ใน partition 3.5 MB) |

## 2. สิ่งที่ได้ (ตาม task)

| Task | งาน | Commit | หลักฐาน |
|---|---|---|---|
| 1 | `telemetry_hub`: timestamp ต่อกลุ่ม, event FLOW และ ALARM | `7445e1d` | self-test 65 checks |
| 2 | `contact_debounce` (pure) + `flow_switch` (GPIO 9, 100 ms sample, นิ่ง 1 s) + ต่อใน `app_main` | `d3491a0` | self-test 74 |
| 3 | Flow บนเว็บ (`/api/sensors/status`, SSE `flow`, การ์ด) + เปลี่ยนชื่อ pump1/pump2 → pump/chiller ทั้งระบบ | `3fbdae4` | verify 12/12 |
| 4 | `signal_registry` (36 สัญญาณ, กฎ trusted, NaN/Inf = ไม่ trusted) | `646c358` | self-test 721 |
| 5 | `alarm_rules`: โมเดลกฎ, validation, JSON (ชื่อ 96/97 ไบต์, UTF-8, ค่า 1e40 ฯลฯ) | `68e81f1` | self-test 772 |
| 6 | State machine (delay, hysteresis, gate, fault/transient, stale, latch, ack, reset, counts) + test การข้ามรอบ uptime 49.7 วัน | `99454f3` | self-test 840 |
| 7 | History ring 32, preset 13 ข้อ, blob + NVS, `alarm_engine` (task 1 s), LED กระพริบแดงเมื่อ critical ยังไม่รับทราบ | `47f112a` | self-test 874, build |
| 8 | REST `/api/alarms/*` + SSE `alarm` (guard ร่วม `api_begin_json_post`) | `21dfba0` | verify 36/36 |
| 9 | หน้า **แจ้งเตือน**, แบนเนอร์, badge, dialog กฎ, poll สรุปทุก 10 วินาทีเป็น fallback เมื่อ SSE เต็ม | `31215a9` | verify 76/76 |
| 10 | `uplink_format`: payload แบบ ThingsBoard, คำนวณ ts, นับ event ที่หลุด | `6a7f173` | self-test 913 |
| 11 | `uplink` (cadence, store-and-forward, transport plug, wall clock hook) + `/api/uplink/preview` + การ์ดบนหน้าสถานะ | `6255578` | verify 87/87 |
| 12 | คู่มือทดสอบภาษาไทย + อัปเดต backlog | `26a2b6b` | 4 gate ด้านบน |

รายการ commit ทั้งหมด:  `git log firmware/esp32s3-dev..feature/alarm-rules`
(มี commit `fb6bbfb` เก็บ spec/plan และ commit แก้ไขเรื่อง `.vscode/settings.json` ตามข้อ 5)

### หน้าตาที่ได้ (สรุปสั้น)
- **หน้า "แจ้งเตือน"**: กำลังแจ้งเตือน (ปุ่มรับทราบ/รับทราบทั้งหมด), กฎ (x/24, เปิด-ปิด, แก้, ลบ), ประวัติ 32 รายการ (หายเมื่อรีบูต)
- **แบนเนอร์** ทุกหน้าเมื่อมีแจ้งเตือน (แดงถ้า critical) + **badge** ที่เมนู
- **การ์ด "การไหลของน้ำ"** หน้าเซนเซอร์, และหัวข้อ "อินพุตตายตัว" ในหน้า Hardware
- **การ์ดตัวอย่างข้อมูลที่จะส่ง ThingsBoard** ท้ายหน้าสถานะระบบ
- ไฟ LED กระพริบแดง 1 Hz เมื่อมี critical ที่ยังไม่รับทราบ

## 3. การตัดสินใจที่ทำแทนคุณ (Rulings) และผลถ้าผิด

1. **Self-test ใช้ cJSON ที่โหลดไว้แล้วผ่าน `EXTRA_COMPONENT_DIRS`** แทนการเพิ่ม `idf_component.yml` — ทำงานออฟไลน์ เวอร์ชันเดียวกับ firmware · ถ้าผิด: สลับเป็นไฟล์ yml ได้ทันที
2. **`alarm_rule_to_json` ไม่ใส่ `id` เมื่อ id = 0** — เพื่อให้กฎที่ยังไม่เคยบันทึกวนกลับเป็น "สร้างใหม่" ได้ · ถ้าผิด: ไม่มีผลที่ผู้ใช้เห็น
3. **gate ที่สัญญาณ "ปรากฏแต่ไม่ trusted" ถือว่า gate ปิด** (spec พูดถึงแค่ "ไม่มี" หรือ "ค่าไม่ตรง") — กันไม่ให้ค่าเก่าของเซนเซอร์เสียมาเปิดกฎ · ถ้าผิด: กฎที่ gate ด้วยสัญญาณ bool ของเซนเซอร์ที่เสียจะไม่ทำงานระหว่างที่เสีย
4. **สถานะ LATCHED ที่เงื่อนไขกลับมาก่อนกดรับทราบ จะคง LATCHED** (ไม่ส่ง raised ซ้ำ) — รักษาสัญญา "หนึ่ง raised ต่อหนึ่ง cleared" · ถ้าผิด: ไม่มีผลที่เห็น
5. **เปลี่ยน `strncpy` เป็น `memcpy` แบบกันล้น** เพราะ `-Werror=stringop-truncation` — พฤติกรรมเท่าเดิม
6. **เช็ค auth ของ POST ผ่าน guard กลาง** (`api_begin_json_post` ซึ่งมี `require_auth` + `is_same_origin`) แทนการเช็คซ้ำในทุก handler — เจตนาเดียวกับที่ plan เขียน · ถ้าผิด: ไม่มี
7. **ปรับ `verify_sensor_integration.py`** ให้ตรวจ guard ใหม่ (เพราะ `sensors_begin_post` กลายเป็น wrapper บางๆ)
8. **แก้ test เดิมให้ใช้ `static`** สำหรับ rule set ขนาด ~3 KB ใน `test_alarm_eval` — stack ของ main task ใน self-test เล็ก เมื่อโค้ดเพิ่มขึ้นทำให้ stack ล้นเป็น heap corruption (แก้เฉพาะตัว test)
9. **เพิ่มหมายเหตุในคู่มือ sensor เดิม** (`docs/bench-test-sensors.md`) ว่า "ปั๊ม 1/2" ตอนนี้คือ "ปั๊มน้ำ/Chiller" เพื่อไม่ให้คู่มือเก่าทำให้สับสน
10. **เผลอ commit ไฟล์ `firmware/.vscode/settings.json`** (ค่า `idf.port: COM8` ที่คุณแก้ค้างไว้) เข้าไปใน commit `21dfba0` — แก้แล้วด้วย commit ย้อน และคืนค่าเป็นการแก้ที่ยังไม่ commit ในเครื่องคุณเหมือนเดิม

## 4. สิ่งที่ **ไม่ได้ทำ / ยังไม่ได้ยืนยัน**

- **ไม่ได้ flash และไม่ได้ดู serial log** (Task 7 ขั้น 9, Task 11 ขั้น 5, Task 12 ขั้น 3 และ 5): PC เครื่องนี้ไม่เห็นพอร์ต COM12 (มีแต่ Bluetooth COM3–6, 10, 11) และไฟล์ตั้งค่า VS Code ของคุณชี้ไปที่ `COM8` — จึงยังไม่ได้ยืนยันบนฮาร์ดแวร์ว่า: บูตปกติ, เห็นบรรทัด `alarm_engine: 13 rules loaded`, heap นิ่ง, task เพิ่มพอดี 3 ตัว, ปั๊ม auto-start เหมือนเดิม
- **ไม่ได้เปิดหน้าเว็บกับบอร์ดจริง**: ตรวจ JS ด้วย `node --check`, ตรวจ id ทุกตัวที่ JS อ้างว่ามีใน HTML, และ script ตรวจ contract เท่านั้น (ยังไม่เคยเห็นหน้าจอจริง)
- **ไม่ได้ทดสอบ `alarm_engine` / `uplink` / REST บนเป้าหมายจริง**: logic ล้วนผ่าน QEMU แต่ส่วนที่ผูกกับ FreeRTOS, NVS และ HTTP ผ่านแค่ build
- **การรีวิวสุดท้าย:** skill กำหนดให้ใช้ reviewer ใหม่ (fresh context) 1 รอบ แต่ session นี้ไม่ได้เรียก subagent (ไม่ได้ขอให้ใช้) จึง **รีวิวเองโดยผู้เขียน** ซึ่งอ่อนกว่า — แนะนำให้ให้คนอื่น/รอบใหม่รีวิวก่อน merge (ใช้ `/code-review`)

ขั้นตอนที่ต้องทำต่อ: flash → ทำตาม `docs/bench-test-alarms.md` → แจ้งข้อที่ไม่ผ่านเพื่อแก้เป็น commit ละข้อ

## 5. การรีวิวตัวเอง (Review Focus ใน plan)

| หัวข้อ | สถานะ |
|---|---|
| 1. uptime ข้ามรอบ 49.7 วัน | มี test (`test_alarm_eval_wrap`, debounce ข้ามรอบ, อายุสัญญาณข้ามรอบ, `uplink_event_ts`) |
| 2. storage แปลก/เสียหาย | test blob: version ผิด, ตัดท้าย, count เกิน, signal ที่ไม่มี (ข้ามกฎนั้นเก็บกฎอื่น) ผ่าน |
| 3. input อันตราย | test: ชื่อ 96/97 ไบต์, `"`, `\`, newline, UTF-8 เสีย, 1e40, string แทน number, gate ชี้ตัวเอง → ได้ชื่อช่องที่ผิด |
| 4. ค่า NaN/Infinity | registry ถือว่าไม่ trusted, payload ไม่มี `nan`/`inf` (test), SSE/REST ใช้ null |
| 5. SSE เต็ม | poll สรุปทุก 10 วินาทีเรียก `syncAlarmStatus` เมื่อ seq/boot_id/จำนวนเปลี่ยน (script ตรวจโค้ด) · **ยังต้องทดสอบจริง** ข้อ 12 ในคู่มือ |

### ข้อสังเกตเล็กๆ ที่ยังไม่แก้ (Deferred minors)
- บันทึกกฎลง NVS ขณะถือ mutex ของ engine: ถ้า flash ช้า หน้าเว็บ/LED อ่านสรุปช้าลงชั่วครู่ (เกิดเฉพาะตอนผู้ใช้กดบันทึก)
- blob กฎเก็บ struct ดิบ (`alarm_rule_t`): ถ้าแก้ขนาด struct ในอนาคตต้องเพิ่มเลข `ALARM_BLOB_VERSION` และเขียนตัวแปลง (ตอนนี้ขนาดไม่ตรง = ใช้ preset)
- `POST /api/alarms/ack {"id":n}` ตอบ `acked:1` แม้กฎนั้นรับทราบไปแล้ว (ไม่เปลี่ยนสถานะซ้ำ)
- badge = แจ้งเตือนที่ active + เหตุการณ์ใหม่ที่ยังไม่เห็น ตาม spec ดังนั้นแจ้งเตือนที่เพิ่งเกิดนับ 2 จนกว่าจะเปิดหน้าแจ้งเตือน
- ไม่มีปุ่ม/ตัวเลือกกลับขั้ว flow switch (ถ้าใช้ขั้ว NC ค่าจะกลับด้าน) ตามที่ spec ระบุไว้

## 6. วิธีตรวจซ้ำเอง

```bash
# ในเทอร์มินัล PowerShell (โหลด ESP-IDF ก่อน)
. C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1
powershell -File firmware/test/sensor_selftest/run_selftest.ps1   # SELFTEST PASS (913 checks)
python firmware/scripts/verify_alarm_system.py                     # PASSED: 87  FAILED: 0
python firmware/scripts/verify_sensor_integration.py               # PASSED: 45  FAILED: 0
idf.py -C firmware build
```
