# คู่มือทดสอบ: ทางของร้าน การทิ้งคำสั่งอันตรายทางตรง และ inactivityTimeout (ThingsBoard)

ผู้ทดสอบคือเจ้าของโปรเจกต์ ทุกข้อที่ต้องล็อกอินหรือพิมพ์รหัสผ่าน ผู้ทดสอบพิมพ์เอง ไฟล์นี้ใช้กับ branch `feature/tb-shop-path`

ที่มาอยู่ใน spec [`docs/superpowers/specs/2026-11-24-frontend-app-design.md`](superpowers/specs/2026-11-24-frontend-app-design.md) ข้อ 6.3 และ 7.1 ส่วนวิธีทำงานอยู่ใน [`thingsboard/README.md`](../thingsboard/README.md) หัวข้อ "คำสั่งอันตรายและทางของร้าน"

> สถานะ: `[ ]` ยังไม่ทำ · `[/]` ผ่าน · `[x]` ไม่ผ่าน (จดอาการในบรรทัด "บันทึก")
> `tb_setup.py` แก้ rule chain ที่บอร์ดจริงใช้อยู่ ให้ทำ T1 ตอนที่พร้อมทดสอบต่อจนจบ

**เตรียมของ**

- ThingsBoard CE ที่ `http://192.168.1.51:8080` และบอร์ด `fishpump-1de564` ต่ออยู่ (`active` = true)
- รหัสผ่าน tenant admin
- ผู้ใช้ของ customer `ลูกค้าทดสอบ` 1 คนพร้อมรหัสผ่าน ถ้ายังไม่มีให้สร้างในหน้า Customers → ลูกค้าทดสอบ → Users
- จดค่า `protection.get` ปัจจุบันไว้ เพื่อใช้ใน T3 และใช้คืนค่า

---

### T1. ติดตั้ง rule chain, asset profile และ asset ของร้าน

1. `python thingsboard/tb_setup.py --user <อีเมล tenant admin> --allow-direct fishpump-1de564`
   **ต้องเห็น:**
   - บรรทัด `rule chain fishpump ...` และ `rule chain fishpump_shop ...` ไม่มี `ERROR`
   - `asset prof. fishpump_shop ...`
   - `shop asset shop-fishpump-1de564 ...` ทุกบอร์ดของ profile `fishpump` มีบรรทัดของตัวเอง
   - บรรทัดสุดท้าย `bench_allow_direct = true`
2. หน้าเว็บ ThingsBoard → Assets → `shop-fishpump-1de564`
   **ต้องเห็น:**
   - ช่อง Customer ว่าง
   - แท็บ Relations มี `fishpump_board` ไปที่ `fishpump-1de564`
   - server attribute `bench_allow_direct` = true
3. Devices → `fishpump-1de564` → Attributes → Server attributes
   **ต้องเห็น:** `inactivityTimeout` = 180000
   - [ ] ผ่าน  บันทึก: ______________________

### T2. ทดสอบอัตโนมัติด้วยบอร์ดปลอม

1. `python thingsboard/rpc_guard_test.py --user <อีเมล tenant admin> --customer-user <อีเมลผู้ใช้ลูกค้าทดสอบ>`
   **ต้องเห็น:**
   - ทุกบรรทัด G1–G7 เป็น `PASS`
   - บรรทัดสุดท้าย `summary: <N> PASS, 0 FAIL, 0 SKIP` และ exit code 0
   - ใช้เวลาราว 2–3 นาที ส่วน G4 รอนานสุดราว 20 วินาที
   - ไม่ต้องใช้บอร์ดจริง และบอร์ดจริงไม่ได้รับคำสั่งใดๆ
   - [ ] ผ่าน  บันทึก: ______________________

### T3. บอร์ดจริง: ทางตรงถูกทิ้ง ทางของร้านใช้ได้

1. `python thingsboard/tb_setup.py --user <อีเมล> --block-direct fishpump-1de564`
2. dashboard ทดสอบ หน้า Protection: กด `protection.set` → "โหลดค่าปัจจุบัน" → ส่งค่าเดิมโดยไม่แก้
   **ต้องเห็น:**
   - คำตอบเป็นหมดเวลาหรือ error ไม่ใช่ `ok:true`
   - serial log ของบอร์ดไม่มีบรรทัด `dry-run settings saved`
   - log ของ rule chain (`fishpump` → node `Log dropped RPC`) มี `Dropped dangerous RPC protection.set`
3. กด `protection.get`
   **ต้องเห็น:** ได้ค่าเดิมตามที่จดไว้ และคำสั่งนี้ไม่ถูกทิ้ง
4. Assets → `shop-fishpump-1de564` → Attributes → Server attributes → `+`
   - key: `apply_request`
   - ชนิด: JSON
   - ค่า: `{"request_id": "t3-1", "method": "protection.set", "params": <ค่า pump ที่จดไว้ในรูป {"pump": {...}}>}`
   **ต้องเห็น:**
   - ภายในไม่กี่วินาทีมี `apply_result` ที่ `request_id` = `t3-1`, `ok` = true และ `reply.ok` = true
   - serial log มี `dry-run settings saved`
5. ทำข้อ 4 ซ้ำด้วย `{"request_id": "t3-2", "method": "protection.set", "params": {"pump": {"prime_sec": 121}}}`
   **ต้องเห็น:** `apply_result` มี `ok` = false และ `error` = `value_out_of_range` บอร์ดยังกันค่านอกช่วงเองเป็นชั้นสุดท้าย
   - [ ] ผ่าน  บันทึก: ______________________

### T4. บอร์ดหลุดแล้ว ThingsBoard รู้ภายในราว 3 นาที

1. จดเวลา แล้วถอดไฟบอร์ด
2. ดู Devices → `fishpump-1de564` ที่ช่อง Active หรือ server attribute `active`
   **ต้องเห็น:** เป็น false ภายในราว 3–4 นาที (ค่าเดิมของ ThingsBoard คือราว 10 นาที)
3. เสียบไฟคืน
   **ต้องเห็น:** `active` กลับเป็น true หลังบอร์ดต่อ ThingsBoard ได้
   - [ ] ผ่าน  บันทึก: ______________________

### T5. บอร์ดบนโต๊ะกลับมาใช้ parity ได้

1. `python thingsboard/tb_setup.py --user <อีเมล> --allow-direct fishpump-1de564`
2. `python thingsboard/parity_test.py --user <อีเมล> --device fishpump-1de564`
   **ต้องเห็น:** `summary: <N> PASS, 0 FAIL, <M> SKIP` เหมือนผลรอบ firmware 0.5 คือ 130 PASS และ 7 SKIP
   - [ ] ผ่าน  บันทึก: ______________________

---

**หลังทดสอบ:** บอร์ดบนโต๊ะคงไว้ที่ `--allow-direct` ส่วนบอร์ดของลูกค้าต้องเป็นค่าเริ่มต้น (`bench_allow_direct` = false) เสมอ ถ้าข้อใดไม่ผ่าน ให้จดบรรทัดที่ไม่ผ่าน คำตอบ RPC และค่า `apply_result` ไว้ในบรรทัด "บันทึก"

---

## ผลรอบที่ 1 (2026-11-25, ThingsBoard CE 4.3.1 บนเครื่อง และบอร์ด fishpump-1de564 ที่รัน firmware 0.5.0)

Claude รันสคริปต์ในแท็บเทอร์มินัล เจ้าของโปรเจกต์พิมพ์รหัสผ่านเอง ถอดไฟบอร์ด และเสียบไฟคืน

| ข้อ | ผล | บันทึก |
|---|---|---|
| T1 | ผ่าน | ThingsBoard รับ rule chain `fishpump` ที่แก้แล้ว และสร้าง `fishpump_shop` กับ asset profile `fishpump_shop` ได้ รวมทั้ง node เปลี่ยนผู้ส่ง (`configurationVersion` 0) สร้าง `shop-fishpump-1de564` แล้วผูกกับบอร์ด ตั้ง `inactivityTimeout` 180000 และ `bench_allow_direct` = true |
| T2 | ผ่าน | `summary: 16 PASS, 0 FAIL, 1 SKIP` คำสั่งอันตรายทางตรงทั้ง 7 ตัว รวม `cooling.mode.set` ที่เป็น `test`/`test_on` ได้ HTTP 408 และไม่ถึงบอร์ดปลอม ส่วน `cooling.mode.set auto` ถึงบอร์ด (ยืนยันการแก้เรื่อง params เป็นข้อความ) บอร์ดออฟไลน์ได้ `board_offline` ส่วน G7 ยังไม่ได้รัน เพราะยังไม่มีบัญชีผู้ใช้ของลูกค้าทดสอบ |
| T3 | ผ่าน | รันด้วยสคริปต์แทนการกดใน dashboard ทางตรง `protection.set` ได้ HTTP 408 และค่าไม่เปลี่ยน ทางของร้านส่งค่าเดิมได้ `ok` บอร์ดปฏิเสธ `prime_sec` 121 ด้วย `value_out_of_range` ตอนจบค่ายังเป็น 15/10/5/backoff/3 (`stored`) ไม่ได้ดู serial log |
| T4 | ผ่าน | `active` เป็น false หลังกิจกรรมล่าสุด 186 วินาที (16:04:31) เสียบไฟคืนแล้วกลับเป็น true ตอน 16:07:16 |
| T5 | ผ่าน | `--allow-direct` แล้ว `parity_test.py` ได้ `summary: 130 PASS, 0 FAIL, 7 SKIP` เท่ากับรอบ firmware 0.5 คำสั่งอันตรายทุกตัวยังถึงบอร์ดบนโต๊ะ (SKIP ทั้ง 7 เป็นเคสเขียนค่าที่ต้องใช้ `--writes`) |

**ยังค้าง:** G7 (สิทธิ์ของลูกค้า) ต้องมีผู้ใช้ของ customer `ลูกค้าทดสอบ` ก่อน แล้วรัน `rpc_guard_test.py --customer-user <อีเมล>`
