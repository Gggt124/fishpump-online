# คู่มือทดสอบบนบอร์ด: การเชื่อมต่อ ThingsBoard (MQTT)

Firmware: branch `feature/thingsboard-mqtt` (ทดสอบตั้งแต่ commit `c058d1a` ถึง `e9f9a92` แต่ละรอบเขียน commit ของตัวเองไว้)
Spec: `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`
Plan: `docs/superpowers/plans/2026-11-18-thingsboard-link-m1.md` (Task 7) ถึง `docs/superpowers/plans/2026-11-21-thingsboard-soak-m6.md` (milestone 1–6)
ฝั่ง ThingsBoard: `thingsboard/README.md` (ThingsBoard CE 4.3.1 บนเครื่อง copter `192.168.1.51`, HTTP 8080, MQTT 1883)

> **รอบนี้บอร์ดต้องต่อ Wi-Fi บ้าน (STA)** ไม่ใช่ AP `FishPump-Setup` เพราะทั้ง PC และบอร์ดต้องเห็น `192.168.1.51`
> เปิดเว็บของบอร์ดด้วย IP ที่บอร์ดได้จากเราเตอร์บ้าน (ดูได้จาก log ตอนบูตหรือหน้าเราเตอร์)
> log ดูได้ที่พอร์ต **USB (COM12)** ด้วย `idf.py -C firmware -p COM12 monitor`
> สถานะ: `[ ]` ยังไม่ทำ · `[/]` ผ่าน · `[x]` ไม่ผ่าน (จดอาการในบรรทัด "บันทึก")

**ตำแหน่งการ์ด:** หน้า **สถานะ** → การ์ด **"การเชื่อมต่อ ThingsBoard"** (อยู่เหนือการ์ดตัวอย่างข้อมูลที่จะส่ง)
**provision key / secret:** พิมพ์ออกมาตอนรัน `tb_setup.py` และเก็บไว้ใน `thingsboard/.local/provision.json` (ไม่อยู่ใน git)
**รหัส ThingsBoard:** ผู้ทดสอบพิมพ์เองตอนสคริปต์ถาม ไม่เก็บลงไฟล์

---

## 0. เตรียมก่อนเริ่ม

- [/] flash เฟิร์มแวร์ branch นี้ลงบอร์ด (COM12) ไม่ต้อง erase จาก PowerShell ที่เปิด ESP-IDF แล้ว:
  ```powershell
  . C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1; Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
  idf.py -C firmware -p COM12 flash
  idf.py -C firmware -p COM12 monitor
  ```
  ปล่อยหน้าต่าง monitor เปิดไว้ทั้งการทดสอบ — 2026-11-19 flash `45a5fe8` (ตรวจ hash ผ่าน) บูตปกติ
- [/] บอร์ดต่อ Wi-Fi บ้านได้ และ log มีบรรทัด `time set:` (SNTP ได้เวลาจริงแล้ว) — บอร์ดได้ IP `192.168.1.34` และ `time set: 2026-11-19T05:06:43Z` ประมาณ 1.3 วินาทีหลังได้ IP (ต้องเปิด Wi-Fi auto-connect เพราะเดิมปิดอยู่)
- [/] เปิดเว็บของบอร์ดแล้ว login ได้ และเปิด ThingsBoard `http://192.168.1.51:8080` ได้ (บัญชี tenant)

บันทึก: ______________________________________________

## รอบ 1: การเชื่อมต่อ

- [/] **B1 ลงทะเบียนครั้งแรก** — รัน `python thingsboard/tb_setup.py` แล้วใส่ในการ์ด: host `192.168.1.51`, port `1883`, provision key และ secret ที่พิมพ์ออกมา กด **บันทึก**
  **ต้องเห็น:** การ์ดเปลี่ยนจาก provisioning เป็น connected ภายใน 30 วินาที ใน ThingsBoard มี device `fishpump-xxxxxx` ที่ใช้ profile `fishpump` จากนั้นรัน `python thingsboard/tb_setup.py --assign fishpump-xxxxxx`
  บันทึก: 2026-11-19 ลงทะเบียนเสร็จและ connected ประมาณ 3 วินาทีหลังบันทึก device `fishpump-1de564` profile `fishpump` และ `--assign` ผูกกับลูกค้าทดสอบแล้ว

- [/] **B2 attribute และ telemetry ครบ** — ใน ThingsBoard เปิด device → แท็บ Attributes และ Latest telemetry
  **ต้องเห็น:** client attributes `fw_type=fishpump`, `contract_version=1`, `signals` (array ครบทั้งก้อน) และ `alarm_rules` ส่วน telemetry อัปเดตทุก 30 วินาที
  บันทึก: attributes ครบ: `fw_type=fishpump`, `contract_version=1`, `signals` เป็น array 37 ตัว, `alarm_rules` เป็น array 24 ข้อ telemetry มาทุก 30 วินาที (uptime 515 → 545)

- [/] **B3 ลงทะเบียนซ้ำทั้งที่ device ยังอยู่** — กด **ลงทะเบียนใหม่** โดยไม่ลบ device ใน ThingsBoard
  **ต้องเห็น:** การ์ดขึ้น backoff และ `last_error` เป็นข้อความจาก ThingsBoard (ไม่ใช่ "bad reply") ลองใหม่ประมาณทุก 60 วินาที จากนั้นคัดลอก token จาก ThingsBoard (device → Copy access token) ใส่ในการ์ด → connected
  บันทึก: การ์ดขึ้น `provision: Failed to provision device!` (ข้อความจาก ThingsBoard) ลองใหม่ที่ 662/725/788 วินาที (ห่างกันประมาณ 60 วินาที) ใส่ token เองแล้ว connected ภายใน 1 วินาที

- [/] **B4 host ผิด / server ไม่อยู่** — เปลี่ยน host เป็น `192.168.1.250` (ไม่มีเครื่องนี้) กด **บันทึก**
  **ต้องเห็น:** การ์ดนับถอยหลัง backoff 5 → 10 → 20 → 40 → 60 วินาที เว็บของบอร์ดยังตอบเร็ว (`/api/status` ไม่เกิน 300 ms) แล้วเปลี่ยน host กลับเป็น `192.168.1.51` → connected
  หมายเหตุ: บรรทัด `E` จาก tag `mqtt_client` และ `transport_base` ใน log ระหว่างข้อนี้เป็นเรื่องปกติ
  บันทึก: backoff 5 → 10 → 20 → 40 → 60 → 60 วินาที (แต่ละรอบใช้เวลาต่อไม่ติดประมาณ 10 วินาที) error `mqtt: transport error` `/api/status` ช้าสุด 13 ms เปลี่ยน host กลับแล้ว connected ใน 1.3 วินาที

- [/] **B5 กดบันทึกรัวตอนเชื่อมต่ออยู่** — กด **บันทึก** สองครั้งภายใน 1 วินาทีขณะ connected
  **ต้องเห็น:** สุดท้ายกลับมา connected ไม่มี crash ไม่มีบรรทัด `E` จาก tag `tb_link` และจำนวน `tasks` ใน `BOARD_DIAG` ไม่เปลี่ยน
  บันทึก: กดบันทึก 6 ครั้งติดกัน บอร์ดรวบเป็นการต่อใหม่ครั้งเดียว connected ใน 2.4 วินาที ไม่มี `E` จาก `tb_link` tasks = 22 ก่อนและหลัง

- [/] **B6 alarm เกิดและหาย** — เปิดกฎหนึ่งข้อที่ทำให้เกิดได้ด้วยจัมเปอร์ (เช่นกฎ flow) ทำให้เกิด แล้วทำให้หาย
  **ต้องเห็น:** ใน ThingsBoard มี alarm `rule_<id>` เปิดแล้วเคลียร์ เวลาของ event (`ts`) เป็นเวลาที่เกิดจริงจาก SNTP ไม่ใช่เวลาที่ ThingsBoard ได้รับ
  บันทึก: กฎทดสอบเต็ม 24/24 จึงแก้กฎ id 19 ชั่วคราวเป็น `flow_on` เป็นจริง (คืนค่าเดิมแล้วหลัง bench) alarm `rule_19` เปิดที่ 06:18:52Z และเคลียร์ที่ 06:29:10Z ตรงกับเวลาบนบอร์ดในระยะประมาณ 1 วินาที

- [/] **B7 alarm ระหว่างลิงก์หลุด** — เปลี่ยน host เป็น `192.168.1.250` ทำให้กฎเกิดแล้วหายระหว่างที่หลุด รอ 60 วินาที แล้วเปลี่ยน host กลับ
  **ต้องเห็น:** หลังต่อใหม่ event ทั้งสองมาถึงตามลำดับพร้อมเวลาเดิม alarm เปิดและปิดอย่างละครั้ง ไม่ค้าง ACTIVE
  หมายเหตุ: การบันทึก config ใหม่ทิ้ง outbox ของ MQTT (known limit ในแผน) ถ้า event หายในข้อนี้ ให้จดว่ามาจาก uplink buffer หรือจาก outbox
  บันทึก: เหตุการณ์ seq 3 (เกิด) และ seq 4 (หาย) เกิดตอนลิงก์หลุด ต่อใหม่หลังประมาณ 100 วินาที ใน ThingsBoard alarm เปิดด้วยเวลา 06:38:15Z ซึ่งเป็นเวลาจริงบนบอร์ด ไม่ใช่เวลาที่ได้รับ (06:40:13Z) เคลียร์ครั้งเดียว `last_alarm_event` = seq 4 หมายเหตุ: `clear_ts` ของ ThingsBoard เป็นเวลาที่ได้รับ เพราะ node Clear Alarm ใช้เวลาเซิร์ฟเวอร์ (เก็บไว้แก้ใน milestone ถัดไป)

- [/] **B8 ทนทาน 15 นาที** — ดู `BOARD_DIAG` ใน serial 15 นาทีขณะ connected
  **ต้องเห็น:** `heap_free` ไม่ต่ำกว่า 100 KB, `tb_hwm` ไม่ต่ำกว่า 512 และไม่มีรีบูต
  บันทึก: 2026-11-19 06:40–06:56Z (uptime 5880 → 6781 วินาที) connected ตลอด `heap_free` 148–154 KB (`heap_min` ต่ำสุด 110 KB) `tb_hwm` = 2880 tasks = 22 ไม่มีรีบูต ThingsBoard ได้ telemetry 35 จุดในช่วงนี้

## รอบ 2: device_api + RPC

ทดสอบ 2026-11-19 กับ firmware b971a55 (กลุ่ม alarms ผ่าน device_api และ RPC ของ ThingsBoard) และไฟล์ฝั่ง ThingsBoard ของ commit b870e30 กฎที่ใช้ทดสอบการเขียนคือ id 19 (ปิดอยู่)

- [/] **A1 แฟลชและบูต** — `idf.py -C firmware -p COM12 flash` แล้วเปิด monitor
  **ต้องเห็น:** บูตได้ ลิงก์ connected และ `BOARD_DIAG` มี `tb_rpc_hwm`
  บันทึก: connected ที่ 4.4 วินาทีหลังบูต `tb_rpc_hwm=5264` `heap_free` ประมาณ 148 KB (`heap_min` 120 KB) ไม่มีรีบูต

- [/] **A2 golden compare** — `api_golden.py compare --base base --label a2 --with-writes`
  **ต้องเห็น:** ทุกเคสผ่าน
  บันทึก: ผ่าน 148/149 เคส alarms ที่ย้ายแล้วผ่านทั้งหมด เคสที่ไม่ผ่านคือ `uplink_preview` `$.last_event` (object กับ null) เป็นสถานะบอร์ด ไม่ใช่ regression: baseline มี event ค้างจาก B6 (กฎ 19, seq 4) แต่หลังแฟลชยังไม่เกิด event ใหม่ และ uplink ยังไม่ได้ย้าย (Task 12)

- [/] **A3 ติดตั้ง dashboard** — `tb_setup.py` สองครั้ง แล้วเปิด dashboard
  **ต้องเห็น:** bundle, widget และ dashboard สร้างครั้งเดียว ครั้งที่สองเป็นการอัปเดต หน้า Alarms โหลดได้
  บันทึก: ครั้งแรกติดที่ `GET /api/widgetType` → 400 "Invalid fqn!" เพราะ ThingsBoard 4.3.1 ต้องการ `tenant.<fqn>` แก้ใน c5a8f87 แล้วรันใหม่ ครั้งแรกสร้าง widget type, bundle และ dashboard "Fishpump — ทดสอบ" (2 หน้า กำหนดให้ลูกค้าทดสอบ) ครั้งที่สองอัปเดตทั้งหมด ไม่มีของซ้ำ หน้า Alarms โหลดได้ มีตารางค่า alarm ล่าสุดของ fishpump-1de564

- [/] **A4 parity alarms** — `parity_test.py --group alarms --writes --rule-id 19`
  **ต้องเห็น:** ผ่านทั้งหมด
  บันทึก: 13 PASS, 0 FAIL (อ่าน 5, error 7, เขียน 1: `delay_sec` ของกฎ 19 จาก 0 → 1 แล้วคืนเป็น 0)

- [/] **A5 กดทุกปุ่มในหน้า Alarms**
  **ต้องเห็น:** ทุกปุ่มแสดงคำตอบ มีคำเตือนก่อนลบกฎ กดยกเลิกแล้วไม่มีอะไรส่งออกไป (serial ไม่มีบรรทัด `rpc`)
  บันทึก: ปุ่มอ่าน 5 ปุ่มตอบ 200 OK `rules.set` ตอบ 409 `rules_full` (กฎเต็ม 24/24 จึงไม่มีอะไรเปลี่ยน) ปุ่มโหลดค่าดึงกฎ 1 มาได้ ลบ id 65000 ตอบ `not_found` รับทราบทั้งหมดตอบ `acked: 0` รับทราบตาม id ตอบ `acked: 1` คำเตือนก่อนลบขึ้นว่า "ลบกฎแจ้งเตือน ใช่หรือไม่? (id 65000)" และคำเตือนก่อนรับทราบทั้งหมดขึ้นครบ กดยกเลิกแล้ว serial ไม่มีบรรทัด `rpc` เพิ่ม หมายเหตุ: Claude กดผ่าน browser โดยแทน `window.confirm` ด้วยตัวที่จดข้อความแล้วตอบยกเลิก

- [/] **A6 ยิง RPC รัว** — `parity_test.py --group alarms --burst 20` แล้ว `--burst 30` ระหว่างนั้นกด **บันทึก** ในการ์ด ThingsBoard (ค่าเดิม) หนึ่งครั้ง
  **ต้องเห็น:** PASS หรือมีแค่ `busy` ไม่มี crash `heap_free` ไม่ต่ำกว่า 100 KB `tb_rpc_hwm` ไม่ต่ำกว่า 512
  บันทึก: รอบ 20: 20 คำตอบ (busy 0) poll HTTP 80 ครั้งได้ 200 ทั้งหมด รอบ 30 กดบันทึกกลางทาง: ลิงก์ wait_wifi → connected ใน 1.1 วินาที 30 คำตอบ (busy 0) poll 129 ครั้งได้ 200 ทั้งหมด ไม่มี RPC timeout หลังจบ `heap_free` 147.5 KB `heap_min` 112.5 KB `tb_rpc_hwm` 3616 uptime 12,700 วินาทีไม่มีรีบูต หมายเหตุ: RPC id 90 ถูกส่งซ้ำหลังต่อใหม่ บอร์ดจึงทำซ้ำหนึ่งครั้ง (เป็นการอ่าน ไม่มีผลเสีย ตรงกับข้อสังเกตของ Task 3 เรื่องการส่งซ้ำหลัง reconnect)

## รอบ 3: device_api ครบทุกกลุ่ม (ทดสอบสุดท้ายของ milestone 2)

ทดสอบ 2026-11-20 กับ firmware 84fdbad (38 method ทุกกลุ่มผ่าน device_api) และไฟล์ฝั่ง ThingsBoard ของ commit eb26c87 กฎที่ใช้ทดสอบการเขียนคือ id 19 (ปิดอยู่) บนบอร์ดไม่มีเซนเซอร์ pH, PZEM และ PT100

- [/] **F1 แฟลชและบูต** — `idf.py -C firmware -p COM12 flash` แล้วเปิด monitor
  **ต้องเห็น:** บูตได้ ลิงก์ connected ไม่มีบรรทัด `E` จาก device_api หรือ tb_link
  บันทึก: hash ตรง connected ที่ 4.1 วินาทีหลังบูต ได้เวลา SNTP `tb_rpc_hwm=5264` `heap_free` 140.9 KB (`heap_min` 119.6 KB) บรรทัด `E` มีแค่ pt100_max31865 (ไม่ได้ต่อเซนเซอร์)

- [/] **F2 golden compare** — `api_golden.py compare --base base --label f2 --with-writes`
  **ต้องเห็น:** ALL PASS (เว็บแอปได้คำตอบเหมือนก่อนย้ายเข้า device_api ทุกไบต์)
  บันทึก: ALL PASS ทุก case รวมการเขียนแบบคืนค่าเดิม

- [/] **F3 ไฟล์เว็บแอปไม่เปลี่ยน** — `git diff --stat be0ab72 -- firmware/main/static`
  **ต้องเห็น:** ว่าง
  บันทึก: ว่าง

- [/] **F4 parity ทุกกลุ่ม** — `tb_setup.py` (อัปเดต dashboard) แล้ว `parity_test.py --writes --rule-id 19`
  **ต้องเห็น:** PASS ทั้งหมด SKIP ต้องมีเหตุผล
  บันทึก: tb_setup อัปเดต widget และ dashboard 9 หน้า รอบแรก 90 PASS 1 FAIL 1 SKIP FAIL คือการเขียน `pump.config.set` ผ่าน ThingsBoard ตอบ `invalid_body` สาเหตุอยู่ที่ case ทดสอบ ไม่ใช่ firmware: case ส่งคำตอบของ `pump.config.get` กลับไปทั้งก้อน รวม key สถานะ 3 ตัวและชื่อสำรองซ้ำทุกตัว ยาว 600 ไบต์ เกินเพดาน 512 ไบต์ที่ HTTP ก็ใช้เหมือนกัน (เว็บแอปส่งจริงประมาณ 460 ไบต์) แก้ใน eb26c87 ให้ส่งเฉพาะ key ที่แก้ได้ แล้วรันกลุ่ม pump ซ้ำ: 17 PASS 0 FAIL (`water_set1_off` 60 → 120 แล้วคืนค่า) SKIP คือ `water.offset` เพราะไม่มีเซนเซอร์ pH (`offsets.known` เป็น false) serial ไม่มีรีบูต `heap_min` 110.6 KB `tb_rpc_hwm` 3520

- [/] **F5 กดทุกปุ่มใน dashboard** (ยกเว้นปุ่มของ F6)
  **ต้องเห็น:** ทุกปุ่มแสดงคำตอบ ข้อความยืนยันตรงกับเว็บแอป
  บันทึก: ปุ่มอ่านตอบ 200 ทุกหน้า (protection, pump, cooling, wifi, status, sensors, hardware; หน้า alarms ทดสอบแล้วใน A5) `protection.set` กดโหลดค่าแล้วส่งค่าเดิมตอบ 200 แม่แบบ `pump.config.set` และ `cooling.config.set` ถูกปฏิเสธตามคาด (`missing_or_invalid_field`, `invalid_type`) `cooling.mode.set` auto (เดิมเป็น auto) ตอบ 200 `water.calibrate.cancel` ตอบ 202 `queued` ข้อความยืนยัน 12 ข้อขึ้นครบและตรงกับเว็บแอป (ที่ dashboard ไม่รู้ชื่อกฎ ชื่อเครือข่าย หรือค่า จะใช้ข้อความกลาง) กดยกเลิกแล้ว serial ไม่มีบรรทัด `rpc` ปุ่ม `wifi.profiles.save`, `setauto` และ `wifi.connect` ไม่ได้กดจาก dashboard เพราะจะเปลี่ยนโปรไฟล์ Wi-Fi นอกการทดสอบที่คืนค่าเดิม F4 ทดสอบทั้งสามแล้ว หมายเหตุ: Claude กดผ่าน browser โดยแทน `window.confirm` ด้วยตัวที่จดข้อความ

- [/] **F6 สั่งปั๊มและวัดจาก dashboard** (ผู้ใช้ดูรีเลย์)
  **ต้องเห็น:** เหมือนปุ่มในเว็บแอป มีคำเตือนก่อนเริ่มวัด
  บันทึก: ปั๊มเดินตามตั้งเวลาอยู่ จึงทำตามลำดับ `pump.stop` (รีเลย์ดับ) → เริ่มวัด (คำเตือน 120 วินาทีขึ้น กดตกลง ปั๊มเดิน) → หยุดวัดที่ 10.3 วินาที (รีเลย์ดับ สถานะ `stopped`) → `pump.start` (รีเลย์ติด ชุด 1 นาน 50 วินาที) ผู้ใช้ยืนยันว่ารีเลย์ตรงทุกขั้น serial ตอบ 200 ทุก RPC ปั๊มกลับไปเดินตามเดิม

- [/] **F7 ยิง RPC รัวและทนทาน 15 นาที** — เปิดหน้า Status ของเว็บแอปค้างไว้ แล้ว `parity_test.py --burst 30` ตามด้วยดู `BOARD_DIAG` 15 นาที
  **ต้องเห็น:** ไม่มี crash `heap_free` ไม่ต่ำกว่า 100 KB และ HWM ทุกตัวรวม `tb_rpc_hwm` ไม่ต่ำกว่า 512
  บันทึก: burst 30 ได้ 30 คำตอบ (busy 0) poll HTTP 123 ครั้งได้ 200 ทั้งหมด (รันพร้อมอ่านและ error case ทุกกลุ่ม: 86 PASS 0 FAIL 7 SKIP ที่ SKIP คือ case เขียนที่ต้องใช้ `--writes`) หลังจากนั้นดู `BOARD_DIAG` อีก 15 นาที ช่วง uptime 22,299 → 25,031 วินาที (92 บรรทัด) ไม่มีรีบูตหรือ crash `heap_free` 142–149 KB (`heap_min` 110.6 KB) HWM ต่ำสุด `main_stack_hwm` 1188 `hwm_alarm` 2600 `hwm_uplink` 2048 `hwm_dry_run` 3204 `tb_hwm` 3700 `tb_rpc_hwm` 3456 HTTP `failed=0` จาก 475 request (หน้า Status ของเว็บแอปเปิดค้างไว้)

### หลังแก้ตาม final review (firmware 4a698f6, ไฟล์ ThingsBoard 5eae70e)

final review พบ 3 ข้อที่ต้องแก้ก่อน merge: RPC ที่ ThingsBoard ส่งซ้ำทำให้คำสั่งเขียนทำสองครั้ง, `setauto`/`save` จาก ThingsBoard ทำให้บอร์ดหลุดเน็ตถาวรได้, และ polarity ของรีเลย์แก้จาก ThingsBoard ได้โดยไม่เห็นค่าเดิม แก้แล้วทดสอบซ้ำ:

- [/] **H1 แฟลชและบูต** — hash ตรง connected ที่ 4.1 วินาที `heap_min` 120.4 KB `tb_rpc_hwm` 5312 บรรทัด `E` มีแค่ pt100
- [/] **H2 golden compare** — `--label h2 --with-writes` ALL PASS (เว็บแอปได้คำตอบเหมือนเดิมทุกไบต์)
- [/] **H3 อัปเดต dashboard** — `tb_setup.py` อัปเดต widget และ dashboard 9 หน้า (แม่แบบ `protection.set` เป็นค่า "แก้ก่อนส่ง" และ `pump.config.set`/`cooling.config.set` มีปุ่มโหลดค่า)
- [/] **H4 parity ทุกกลุ่มพร้อมการเขียน** — 92 PASS 0 FAIL 1 SKIP (SKIP คือ `water.offset` ไม่มีเซนเซอร์ pH) `setauto` จาก ThingsBoard ด้วย index 99 และ -1 ได้ `in_use` ส่วน index ของเครือข่ายที่ต่ออยู่ยังทำได้ การเขียน pump และ cooling ที่ส่ง polarity เดิมผ่าน log `dapi_wifi` ขึ้นใน serial แล้ว ไม่มีรีบูต `heap_min` 112.6 KB `tb_rpc_hwm` 3600
- ยังไม่ได้ทดสอบบนบอร์ด: แคช RPC ซ้ำ (ไม่เกิดการส่งซ้ำในรอบนี้) และการปฏิเสธ polarity ที่ต่างจากค่าเดิม (ไม่ส่งจริงเพราะถ้า guard พลาดรีเลย์จะกลับขั้ว) ทั้งสองข้อมี selftest ใน QEMU

## รอบ 4: ค่าตั้งเป็น attribute และ telemetry สถานะ (milestone 3)

firmware 0258279 (โค้ดถึง 1bb904a) สคริปต์ ThingsBoard 1305104 วันที่ 2026-11-20 ตาม Task 7 ของ `docs/superpowers/plans/2026-11-20-thingsboard-state-m3.md`

- [/] **I0 เก็บคำตอบก่อน flash** — `api_golden.py capture --label m3pre --with-writes` บนเฟิร์มแวร์ 4a698f6 ได้ 149 case
- [/] **I1 flash และเทียบคำตอบ** — บูตปกติ ต่อ Wi-Fi 3.3 วินาที ต่อ ThingsBoard 4.1 วินาที ส่ง attribute ครบ 6 ตัวที่ 4.9 วินาที ไม่มี "send failed" (`cfg_pump` 817 B, `cfg_cooling` 929 B, `cfg_protection` 134 B, `hardware_map` 855 B, `wifi` 190 B, `tb_link` 67 B) `compare --base m3pre --label m3post --with-writes` ตรงกัน 148/149 ข้อที่ต่างคือ `uplink_preview` มี key ใหม่ `last_status` ตามที่ตั้งใจ (เว็บแอปไม่เปลี่ยน)
- [/] **I2 parity อ่านค่า (attribute + สถานะ)** — รอบแรก 126 PASS 1 FAIL 7 SKIP ข้อที่พลาดคือ `attr hardware_map` ที่ `pending` สาเหตุ: บอร์ดส่ง `"pending":null` (ขนาด 855 B ตรงกับคำตอบ HTTP ที่มี null พอดี) แต่ ThingsBoard ไม่เก็บสมาชิกที่เป็น null ใน attribute แบบ JSON แก้ที่สคริปต์ parity ให้ตัด null ทั้งสองฝั่งก่อนเทียบ (1305104 มีเทสต์ที่เห็นพลาดก่อน) และบันทึกใน `device-api.md` ข้อ 11.1 รันซ้ำ `--group state` ได้ 41 PASS 0 FAIL ค่าสถานะครบ 35 ค่า
- [/] **I3 attribute เปลี่ยนตามการเขียน** — `--writes --group pump --group protection --group cooling` ได้ 55 PASS 0 FAIL ทุกค่าคืนเดิม เวลาที่ attribute เปลี่ยนตาม (เขียนผ่าน ThingsBoard / คืนค่าผ่านเว็บ): `water_set1_off` 0.3 / 0.0 วินาที, `prime_sec` 0.0 / 0.0, `min_off_sec` 0.4 / 0.3 (spec ต้องการภายใน 2 วินาที)
- [/] **I4 แก้ค่าบนหน้าเว็บแอปแล้ว ThingsBoard เห็น** — สองรอบแรก (ผู้ใช้แก้เอง เฝ้า 3 และ 4 นาที) บอร์ดไม่ได้รับคำสั่งเขียนเลย (ไม่มี `write pump.config.set src=http` ใน log) รอบที่สามทำผ่านหน้าเว็บของบอร์ดในหน้าต่าง browser หลังผู้ใช้ล็อกอิน: แก้ "หยุดพัก (Idle)" ของ Set 1 จาก 1:00 เป็น 1:30 แล้วกด "บันทึกการตั้งค่า" ได้ `POST /api/pump/config` 200 และ `cfg_pump.water_set1_off` ใน ThingsBoard เป็น 90 ภายในไม่กี่วินาที คืนเป็น 1:00 ได้ 200 อีกครั้ง และ ThingsBoard เห็น 60 ภายใน 1.7 วินาทีนับจากกดบันทึก (รวมเวลาที่ REST ตอบ) สองรอบแรกจึงเป็นเพราะหน้าเว็บไม่ได้ส่งคำขอ ไม่ใช่เฟิร์มแวร์
- [/] **I5 สถานะส่งทันทีเมื่อ phase เปลี่ยน** — เฝ้า 3 นาทีเห็น phase เปลี่ยน 4 ครั้ง ข้อความถึง ThingsBoard ตอนนับถอยหลังเหลือ 49/50, 59/60, 49/50, 60/60 (ภายในประมาณ 1 วินาที) ช่วงที่ไม่มีอะไรเปลี่ยนข้อความสถานะมาทุก 29.2–30.2 วินาที
- [/] **I6 dashboard ทุกหน้า** — `tb_setup.py` อัปโหลด dashboard 9 หน้า หน้า Pump, Cooling, Protection, Wi-Fi, Status และ Hardware มีตาราง "ค่าตั้งปัจจุบัน (client attributes)" พร้อมข้อมูล และตารางค่าล่าสุดมีคีย์สถานะใหม่ ปุ่ม "โหลดค่าปัจจุบัน" ของ `pump.config.set` ได้ 14 key ที่เขียนได้ (377 B) และกดส่งต่อได้ `ok:true` (แก้ข้อค้างจาก M2) หน้า Hardware ตารางค่าล่าสุดว่างเพราะหน้านี้ไม่มีคีย์ telemetry
- [/] **I7 ปิดลิงก์แล้วต่อใหม่** — (ทำแล้วใน L2 ของรอบ 7: ต่อกลับเอง ส่ง attribute ครบ 6 ตัวใหม่) เดิม: ไม่ได้ทำ ต้องแก้ host บนการ์ด ThingsBoard ในเว็บแอป ซึ่งรอบนี้หน้าเว็บบันทึกไม่ถึงบอร์ด (ดู I4) เลื่อนไปทดสอบรวมกับ ThingsBoard ล่มใน milestone 6 การส่ง attribute ครบหลังต่อติดเห็นแล้วใน I1 (6 ตัวภายใน 0.9 วินาทีหลัง connected)
- [/] **I8 งบหน่วยความจำ** — uptime 92 นาที เปิด dashboard ค้างตั้งแต่ 09:28 ไม่มีรีบูต `heap_free` 146 KB `heap_min` 107.5 KB (M2 110.6 KB ลดลงประมาณ 3 KB ยังเกิน 100 KB) HWM ต่ำสุด `main_stack_hwm` 1284 `hwm_alarm` 2664 `hwm_uplink` 2928 (stack ใหม่ 6144) `hwm_dry_run` 3364 `tb_hwm` 3700 `tb_rpc_hwm` 3568 HTTP 300 request `failed=0`

## รอบ 5: เปลี่ยน Wi-Fi จากระยะไกล (milestone 4)

firmware 7c3cacc (โค้ดถึง f07e58b) วันที่ 2026-11-20 ตาม Task 6 ของ `docs/superpowers/plans/2026-11-20-thingsboard-remote-wifi-m4.md` เครือข่ายที่สองคือ hotspot มือถือ `Ggggg` (2.4 GHz) ซึ่งมองไม่เห็น ThingsBoard ที่ 192.168.1.51 จึงทำ J4 แบบเต็มไม่ได้

- [/] **J0 เก็บคำตอบก่อน flash** — `api_golden.py capture --label m4pre --with-writes` บนเฟิร์มแวร์ 0258279 ได้ 149 case
- [/] **J1 flash เทียบคำตอบ และ parity** — บูตปกติ ต่อ `KRAPAO JADJAN_2.4G` และ ThingsBoard ภายใน 5.3 วินาที `compare --base m4pre` ALL PASS (เว็บแอปเหมือนเดิม) `tb_setup.py` อัปโหลด dashboard 9 หน้า `parity_test.py` 129 PASS 0 FAIL 7 SKIP แถว Wi-Fi ใหม่ผ่านทั้งสาม (static IP → `local_only`, `{}` → `missing_ssid`, SSID 33 ตัว → `ssid_too_long`) ไม่มี case ไหนทำให้ stage บอร์ดไม่รีบูต
- [/] **J2 รหัสผิด** — ส่ง `wifi.connect {"ssid":"Ggggg","password":<ผิด>}` ได้ 200 OK รีบูตหลังตอบ 0.2 วินาที (ได้ PUBACK) บูตแล้วต่อ `Ggggg` ไม่ได้ (`connection_fail` 2 ครั้ง) wifi_manager ถอยไปใช้ credential เดิมเอง ต่อบ้านและ ThingsBoard ที่ 19 วินาที hook บอก `not on staged Ggggg; leaving it staged` ไม่ยืนยัน ครบ 180 วินาทีย้อนกลับ (`STAGING_ROLLBACK`) ต่อ ThingsBoard ใหม่ใน 4 วินาที รายการโปรไฟล์และ `auto_index` (0) เหมือนก่อนเริ่ม ทางย้อนกลับเป็น 180 วินาที ไม่ใช่ 30 วินาที เพราะถอยไปเครือข่ายเดิมได้ก่อน (ตามที่แก้ในแผนหลัง final review)
  - รอบแรกผู้ใช้ส่ง params ตัวอย่างไป บอร์ด stage SSID `แก้ก่อนส่ง` รีบูตหลังตอบ 1.1 วินาที หาเครือข่ายไม่เจอ (`no_ap_found`) ถอยไปบ้าน แล้วย้อนกลับที่ 180 วินาทีเหมือนกัน ระหว่างนั้น `wifi.connect` อีกครั้งที่ 80 วินาทีได้ 409 `staging_active` ไม่ stage ซ้ำ
- [/] **J3 hotspot ที่มองไม่เห็น ThingsBoard** — `wifi.connect {"ssid":"Ggggg"}` (ใช้รหัสที่บันทึกไว้) บอร์ดได้ IP 10.137.255.222 MQTT ลองใหม่ทุก 5 วินาที (`retry in 5 s` ตลอด รอบละประมาณ 16 วินาทีรวม timeout 10 วินาที) ครบ 180 วินาทีย้อนกลับเอง ThingsBoard เห็นบอร์ดหาย 192 วินาที ต่อกลับใน 4 วินาทีหลังรีบูต รายการโปรไฟล์เหมือนเดิม
- [/] **J4 สลับสำเร็จ — ทำแบบแทน** — ไม่มีเครือข่ายที่สองที่เห็น ThingsBoard จึงทดสอบส่วนบันทึกโปรไฟล์ (บั๊ก F1 ของ final review) แทน: ลบโปรไฟล์ `Ggggg` (รายการเหลือ `["KRAPAO JADJAN_2.4G"]`) แล้ว stage `Ggggg` ใหม่จาก dashboard รอบนี้บอร์ดต่อ hotspot ไม่ได้ (`connection_fail`) จึงถอยไปบ้าน ผู้ใช้กดยืนยันบนเว็บแอป log `Committed staged changes type=1` แล้ว timer หยุด รายการกลายเป็น `["KRAPAO JADJAN_2.4G", "Ggggg"]` (โค้ดก่อนแก้จะไม่บันทึก) ถอดไฟแล้วเสียบใหม่ (`reset=power_on`) กลับมาบ้าน รายการยังอยู่ auto_index 0
  - **ยังไม่ได้ยืนยันบนบอร์ด:** การ commit ผ่าน hook ตอนต่อ ThingsBoard ติดบนเครือข่ายใหม่ และการตั้ง auto-connect ให้เครือข่ายใหม่ ต้องมี access point ตัวที่สองใน LAN เดียวกับ ThingsBoard
  - รหัสของโปรไฟล์ `Ggggg` ที่บันทึกไว้ตอนนี้เป็นรหัสที่พิมพ์รอบนี้ (อาจผิด)
- [/] **J5 ปุ่ม Wi-Fi บน dashboard** — `wifi.profiles.save` เครือข่ายที่ต่ออยู่ด้วยรหัสเดิม → `{"ok":true,"reboot_pending":false}` · `wifi.profiles.setauto {"index":0}` → `{"ok":true}` · `wifi.profiles.save` SSID ที่ไม่ได้บันทึกและไม่ได้ต่อ รหัสว่าง → `not_connected` (guard `in_use` ใช้กับเครือข่ายที่ต่ออยู่แต่ไม่มีโปรไฟล์เท่านั้น)
- [/] **J6 งบหน่วยความจำ** — ตลอดรอบ `heap_min` ต่ำสุด 104.96 KB (boot ที่ staged) หลังถอดไฟ 107.96 KB HWM ต่ำสุด `main_stack_hwm` 1060 `hwm_alarm` 2712 `hwm_uplink` 2928 `hwm_dry_run` 3204 `tb_hwm` 2964 `tb_rpc_hwm` 3544 ทุกค่าเกิน 512 และ heap เกิน 100 KB (HWM ของ tb_link หลัง commit ผ่าน hook ยังไม่ได้วัด เพราะ J4 แบบเต็มไม่ได้ทำ)

## รอบ 6: อัปเดตเฟิร์มแวร์จาก ThingsBoard (milestone 5)

firmware 98e5272 (โค้ดถึง 82e035d) วันที่ 2026-11-20 ตาม Task 7 ของ `docs/superpowers/plans/2026-11-20-thingsboard-ota-m5.md` บัญชี ThingsBoard คือ `tenant@thingsboard.org` (README เขียน `<อีเมล>`) image ทดสอบอยู่ใน `thingsboard/.local/ota/` build ใน `firmware/build_ota` (0.3.3, 0.3.6) และ `firmware/build_block` (0.3.5) ต้องสั่ง `reconfigure` ทุกครั้งที่เปลี่ยน `version.txt` ไม่อย่างนั้น CMake ไม่อ่านไฟล์ใหม่ (รอบแรก 0.3.6 ได้เวอร์ชันข้างใน 0.3.3 ซึ่ง `tb_ota.py upload` ปฏิเสธอยู่แล้ว)

- [/] **K0 เก็บคำตอบก่อน flash** — `api_golden.py capture --label m5pre --with-writes` บนเฟิร์มแวร์ 7c3cacc ได้ 149 case
- [/] **K1 flash เทียบคำตอบ และ parity** — บูตจาก `ota_0` สถานะ VALID `tb_ota: boot state 0 (pending none, running 0.3.2)` ต่อ ThingsBoard ที่ 4.2 วินาที รายงาน `current_fw_version` 0.3.2 (ThingsBoard เห็นค่านี้) `compare --base m5pre` ALL PASS `tb_setup.py` ผ่าน `parity_test.py` 129 PASS 0 FAIL 7 SKIP บรรทัดขอ attribute ตอนต่อไม่มีใน log ระดับ info
- [/] **K2 อัปเดตสำเร็จ (0.3.3)** — assign แล้วเริ่มดาวน์โหลดใน 1.1 วินาที 127 chunk (16 KB) ใช้ 2 นาที 22 วินาที (ประมาณ 14.5 KB/s) log บอร์ด DOWNLOADED → VERIFIED → UPDATING ห่างกันราว 0.2 วินาที (poll ทุก 2 วินาทีจึงเห็นใน ThingsBoard แค่ DOWNLOADING → UPDATING → UPDATED) รีบูต `TB_OTA_REBOOT` บูตจาก `ota_1` แบบ PENDING_VERIFY log "waiting for the ThingsBoard connect to mark valid" ต่อที่ 4.1 วินาที "image marked valid" แล้ว `UPDATED` `current_fw_version` 0.3.3 ไม่มีดาวน์โหลดซ้ำหลังรีบูต flash ผ่าน USB แล้ว OTA ครั้งแรกเข้า PENDING_VERIFY ได้ (ย้อนกลับได้)
  - **ข้อที่พบ (มีมาก่อน M5):** banner ยังเขียน `v0.3.2` บน image 0.3.3 สาเหตุ: `add_compile_definitions(APP_TEMPLATE_FIRMWARE_VERSION="v${PROJECT_VERSION}")` ใน `firmware/CMakeLists.txt` ได้ค่าว่าง (`"v"`) แล้ว `app_config.h` ใช้ค่าสำรอง `"v0.3.2"` ที่เขียนตายตัว ค่านี้ใช้ใน banner, `/api/status` `project_version`, `fw_version` ของสถานะ ThingsBoard และ uplink identity การตัดสินใจของ OTA ใช้ `esp_app_desc` จึงถูกต้อง
- [x] **K3 อิมเมจเสีย** — ThingsBoard ปฏิเสธ checksum ผิดตอน upload (`Wrong otaPackage file!` และสร้าง package ว่างไว้ ลบแล้ว) จึงใช้ไฟล์สุ่ม 200 KB เป็น 0.3.4 (`--no-version-check`) ผล `FAILED write_failed` ไม่รีบูต ยังอยู่ที่ 0.3.3 ข้อที่ไม่ตรงแผน: คาดว่าได้ `invalid_image` แต่ `esp_ota_write` ปฏิเสธ chunk แรก (`invalid magic byte`, `ESP_ERR_OTA_VALIDATE_FAILED`) แล้ว `tb_ota.c:177` แปลงทุก error ของการเขียนเป็น `write_failed` ซึ่งไม่ใช่รหัส final ไฟล์เสียเวอร์ชันเดิมจึงดาวน์โหลดซ้ำได้หลัง reconnect
- [/] **K4 ไม่ต่อ ThingsBoard (0.3.5 test-block)** — ดาวน์โหลด 2 นาที 6 วินาที บูต 0.3.5 ThingsBoard ค้างที่ UPDATING ที่ 301 วินาทีหลังบูต log `no ThingsBoard connect within 5 min, rolling back` รีสตาร์ทปกติ (Saved PC `esp_restart_noos` ไม่มี panic) กลับมา `ota_1` 0.3.3 VALID `boot state 3 (pending 0.3.5, running 0.3.3)` ต่อแล้วรายงาน `FAILED rolled_back` ไม่มี DOWNLOADING ซ้ำใน 90 วินาทีต่อมา หน้าเว็บ (`GET /`) ตอบ 200 ทุก 30 วินาทีตลอดรอบ (18 ครั้ง) การ์ด `test build: connect blocked` บนเว็บแอปไม่ได้ดู (ต้อง login)
- [/] **K5 วัด dry-run ระหว่างอัปเดต (0.3.6)** — ปั๊มน้ำเดินตามรอบเวลา `protection.measure.set start` จึงได้ `measure_unavailable` ก่อน (ผู้ใช้อนุญาตให้หยุดปั๊ม) assign 0.3.6 ที่ 21:19:47 ใกล้ดาวน์โหลดเสร็จสั่ง `pump.control {"system":"water","action":"stop"}` แล้ว `measure.set start` ได้ `running` (21:21:29) VERIFIED ที่ 21:22:09 log `dry-run measurement running; delaying the reboot` UPDATING ที่ 21:23:29 (รอ 80 วินาทีจนวัดจบ) UPDATED 0.3.6 ที่ 21:23:35 หลังรีบูตปั๊มน้ำเดินเองตาม `auto_start` (`water_running=true`) ไม่ต้องสั่ง start `main_stack_hwm` หลังบูตนี้ 932 (ยังเกิน 512)
- [/] **K6 unassign** — บอร์ดไม่ทำอะไร ไม่มี log ของ `tb_ota` ข้อความ attribute ที่ได้รับไม่ได้ log ในระดับ info (spike เห็นเป็น `{"deleted":[...]}`)
- [/] **K7 งบหน่วยความจำ** — ระหว่างดาวน์โหลด (task 24 ตัว) `heap_free` 135–139 KB `heap_min` 105.3 KB `heap_largest` 86–90 KB หลังดาวน์โหลดและหลังรีบูต `heap_min` 107.1–107.5 KB HWM ต่ำสุด `main_stack_hwm` 1124 `hwm_alarm` 2600 `hwm_uplink` 2928 `hwm_dry_run` 3204 `tb_hwm` 3064 `tb_rpc_hwm` 3592 ทุกค่าเกิน 512 HWM ของ task `tb_ota` วัดไม่ได้ เพราะไม่มีที่ไหนรายงาน
- [/] **K3 ซ้ำหลังแก้ (e9f9a92, 2026-11-21)** — flash 0.3.2 ที่แก้แล้วทาง USB (banner `v0.3.2`, `ota_0` VALID, ไม่มี DOWNLOADING หลัง unassign 0.3.6) assign 0.3.4 (ไฟล์สุ่ม) log `OTA image has invalid magic byte (expected 0xE9, saw 0x3b)` ตามด้วย `state FAILED, error invalid_image` ภายใน 1.2 วินาที unassign แล้ว assign 0.3.4 ซ้ำ บอร์ดไม่ดาวน์โหลด (ไม่มี `update offered` ใน 40 วินาที) แปลว่ารหัส final ถูกจำไว้ เรื่องเวอร์ชัน: image ทดสอบ 0.3.7 ที่ build หลังแก้มีสตริง `v0.3.7` และไม่มี `v0.3.2` แล้ว
- [/] **เก็บกวาด** — unassign แล้วลบ package ทดสอบ 0.3.3, 0.3.4, 0.3.5, 0.3.6 ออกจาก ThingsBoard (ผู้ใช้อนุญาต) บอร์ดอยู่ที่ 0.3.2 (e9f9a92) ไม่มี firmware ผูกไว้

## รอบ 7: ทนทานและ ThingsBoard ล่ม (milestone 6)

firmware e9f9a92 (0.3.2 flash ทาง USB วันที่ 2026-11-21) ไม่แก้เฟิร์มแวร์ในรอบนี้ ตาม Task 2 ของ `docs/superpowers/plans/2026-11-21-thingsboard-soak-m6.md` งบตาม spec ข้อ 6.7: `heap_free` ไม่ต่ำกว่า 100 KB และ HWM ทุกตัว (`main_stack_hwm`, `hwm_alarm`, `hwm_uplink`, `hwm_dry_run`, `tb_hwm`, `tb_rpc_hwm`) ไม่ต่ำกว่า 512

- [/] **L0 เตรียม** — เปิด monitor ที่ COM12 เปิดตัวคุมการทดสอบ (ผู้ทดสอบพิมพ์รหัส ThingsBoard เอง) และเปิดสคริปต์ที่เช็ค `GET http://192.168.1.34/` ทุก 30 วินาที บันทึกสถานะและเวลาลงไฟล์
  - **ต้องเห็น:** banner `v0.3.2` บูตจาก `ota_0` สถานะ VALID ไม่มี firmware ผูกไว้ใน ThingsBoard ลิงก์ connected สคริปต์เช็คได้ 200
  - บันทึก: 2026-11-21 07:21 บอร์ดบูต e9f9a92 (`v0.3.2`, `ota_0` VALID) มาแล้ว 44 นาที ThingsBoard `current_fw_version` 0.3.2 ไม่มี firmware ผูก (package ทดสอบลบแล้ว) เก็บ log COM12 ด้วยสคริปต์ pyserial ที่ใส่เวลาเครื่องทุกบรรทัด (DTR/RTS ต่ำ บอร์ดไม่รีเซ็ต uptime ต่อเนื่อง) แทน idf monitor ตัวเช็คหน้าเว็บเริ่ม 07:21:49 ได้ 200 ค่าเริ่ม `heap_free` 145.8 KB `heap_min` 107.5 KB tasks 23 HTTP_DIAG `failed=1` (มาจากคำสั่งเช็คครั้งแรกที่ปิดการเชื่อมต่อก่อนอ่าน `GET /` จบ ใช้เป็นฐาน) ผู้ทดสอบพิมพ์รหัส ThingsBoard และ login เว็บแอปใน browser pane เอง
- [/] **L1 TLS ผิดพอร์ต** — บนการ์ด ThingsBoard ติ๊ก TLS host `192.168.1.51` port `1883` แล้วบันทึก รอดู backoff อย่างน้อย 3 รอบ แล้วเอาติ๊ก TLS ออกและบันทึก
  - **ต้องเห็น:** การ์ดขึ้น error ไม่ใช่ "connected" ลองใหม่ห่างขึ้น 5 → 10 → 20 วินาที บรรทัด `E` ของ `tb_link` มีแค่ error ของ mqtt/transport ไม่รีบูต สคริปต์เช็คได้ 200 ตลอด หลังปิด TLS ต่อติดภายใน 10 วินาที และมี `uplink: attribute … sent` ครบ 6 บรรทัดภายใน 2 วินาทีหลัง connected
  - บันทึก: บันทึก TLS on ที่ 07:26:09 ลิงก์ไป wait_wifi ทันที ทุกครั้งที่ต่อได้ `mbedtls_ssl_handshake returned -0x7280` ตามด้วย `mqtt: transport error` (ระดับ W ของ `tb_link` บรรทัด E มาจาก esp-tls/transport/mqtt_client เท่านั้น) backoff 5 → 10 → 20 → 40 วินาที การ์ดขึ้น "เชื่อมต่อไม่ได้ · ลองใหม่ใน N วิ" และ "mqtt: transport error" ไม่รีบูต (ข้อความนี้เหมือนกับตอน host ผิดใน L2 การ์ดไม่ได้บอกว่าเป็นเรื่อง TLS จึงยังไม่ถึง "error ที่ชัดเจน" ของ spec ข้อ 6.6 ดู backlog) ปิด TLS แล้วบันทึก ต่อติดที่ 07:26:55 (ประมาณ 6 วินาทีหลังกด) attribute ครบ 6 ตัว (`cfg_pump`, `cfg_cooling`, `cfg_protection`, `hardware_map`, `wifi`, `tb_link`) ภายใน 0.21 วินาทีหลัง connected หน้าเว็บ 200 ตลอด
- [/] **L2 I7 ปิดลิงก์แล้วต่อใหม่** — เปลี่ยน host เป็น `192.168.1.250` แล้วบันทึก รออย่างน้อย 60 วินาที แล้วเปลี่ยนกลับเป็น `192.168.1.51`
  - **ต้องเห็น:** ระหว่าง host ผิด backoff ขึ้นไปถึง 60 วินาที ไม่รีบูต หลังคืนค่าต่อติด ส่ง attribute ครบ 6 ตัวใหม่ และ `uptime` ล่าสุดใน ThingsBoard ขยับอีกภายใน 30 วินาที
  - บันทึก: host `192.168.1.250` บันทึกที่ 07:27:32 แต่ละรอบ connect timeout 10 วินาทีแล้ว `transport error` backoff 5 → 10 → 20 → 40 → 60 วินาที (60 ที่ 07:29:42) ไม่รีบูต คืน host `192.168.1.51` ที่ประมาณ 07:29:53 ลิงก์ไป wait_wifi ที่ 07:29:57.7 connected 07:29:59 attribute ครบ 6 ตัวภายใน 0.9 วินาที `uptime_sec` ใน ThingsBoard ขาดตั้งแต่ 07:27:08 แล้วมาใหม่ที่ 07:30:17 (18 วินาทีหลังต่อติด) หน้าเว็บ 200 ทุกครั้ง HTTP_DIAG `failed` ยัง 1 ข้อสังเกต: หลังกดบันทึกขณะอยู่ใน backoff ลิงก์เริ่มใหม่ช้ากว่าตอนที่ connected ได้ถึงประมาณ 4–5 วินาที ยังอยู่ในเกณฑ์ 10 วินาที สาเหตุยังไม่ได้ยืนยัน (task ตื่นทุก 1 วินาทีและการบันทึกปลุก task ทันที ที่น่าจะเป็นคือ `esp_mqtt_client_destroy` ใน `task_teardown` รอ client ที่กำลังรอ backoff)
- [/] **L3 ThingsBoard ล่ม** — ก่อนเริ่ม: อ่านกฎ id 19 ด้วย `alarms.rules.get` จดค่าไว้ แล้วตั้งด้วย `alarms.rules.set` เป็น `signal` `relay1_on` `condition` `is_true` `enabled` true ล่ม: ผู้ทดสอบรัน `docker stop tb-node` บน copter (ขอ OK ก่อน) รออย่างน้อย 3 นาที จดเวลาบอร์ดที่กฎ 19 เกิดและหายจาก serial log กลับมา: ผู้ทดสอบรัน `docker start tb-node` จบแล้วคืนกฎ 19 เป็นค่าที่จดไว้ (ปิด) และอ่าน `alarms.rules.get` ยืนยัน
  - **ต้องเห็น:** ก่อนล่ม alarm `rule_19` เกิดและหายในรอบปั๊มเดียว ระหว่างล่ม ลองต่อใหม่ห่างสุด 60 วินาที ไม่รีบูต (uptime ใน BOARD_DIAG เพิ่มต่อเนื่อง) PUMP_DIAG เปลี่ยน phase ตามรอบ สคริปต์เช็คได้ 200 `heap_free` ไม่ต่ำกว่า 100 KB ไม่มี `unsent alarm event(s) overwritten` หลังกลับมา ต่อติดภายใน 60 วินาทีหลัง ThingsBoard ตอบที่ 8080 timeseries `alarm_event` ช่วงล่มมีครบทุก seq เรียงตามลำดับ `ts` ห่างจากเวลาใน serial log ไม่เกิน 2 วินาที ประวัติ alarm `rule_19` ใน ThingsBoard มีเกิดและหายตรงกัน ส่ง attribute ใหม่ครบ
  - บันทึก: กฎ 19 เดิม `{"name":"ๆไๆไๆไ","enabled":false,"signal":"pump_is_running","condition":"is_true","hysteresis":0,"delay_sec":0,"severity":"warning","latch":false}` ตั้งเป็น `relay1_on is_true` เปิด ที่ 07:31:13 (RPC 200 ใน 1.2 วินาที) seq 1 เกิด 07:31:18 seq 2 หาย 07:32:08 ThingsBoard มี alarm `rule_19` สร้างและเคลียร์ตรงกัน (รอบปั๊มจริงคือ ON 50 / OFF 60 วินาที ประมาณ 1.1 event ต่อนาที) ผู้ทดสอบรัน `docker stop tb-node` ลิงก์หลุด 07:45:19 (`transport_read(): EOF`) ThingsBoard ล่มจริงประมาณ 38 นาที (กลับมาต่อติด 08:23:18) นานกว่าแผน 3 นาทีมาก เพราะรอผู้ทดสอบ ระหว่างล่ม: ลองต่อใหม่ 5 → 10 → 20 → 40 → 60 วินาทีแล้วคงที่ 60 (error มีแค่ `Connection reset by peer` ระดับ transport) ไม่รีบูต (uptime 4047 → 6311 วินาทีต่อเนื่อง) PUMP_DIAG สลับ ON/OFF ตามรอบตลอด หน้าเว็บ 83 ครั้งได้ 200 ทั้งหมด (ช้าสุด 0.51 วินาที) `heap_free` ต่ำสุด 144.4 KB `heap_min` 107.5 KB tasks 23 event เกิด seq 17–57 ระหว่างล่ม (41 ตัว) ring 32 ช่องเต็มตั้งแต่ seq 49 (08:15:18) log `1 unsent alarm event(s) overwritten` ครั้งละตัว 9 ครั้ง (seq 49–57) เสีย seq 17–25 ที่เก่าที่สุด (30 นาทีแรกของการล่ม 07:45:19–08:15:18 ไม่มี `overwritten` เลย ล่ม 3 นาทีตามแผนจึงไม่เสีย event) หลังกลับมา: connected 08:23:18 ในรอบลองใหม่ 60 วินาที รอบก่อนหน้า (08:22:16) ยังได้ `Connection reset by peer` (พอร์ตยังปิด) ไม่มี connect ที่ถูกปฏิเสธหรือ auth error ระหว่างที่ ThingsBoard กำลังขึ้น จึงต่อติดไม่เกิน 62 วินาทีหลัง broker เริ่มรับ (เวลาที่ 8080 เริ่มตอบไม่ได้วัด เพราะไม่ได้ดู `docker logs` ของ copter) แล้ว backfill seq 26–57 ภายใน 2 วินาที (08:23:19.45–08:23:21.03) ThingsBoard มี alarm `rule_19` 16 ตัวสำหรับคู่เกิด/หาย seq 27/28 ถึง 57/58 เรียงตาม seq (seq 26 เป็น cleared ของ alarm ที่ seq 25 ถูกเขียนทับไปแล้ว จึงไม่มีผล) `last_alarm_event` = `{boot_id 2959466143, seq 59}` เวลา: `startTs` ของ alarm คือ `ts` ที่บอร์ดส่ง (เวลาเกิดจริง ไม่ใช่เวลาที่ต่อกลับ) เช่น seq 27 `startTs` 07:55:07.559 เทียบเวลาบอร์ดจาก tick ใน serial log 07:55:07.93 ต่างประมาณ 0.4 วินาที seq 57 ต่าง 0.2 วินาที (เทียบ 2 ตัวอย่าง ทั้งคู่เป็น raised) ส่วนเวลา cleared เป็นเวลาของ ThingsBoard ตอนรับ (ข้อจำกัดของ clear alarm node) ตอนต่อติด: attribute ครบ 6 ตัว แต่ `rules attribute send failed: ESP_FAIL` 2 ครั้งและ `status send failed` 1 ครั้ง เพราะ outbox เต็มด้วย event ที่ backfill rules ลองใหม่ทุก tick (`want_rules` ค้างจนส่งได้) ไม่มี failure หลัง 08:23:20 และ `uptime_sec` มาใหม่ทุก 30 วินาที คืนกฎ 19 ด้วย `alarms.rules.set` ที่ 08:25:46 อ่านกลับจากเว็บแอปตรงค่าเดิม (alarm ที่ seq 59 ค้างอยู่ถูกเคลียร์ด้วย seq 60 ที่ 08:25:19 ก่อนคืนกฎ) หมายเหตุ: ThingsBoard ไม่ได้เก็บ `alarm_event` เป็น timeseries (rule chain แปลงเป็น alarm และเก็บ `last_alarm_event`) จึงตรวจจาก alarm แทน `hwm_alarm` ลดลงเป็น 2136 ในช่วง ThingsBoard ล่ม (ก่อนหน้า 2632 สาเหตุยังไม่ชัด การเขียน ring ของ task alarm เหมือนเดิมไม่ว่าจะส่งได้หรือไม่)
- [/] **L4 ทนทาน 1 ชั่วโมง** — เปิด dashboard ของ ThingsBoard ค้างใน browser pane 60 นาที (ผู้ทดสอบ login) ทุก 10 นาทียิง RPC อ่าน 10 คำสั่ง (`status.get`, `pump.status`, `cooling.status`, `sensors.get`, `alarms.status` อย่างละ 2 ครั้ง) จดค่าต่ำสุดของแต่ละตัว
  - **ต้องเห็น:** RPC ทุกคำสั่งตอบ ok ภายใน timeout 10 วินาที ตลอดชั่วโมง `heap_free` และ `heap_min` ไม่ต่ำกว่า 100 KB HWM ทุกตัวไม่ต่ำกว่า 512 จำนวน `tasks` คงที่ ไม่รีบูต `HTTP_DIAG failed=0` (หรือไม่เพิ่ม) สคริปต์เช็คได้ 200
  - บันทึก: 09:12:18–10:12:31 (uptime 9241 → 12873 วินาที) dashboard "Fishpump — ทดสอบ" เปิดค้างใน browser pane (ผู้ทดสอบ login) burst 7 รอบ ทุกรอบ 10/10 ได้ 200 ok ช้าสุดของแต่ละรอบ 1.28–1.83 วินาที (รวม 70 คำสั่ง ไม่มี timeout) ตลอดชั่วโมง `heap_free` 143.8–145.5 KB `heap_min` 107.5 KB `heap_largest` 96 KB tasks 23 คงที่ HWM ต่ำสุด `main_stack_hwm` 1012 `hwm_alarm` 2136 `hwm_uplink` 2928 `hwm_dry_run` 3204 `tb_hwm` 2988 `tb_rpc_hwm` 3448 ไม่มีรีบูต ไม่มีบรรทัด W/E ใน serial log ลิงก์ไม่หลุด (การ์ดเชื่อมต่อแล้ว 6603 วินาทีตอนจบ) HTTP_DIAG 785 request `failed=1` (ค่าเดิมจาก L0) `stack_hwm_min` 4532 หน้าเว็บ 121 ครั้งได้ 200 ทั้งหมด (ช้าสุด 0.34 วินาที)
- [ ] **L5 J4 แบบเต็ม (ถ้ามี access point ตัวที่สองใน LAN เดียวกับ ThingsBoard)** — อ่าน `wifi.profiles.get` ก่อน ส่ง `wifi.connect` ไป AP ตัวที่สองจาก ThingsBoard อ่าน `wifi.profiles.get` อีกครั้ง ถอดไฟแล้วเสียบใหม่ แล้วสลับกลับเครือข่ายบ้านด้วยวิธีเดียวกัน ถ้าไม่มี AP ตัวที่สอง เว้น `[ ]` ไว้พร้อมเหตุผล (F1 ยังไม่ได้ยืนยันบนบอร์ด)
  - **ต้องเห็น:** บอร์ด stage เครือข่ายใหม่ ต่อได้ ต่อ ThingsBoard ได้ แล้ว commit เอง รายการโปรไฟล์มี SSID ใหม่ `auto_index` ชี้ไปที่ตัวใหม่ โปรไฟล์เดิมยังอยู่ หลังถอดไฟกลับมาที่เครือข่ายใหม่
  - บันทึก: ไม่ได้ทำ ผู้ทดสอบยืนยันว่าไม่มี access point 2.4 GHz ตัวที่สองใน LAN เดียวกับ ThingsBoard (hotspot `Ggggg` ของรอบ 5 มองไม่เห็น 192.168.1.51) การแก้ F1 ส่วนที่ commit ผ่าน hook ตอนต่อ ThingsBoard ติดบนเครือข่ายใหม่ และการตั้ง `auto_index` ให้เครือข่ายใหม่ ยังไม่ได้ยืนยันบนบอร์ด (ส่วนบันทึกโปรไฟล์ยืนยันแล้วใน J4 แบบแทนของรอบ 5)
- [/] **L6 สรุปงบหน่วยความจำ** — รวมค่าต่ำสุดของ heap และ HWM แต่ละตัวตลอด L1–L4 เทียบกับงบ
  - **ต้องเห็น:** `heap_min` ไม่ต่ำกว่า 100 KB และ HWM ทุกตัวไม่ต่ำกว่า 512
  - บันทึก: ทั้งรอบ (07:21–10:13, uptime ถึง 3.6 ชั่วโมง รวม TLS ผิด, host ผิด, ThingsBoard ล่ม 38 นาทีพร้อม backfill 32 event และ soak) ต่ำสุด `heap_free` 143.2 KB (BOARD_DIAG ช่วง 07:23–07:24 ระหว่าง login เว็บแอป) `heap_min` 107.5 KB (งบ 100 KB) `main_stack_hwm` 1012 `hwm_alarm` 2136 `hwm_uplink` 2928 `hwm_dry_run` 3204 `tb_hwm` 2988 `tb_rpc_hwm` 3448 (งบ 512 ทุกตัว) tasks 23 ตลอด หน้าเว็บ 343 ครั้งได้ 200 ทั้งหมด ผ่านงบทุกข้อ ค่า `hwm_alarm` ต่ำสุดของรอบนี้ (2136) ต่ำกว่ารอบก่อน (2600–2712) เกิดในช่วง ThingsBoard ล่ม (สาเหตุยังไม่ชัด) ยังเหลือเกิน 4 เท่าของงบ

- [/] **หลังรอบ 7** — host `192.168.1.51` TLS ปิด ลิงก์ connected (การ์ด "เชื่อมต่อแล้ว · 6603 วิ" ส่งแล้ว 846 ล้มเหลว 3 คือ 3 ครั้งตอน backfill ใน L3) กฎ 19 คืนค่าเดิม (`pump_is_running`, ปิด) อ่านกลับจากเว็บแอปแล้ว ตัวคุมการทดสอบ quit ตัวเช็คหน้าเว็บและตัวเก็บ serial log หยุดแล้ว

## ครอบคลุม spec ข้อ 8

ข้อ bench ของ spec ข้อ 8 (`docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`) กับขั้นที่ทดสอบ:

- ลงทะเบียนจากค่าว่าง — B1 (และลงทะเบียนซ้ำ B3)
- telemetry และ event ที่มี `ts` ถูก หลัง SNTP — B2, B6 · หลังหลุด 2 นาที — B7 · หลัง ThingsBoard ล่มนาน — L3 (`startTs` ของ alarm ตรงเวลาบอร์ดไม่เกิน 0.4 วินาทีใน 2 ตัวอย่าง)
- parity script — A4, F4, H4, I2, I3, J1, K1
- เดิน dashboard ทุกหน้า — A5, F5, I6 (และปุ่ม Wi-Fi J5)
- เปลี่ยน Wi-Fi จากระยะไกล รหัสผิดแล้วย้อนกลับ — J2, J3 · สำเร็จ — J4 แบบแทนเท่านั้น **ยังค้าง** J4 แบบเต็ม (L5) เพราะไม่มี access point ตัวที่สองใน LAN เดียวกับ ThingsBoard
- OTA สำเร็จ — K2, K5 · อิมเมจเสีย — K3 (ไฟล์สุ่ม ไม่ใช่ checksum ผิด เพราะ ThingsBoard ปฏิเสธ checksum ผิดตั้งแต่ upload) และ K3 ซ้ำหลังแก้ **ยังค้าง** ทาง `checksum_mismatch` ของบอร์ด (`tb_ota.c`) ยังไม่เคยรันบนบอร์ดจริง · อิมเมจที่ไม่ต่อ ThingsBoard ย้อนกลับใน 5 นาที — K4
- ThingsBoard ล่ม (`docker stop tb-node` ด้วย OK ของผู้ใช้) งานในบอร์ดไม่สะดุด ต่อกลับและ backfill — L3 (ต่อติดในรอบแรกหลังพอร์ตเปิด เวลาที่ 8080 พร้อมไม่ได้วัด)
- heap และ HWM ตลอด soak 1 ชั่วโมงที่เปิด dashboard — L4, L6 (และ B8, F7, I8, J6, K7)
- นอกข้อ 8 แต่ spec ข้อ 6.6 กำหนด: TLS ชี้พอร์ตที่ไม่มี TLS ต้องล้มอย่างเรียบร้อย — L1 (ล้มเรียบร้อย แต่การ์ดขึ้นแค่ "mqtt: transport error" แบบเดียวกับ host ผิด **ยังค้าง** error ที่บอกว่าเป็นเรื่อง TLS)

## หลังทดสอบ

- [/] ปิดกฎที่เปิดไว้ทดสอบ ให้ลิงก์ยังตั้งค่าอยู่ และ host เป็น `192.168.1.51` — กฎ id 19 คืนเป็น `pump_is_running` และปิดแล้ว (ตรวจจาก attribute `alarm_rules` ใน ThingsBoard) host = `192.168.1.51` ลิงก์ connected
