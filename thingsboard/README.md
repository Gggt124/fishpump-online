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
- `tb_dashboard.py` สร้าง widget bundle `fishpump`, widget `fishpump_rpc_panel` และ dashboard `Fishpump — ทดสอบ` (หนึ่ง state ต่อหนึ่งหน้า) ให้ลูกค้าทดสอบ; `tb_setup.py` เรียกให้เองหลังขั้นตอน customer; `--check` ตรวจไฟล์แบบออฟไลน์
- `dashboard_pages.json`, `parity_cases.json`, `parity_test.py`, `tb_dump.py` ดูหัวข้อ "Dashboard ทดสอบและ parity" ด้านล่าง
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
- กลุ่ม Wi-Fi ไม่เคยส่ง `wifi.connect` ที่อาจ stage ได้ (the Wi-Fi group never sends a `wifi.connect` that could stage): เคส error ทั้งหมดถูกปฏิเสธก่อน stage (`local_only`, `missing_ssid`, `ssid_too_long`)
- ถ้าเคส `exact` ต่างกันเองระหว่างสองรอบบนเฟิร์มแวร์เดิม ให้ย้ายเป็น `shape` แล้วใส่ key ที่เปลี่ยนใน `ignore`

## Dashboard ทดสอบและ parity

dashboard เขียน JSON ด้วยมือตามไฟล์ตัวอย่างจาก ThingsBoard 4.3.1 (`tb_dump.py` เก็บไว้ที่ `.local/dump/`) รอบแรกที่รันจริงอาจต้องแก้

```
python thingsboard/tb_dump.py --user <อีเมล> --widget-type control_widgets.rpcbutton   # หรือ --dashboard <ชื่อ>
python thingsboard/tb_dashboard.py --check                                             # ออฟไลน์
python thingsboard/tb_dashboard.py --user <อีเมล> [--device fishpump-1de564]           # หรือผ่าน tb_setup.py
python thingsboard/parity_test.py --selftest
python thingsboard/parity_test.py --user <อีเมล> --device fishpump-1de564 [--group alarms] [--writes --rule-id N] [--burst 30]
```

### dashboard_pages.json

ลิสต์ของหน้า `{"id","title","telemetry":[key],"actions":[...]}` กลุ่มถัดไปเพิ่มหน้าต่อท้าย แต่ละ action:
`{"label","method","params":obj|null,"edit":bool,"load":"<method อ่านค่า>"|null,"confirm":"<ข้อความไทย>"|null}`
ส่วนเสริม `load_path` (เช่น `rules.0`) เลือกส่วนย่อยของผลที่โหลดมาใส่ช่อง params
- `edit: true` มีช่อง params ให้แก้ก่อนส่ง, `load` เติมช่อง params จากผลของ method อ่านค่า (ตัด `ok` ออก)
- `confirm` แสดง `window.confirm` ก่อนส่ง ใช้ข้อความเดียวกับเว็บแอป (ถ้าเว็บแอปไม่ถาม หน้านี้ก็ไม่ถาม)
- ทุกหน้ามี widget ค่าล่าสุดของ `telemetry` และปุ่มสลับหน้า
- `tb_dashboard.py --check`: id หน้าไม่ซ้ำ, method ทุกตัวต้องอยู่ใน `parity_cases.json` (หรือ allow-list `NON_PARITY_METHODS`),
  `confirm` ต้องเป็นข้อความไทยไม่ว่าง, ไม่มีอักขระ `<>&"'` ในข้อความ, สคริปต์ widget ผ่าน `node --check` (ถ้ามี node)

### parity_cases.json

`{"<group>":{"reads":[...],"errors":[...],"writes":[...]}}`
- `reads`: `{"method","http":"/api/...","ignore":[key]}` เทียบ body ของ RPC กับ HTTP โดยข้าม key ที่ระบุทุกระดับ
- `errors`: `{"method","params","http":"/api/..."|null,"error":"<code>"|null,"body":<ตัวเลือก>}`
  ส่ง RPC ต้องได้ `ok:false` และ error code เท่ากับ `error` (ถ้าระบุ) แล้ว POST `body` (ค่าเริ่มต้น = `params`) ไป `http` ต้องได้ code เดียวกัน; `http: null` = ไม่มีฝั่ง HTTP
- `writes`: `{"name","get","get_http","rule_path","guard","path","change","set","set_http"}`
  - `rule_path` เลือก sub-object ของผล GET ที่ใช้เป็น body ของ set (`rules[id=$RULE_ID]` เลือก element ที่ `id` เท่ากับ `--rule-id`)
  - `guard` `{"path","equals"}` ต้องเป็นจริงก่อนเขียน (เช่น กฎต้อง `enabled:false`) ไม่งั้น SKIP
  - `path` จุดที่แก้ (แบบจุด เช่น `delay_sec`) ภายใน `rule_path`; `change` เป็น `{"add":n}` หรือ `{"value":v}`
  - ลำดับ: อ่านทาง TB, เขียนทาง TB, อ่านกลับทาง HTTP (ต้องเห็นค่าใหม่), คืนค่าทาง HTTP, อ่านทาง TB (ต้องเป็นค่าเดิม)
    ถ้าคืนค่าไม่สำเร็จ จะหยุดเคสเขียนที่เหลือและพิมพ์ค่าเดิม
  - `send_as` ส่งเฉพาะ `{<send_as>: ค่าใหม่}` เป็น body ของ set (เมื่อ method รับ key ชื่อต่างจากผล GET); `change` `{"same":true}` ส่งค่าเดิม (no-op)
  - เคส `reads` ที่มี `"shape": true` เทียบเฉพาะโครงสร้างคำตอบ (key และชนิด ไม่สนค่าและความยาวลิสต์) เช่น `wifi.scan`
  - เคส `errors` ที่มี `params_from` `{"key","method","path"}` อ่านค่าจากบอร์ดผ่าน TB ก่อนแล้วใส่ใน params (เช่น SSID ที่เชื่อมอยู่ สำหรับ `in_use`); ไม่พบค่า = SKIP
  - ถ้า method ของ `get` ยังไม่มีบนบอร์ด (`unknown_method`) เคสนั้น SKIP "needs <get>" เช่น `sensors.get` ก่อน Task 12
- `parity_test.py` ผลลัพธ์ `PASS`/`FAIL`/`SKIP` ต่อเคส exit code 0 เมื่อไม่มี FAIL; เคสเขียนรันเมื่อใส่ `--writes` และต้องมี `--rule-id`
  (กฎที่ปิดอยู่); `--burst N` ส่ง `alarms.rules.get` N ครั้งติดกันขณะ poll `/api/alarms/summary` ทุก 200 ms
- ถ้าบอร์ดตอบ 403 `force_password_change` เคสนั้น SKIP ("board web password is still the default")

### state_cases.json (client attributes และ status telemetry)

`parity_test.py` อ่านไฟล์นี้ด้วย (รันพร้อมรอบอ่าน; `--group state` รันเฉพาะส่วนนี้)
- `attributes`: `{"key","http","drop":[key]}` แถว `attr <key>` ค่า client attribute ใน ThingsBoard ต้องเท่ากับคำตอบ HTTP หลังตัด `drop`
  (ใช้รายการ `ignore` ของเคส read ที่ชี้ `http` เดียวกันใน `parity_cases.json`); `attr_present`: แถว `attr <key> present` ต้องมีค่า
- `status_keys`: แถว `status <key>` ต้องมีค่า telemetry ล่าสุดและอายุไม่เกิน `status_max_age_s`; รายการนี้ต้องตรงกับ `uplink_status_fields[]`
  ใน `uplink_format.c` ตามลำดับ ตามด้วย 7 key ของ `tb_state.c` (`python firmware/scripts/verify_tb_link.py` เช็ค drift)
- เคส `writes` ที่มี `"attr": {"key","path"}` (path อยู่ในค่า attribute) มีแถว `.../write <ชื่อ> follow` เพิ่ม:
  หลังเขียนทาง TB และหลังคืนค่าทาง HTTP จะ poll attribute ทุก 250 ms นานสุด `follow_timeout_s` จนค่าตาม PASS เมื่อเห็นทั้งสองครั้งภายใน
  `follow_pass_s` (แสดงเวลาที่วัดได้ เช่น `follow 1.1 s / 0.9 s`) **แถว follow รันเฉพาะเมื่อใส่ `--writes`**

## ทดสอบอัปเดตเฟิร์มแวร์ (OTA) จาก ThingsBoard

`tb_ota.py` ใช้ session ของ tenant admin เท่านั้น ไม่ใช้ access token ของอุปกรณ์ (ThingsBoard เก็บ session เดียวต่ออุปกรณ์ ถ้าต่อซ้ำบอร์ดจริงจะหลุด)

```
python thingsboard/tb_ota.py --selftest                                         # ออฟไลน์
python thingsboard/tb_ota.py upload firmware/build/fishpump_firmware.bin --version 0.3.3 --user <อีเมล>
python thingsboard/tb_ota.py assign fishpump-1de564 0.3.3 --user <อีเมล>
python thingsboard/tb_ota.py status fishpump-1de564 --user <อีเมล>              # ดูซ้ำจนเห็น UPDATED
python thingsboard/tb_ota.py unassign fishpump-1de564 --user <อีเมล>
python thingsboard/tb_ota.py delete 0.3.3 --user <อีเมล>
```

- ไฟล์ที่อัปโหลดคือ `.bin` จากการ build ตรงๆ (ไม่ใช่ไฟล์เข้ารหัสของเว็บอัปโหลด) ชื่อ package ต้องเป็น `fishpump` (ค่าเริ่มต้น) และ `--version` ต้องสูงกว่าเวอร์ชันที่บอร์ดรันอยู่ (ตั้งใน `firmware/version.txt` ก่อน build) ไม่งั้นบอร์ดข้าม
- `upload` พิมพ์ id, ขนาด และ checksum SHA-256 ที่ ThingsBoard เก็บ ให้เทียบกับไฟล์ในเครื่อง
- `status` พิมพ์ `fw_state`, `fw_error`, `current_fw_*`, `target_fw_*` ล่าสุด และ shared attribute `fw_*`
- การ assign ทำที่หน้าอุปกรณ์ใน ThingsBoard ได้เหมือนกัน (ช่อง Firmware ในหน้ารายละเอียดอุปกรณ์)
- dashboard มีหน้า "เฟิร์มแวร์ (Firmware)" แสดง `fw_state`, `fw_error`, `current_fw_title`, `current_fw_version`, `target_fw_version`
- รายละเอียดโปรโตคอล รหัส `fw_error` และกฎ 5 นาที: `docs/firmware/device-api.md` หัวข้อ 12
