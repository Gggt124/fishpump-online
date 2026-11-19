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
- `board_http.py`, `api_golden.py`, `golden_cases.json` ชุดเช็คคำตอบ HTTP ของบอร์ด (ดูหัวข้อ "Golden run" ด้านล่าง)

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

## Golden run: เช็คว่า web API ของบอร์ดตอบเหมือนเดิม

ใช้ก่อน/หลังแก้เฟิร์มแวร์ที่ย้าย handler ใน `web_server.c` เพื่อพิสูจน์ว่าคำตอบ HTTP ไม่เปลี่ยน
(stdlib อย่างเดียว; รหัสผ่านบอร์ดมาจาก env `BOARD_PASSWORD` หรือพิมพ์ตอนสคริปต์ถาม ไม่ถูกเก็บ)

```
python thingsboard/api_golden.py --selftest                                   # เช็คออฟไลน์ ต้องขึ้น SELFTEST PASS
python thingsboard/api_golden.py capture --label base --with-writes           # บันทึกคำตอบจากบอร์ดจริง
python thingsboard/api_golden.py compare --base base --label new --with-writes   # จับใหม่แล้วเทียบ 1 บรรทัดต่อเคส
```

- `--board` (ค่าเริ่มต้น `http://192.168.1.34`) และ `--board-user` (ค่าเริ่มต้น `admin`)
- ผลบันทึกที่ `thingsboard/.local/golden/<label>.json` (อยู่ใน `.local/` จึงไม่เข้า git) exit code เป็น 0 เมื่อทุกเคสผ่านเท่านั้น
- ถ้ามีเคสได้ status 0 (request ตาย) หรือ 401/403 (session หลุด) จะไม่บันทึกและ exit 1 พร้อมรายชื่อเคส (เคสที่ตั้งใจให้ได้ 403 ใส่ `"expect_status":[403]`); ถ้า label มีไฟล์อยู่แล้วจะปฏิเสธ ต้องใส่ `--force` ถึงจะเขียนทับ (กันเขียนทับ baseline)
- `golden_cases.json` เป็นลิสต์ของ `{"name","method","path","body","mode","ignore"}`
  - `mode: exact` เทียบ status กับ body ตรงตัว
  - `mode: shape` เทียบ status กับ key และชนิด JSON แบบ recursive ไม่สนค่า และข้าม key ใน `ignore` ทุกระดับ
    (array เทียบแค่ element แรก ถ้าว่างข้างใดข้างหนึ่งถือว่าผ่าน)
- เคสที่มี `"needs":"get:<ชื่อเคส>"` คือการเขียนแบบ idempotent (ส่งค่าที่เพิ่งอ่านกลับไป) รันเฉพาะเมื่อใส่ `--with-writes`
  body คือ JSON ที่เคส GET นั้นตอบในรอบเดียวกัน ตัด key `ok` ชั้นนอกออก แล้วแก้ตามฟิลด์เสริมของเคส (ตามลำดับนี้):
  `when_path` (path ต้องมีค่าจริง ไม่งั้นข้ามเคส), `from_path` (เลือก sub-value เช่น `rules.0`),
  `pick` (เก็บเฉพาะ key ที่ระบุ), `wrap` (ห่อด้วย key เช่น `{"pump": ...}`) ถ้าทำไม่ได้เคสจะถูกบันทึกเป็น skipped
- เคส POST อื่นๆ ส่งเฉพาะคำขอที่ถูกปฏิเสธก่อนแตะสถานะ (ฟิลด์หาย ชนิดผิด เกินช่วง id ที่ไม่มี โหมดที่ไม่รู้จัก)
  ไม่มีเคสที่สตาร์ทปั๊ม/วัด/คาลิเบรต/รีเซ็ตพลังงาน/เชื่อม Wi-Fi/บันทึก hardware map
  และไม่ครอบคลุม `/api/tb*`, login/logout/credentials, `/api/confirm`, `/api/ota`, `/api/events`
- `/api/wifi/scan` รันเป็นเคสสุดท้ายเสมอ
- ถ้าเคส `exact` ต่างกันเองระหว่างสองรอบบนเฟิร์มแวร์เดิม ให้ย้ายเป็น `shape` แล้วใส่ key ที่เปลี่ยนใน `ignore`
