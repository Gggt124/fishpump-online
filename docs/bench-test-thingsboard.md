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

## หลังทดสอบ

- [/] ปิดกฎที่เปิดไว้ทดสอบ ให้ลิงก์ยังตั้งค่าอยู่ และ host เป็น `192.168.1.51` — กฎ id 19 คืนเป็น `pump_is_running` และปิดแล้ว (ตรวจจาก attribute `alarm_rules` ใน ThingsBoard) host = `192.168.1.51` ลิงก์ connected
