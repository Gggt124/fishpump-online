# ThingsBoard (fishpump)

สคริปต์ Python (ใช้แค่ stdlib) สำหรับตั้งค่าและทดสอบ ThingsBoard CE ที่ `http://192.168.1.51:8080`
รหัสผ่านของ tenant admin ไม่ถูกเก็บที่ไหนเลย: ตั้ง env `TB_PASSWORD` หรือพิมพ์เองตอนสคริปต์ถาม

## ไฟล์

- `tb_setup.py` ตั้งค่าแบบ idempotent (รันซ้ำได้ ไม่สร้างซ้ำ): rule chain `fishpump`, device profile `fishpump`
  (provision แบบ ALLOW_CREATE_NEW_DEVICES), customer `ลูกค้าทดสอบ` แล้วพิมพ์ provision key/secret
  (เก็บใน `thingsboard/.local/provision.json` ซึ่งไม่เข้า git) เอาไปกรอกในหน้าการ์ดของบอร์ด
- `rule_chains/fishpump.json` rule chain ต้นฉบับ (แหล่งความจริงอยู่ในไฟล์นี้)
- `tb_event_test.py` ทดสอบ rule chain: สร้างอุปกรณ์ `fishpump-ruletest` ส่ง alarm event ผ่าน MQTT แล้วเช็ค alarm ทาง REST
  พิมพ์ตาราง PASS/FAIL และคืนจำนวนที่ FAIL เป็น exit code
- `mqtt_mini.py` MQTT 3.1.1 client ขนาดเล็ก (`python thingsboard/mqtt_mini.py --selftest` เช็ค packet แบบออฟไลน์)
- `tb_api.py` ตัวช่วยเรียก REST API

## วิธีรัน

```
python thingsboard/tb_setup.py --user <อีเมล tenant admin>
python thingsboard/tb_setup.py --user <อีเมล> --assign fishpump-a1b2c3   # หลังบอร์ด provision แล้ว
python thingsboard/tb_event_test.py --user <อีเมล> [--keep]
```

`--url` ค่าเริ่มต้น `http://192.168.1.51:8080` (หรือ env `TB_USER` แทน `--user`)

## ลำดับการทำงานของ rule chain

telemetry ปกติถูกบันทึกเหมือนเดิม ส่วนข้อความที่มี `alarm_event` จะดึง `last_alarm_event` (`{boot_id, seq}`)
มาตัดข้อมูลซ้ำ (boot_id เท่าเดิมและ seq น้อยกว่าหรือเท่ากับที่เก็บไว้ = ทิ้ง) แล้ว raised สร้าง alarm `rule_<rule_id>`
(critical=CRITICAL, warning=WARNING, info=INDETERMINATE) / cleared เคลียร์ alarm นั้น จากนั้นบันทึก `last_alarm_event` ใหม่
