# ผลการดำเนินงาน: ระบบป้องกันปั๊มเดินแห้ง (Pump dry-run protection)

วันที่: 2026-11-16
Branch: `feature/alarm-rules` — **ยังไม่ merge ยังไม่ push** (commit `8e7cabc`..`3a38f1a` ต่อจาก `227b0fd`)
Spec: `docs/superpowers/specs/2026-11-16-pump-dry-run-protection-design.md`
Plan: `docs/superpowers/plans/2026-11-16-pump-dry-run-protection.md`
คู่มือทดสอบบนบอร์ด (ให้คุณรัน): `docs/bench-test-dry-run.md`

---

## 1. สรุป

ทำครบทั้ง **9 tasks** ตาม plan (Task 9 คือเอกสารนี้และคู่มือทดสอบ)

> **ไม่ได้ flash บอร์ดและไม่ได้รันอะไรบนฮาร์ดแวร์เลย** ผู้ทำโค้ดไม่มีบอร์ดต่อกับเครื่องนี้
> การตรวจสอบทั้งหมดคือ **self-test บน QEMU + การตรวจโค้ดแบบ static (สคริปต์ verify) + build + code review เท่านั้น**
> พฤติกรรมของปั๊มจริง รีเลย์ หน้าเว็บบนบอร์ด การรีบูตตอนล็อก ยังไม่เคยเห็นทำงานจริง ต้องให้คุณ flash และทดสอบตาม `docs/bench-test-dry-run.md`

## 2. Commit ต่อ task และผล gate

Gate: `verify_alarm_system.py` (FAILED 0) · `verify_dry_run.py` (FAILED 0, เริ่มมีใน Task 3) · `verify_sensor_integration.py` (exit 0, 45 checks) · `run_selftest.ps1` (QEMU, `SELFTEST PASS`) · `idf.py build` (`-Werror`, 0 warnings)

| Task | งาน | Commit | verify_alarm | verify_dry_run | Self-test (checks) |
|---|---|---|---|---|---|
| 1 | `dry_run_logic`: config, wait, prime suggestion | `8e7cabc` | 0 fail | ข้าม (ยังไม่มีไฟล์) | 929 |
| 2 | state machine: back-off, lock, ack | `2161bd7` | 99 pass | ข้าม | 1044 |
| 3 | system alarms ใน alarm engine; เลิก preset | `3d83be0` | 99 pass | 7 pass | 1048 |
| 4 | water guard inhibit, โหมดทดสอบรีเลย์ 1, สัญญาณ `pump_dry_run_state` | `75342c5` | 99 pass | 14 pass | 1086 |
| 4 (แก้ รอบ 1) | backstop 130 วินาทีของโหมดทดสอบ | `499af9f` | 99 pass | 15 pass | 1086 |
| 5 | `dry_run_guard` task, NVS, ล็อกตอนบูต | `0a0dc26` | 99 pass | 23 pass | 1086 |
| 5 (แก้ รอบ 1) | stack ของ guard เป็น 4096 | `57b657f` | 99 pass | (ผ่าน) | 1086 |
| 6 | API `/api/protection*`, บล็อก start ตอน WAIT/LOCKED | `a8c7453` | 99 pass | 46 pass | 1086 |
| 7 | การ์ด UI, ปุ่มวัดให้, ข้อความตอน start | `6ed9d57` | 99 pass | 80 pass | 1086 |
| 7 (แก้ รอบ 1) | sync สถานะการวัดตอนกลับมาหน้า dashboard | `532a4cb` | 99 pass | 82 pass | 1086 |
| 8 | alarm engine: บันทึก NVS นอก lock | `3a38f1a` | 99 pass | 89 pass | 1086 |

ผล gate ล่าสุด (Task 8 / `3a38f1a`): `verify_alarm_system` PASSED 99 FAILED 0 · `verify_dry_run` PASSED 89 FAILED 0 · `verify_sensor_integration` exit 0 · `SELFTEST PASS (1086 checks)` · `idf.py build` exit 0 ไม่มี warning

หมายเหตุ: Task 5 รอบแก้ รายงานระบุเพียงว่า verify_dry_run FAILED 0 โดยไม่บอกจำนวน check ที่ผ่าน ตัวเลขอื่นในตารางมาจาก report ของแต่ละ task

ระบบ alarm engine เองไม่มี test แบบ runtime ใน self-test (ต้องมี FreeRTOS/NVS) พฤติกรรมของมันและของ `pump_control`/`dry_run_guard` ผ่านการตรวจโค้ด, review และสคริปต์ static เท่านั้น

## 3. การตัดสินใจระหว่างทำ (Ruling) เรียงตามลำดับ

แต่ละข้อมีต้นทุนถ้าตัดสินผิด ตามที่บันทึกไว้ใน ledger

1. **Task 3 / gate:** Tasks 1–2 ข้าม gate `verify_dry_run.py` เพราะไฟล์ยังไม่มี (plan ระบุว่า "created in Task 3") — ต้นทุนถ้าผิด: ไม่มี
2. **Review Focus 5:** ข้อความ "bench step 5 in Task 8" หมายถึง Task 9 (คู่มือทดสอบ) เป็นคำพิมพ์ผิดใน plan — ต้นทุนถ้าผิด: ไม่มี
3. **Task 4:** ยกระดับข้อเสนอแนะ Minor #1 ของ reviewer (ไม่มี backstop ของโหมดทดสอบใน `pump_control`) เป็น Important ตามผลที่เกิด: ถ้า glue task ตาย/ค้าง หรือ `water_test(false)` คืนค่า false รีเลย์ 1 จะเดินแห้งไม่หยุดขณะ guard มองไม่เห็น (§5.7) — ต้นทุนถ้าผิด: โค้ดเพิ่มประมาณ 6 บรรทัด
4. **Task 5 (ผู้คุมงานตัดสินในคำสั่งงาน):** guard mutex ไม่ถูกถือข้ามการเรียก pump/engine/NVS · การวัดลองสั่ง `water_test(false)` ซ้ำจนกว่าจะปิด · ผลการวัดใช้ `last_change_ms` เฉพาะเมื่อเกิดหลัง t0 · ถ้า NVS ล็อกแต่ปิดการป้องกัน ให้ไม่สนใจและล้าง flag · `set_config` บันทึก NVS ก่อนสลับค่า · init พลาดให้ log แล้วบูตต่อ — spec ไม่ได้ระบุ — ต้นทุนถ้าผิด: แก้ Task 5 เล็กน้อย
5. **Task 5:** stack ของ guard task 3072 → **4096** (ต่างจาก plan) reviewer ไล่ path ตอนตัด (raise → publish_pending → telemetry → web SSE snprintf) ใช้ประมาณ 2.5–2.8 KB และ `alarm_eng` ใช้ path เดียวกันบน 4096 ล้น = รีบูตตอนตัด และสูญเสีย WAIT — ต้นทุนถ้าผิด: RAM 1 KB
6. **Task 6:** `pump/control` แบบ `all` + start ที่ถูก guard บล็อกให้ตอบ 409 โดยไม่เริ่ม chiller · ถ้าสถานะ guard ใช้ไม่ได้ ไม่บล็อก start และ `GET /api/protection` ตอบ 503 unavailable · `set_config` ได้ INVALID_STATE ตอบ 503 — spec ไม่ได้ระบุ — ต้นทุนถ้าผิด: แก้ handler เล็กน้อย
7. **Task 7:** การ์ด poll `/api/protection` ทุก 5 วินาทีเฉพาะตอนหน้า dashboard มองเห็น และนับถอยหลังในเครื่องระหว่างนั้น · 503 → "ระบบป้องกันไม่พร้อมใช้งาน" พร้อมปิดปุ่ม · ออกจาก dashboard ระหว่างวัดจะหยุด poll แต่ไม่หยุดการวัด (เฟิร์มแวร์จำกัดที่ 120 วินาที) — spec ไม่ได้ระบุ — ต้นทุนถ้าผิด: ปรับ UI เล็กน้อย

## 4. รายการที่เลื่อนไว้ (minor, deferred)

**Task 1:** test ค่าขอบบางส่วน (ขอบที่ใช้ได้, 241/121, trip 0/255) · `NULL cfg` ไม่ถูกตรวจ · เทียบ uint64 กับค่าคงที่ int

**Task 2:** การเช็ก trip ทำงานตอนรีเลย์ปิด ทำให้ถ้าลด `dry_trip_sec` ต่ำกว่า accum ที่ค้างอยู่จะ trip ใน tick ถัดไป · ค่า ack = accum บางส่วน · `dt` ไม่มีขอบบนเมื่อ scheduler ค้าง · ช่องว่างของ test (ack ใน DISABLED, สาย flow ขาดกลางคัน, ลด `max_retries` ระหว่าง WAIT, wrap + recovery)

**Task 3:** verify พิสูจน์ว่า callback ถูกเรียกหลัง Give แต่ไม่พิสูจน์ว่าไม่มีก่อน · `on_ack` เป็น NULL ได้ค่า `acked:1` · คอมเมนต์เก่าใน `alarm_presets.c:18-19` · engine ไม่มี runtime test

**Task 3 → Task 6 (ยกไปทำ):** buffer active ของ `/api/alarms/status` ขนาด `ALARM_MAX_RULES` (`web_server.c:5765,5774`) อาจทำให้แจ้งเตือนของระบบหล่นเมื่อ active ครบ 24 กฎ ต้องขยายเป็น `ALARM_MAX_RULES + ALARM_ENGINE_MAX_SYSTEM` และปรับคอมเมนต์หัวฟังก์ชัน `get_active`

**Task 3 → Task 5 (ยกไปทำ):** `on_ack` ทำงานหลังปลดล็อก อาจมาช้า guard ต้องตัดสินจากสถานะของตัวเอง (`dry_run_ack` ไม่สนใจเมื่อ `!alarm_active`)

**Task 4:** regex ของ verify ใน `apply_water_set_start_phase` ผ่านด้วยคอมเมนต์ (ควรเข้มเป็น `water_inhibited_locked()`) · เงื่อนไข inhibit เขียนซ้ำ 3 ที่ · `on_phase` ซ้ำกับ `start_phase` · header ไม่ได้อธิบายว่า `resume=false && running` จะเริ่มที่ start phase · คอมเมนต์ `:429` ไม่ตรง · ไม่มี log ตอน inhibit ใน `pump_control` · `active_set` หยุดเปลี่ยนตอน inhibited (แสดงผลเท่านั้น) · `water_test(false)` ถูกปฏิเสธเมื่อ faulted (ไม่เป็นอันตราย) · ไม่มี runtime test ของ `pump_control`

**Task 4 (เพิ่มเติม):** `notify_state_changed` ถูกเรียกใต้ `s_pump_mutex` (ยังไม่มี callback ลงทะเบียน) · การตรวจ cap ของ verify เป็น substring อย่างเดียว

**Task 5:** อ่านสถานะล้มเหลวแล้วป้อน `relay_on=false` (เริ่มหน้าต่างผ่อนผันใหม่) · flow_switch ยังไม่ init → ตัดเสมอ (fail-safe ตาม spec) · flag resume ลดระดับเมื่ออ่านสถานะตอน ack ล้มเหลว/ซ่อมการ sync · โหลด NVS สองครั้งตอนบูต (WARN สองครั้ง, ช่องว่างเล็กๆ ถ้าโหลดครั้งแรกพลาด) · `portMAX_DELAY` บน `s_apply` ใน path HTTP · `pending_off` ถูกล้างผ่านสถานะสำรองเมื่อเขียนรีเลย์พลาด · init พลาดหลัง register ไม่มีทางออก (อยู่ฝั่งปลอดภัย) · `t0` นับหลัง `water_test` คืนค่า · `ESP_LOGE` ใน `persist_failed` · persist ตอนล็อกพลาดไม่ถูกลองใหม่ · verify gate อิงชื่อ

**Task 6:** `measure_start` error อื่นนอกจาก INVALID_STATE ตอบ 409 ไม่ใช่ 503 · start ตอนปั๊มทำงานอยู่และ inhibited ตอนนี้ได้ 409 แทน `already_running` (การเปลี่ยน API ที่ตั้งใจ) · `POST /api/protection` ไม่มีการตรวจ `is_password_default` (ต้องเทียบความสอดคล้องกับ POST ของ alarm) · verify ไม่ตรวจว่า stop ไม่ถูกบล็อก

**Task 7:** ฟอร์มไม่เติมค่าใหม่หลัง 503 หาย · poll ของการวัดที่ error ถูกเมินตลอดไป (ไม่นับความล้มเหลว) · รายการฟังก์ชันใน `test_ui` ไม่ครบ ไม่เช็กข้อความไทย · การจัดการ class `is-warn` · (หลังแก้ รอบ 1) GET resume ที่มาช้าหลังออกจาก dashboard อาจเริ่ม poll 1 วินาทีที่หน้าที่ซ่อนอยู่หนึ่งครั้ง · GET resume ที่ล้มเหลวทำให้ปุ่มวัดเปิดใช้อีกครั้งทั้งที่อุปกรณ์อาจยังวัดอยู่ (กด start จะได้ข้อความ "กำลังวัดอยู่")

**Task 8:** regex ตรวจลำดับ lock ยังอ่อน (ไม่พิสูจน์ว่า Take แรกคือ `write_lock` หรือมี Give) · ไม่มีคอมเมนต์ว่าการตรวจค่าบนสำเนาพึ่ง `write_lock`

## 5. สิ่งที่ยังไม่ได้พิสูจน์บนฮาร์ดแวร์

- **ทุกพฤติกรรมของ bench** (`docs/bench-test-dry-run.md` ข้อ 0–10) ยังไม่ได้ทำ รอคุณรัน
- **Backstop 130 วินาที** ของโหมดทดสอบรีเลย์ 1 (`pump_control`, commit `499af9f`): ไม่มีทางกระตุ้นจากหน้าเว็บหรือการใช้งานปกติ (การวัดหยุดเองที่ 120 วินาที) สังเกตได้ทาง log เท่านั้น (`Relay-1 test exceeded 130000 ms cap, switching relay 1 off`) **ยังไม่ได้ทดสอบ**
- **ที่เหลือของ stack 4096 ไบต์ของ `dry_run_guard`:** เฟิร์มแวร์ไม่มีตัวรายงาน `uxTaskGetStackHighWaterMark` ของ task นี้ (มีเพียง `main_stack_hwm` ใน `[BOARD_DIAG]` ซึ่งเป็นของ task หลัก และ `stack_hwm` ในสถิติ route ของ web ซึ่งเป็นของ httpd) จึง **ไม่ได้ยืนยันบนฮาร์ดแวร์** ข้อ 11 ของคู่มือตรวจเพียงว่าบอร์ดไม่รีบูตตอนตัดขณะเปิด dashboard ซึ่งไม่ใช่การวัดค่าที่เหลือ ถ้าต้องการตัวเลขจริง ต้องเพิ่ม log รายงาน HWM ในรอบถัดไป
- ปั๊มจริงและ flow switch จริงในท่อ ยังไม่เคยทดสอบ

## 6. ไฟล์ที่เพิ่มใน Task 9

- `docs/bench-test-dry-run.md`
- `docs/superpowers/results/2026-11-16-pump-dry-run-protection-results.md`

ไม่ได้แตะ `docs/bench-test-alarms.md` และ `firmware/.vscode/settings.json` (เป็นการแก้ในเครื่องของคุณ)
