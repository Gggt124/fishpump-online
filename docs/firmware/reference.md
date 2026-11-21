# เอกสารอ้างอิงเฟิร์มแวร์ fishpump (เวอร์ชัน 0.4.0)

เอกสารนี้อธิบายว่าเฟิร์มแวร์ปัจจุบันทำอะไร แบ่งเป็นส่วนไหน และส่วนต่างๆ คุยกันอย่างไร
รายละเอียดของ API ทุก method, attribute และ telemetry ของ ThingsBoard อยู่ใน [`device-api.md`](device-api.md)
ขั้นตอน build, flash และการตั้งเวอร์ชันอยู่ใน [`../firmware-workflow.md`](../firmware-workflow.md)

---

## 1. ฮาร์ดแวร์และเครื่องมือ

- บอร์ด: ESP32-S3 WROOM-1 N16R8 (flash 16 MB, PSRAM แบบ octal 8 MB)
- ESP-IDF v6.0.1
- เวอร์ชันเฟิร์มแวร์มาจาก `firmware/version.txt` บรรทัดเดียว `CMakeLists.txt` ส่งต่อให้ `app_config` (ไม่มีค่าสำรองเขียนตายตัว)
- พาร์ทิชัน (`firmware/partitions.csv`): NVS 24 KB, app สองช่อง `ota_0` และ `ota_1` ช่องละ 3.5 MB (สำหรับอัปเดตแบบย้อนกลับได้), `nvs_keys` และ `coredump`

### ขาที่ใช้ (ค่าเริ่มต้น)

ขาของรีเลย์ ลูกลอย และ MAX31865 CS เปลี่ยนได้ที่หน้า Hardware ของเว็บแอปในเครื่องเท่านั้น (เปลี่ยนจาก ThingsBoard ไม่ได้) ขา UART เป็นค่าคงที่

| หน้าที่ | ขา |
|---|---|
| รีเลย์ 1: ปั๊มน้ำ | GPIO 5 |
| รีเลย์ 2: ปั๊มล้าง (ตั้งเวลาแยก) | GPIO 6 |
| ลูกลอย (เลือกชุดเวลาปั๊มน้ำ 1 หรือ 2) | GPIO 16 |
| flow switch (ต่อ GND = น้ำไหล) | GPIO 9 |
| PT100 ผ่าน MAX31865 (SPI2) | CS 10, MOSI 11, CLK 12, MISO 13 |
| สัญญาณสั่ง inverter ของ Chiller | GPIO 8 |
| PZEM-004T ของปั๊มน้ำ (UART0, 9600) | TX 4, RX 42 |
| PZEM-004T ของ Chiller (UART1, 9600) | TX 40, RX 41 |
| RS485 → pH SEN0708 (UART2, 4800) | TX 21, RX 14 |
| ปุ่ม BOOT | GPIO 0 |
| LED RGB บนบอร์ด / LED ภายนอก | GPIO 48 / GPIO 39 |

---

## 2. สิ่งที่บอร์ดทำ

- **ปั๊มน้ำ (รีเลย์ 1):** เดินตามตารางเปิด/ปิด มีสองชุดเวลา ลูกลอยเป็นตัวเลือกว่าใช้ชุดไหน สั่ง start/stop และโหมดทดสอบรีเลย์ได้จากเว็บและ ThingsBoard
- **ปั๊มล้าง (รีเลย์ 2):** ตั้งเวลาแยก เริ่มเองหรือสั่งเองได้
- **ระบบทำความเย็น:** อ่านอุณหภูมิน้ำจาก PT100 แล้วคำนวณแรงดันสั่ง inverter ของ Chiller แบบเส้นตรง (ค่าเริ่มต้น 24.0 °C → 5 V, 35.0 °C → 10 V) มีโหมด auto, ปิดบังคับ และทดสอบ (หมดเวลาเองใน 30 วินาที) และเวลาพักขั้นต่ำ 180 วินาทีก่อนเปิดใหม่
- **ป้องกันปั๊มเดินแห้ง:** ถ้ารีเลย์ปั๊มน้ำเปิดแต่ flow switch ไม่เห็นน้ำไหลเกินเวลาที่ตั้ง บอร์ดตัดปั๊ม รอแล้วลองใหม่แบบ backoff (เช่น 60 → 120 → 240 วินาที) ครบจำนวนครั้งแล้วล็อกจนกว่าจะกดรับทราบ การล็อกอยู่รอดการรีบูต มีปุ่ม "วัดให้" สำหรับวัดเวลาล่อน้ำจริงของท่อ
- **เซนเซอร์:** PZEM-004T สองตัว (ปั๊มน้ำและ Chiller: V, A, W, kWh, Hz, PF), pH และอุณหภูมิจาก SEN0708 (คาลิเบรต 2 จุดและ offset), flow switch, ลูกลอย และ PT100 เซนเซอร์ที่ไม่ตอบขึ้น fault บนการ์ด และ log ครั้งเดียวตอนเสียและตอนกลับมา
- **แจ้งเตือน:** กฎได้ถึง 24 ข้อ มี preset 13 ข้อ (ปิดไว้ตอนเริ่ม) แต่ละกฎมีสัญญาณ เงื่อนไข เวลาหน่วง ช่วงกันสั่น เงื่อนไข "เฉพาะตอน" (gate) ระดับ (วิกฤต, เตือน, ข้อมูล) และแบบค้างรอรับทราบ (latch) ประวัติล่าสุด 32 รายการ LED แสดงระดับสูงสุดที่ยังไม่รับทราบ หน้าเว็บมีกระดิ่ง และ overlay พร้อมเสียงสำหรับเหตุวิกฤต
- **เว็บแอปในเครื่อง (ภาษาไทย):** login, แผงควบคุม, ปั๊ม, ระบบทำความเย็น, เซนเซอร์, แจ้งเตือน, Wi-Fi, Hardware, การ์ด ThingsBoard และอัปโหลดเฟิร์มแวร์ ไฟล์เว็บถูกบีบอัด gzip ตอน build
- **Wi-Fi:** โหมด AP `FishPump-Setup` (192.168.4.1 พร้อม captive portal) และโหมด STA เก็บโปรไฟล์ได้ 5 เครือข่าย การเปลี่ยนเครือข่ายใช้ staging คือถ้าไม่ยืนยันภายใน 180 วินาทีจะย้อนกลับเครือข่ายเดิมเอง
- **ThingsBoard:** ดูข้อ 5

**หลักสำคัญ:** กฎแจ้งเตือนและระบบป้องกันทั้งหมดอยู่ในเฟิร์มแวร์ ทำงานได้แม้ไม่มีอินเทอร์เน็ตหรือ ThingsBoard ล่ม ThingsBoard มีหน้าที่เก็บประวัติ แสดงผล และส่งคำสั่ง

---

## 3. Component

อยู่ใน `firmware/components/` ส่วน `firmware/main/` เป็นตัวประกอบและ handler

| กลุ่ม | Component | หน้าที่ |
|---|---|---|
| แพลตฟอร์ม | `app_config` | เวอร์ชันและค่าคงที่ตอน compile |
| | `hardware_map` | ตารางขา บทบาทของขา ขั้วรีเลย์ ขา UART คงที่ |
| | `relay_safe_init` | ปิดรีเลย์ทุกตัวตั้งแต่คำสั่งแรกหลังรีเซ็ต |
| | `nvs_store` | เก็บค่าตั้งทั้งหมดใน NVS (Wi-Fi, ปั๊ม, ความเย็น, กฎ, ป้องกันเดินแห้ง, ThingsBoard, OTA) |
| | `wifi_manager`, `dns_server`, `session` | AP/STA, captive portal, session ของการ login |
| | `telemetry_hub` | snapshot กลางของค่าเซนเซอร์และสถานะ ให้ส่วนอื่นอ่านโดยไม่ต้องรู้จักกัน |
| | `time_sync` | SNTP (`pool.ntp.org`, `time.google.com`) เพื่อประทับเวลาเหตุการณ์ |
| เซนเซอร์ | `pzem004t`, `power_monitor` | driver Modbus และ task อ่านมิเตอร์ 2 ตัว |
| | `sen0708`, `water_quality` | driver RS485 และ task อ่าน pH พร้อมคาลิเบรต |
| | `flow_switch`, `contact_debounce` | อินพุต flow switch และตัวกันสั่น |
| | `sensor_health` | ติดตาม fault และการกลับมาของเซนเซอร์ |
| | `signal_registry` | สัญญาณที่มีชื่อและชนิดบน snapshot ใช้ร่วมกันโดยกฎแจ้งเตือนและ uplink |
| ควบคุม | `pump_control` | ตารางปั๊มน้ำ ปั๊มล้าง ลูกลอย โหมดทดสอบรีเลย์ |
| | `cooling_control` | PT100, คำนวณแรงดัน inverter, โหมด, fault |
| | `dry_run_logic`, `dry_run_guard` | สูตรตัดสินใจ (ทดสอบบน QEMU ได้) และ task ป้องกันเดินแห้ง |
| แจ้งเตือน | `alarm_rules` | โมเดลกฎ ตรวจความถูกต้อง แปลง JSON |
| | `alarm_engine` | ประเมินกฎ เก็บประวัติ รับทราบ สี LED preset |
| API | `device_api`, `dapi_guards` | ตาราง method เดียวที่ทั้งเว็บและ ThingsBoard เรียก พร้อมตัวตรวจ input |
| ThingsBoard | `tb_link`, `tb_link_logic` | MQTT, ลงทะเบียนอุปกรณ์, backoff, RPC, ข้อความ error บนการ์ด |
| | `uplink`, `uplink_format` | ส่ง telemetry, เหตุการณ์แจ้งเตือน (ring 32 ช่อง), identity และกฎ |

ใน `firmware/main/`:
- `app_main.c`: ลำดับการเริ่มระบบ, watchdog, task LED/ปุ่ม, log วินิจฉัย `BOARD_DIAG`
- `web_server.c`, `api_json.c`: HTTP server, login, SSE, อัปโหลดเฟิร์มแวร์
- `dapi_*.c`: method ของ `device_api` แยกตามกลุ่ม (status, pump, cooling, protection, alarms, water/power, hardware, wifi)
- `tb_state.c`: ส่งค่าตั้งเป็น client attribute และ telemetry สถานะ
- `tb_ota.c`, `ota_lock.c`: อัปเดตเฟิร์มแวร์จาก ThingsBoard และกุญแจให้มีผู้เขียน OTA ได้ทีละราย (เว็บหรือ ThingsBoard)
- `wifi_remote.c`: ยืนยันเครือข่ายใหม่ที่สั่งจาก ThingsBoard เมื่อต่อ ThingsBoard ติด
- `static/`: ต้นฉบับเว็บแอป, `static_compressed/`: ไฟล์ gzip ที่ฝังลงเฟิร์มแวร์ (สร้างด้วย `scripts/build.ps1`)

---

## 4. ลำดับการเริ่มระบบและ task

ลำดับใน `app_main`:
1. ปิดรีเลย์ทุกตัว (`relay_safe_init`)
2. NVS และ `telemetry_hub`
3. ตั้ง task watchdog (10 วินาที)
4. ปั๊ม ความเย็น และเซนเซอร์ (`power_monitor`, `water_quality`, `flow_switch`)
5. `alarm_engine`, `dry_run_guard`, `uplink`
6. session, Wi-Fi, SNTP, `tb_ota` (เช็กว่าบูตจากการอัปเดตของ ThingsBoard หรือไม่), `tb_link`
7. `device_api`, `tb_state`, web server, mDNS, DNS ของ captive portal
8. task LED และปุ่ม

task หลัก: `cooling_control`, `power_mon`, `water_q`, `flow_sw`, `alarm_eng`, `dry_run`, `uplink`, `tb_link`, `tb_rpc`, task ของ esp-mqtt, `dns_server`, `hardware_ui_task` และ `tb_ota` (เฉพาะตอนอัปเดต)

**งบหน่วยความจำ** (วัดบนบอร์ดระหว่างทดสอบ 1 ชั่วโมงที่เปิด dashboard และยิง RPC): heap ภายในต่ำสุด 107.5 KB (งบ ≥ 100 KB) HWM ทุก task ต่ำสุด 1012 ไบต์ (งบ ≥ 512) มี 23 task log `BOARD_DIAG` พิมพ์ค่าเหล่านี้เป็นระยะ

---

## 5. ThingsBoard

**การเชื่อมต่อ**
- ตั้งค่าที่การ์ด "ThingsBoard" ในเว็บแอป: host, port (ค่าเริ่มต้น 1883), TLS, CA, ชื่ออุปกรณ์ (`fishpump-<MAC 3 ไบต์ท้าย>`), provision key และ secret ปล่อย host ว่างคือปิดลิงก์
- ครั้งแรกบอร์ดลงทะเบียนตัวเองด้วย provision key/secret ได้ token มาเก็บไว้ หรือจะพิมพ์ token เองก็ได้
- หลุดแล้วต่อใหม่เองแบบ backoff 5 → 10 → 20 → 40 → 60 วินาที ลิงก์ล้มไม่ทำให้ส่วนอื่นค้างหรือรีบูต
- การ์ดบอกสถานะ, error ล่าสุด, เวลาที่ต่ออยู่, จำนวนข้อความที่ส่งได้และล้มเหลว error ของ TLS แยกให้เห็นชัด (`tls: handshake failed ...`, `tls: server certificate not trusted`)

**สิ่งที่ส่งขึ้นไป**
- telemetry สัญญาณ 37 ตัวพร้อมสรุปแจ้งเตือน ทุก 30 วินาที
- telemetry สถานะ (phase ของปั๊ม, สถานะป้องกัน, โหมดความเย็น, RSSI, heap, HWM ฯลฯ) ทุก 30 วินาที และทันทีเมื่อเปลี่ยน (ห่างอย่างน้อย 2 วินาที)
- client attribute ค่าตั้งปัจจุบัน 6 ตัว (`cfg_pump`, `cfg_cooling`, `cfg_protection`, `wifi`, `hardware_map`, `tb_link`) ตอนต่อติดและภายใน 2 วินาทีหลังมีการแก้จากทางไหนก็ได้ ไม่มีรหัสผ่าน
- เหตุการณ์แจ้งเตือน (เกิด/หาย) พร้อมเวลาจริงของบอร์ด ถ้าลิงก์หลุด เก็บไว้ใน ring 32 ช่องแล้วส่งตามลำดับเมื่อต่อกลับ rule chain ของ ThingsBoard แปลงเป็น alarm `rule_<id>` และตัดข้อมูลซ้ำด้วย `last_alarm_event`

**คำสั่งจาก ThingsBoard**
- ทุกคำสั่งเป็น RPC แบบสองทาง เรียก method เดียวกับเว็บแอปผ่าน `device_api` จึงได้ผลและรหัส error เดียวกัน ผ่านตัวตรวจเดียวกัน (เช่นล็อกเดินแห้ง 409, ช่วงค่า, การตรวจกฎ)
- RPC ที่ส่งซ้ำภายใน 30 วินาทีได้คำตอบเดิมจากแคช ไม่ทำซ้ำ
- เปลี่ยนขาและรหัสผ่านเว็บจาก ThingsBoard ไม่ได้ (อ่านได้อย่างเดียว)
- **เปลี่ยน Wi-Fi:** ใช้ staging เดียวกับเว็บ บอร์ดยืนยันเองเมื่อต่อ ThingsBoard ติดบนเครือข่ายใหม่ ไม่ติดภายใน 180 วินาทีจะย้อนกลับ
- **อัปเดตเฟิร์มแวร์ (OTA):** ใช้ OTA ของ ThingsBoard (`fw_title` ต้องเป็น `fishpump` และเวอร์ชันต้องต่างจากที่รันอยู่) โหลดเป็นช่วงละ 16 KB ตรวจ SHA-256 แล้วรีบูต อิมเมจใหม่ต้องต่อ ThingsBoard ติดภายใน 5 นาที ไม่งั้นย้อนกลับเวอร์ชันเดิมเอง ระหว่างวัดเดินแห้งจะรอให้วัดเสร็จก่อนรีบูต

รายละเอียด key, method และรหัส error: [`device-api.md`](device-api.md) ข้อ 9–12 · ฝั่งเซิร์ฟเวอร์: [`thingsboard/README.md`](../../thingsboard/README.md)

---

## 6. ความปลอดภัยของการควบคุม

- รีเลย์ปิดตั้งแต่รีเซ็ตและทุกครั้งที่รีบูต
- ป้องกันเดินแห้งตัดปั๊มเองโดยไม่ต้องพึ่งเน็ต และล็อกค้างข้ามการรีบูต
- โหมดทดสอบรีเลย์ 1 มีเพดาน 130 วินาทีเป็นตัวกันสุดท้าย
- คำสั่งจากระยะไกลผ่านตัวตรวจเดียวกับเว็บเสมอ
- เปลี่ยน Wi-Fi และอัปเดตเฟิร์มแวร์จากระยะไกลย้อนกลับเองได้ ถ้าบอร์ดติดต่อกลับไม่ได้
- เว็บ API ทุกตัวต้อง login และตรวจ origin ส่วนการ์ด ThingsBoard ใช้ได้เฉพาะในเครื่อง

---

## 7. การทดสอบ

| สิ่งที่ทดสอบ | คำสั่ง |
|---|---|
| build เฟิร์มแวร์ | `idf.py -C firmware build` |
| self-test บน QEMU (2108 checks: ตรรกะของเซนเซอร์, กฎ, เดินแห้ง, device_api, uplink, tb_link, OTA) | `firmware/test/sensor_selftest/run_selftest.ps1` |
| ตรวจโค้ดตามแต่ละ feature | `python firmware/scripts/verify_<ชื่อ>.py` (`tb_link`, `device_api`, `alarm_system`, `dry_run`, `pre_mqtt`, `sensor_integration`, `pt100_inverter_system`) |
| เว็บ API กับ RPC ตอบตรงกัน (ต้องมีบอร์ดและ ThingsBoard) | `python thingsboard/parity_test.py` (`--selftest` ตรวจแบบออฟไลน์) |
| คำตอบของเว็บ API ไม่เปลี่ยนหลังแก้ handler | `python thingsboard/api_golden.py` |
| ทดสอบบนบอร์ดที่ยังค้าง | [`../bench-test-pending.md`](../bench-test-pending.md) |
