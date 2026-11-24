# คู่มือทดสอบบนบอร์ด: ขีดจำกัดความปลอดภัยของค่าตั้ง และการหยุดปั๊มแบบตั้งเวลา (firmware 0.5.0)

ผู้ทดสอบ: เจ้าของโปรเจกต์ (แฟลช ต่อ Wi-Fi และกดปุ่มเอง) ไฟล์นี้ใช้กับ firmware 0.5.0 ที่แฟลชบนบอร์ดแล้ว รายละเอียดค่าทั้งหมดมาจาก [`docs/firmware/device-api.md`](firmware/device-api.md) ข้อ 3, 4 และ 11.2

> สถานะ: `[ ]` ยังไม่ทำ · `[/]` ผ่าน · `[x]` ไม่ผ่าน (จดอาการในบรรทัด "บันทึก")
> ต่อ Wi-Fi `FishPump-Setup` แล้ว PC จะไม่มีอินเทอร์เน็ต เปิดไฟล์นี้ไว้ก่อน
> log ดูที่ USB (COM12): `idf.py -C firmware -p COM12 monitor`
> HTTP ของบอร์ดอยู่ที่ IP ของบอร์ดใน LAN และ ThingsBoard ใช้ dashboard ทดสอบ (หน้า Pump, Protection, Alarms) ตาม [`thingsboard/README.md`](../thingsboard/README.md)

**เตรียมของ**

- บอร์ดแฟลช 0.5.0 แล้ว ตรวจด้วย `status.get` ว่า `project_version` = `0.5.0`
- ThingsBoard ต่อกับบอร์ดได้ (เห็น telemetry และ RPC ตอบ)
- หน้าเว็บของบอร์ดเปิดในเบราว์เซอร์ เปิดแท็บ Network ของ DevTools ค้างไว้ (ใช้ใน B1)
- สวิตช์น้ำไหล: ของจริง หรือจัมเปอร์ GPIO 9–GND (ต่อ = น้ำไหล, ถอด = ไม่ไหล)
- B2 ต้องมีบอร์ดที่ยังรัน 0.4.0 หรือแฟลช 0.4.0 กลับไปตั้งค่า แล้วแฟลช 0.5.0 ทับโดยไม่ลบ NVS (web OTA หรือ `idf.py flash`)

---

### B1. ค่านอกช่วงถูกปฏิเสธทั้งจากหน้าเว็บและ RPC

1. หน้าเว็บของบอร์ด การ์ดป้องกันเดินแห้ง: ตั้ง `prime_sec` = 121 แล้วกดบันทึก (หน้าเว็บยังยอมให้พิมพ์ เพราะตรวจฝั่งเบราว์เซอร์ที่ 1..600 คำตัดสินอยู่ที่บอร์ด)
   **ต้องเห็น:** ขึ้น toast "บันทึกไม่สำเร็จ" ช่อง `prime_sec` ขึ้นข้อความสีแดง "ค่าไม่ถูกต้อง" แท็บ Network เห็น `POST /api/protection` ตอบ **400** body `{"ok":false,"error":"value_out_of_range","message":"prime_sec"}` ค่าที่บอร์ดใช้ไม่เปลี่ยน (รีเฟรชหน้าแล้วยังเป็นค่าเดิม) และ serial log ไม่มีบรรทัด `dry-run settings saved`
2. ตั้ง `dry_trip_sec` = 31 แล้วกดบันทึก
   **ต้องเห็น:** เหมือนข้อ 1 แต่ `message` เป็น `dry_trip_sec`
   - [ ] ผ่าน  บันทึก: ______________________
3. ThingsBoard หน้า Protection กด `protection.set` → "โหลดค่าปัจจุบัน" → แก้ `pump.prime_sec` เป็น 121 → ส่ง
   **ต้องเห็น:** คำตอบ RPC เป็น `{"ok":false,"error":"value_out_of_range","message":"prime_sec"}` แถบสถานะของแผงเป็นสีแดง `ERROR`
4. แก้ `pump.dry_trip_sec` เป็น 31 แล้วส่งอีกครั้ง
   **ต้องเห็น:** `{"ok":false,"error":"value_out_of_range","message":"dry_trip_sec"}`
5. ตั้งค่ากลับเป็นค่าที่ใช้งานจริง (เช่น `prime_sec` 15, `dry_trip_sec` 10) แล้วส่ง
   **ต้องเห็น:** `ok:true`, `config_source` เป็น `"stored"`, log มี `dry_run_guard: dry-run settings saved, protection ...`
   - [ ] ผ่าน  บันทึก: ______________________

### B2. บอร์ด 0.4.0 ที่ตั้ง `prime_sec` = 300 ไว้ ถูก clamp ตอนแฟลชรุ่นใหม่

1. บนบอร์ดที่รัน 0.4.0 (ช่วงที่รุ่นนั้นยอมรับ `prime_sec` 1..600): จากหน้าเว็บตั้ง `prime_sec` = 300, `dry_trip_sec` = 25, เปิดสวิตช์ป้องกัน (enabled = true) แล้วบันทึก
   **ต้องเห็น:** 0.4.0 บันทึกผ่าน ไม่มี 400, `protection.get` ของ 0.4.0 คืน `prime_sec` = 300 (รุ่นเก่าไม่มี `config_source`)
2. แฟลช 0.5.0 ทับโดยไม่ลบ NVS แล้วเปิด ThingsBoard/หน้าเว็บ เรียก `protection.get`
   **ต้องเห็น:** `enabled` ยังเป็น `true`, `prime_sec` = **120**, `dry_trip_sec` = 25 (ไม่ถูกแตะ), `config_source` = `"clamped"`, และ log ตอนบูตมี `nvs_store: Dry-run settings out of range (prime_sec); clamped`
3. กดบันทึกจากหน้าเว็บอีกครั้งโดยไม่แก้อะไร
   **ต้องเห็น:** `config_source` เปลี่ยนเป็น `"stored"` ทันที (ค่า 120 ถูกเขียนลง NVS)
4. รีสตาร์ตบอร์ด แล้วเรียก `protection.get` อีกครั้ง
   **ต้องเห็น:** `config_source` = `"stored"`, `prime_sec` = 120 (ไม่กลับไป 300)
   - [ ] ผ่าน  บันทึก: ______________________

### B3. `limits` มาทั้งใน `protection.get` และ attribute `cfg_protection`

1. ThingsBoard หน้า Protection กด `protection.get`
   **ต้องเห็น:** คำตอบมี top-level `limits` ตรงนี้เป๊ะ: `{"prime_sec":[5,120],"dry_trip_sec":[3,30],"retry_wait_min":[1,60],"max_retries":[0,5]}`
2. เปิดแท็บ Attributes ของ device ดู client attribute `cfg_protection` (รอไม่เกิน 60 วินาที หรือกดบันทึกป้องกันหนึ่งครั้งเพื่อให้ uplink สร้างใหม่ทันที)
   **ต้องเห็น:** ใน `cfg_protection` มี `limits` ชุดเดียวกัน มี `config_source` และมี `pump` (ไม่มี `pump_status`) ค่าตัวเลขตรงกับที่ `protection.get` ตอบ
   - [ ] ผ่าน  บันทึก: ______________________

### B4. หยุดแบบตั้งเวลา 5 นาที แล้วปั๊มเปิดเอง

1. ทำให้สวิตช์น้ำไหลอ่าน "มีน้ำไหล" (ต่อจัมเปอร์ GPIO 9–GND หรือให้น้ำไหลจริง) เพื่อไม่ให้การป้องกันเดินแห้งตัดปั๊มระหว่างรอ แล้วเปิดปั๊มน้ำให้เดินอยู่ และถ้าเปิดการป้องกันไว้ต้องไม่อยู่ในสถานะล็อกหรือรอ
2. ThingsBoard หน้า Pump กด `pump.control (หยุดน้ำ)` → แก้ params เป็น `{"system":"water","action":"stop","resume_after_min":5}` → ส่ง
   **ต้องเห็น:** `ok:true`, `pump.status` → `system_water.running` เป็น `false`, `system_water.resume_in_sec` ≈ 300 แล้วลดลงทุกวินาที
3. ดู Device → Latest telemetry ช่อง `pump_resume_sec` (หน้านี้แสดงทุก key; ถ้าดูจากการ์ด telemetry ของหน้า Pump ต้องเพิ่ม key นี้ก่อน)
   **ต้องเห็น:** ค่าลดลงทีละ ~30 วินาที (key นี้ไม่ได้ตั้ง watched จึงไปกับรอบส่ง 30 วินาที) จาก ~300 ลงถึง 0
4. รอจนครบ 5 นาที
   **ต้องเห็น:** ปั๊มเริ่มเองโดยไม่มีใครสั่ง (`running` กลับเป็น `true`) `resume_in_sec` กลับเป็น 0 ค้าง และ log มี `pump_resume: timed stop over: pump.start -> 200 OK` หนึ่งครั้ง
   - [ ] ผ่าน  บันทึก: ______________________

### B5. ครบเวลาแต่ปั๊มถูกล็อก: ต้องไม่เปิด

1. ตั้ง `retry_wait_min` = 1 และ `max_retries` = 0 เปิดการป้องกัน แล้วบันทึก (ทำให้ล็อกตั้งแต่รอบแรก)
2. เริ่มปั๊มน้ำโดยไม่มีน้ำไหล (ถอดจัมเปอร์หรือสายสวิตช์น้ำไหล) แล้วรอประมาณ `prime_sec` + `dry_trip_sec` (15 + 10 วินาที)
   **ต้องเห็น:** บอร์ดตัดปั๊มและล็อก log มี `dry_run_guard: dry-run locked after 1 trips, pump stopped until acknowledged` การ์ดป้องกันขึ้น "ล็อกปั๊ม รอรับทราบที่หน้าแจ้งเตือน" และ `dry_run_state` = `locked`
3. ตอนล็อกอยู่ ส่ง `pump.control` `{"system":"water","action":"stop","resume_after_min":5}` (การล็อกไม่กันการตั้งเวลา)
   **ต้องเห็น:** `ok:true` และ `resume_in_sec` ≈ 300
4. รอจนครบ 5 นาที
   **ต้องเห็น:** ปั๊ม**ไม่**เปิด (`running` เป็น `false`, รีเลย์ไม่ทำงาน) `resume_in_sec` กลับเป็น 0 log มี `pump_resume: timed stop over: pump.start -> 409 Conflict` เพียงครั้งเดียว (ไม่ลองใหม่) และสถานะป้องกันยังเป็น `locked`
5. กดรับทราบที่หน้าแจ้งเตือน ต่อสวิตช์น้ำไหลกลับ แล้วตั้ง `retry_wait_min`/`max_retries` กลับเป็นค่าที่ใช้งานจริง
   **ต้องเห็น:** หลังรับทราบ ปั๊มไม่เปิดเอง (ผู้ใช้เป็นคนสั่ง) และตั้งค่าใหม่บันทึกผ่าน
   - [ ] ผ่าน  บันทึก: ______________________

### B6. กดเปิดจากหน้าเว็บระหว่างนับถอยหลัง: ตัวจับเวลาถูกยกเลิก

1. เริ่มหยุดแบบตั้งเวลา 5 นาทีตาม B4 ข้อ 2 (ยังไม่ครบเวลา)
2. ที่หน้าเว็บของบอร์ด การ์ดปั๊มน้ำ กดเปิดปั๊ม (`pump.start`)
   **ต้องเห็น:** ปั๊มเดิน `pump.status` → `system_water.running` เป็น `true` และ `system_water.resume_in_sec` = **0** ทันที telemetry `pump_resume_sec` เป็น 0
3. รอเลยกำหนดเดิมไปอีก 6 นาที โดยให้สวิตช์น้ำไหลอ่าน "มีน้ำไหล" ตลอด (กันการป้องกันเดินแห้งตัดปั๊มแล้วเริ่มใหม่ด้วยเหตุอื่น)
   **ต้องเห็น:** ไม่มีบรรทัด `pump_resume: timed stop over` ใหม่ และไม่มี `pump.start` ที่บอร์ดสั่งเอง (การเปิด/ปิดหลังจากนี้เป็นไปตามตารางปกติของปั๊มเท่านั้น)
   - [ ] ผ่าน  บันทึก: ______________________

### B7. รีสตาร์ตระหว่างหยุดแบบตั้งเวลา: ทำตาม `water_auto_start`

1. ตรวจ `water_auto_start` ใน `pump.config.get` และจดค่าไว้ (หน้า Pump หรือหน้าเว็บ)
2. เริ่มหยุดแบบตั้งเวลา 5 นาที แล้วรอให้เหลือประมาณ 4 นาที
3. ถอดปลั๊กหรือกด EN รีเซ็ตบอร์ด แล้วรอจนบอร์ดบูตและ ThingsBoard ต่อติด
   **ต้องเห็น:** `pump.status` → `system_water.resume_in_sec` = 0 (ตัวจับเวลาไม่ถูกกู้) และหลังบูตไม่มีบรรทัด `pump_resume` ใน log
4. ดูว่าปั๊มทำอะไรหลังบูต โดยให้สวิตช์น้ำไหลอ่าน "มีน้ำไหล" ตลอด
   **ต้องเห็น:** ถ้า `water_auto_start` = true ปั๊มเดินตามตารางเองหลังบูต ถ้า false ปั๊มหยุดรอจนกว่าจะสั่ง ทั้งสองกรณีไม่มีการเปิดตอนครบ 5 นาทีเดิม
5. ทำซ้ำข้อ 1–4 กับ `water_auto_start` อีกค่า เพื่อยืนยันว่าเป็นไปตามค่าที่ตั้ง ไม่ใช่ตามตัวจับเวลา แล้วตั้งค่ากลับเป็นค่าจริง
   - [ ] ผ่าน  บันทึก: ______________________

### B8. parity ระหว่าง RPC กับ HTTP ยังตรงกัน

1. `python thingsboard/parity_test.py --selftest`
   **ต้องเห็น:** `SELFTEST PASS` และ exit code 0
2. `python thingsboard/parity_test.py --user <อีเมล> --device fishpump-1de564`
   **ต้องเห็น:** ทุกเคสขึ้น `PASS` (ไม่มี `FAIL`) บรรทัดสุดท้าย `summary: <N> PASS, 0 FAIL, <M> SKIP` และ exit code 0 โดยเฉพาะ `protection.get` (มี `config_source`/`limits` ทั้งสองทาง), `pump.status` (มี `resume_in_sec`), `pump.control` และส่วน attribute/state ที่ทุก key ต้องมีค่าใหม่ภายใน 45 วินาที
3. (ไม่บังคับ) `python thingsboard/parity_test.py --user <อีเมล> --device fishpump-1de564 --writes --rule-id <id>`
   **ต้องเห็น:** เคสเขียนผ่านและค่าถูกคืนกลับเหมือนเดิม
   - [ ] ผ่าน  บันทึก: ______________________

---

**หมายเหตุ:** ข้อ B4–B7 ใช้เวลารอจริงข้อละ 5 นาทีขึ้นไป ตั้งเวลาให้ครบก่อนเริ่ม และปิดโหลดที่ทำให้ปั๊มเดินแห้งระหว่างรอ ถ้าข้อใดไม่ผ่าน ให้จดค่า `resume_in_sec`, `dry_run_state`, `config_source` และบรรทัด log ที่เกี่ยวข้องไว้ในบรรทัด "บันทึก"
