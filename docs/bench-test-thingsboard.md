# คู่มือทดสอบบนบอร์ด: การเชื่อมต่อ ThingsBoard (MQTT)

Firmware: branch `feature/thingsboard-mqtt` (commit `c058d1a`..`45a5fe8`)
Spec: `docs/superpowers/specs/2026-11-18-thingsboard-mqtt-link-design.md`
Plan: `docs/superpowers/plans/2026-11-18-thingsboard-link-m1.md` (Task 7)
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

## หลังทดสอบ

- [/] ปิดกฎที่เปิดไว้ทดสอบ ให้ลิงก์ยังตั้งค่าอยู่ และ host เป็น `192.168.1.51` — กฎ id 19 คืนเป็น `pump_is_running` และปิดแล้ว (ตรวจจาก attribute `alarm_rules` ใน ThingsBoard) host = `192.168.1.51` ลิงก์ connected
