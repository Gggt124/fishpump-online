# ThingsBoard: การเก็บข้อมูลและการ deploy จริง (ทำหลัง frontend)

สถานะ: แผนงานภายหลัง เขียนเมื่อ 2026-11-21 หลังจบ milestone 6 ของการเชื่อมต่อ MQTT
ทำได้หลัง frontend เสร็จและให้ลูกค้าดูแล้ว เพราะทุกข้อในไฟล์นี้แก้ฝั่งเซิร์ฟเวอร์ (rule chain, tenant profile, ค่าตั้งของ ThingsBoard) ไม่ต้องแก้เฟิร์มแวร์
แต่ต้องทำ **ก่อนเปิดใช้จริงกับลูกค้าหลายราย** เพราะตอนนี้ข้อมูลไม่มีวันถูกลบ

## 1. ตอนนี้ ThingsBoard เก็บอะไรบ้าง

| ข้อมูล | มาจาก | เก็บที่ | ความถี่ | อายุตอนนี้ |
|---|---|---|---|---|
| ค่าเซนเซอร์และสถานะ (signal) 37 key + สรุป alarm 4 key | telemetry ของบอร์ด | timeseries | ทุก 30 วินาที | ตลอดไป (`defaultTTL: 0` ใน `Save Timeseries` ของ `rule_chains/fishpump.json`) |
| สถานะละเอียด 35 key (phase, countdown, dry-run, stack HWM ฯลฯ) | status telemetry | timeseries | ทุก 30 วินาที และทุกครั้งที่ phase เปลี่ยน (ห่างอย่างน้อย 2 วินาที) | ตลอดไป |
| ค่าตั้ง (`cfg_pump`, `cfg_cooling`, `cfg_protection`, `hardware_map`, `wifi`, `tb_link`), รายการ signal, `alarm_rules`, `fw_type`, `fw_version`, `contract_version` | client attributes | ตาราง attribute (ค่าล่าสุดเท่านั้น ไม่มีประวัติ) | เมื่อค่าเปลี่ยน และทุกครั้งที่ต่อใหม่ | ค่าล่าสุด |
| alarm `rule_<id>` (เกิด/หาย) | `alarm_event` ผ่าน rule chain | ตาราง alarm | เมื่อกฎเกิดหรือหาย | ตลอดไป |
| `last_alarm_event` (ตัวกันข้อมูลซ้ำ) | rule chain | server attribute | ทุก event | ค่าล่าสุด |
| ไฟล์ OTA | ผู้ดูแลอัปโหลด | ตาราง OTA package | ตอนอัปโหลด | จนกว่าจะลบ |

`alarm_event` ไม่ได้เก็บเป็น timeseries ประวัติ event จึงอ่านจาก alarm ของ ThingsBoard
RPC ที่ใช้เป็นแบบ two-way ธรรมดา ThingsBoard จึงไม่เก็บประวัติคำสั่งไว้ (ถ้าต้องการ audit ว่าใครสั่งอะไร ต้องออกแบบเพิ่ม)

## 2. ปริมาณโดยประมาณ (ต้องวัดจริงก่อนตัดสินใจ)

ต่ออุปกรณ์หนึ่งตัวต่อวัน:
- telemetry: 41 key × 2,880 ข้อความ ≈ 118,000 แถว
- status: 35 key × ประมาณ 4,400 ข้อความ (2,880 + phase เปลี่ยนประมาณ 1,500 ครั้งที่รอบปั๊ม 110 วินาที) ≈ 155,000 แถว
- รวมประมาณ **270,000 แถวต่อวัน**

ถ้าแถวหนึ่งใน PostgreSQL (`ts_kv` รวม index) ใช้ประมาณ 100 ไบต์ (ค่าประมาณ ต้องวัด):

| จำนวนอุปกรณ์ | ต่อวัน | ต่อเดือน | ต่อปี |
|---|---|---|---|
| 1 | ~27 MB | ~0.8 GB | ~10 GB |
| 10 | ~270 MB | ~8 GB | ~100 GB |
| 50 | ~1.3 GB | ~40 GB | ~500 GB |

วิธีวัดจริง: บนเครื่อง ThingsBoard รัน `SELECT pg_size_pretty(pg_total_relation_size('ts_kv'));` วันนี้และอีก 7 วัน แล้วหารด้วยจำนวนอุปกรณ์และจำนวนวัน

ข้อมูลส่วนใหญ่เป็นค่าที่ไม่เปลี่ยน (เช่น `pump_config_valid`, `wifi_channel`) หรือเปลี่ยนทุกข้อความแต่ไม่มีประโยชน์ระยะยาว (เช่น `pump_countdown_sec`) จึงลดได้มากโดยไม่เสียข้อมูลที่ลูกค้าใช้

## 3. แบ่งความสำคัญ (ข้อเสนอ ผู้ใช้ตัดสิน)

| ระดับ | ใช้ทำอะไร | key | ข้อเสนอการเก็บ |
|---|---|---|---|
| A สำคัญ | กราฟย้อนหลัง รายงาน ค่าไฟ | `water_temp_c`, `water_ph`, `pt100_temp_c`, `pump_power_w`, `pump_current_a`, `pump_energy_wh`, `chiller_power_w`, `chiller_current_a`, `chiller_energy_wh`, `pump_voltage_v`, alarm ทั้งหมด | บันทึกลงฐานข้อมูลทุก 5 นาที เก็บ 1–2 ปี alarm เก็บ 2 ปี |
| B ใช้หาสาเหตุ | ดูย้อนหลังเมื่อมีปัญหา | `relay1_on`, `relay2_on`, `pump_is_running`, `flow_on`, `float_high`, `cooling_on`, `cooling_duty_pct`, `cooling_output_mv`, `pump_dry_run_state`, `dry_run_state`, `dry_run_trips`, ค่า `*_fault`, `pump_phase`, `wash_phase`, `wifi_rssi_dbm`, `uptime_sec`, `alarm_active`, `alarm_max_severity` | บันทึกเมื่อค่าเปลี่ยนหรือทุก 1 นาที เก็บ 30–90 วัน |
| C แสดงสดเท่านั้น | หน้าจอสด ไม่ต้องย้อนหลัง | `pump_countdown_sec`, `wash_countdown_sec`, `pump_cooldown_sec`, `dry_run_accum_sec`, `dry_run_wait_sec`, `cooling_lockout_sec`, `cooling_test_sec`, `pump_cycle_sec`, `pump_config_valid`, `cooling_temp_valid`, `cooling_mode*`, `cooling_auto_enable`, `wifi_connected`, `wifi_channel`, ค่าหน่วยความจำ (`free_heap_bytes`, `free_psram_bytes`, `heap_*`, `stack_hwm_*`, `oom_strikes`), `tb_sent`, `tb_failed` | เก็บเฉพาะค่าล่าสุดและส่งให้หน้าจอสด ไม่บันทึกประวัติ หรือเก็บ 7 วันไว้ดูปัญหาเฟิร์มแวร์ |

ประมาณการหลังใช้นโยบายนี้: ลดเหลือราว 1 ใน 10 (ประมาณ 1 GB ต่อปีต่ออุปกรณ์) ต้องวัดจริงอีกครั้ง

ควรตัดสินจากหน้าที่ frontend จะแสดง: กราฟย้อนหลังกี่วัน รายงานรายเดือนไหม ค่าไฟต่อเดือนไหม ถ้า frontend ไม่แสดง key ใดย้อนหลัง key นั้นอยู่ระดับ C

## 4. วิธีลบข้อมูลเก่าใน ThingsBoard CE (ต้องตรวจกับเวอร์ชัน 4.3.1 ก่อนใช้)

1. **TTL ต่อข้อความใน rule chain:** node `Save Timeseries` มี `defaultTTL` (วินาที) และอ่าน `TTL` จาก metadata ของข้อความได้ ใช้แยก key เป็นหลายข้อความตามระดับ A/B/C แล้วตั้ง TTL ต่างกัน ทั้งหมดอยู่ใน `rule_chains/fishpump.json` จึงติดไปกับ `tb_setup.py`
2. **ลดความถี่ที่บันทึก:** ThingsBoard 4.x มีตัวเลือกการประมวลผลใน `Save Timeseries` ที่แยกการบันทึกลงฐานข้อมูล ค่าล่าสุด และ WebSocket ได้ (เช่นบันทึกลงฐานข้อมูลแค่ทุก N วินาที แต่หน้าจอสดยังได้ทุกข้อความ) ต้องตรวจชื่อและรูปแบบ config ใน 4.3.1
3. **TTL เริ่มต้นของ tenant:** tenant profile มีอายุเก็บ timeseries และ alarm เป็นวัน ใช้เป็นเพดานกันลืม
4. **งานลบของระบบ:** ค่าตั้งของ ThingsBoard (`SQL_TTL_TS_ENABLED`, `SQL_TTL_TS_TS_KEY_VALUE_TTL`, ช่วงเวลาที่รัน) ลบแถวที่หมดอายุทุกวัน และ `ts_kv` แบ่ง partition ตามเดือน
5. **ปิด debug mode ของ rule node ใน production:** debug event กินพื้นที่เร็ว

## 5. หลายลูกค้า หลายอุปกรณ์

- ตอนนี้มี customer ทดสอบหนึ่งรายและ dashboard ทดสอบหนึ่งชุด
- งานที่ต้องเพิ่ม:
  - สคริปต์เพิ่มลูกค้าใหม่ ที่สร้าง customer, user และ assign อุปกรณ์
  - การตั้งชื่อหรือ label อุปกรณ์ให้ลูกค้าเห็น
  - สิทธิ์ว่าลูกค้าเห็นเฉพาะอุปกรณ์ของตัวเอง
- ถ้าลูกค้าแต่ละรายต้องการอายุเก็บข้อมูลไม่เท่ากัน (เช่นแพ็กเกจ) ใช้ tenant profile หรือ rule chain แยกตาม device profile

## 6. Deploy จริงบน VPS

**ทำอัตโนมัติแล้ว (`thingsboard/tb_setup.py`, รันซ้ำได้):**
- rule chain `fishpump`
- device profile `fishpump` รวมการลงทะเบียนเอง (provision key และ secret)
- customer ทดสอบ
- widget และ dashboard ทดสอบ

**ยังไม่มี (ต้องทำในโปรเจกต์ deploy):**
- **ติดตั้ง ThingsBoard:** ไฟล์ `docker-compose` ของ ThingsBoard + PostgreSQL พร้อม volume, ค่าตั้งเรื่อง TTL และหน่วยความจำ ตอนนี้ ThingsBoard บนเครื่อง copter ตั้งด้วยมือ ใน repo ไม่มีไฟล์นี้
- **domain และ TLS:**
  - reverse proxy (HTTPS สำหรับหน้าเว็บ)
  - MQTT TLS พอร์ต 8883 พร้อม certificate
  - เปลี่ยนบอร์ดให้ใช้ TLS (เฟิร์มแวร์รองรับแล้ว ข้อความ error ของ TLS แก้แล้วใน 377408b)
  - ทดสอบข้อความ "certificate not trusted" กับเซิร์ฟเวอร์จริง
- **ความปลอดภัย:**
  - เปลี่ยนรหัสบัญชีเริ่มต้นของ ThingsBoard ทั้ง sysadmin และ tenant
  - ปิดพอร์ตที่ไม่ใช้
  - เก็บ provision secret ให้ปลอดภัย
- **tenant profile:** อายุเก็บข้อมูล เพดานข้อความ และเพดาน API
- **notification:** กฎแจ้งเตือนออกนอกระบบ เช่น LINE หรือ email จาก alarm (ตามแผนแพลตฟอร์ม: กฎอยู่ในเฟิร์มแวร์ ThingsBoard ส่งแจ้งเตือน)
- **backup:**
  - `pg_dump` ตามรอบเวลา และทดสอบกู้คืน
  - backup rule chain และ dashboard (ไฟล์อยู่ใน git อยู่แล้ว)
- **เฝ้าดู:** พื้นที่ดิสก์ ขนาด `ts_kv` และอุปกรณ์ที่ offline
- **ข้อที่ควรแก้ก่อนใช้ alarm แจ้งเตือนจริง:**
  - ข้อจำกัดของ milestone 1: event ใน outbox หายได้เมื่อบันทึกค่าตั้งใหม่ หรือเมื่อลิงก์หลุดเงียบ
  - ring 32 ช่องเต็มเมื่อล่มนานกว่าประมาณ 30 นาที
  - ทั้งสองข้ออยู่ใน `after-frontend.md`

device profile ไม่ต้องตั้งเอง: `tb_setup.py` สร้างและแก้ให้ตรงไฟล์ทุกครั้งที่รัน ถ้ามีเฟิร์มแวร์ชนิดอื่นในอนาคต ให้เพิ่ม profile และ rule chain ต่อชนิด (`fw_type`) ในสคริปต์เดียวกัน

## 7. ลำดับที่แนะนำ

1. squash-merge งาน MQTT เข้า branch สะอาด (เมื่อผู้ใช้สั่ง)
2. ออกแบบและทำ frontend ซึ่งรวมการตัดสินว่าจะแสดงกราฟย้อนหลังกี่วันและค่าไหนบ้าง แล้วให้ลูกค้าดู
3. วัดขนาดฐานข้อมูลจริง 7 วัน (ข้อ 2)
4. ตัดสินนโยบาย A/B/C (ข้อ 3) แล้วแก้ rule chain และ tenant profile (ข้อ 4)
5. โปรเจกต์ deploy (ข้อ 6) พร้อมสคริปต์เพิ่มลูกค้า (ข้อ 5)
