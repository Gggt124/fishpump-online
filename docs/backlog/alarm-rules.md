# Backlog: ระบบแจ้งเตือน (Alarm Rules)

> สถานะ: **ทำแล้ว (2026-11-15) รอทดสอบบนบอร์ด** — ดู spec `docs/superpowers/specs/2026-11-15-alarm-rules-and-uplink-design.md`, plan `docs/superpowers/plans/2026-11-15-alarm-rules-and-uplink.md`, คู่มือทดสอบ `docs/bench-test-alarms.md`, ผลการทำงาน `docs/superpowers/results/2026-11-15-alarm-rules-and-uplink-results.md`
> (ข้อความด้านล่างเป็นโน้ตตั้งต้นเดิม ใช้ชื่อ input เก่า เช่น pump1/pump2 ซึ่งถูกเปลี่ยนเป็น pump/chiller แล้ว)
> บันทึกเมื่อ: 2026-11-15
> ขั้นต่อไป: เปิดรอบ brainstorming → เขียน spec (`docs/superpowers/specs/`) → plan → ลงมือ
> เอกสารนี้เป็น "โน้ตตั้งต้น" ไม่ใช่ spec ที่อนุมัติแล้ว ค่าทุกตัวในนี้ยังเปลี่ยนได้

---

## 1. เป้าหมาย

ระบบแจ้งเตือน **แบบทั่วไป (generic)** แยกเป็นของตัวเอง ไม่ผูกกับเซนเซอร์ตัวใดตัวหนึ่ง

- ผู้ใช้เลือก **input อะไรก็ได้** ที่ระบบมี + ตั้ง **เกณฑ์** เอง
- ค่าไหนเกินเกณฑ์ / ผิดปกติ → แจ้งเตือน
- เพิ่มเซนเซอร์ใหม่ในอนาคต → แค่ลงทะเบียน input เพิ่ม ไม่ต้องแก้ตัว engine

ที่มา: ตอนออกแบบงานเซนเซอร์ ผู้ใช้ตัดสินใจว่า "ระบบแจ้งเตือนเพิ่มแยกไปเลย ค่าไหนถึงเกณฑ์ไม่โอเคค่อยแจ้งเตือน เอาเป็น input อะไรก็ได้"
และ SEN0708 ใช้อ่าน pH อย่างเดียว ไม่มีแจ้งเตือนในตัว

---

## 2. Input ที่มีอยู่แล้วในระบบ (ณ commit `e9ea4ec`)

ทุกค่าวิ่งผ่าน `telemetry_hub` อยู่แล้ว → alarm engine subscribe ที่เดียวได้ครบ
(`firmware/components/telemetry_hub/include/telemetry_hub.h`)

| กลุ่ม | Input | หน่วย | ความถี่ | หมายเหตุ |
|---|---|---|---|---|
| ไฟปั๊ม 1 / ปั๊ม 2 (PZEM) | `voltage_v` | V | 2 s | |
| | `current_a` | A | 2 s | ตัวหลักที่อยากใช้แจ้งเตือน |
| | `power_w` | W | 2 s | |
| | `energy_wh` | Wh | 2 s | สะสม รีเซ็ตได้ |
| | `frequency_hz` | Hz | 2 s | |
| | `pf` | – | 2 s | |
| | `valid` / `fault` | bool | 2 s | fault = ไม่ตอบ ≥3 ครั้งติด |
| น้ำ (SEN0708) | `ph` | pH | ~1–2 s | |
| | `temperature_c` | °C | ~1–2 s | อุณหภูมิน้ำจากหัว pH |
| | `stable` / `valid` / `fault` | bool | | |
| Cooling (PT100) | `current_temp_c` | °C | 1 s | อุณหภูมิ inverter |
| | `is_sensor_valid` | bool | | |
| | `duty_percent`, `output_mv`, `mode`, `lockout_remaining_sec` | | | |
| ปั๊ม | `active_pump`, `is_running`, `relay_1_active`, `relay_2_active` | | ตอนเปลี่ยนสถานะ | ใช้เป็น "เงื่อนไขประกอบ" ได้ |
| ลูกลอย | `float_is_high` | bool | ตอนเปลี่ยน | |
| ระบบ | `free_heap_bytes`, `free_psram_bytes`, `wifi_rssi`, `uptime_sec`, `oom_strikes` | | เป็นระยะ | |

**สัญญาความถูกต้องของข้อมูล (validity contract, spec เซนเซอร์ §5.1a)** — engine ต้องเคารพ:
- `valid` = poll รอบนี้อ่านสำเร็จ
- `fault` = พลาด ≥3 ครั้งติด หรือ init ไม่ผ่าน
- ตัวเลขเก็บ "ค่าดีล่าสุด" ไว้ → **ห้ามเอาตัวเลขตอน fault ไปเทียบเกณฑ์** (ค่าค้างจะหลอกว่าปกติ)

---

## 3. ความต้องการ (ร่าง — รอยืนยันตอน brainstorming)

### 3.1 รูปแบบ rule
- `input` — เลือกจากรายการ input ด้านบน
- `condition` — `>`, `<`, `นอกช่วง [lo, hi]`, `ในช่วง`, `เท่ากับ` (สำหรับ bool), `เซนเซอร์ fault`, `ข้อมูลค้าง (ไม่อัปเดตเกิน N วินาที)`
- `threshold` (+ `threshold_hi` สำหรับช่วง)
- `hysteresis` — กันแจ้งเตือนกระพริบตอนค่าวนอยู่แถวเกณฑ์ (เช่น เกิน 8.5 แจ้ง, ต้องลงต่ำกว่า 8.3 ถึงหาย)
- `delay_sec` — ต้องผิดเกณฑ์ต่อเนื่องนานเท่านี้ก่อนแจ้ง (กันค่า spike ตอนปั๊มสตาร์ท)
- `severity` — info / warning / critical
- `enabled`, `name` (ภาษาไทยได้)
- ตัวเลือก **"เฉพาะตอน..."** (gate) เช่น ทำงานเฉพาะตอนปั๊ม 1 เปิดอยู่ — ดูข้อ 3.3

### 3.2 สถานะของ alarm
`ปกติ → รอยืนยัน (นับ delay) → แจ้งเตือนอยู่ → (ผู้ใช้กดรับทราบ) → หายแล้ว`
- ตัวเลือก latch: ค่ากลับปกติแล้วยังค้างจนกว่าจะกดรับทราบ (เหมาะกับ critical)

### 3.3 ตัวอย่าง rule ที่น่าจะใช้จริง
| ตัวอย่าง | input | เงื่อนไข | ทำไม |
|---|---|---|---|
| ปั๊มเดินแห้ง / ท่ออุดตัน | กระแสปั๊ม 1 | `<` X A, เฉพาะตอนปั๊ม 1 เปิด, delay 10 s | กระแสตกผิดปกติ |
| ปั๊มติดขัด / มอเตอร์หนัก | กระแสปั๊ม 1 | `>` Y A, delay 5 s | กระแสสูงเกิน |
| รีเลย์ค้าง | กระแสปั๊ม 2 | `>` 0.1 A, เฉพาะตอนปั๊ม 2 ปิด | สั่งปิดแล้วยังมีกระแส |
| ไฟตก / ไฟเกิน | แรงดัน | นอกช่วง 200–245 V | |
| pH ผิดช่วง | pH | นอกช่วง 6.5–8.5, delay 60 s | ค่าที่เหมาะกับบ่อปลา (ยืนยันกับผู้ใช้) |
| น้ำร้อนเกิน | อุณหภูมิน้ำ | `>` 32 °C | |
| Inverter ร้อน | PT100 | `>` X °C | |
| เซนเซอร์หาย | PZEM / SEN0708 / PT100 | fault | |
| ข้อมูลค้าง | sensor ใดๆ | ไม่อัปเดตเกิน 30 s | ดูข้อ 6 — task เซนเซอร์ไม่อยู่ใน TWDT |

---

## 4. ช่องทางแจ้งเตือน (ต้องตัดสินใจ)

| ช่องทาง | ต้องการอะไร | หมายเหตุ |
|---|---|---|
| หน้าเว็บ: แบนเนอร์ + badge ที่เมนู + หน้า "แจ้งเตือน" | ไม่มีเพิ่ม | ทำได้ทันที ผ่าน SSE `event: alarm` |
| ประวัติแจ้งเตือน | RAM ring buffer (+ NVS ถ้าอยากรอดรีบูต) | **ไม่มี SNTP ในระบบตอนนี้** → มีแค่ uptime ไม่มีวันเวลาจริง |
| ไฟ LED บนบอร์ด (WS2812 GPIO48) | – | กระพริบแดงตอนมี critical |
| Buzzer | GPIO ว่าง 1 ขา + hardware | ต้องเช็ค allowlist ใน `hardware_map` |
| Telegram bot | โหมด STA + อินเทอร์เน็ต + TLS | แนะนำ: ฟรี ใช้ง่าย |
| LINE Messaging API | โหมด STA + อินเทอร์เน็ต + TLS + LINE OA | **LINE Notify ปิดบริการไปแล้ว (มี.ค. 2025)** ต้องใช้ Messaging API แทน |
| Webhook / MQTT | STA + server ปลายทาง | ยืดหยุ่นสุด แต่ต้องมี server เอง |

ข้อควรรู้:
- ตอนทดสอบบอร์ดอยู่โหมด AP อย่างเดียว (FishPump-Setup) → ช่องทางผ่านอินเทอร์เน็ตใช้ไม่ได้จนกว่าจะต่อ Wi-Fi บ้านแบบ STA
- TLS ใช้ RAM ราว 40 KB ต่อการเชื่อมต่อ → ควรให้ mbedTLS ใช้ PSRAM (บอร์ดมี 8 MB)
- การส่งออกเน็ตต้องมี retry queue และ rate limit (กันส่งรัวตอน alarm กระพริบ)

---

## 5. โครงสถาปัตยกรรม (ร่าง)

```
telemetry_hub ──callback (ห้าม block)──► queue ──► alarm_engine task
                                                    │ ประเมิน rule / hysteresis / delay / gate
                                                    ├─► telemetry_hub: TELEMETRY_EVT_ALARM ─► SSE event: alarm ─► หน้าเว็บ
                                                    ├─► history ring buffer
                                                    └─► notifier task (retry queue) ─► Telegram / LINE / webhook
NVS: rules (blob มีเลขเวอร์ชัน, จำนวนจำกัด เช่น 16 rule)
REST: GET/POST/PUT/DELETE /api/alarms/rules, GET /api/alarms (active + history), POST /api/alarms/ack
```

- component ใหม่ `firmware/components/alarm_engine/` (logic ล้วน ทดสอบใน QEMU selftest ได้แบบ `sensor_health`)
- ตาราง input เป็น registry: `{id, ชื่อไทย, หน่วย, ตัวอ่านค่าจาก snapshot, ตัวอ่าน valid/fault}` → เพิ่มเซนเซอร์ใหม่ = เพิ่มแถว
- ประเมิน rule จาก `telemetry_hub_get_latest()` เป็นรอบ (เช่น ทุก 1 s) ง่ายกว่าประเมินตาม event ทีละตัว และรองรับ "ข้อมูลค้าง" ได้ตรงๆ
- POST ทุกตัวใช้ guard แบบเดียวกับงานเซนเซอร์ (auth + same-origin + JSON) — ดู `sensors_begin_post` ใน `firmware/main/web_server.c`

---

## 6. ข้อจำกัด / สิ่งที่ต้องระวัง

- callback ของ `telemetry_hub` ต้อง **ไม่ block** → ห้ามประเมินหนักหรือส่งเน็ตใน callback
- httpd task ห้ามแตะ UART / ห้ามส่งเน็ตเอง (ใช้ queue แบบงานเซนเซอร์)
- task เซนเซอร์ (`power_mon`, `water_q`) **ไม่ได้อยู่ใน TWDT** โดยตั้งใจ (เซนเซอร์ค้างต้องไม่ทำให้ตัวคุมปั๊มรีบูต) → alarm ระบบต้องมี rule "ข้อมูลค้าง" ไว้จับกรณีนี้ (`age_ms` มีให้อยู่แล้วใน `/api/sensors/status`)
- **ห้ามให้ alarm ไปสั่งปั๊ม/รีเลย์ในเวอร์ชันแรก** (ถ้าจะทำ action อัตโนมัติ เช่น ตัดปั๊มตอนเดินแห้ง ให้คุยแยกเป็นเรื่อง safety)
- ข้อความ UI/แจ้งเตือนเป็นภาษาไทย → buffer ต้องเผื่อ UTF-8 3 byte/ตัว (เคยโดน `-Werror=format-truncation` มาแล้ว)
- `/api/events` (SSE) ไม่มี auth อยู่แล้วแต่เดิม → event alarm จะเห็นได้จากทุกเครื่องใน LAN เหมือนข้อมูลอื่น

---

## 7. คำถามที่ต้องตอบตอน brainstorming

1. ช่องทางแจ้งเตือนหลักคืออะไร: หน้าเว็บอย่างเดียว / Telegram / LINE / อื่นๆ?
2. บอร์ดจะต่อ Wi-Fi บ้าน (STA) ตอนใช้งานจริงไหม? (ถ้าไม่ → ช่องทางผ่านเน็ตตัดทิ้ง)
3. ต้องการประวัติรอดรีบูตไหม? ต้องการวันเวลาจริงไหม (ต้องเพิ่ม SNTP)?
4. จำนวน rule สูงสุดที่คิดว่าใช้จริง?
5. ต้องมี buzzer / ไฟเตือนหน้าตู้ไหม?
6. ต้องการ "เฉพาะตอนปั๊มเปิด" (gate) ในเวอร์ชันแรกเลยไหม? (จำเป็นสำหรับ rule เดินแห้ง / รีเลย์ค้าง)
7. ค่าเกณฑ์เริ่มต้นที่อยากให้มีมาให้เลย (preset) เช่น pH 6.5–8.5?
8. ต้องการให้ alarm สั่งการอัตโนมัติ (ตัดปั๊ม) ในอนาคตไหม? — ถ้าใช่ ทำเป็นเฟสแยก

---

## 8. งานค้างจากงานเซนเซอร์ (ทำก่อนหรือพร้อมกันได้)

ผลทดสอบ bench (`docs/bench-test-sensors.md`): **A1–A9 ผ่านหมด (2026-11-15)**

ยังค้าง:
- [ ] A10 ดู serial (COM12) — ทำไปแล้วบางส่วนตอน flash: boot ปกติ, fault ครบ 3 ตัว, heap นิ่ง 60 s
- [ ] ส่วน B — ต่อ PZEM 2 ตัว + SEN0708 จริง, คาลิเบรต pH จริง (4.01 → 9.18 → ตรวจใน 6.86)
- [ ] merge branch `firmware/esp32s3-dev` → `main` (หลังผ่านส่วน B หรือตามที่ผู้ใช้ตัดสินใจ)

Minor ที่เลื่อนไว้ (จาก final review) — บางข้อควรแก้พร้อมงาน alarm เพราะแตะส่วนเดียวกัน:
- [ ] notice ไม่มีเลขลำดับ/เวลา → notice เก่าอาจโผล่หลังคำสั่งใหม่ *(alarm ก็ต้องมี seq เหมือนกัน — ทำระบบเดียวกัน)*
- [ ] ปุ่ม "รีเซ็ต offset" ส่ง 2 คำสั่ง notice ตัวที่ 2 ทับตัวแรก
- [ ] ปุ่มกลับมากดได้ระหว่าง POST ยังไม่เสร็จ / ช่องที่ fault จนกว่า SSE รอบถัดไป
- [ ] ยกเลิกคาลิเบรตไม่มี dialog ยืนยัน
- [ ] คอมเมนต์ `GET /api/cooling/status` ค้างอยู่เหนือบล็อก sensors ใน `web_server.c`
- [ ] JSON ส่งชนิดผิด (เช่น string แทน number) ได้ `missing_field` แทน `value_out_of_range`
- [ ] priority task เซนเซอร์ (3) ไม่ได้ผูกกับ priority cooling ตอน compile
- [ ] SSE callback ไม่เช็ค `snprintf` truncation ของ json_buf
- [ ] คำสั่ง curl ทดสอบต้องใส่ `-H "Origin: http://<ip>"` ไม่งั้นได้ 403 (เป็นเรื่องเอกสาร)

Log ที่มีอยู่ก่อนแล้ว (ไม่ใช่จากงานเซนเซอร์) ควรเก็บกวาด:
- [ ] PT100 fault log ทุก 1 วินาทีตอนไม่ได้ต่อ PT100 → ควร log เฉพาะตอนเปลี่ยนสถานะ
- [ ] `E task_wdt: TWDT already initialized` ตอนบูต
