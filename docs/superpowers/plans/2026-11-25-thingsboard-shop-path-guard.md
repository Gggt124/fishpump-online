# ThingsBoard: ทางของร้าน ทิ้งคำสั่งอันตรายทางตรง และ inactivityTimeout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ให้ลูกค้าตั้งค่าที่อันตรายผ่าน REST ของ ThingsBoard ไม่ได้ ให้คนของร้านตั้งค่าเหล่านั้นผ่าน asset ของร้านได้พร้อมรู้ผลจริง และให้ ThingsBoard รู้ว่าบอร์ดหลุดภายใน 3 นาที (spec ข้อ 6.3 ชั้น 1–2 และข้อ 7.1)

**Architecture:**
- **ทางตรง:** rule chain `fishpump` ที่มีอยู่ได้ node ทิ้งคำสั่งในขา "RPC Request to Device" ถ้า method อยู่ในรายการอันตราย ยกเว้นบอร์ดที่ asset ของร้านตั้ง `bench_allow_direct = true`
- **ทางของร้าน:** rule chain ใหม่ `fishpump_shop` เป็นค่าเริ่มต้นของ asset profile `fishpump_shop` รับ `apply_request` ที่เขียนลง server attribute ของ asset แล้วส่ง RPC ไปที่บอร์ดที่ผูกไว้ จากนั้นเขียนผลลง `apply_result`
- **ที่เก็บค่ากลาง:** รายการอันตรายและชื่อทั้งหมดอยู่ใน `thingsboard/rpc_policy.json` ไฟล์เดียว ทั้ง test, `tb_setup.py` และแอปในแผน 3 อ่านจากไฟล์นี้
- **การทดสอบ:** ทดสอบแบบออฟไลน์ด้วย unittest ใน CI ส่วนพฤติกรรมจริงทดสอบกับ ThingsBoard ด้วยบอร์ดปลอมผ่าน MQTT

**Tech Stack:** Python 3 (stdlib เท่านั้น ตามแบบ `tb_api.py`), rule chain JSON ของ ThingsBoard CE 4.3.1 (TBEL), MQTT 3.1.1 (`mqtt_mini.py`)

**Spec:** `docs/superpowers/specs/2026-11-24-frontend-app-design.md` ข้อ 6.2, 6.3, 7.1 (`inactivityTimeout`) และ 12

## Global Constraints

- **ThingsBoard ต้องเป็น CE เท่านั้น:** ห้ามใช้ feature ของ PE
- **รายการค่าที่อันตราย (spec ข้อ 6.2):** `protection.set`, `protection.measure.set`, `cooling.config.set`, `water.calibrate`, `water.calibrate.cancel`, `water.offset`, `power.reset_energy` และ `cooling.mode.set` เมื่อ `params.mode` เป็น `"test"` หรือ `"test_on"`
- **ชื่อที่ใช้ร่วมกัน:**
  - rule chain และ asset profile ชื่อ `fishpump_shop`
  - relation จาก asset ไป device ชื่อ `fishpump_board`
  - ชื่อ asset คือ `shop-<ชื่อ device>`
  - server attribute บน asset: `apply_request`, `apply_result` และ `bench_allow_direct` (ค่าเริ่มต้น false)
- **`inactivityTimeout`:** server attribute ของ device มีค่า 180000 (มิลลิวินาที)
- **asset ของร้าน:** ต้องเป็นของ tenant เสมอ ห้ามมอบให้ Customer
- **`tb_setup.py`:** ต้องรันซ้ำได้ และห้ามทับ `bench_allow_direct` ที่ตั้งไว้แล้ว ยกเว้นผู้ใช้สั่งผ่าน flag
- **รหัสผ่าน:** ห้ามเก็บหรือพิมพ์รหัสผ่าน ให้ใช้ env `TB_PASSWORD` หรือ getpass ตามแบบ `tb_api.py` เดิม และ Claude ห้ามพิมพ์รหัสผ่านแทนผู้ใช้
- **ข้อความใน `additionalInfo` และ `description`:** ห้ามมีอักขระ `< > & " '` (`tb_setup.preflight`)
- **git:** ห้าม stage `firmware/.vscode/settings.json` และ `docs/bench-test-alarms.md`
- **commit:** ลงท้ายด้วย `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` และห้าม push จนกว่าผู้ใช้สั่ง
- **branch:** `feature/tb-shop-path` แตกจาก `main` (`63bc80a`)

## Review Focus

1. **บอร์ดที่ยังไม่มี asset ของร้าน (เพิ่ง provision และยังไม่ได้รัน `tb_setup.py`):** คำสั่งอันตรายทางตรงต้องถูกทิ้ง ไม่ใช่ผ่าน (node ดึง attribute ของ asset ล้มต้องไปทางเดียวกับ "ไม่อนุญาต") ทดสอบที่ Task 2 (โครงสร้าง) และ G2b ของ Task 6
2. **`apply_request` ที่ไม่ใช่ object หรือไม่มี `method`/`request_id`:** ต้องได้ `apply_result` ที่ `error = "bad_request"` ไม่ใช่เงียบ ทดสอบที่ Task 3 (โครงสร้าง) และ G5
3. **บอร์ดออฟไลน์ตอนช่างกดใช้ค่า:** `apply_result` ต้องมี `request_id` เดิมและ `error` เป็น `"board_offline"` หรือ `"no_reply"` แอปจึงไม่รอค้าง ทดสอบที่ G4
4. **`cooling.mode.set` ที่ mode เป็น `auto` หรือ `force_off`:** ลูกค้าต้องยังสั่งได้ จะทิ้งแค่ `test`/`test_on` ทดสอบที่ G2c และ G2d
5. **มีคนมอบ asset ของร้านให้ Customer ผ่านหน้าเว็บของ ThingsBoard:** `tb_setup.py` ต้องดึงกลับเป็นของ tenant และพิมพ์เตือน ทดสอบที่ Task 4

---

### Task 1: ไฟล์ policy และ unittest ใน CI

**Files:**
- Create: `thingsboard/rpc_policy.json`
- Create: `thingsboard/rpc_policy.py`
- Test: `thingsboard/test_rpc_policy.py`
- Modify: `.github/workflows/ci.yml:40` (เพิ่มบรรทัด discover ของ `thingsboard`)

**Interfaces:**
- Produces: `rpc_policy.json`
  ```json
  {"dangerous_methods": ["protection.set", "protection.measure.set", "cooling.config.set", "water.calibrate", "water.calibrate.cancel", "water.offset", "power.reset_energy"],
   "dangerous_cooling_modes": ["test", "test_on"],
   "shop": {"rule_chain": "fishpump_shop", "asset_profile": "fishpump_shop", "relation_type": "fishpump_board",
            "asset_name_prefix": "shop-", "request_key": "apply_request", "result_key": "apply_result",
            "bench_key": "bench_allow_direct"},
   "inactivity_timeout_ms": 180000}
  ```
- Produces: `rpc_policy.py` ที่มี `load() -> dict` และ `is_dangerous(method: str, params) -> bool` ไว้เป็นต้นแบบ Python ของ filter ใน TBEL ใช้ใน test และใน Task 6
- Produces: `device_api_methods(text: str) -> set[str]` อ่านชื่อ method จาก code block ใต้หัวข้อ `## 9.` ของ `docs/firmware/device-api.md`

- [ ] **Step 1: เขียน test ที่ยังล้ม** ใน `thingsboard/test_rpc_policy.py` (`unittest.TestCase`)
  - `test_every_dangerous_method_exists_in_device_api`: ทุกตัวใน `dangerous_methods` และ `cooling.mode.set` อยู่ใน `device_api_methods(...)`
  - `test_device_api_has_38_methods`: `len(device_api_methods(...)) == 38` เพื่อกันตัวอ่านพัง
  - `test_is_dangerous`:
    - `True` สำหรับ `("protection.set", {})` และ `("cooling.mode.set", {"mode": "test_on"})`
    - `False` สำหรับ `("cooling.mode.set", {"mode": "auto"})`, `("cooling.mode.set", None)`, `("protection.get", {})` และ `("pump.start", {})`
  - `test_shop_names`: `asset_name_prefix == "shop-"`, `inactivity_timeout_ms == 180000` และ `bench_key == "bench_allow_direct"`

- [ ] **Step 2: รันแล้วต้องล้ม**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: ERROR `ModuleNotFoundError: No module named 'rpc_policy'`

- [ ] **Step 3: สร้าง `rpc_policy.json` ตาม Interfaces และ `rpc_policy.py`** (stdlib, อ่านไฟล์ด้วย `Path(__file__).parent`)

- [ ] **Step 4: รันแล้วต้องผ่าน**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: `OK` ครบ 4 เทสต์

- [ ] **Step 5: เพิ่มใน CI** ใต้บรรทัด discover ของ `firmware/scripts` ใน `.github/workflows/ci.yml`

```yaml
          python -m unittest discover -s thingsboard -p "test_*.py" -v
```

- [ ] **Step 6: Commit**

```bash
git add thingsboard/rpc_policy.json thingsboard/rpc_policy.py thingsboard/test_rpc_policy.py .github/workflows/ci.yml
git commit -m "feat(tb): rpc policy file for dangerous methods and shop path names"
```

---

### Task 2: rule chain `fishpump` ทิ้งคำสั่งอันตรายทางตรง

**Files:**
- Modify: `thingsboard/rule_chains/fishpump.json` (เพิ่ม node 3 ตัวต่อท้าย index 16–18 และแก้ connection `RPC Request to Device`)
- Test: `thingsboard/test_rule_chains.py`

**Interfaces:**
- Consumes: `rpc_policy.load()` (Task 1)
- Produces: helper ใน test `chain(name) -> dict` และ `paths_to(meta, start, node_type)` สำหรับ Task 3

node ที่เพิ่ม (index ตามลำดับต่อจาก 15):
- **16 `org.thingsboard.rule.engine.metadata.TbGetRelatedAttributeNode` "Fetch bench_allow_direct":**
  - `relationsQuery = {"direction": "TO", "maxLevel": 1, "fetchLastLevelOnly": false, "filters": [{"relationType": "fishpump_board", "entityTypes": ["ASSET"]}]}`
  - `dataToFetch = "ATTRIBUTES"`, `dataMapping = {"bench_allow_direct": "fp_bench_allow_direct"}`, `fetchTo = "METADATA"`
- **17 `org.thingsboard.rule.engine.filter.TbJsFilterNode` "Direct RPC allowed?":**
  - TBEL ต้องมีบรรทัด `var dangerous = [...]` ที่เป็นรายการเดียวกับ `dangerous_methods` และ `var testModes = ["test", "test_on"]`
  - คืน `true` เมื่อ `metadata.fp_bench_allow_direct == "true"`
  - คืน `false` เมื่อ method อยู่ใน `dangerous` หรือเป็น `cooling.mode.set` ที่ `msg.params.mode` อยู่ใน `testModes`
  - กรณีอื่นคืน `true` และต้องกัน `msg.params` ที่เป็น null ด้วย
- **18 `org.thingsboard.rule.engine.action.TbLogNode` "Log dropped RPC":** TBEL `return "Dropped dangerous RPC " + msg.method + " for " + metadata.deviceName;`

connection: ลบ `0 → 4 RPC Request to Device` แล้วเพิ่ม `0 → 16 RPC Request to Device`, `16 → 17 Success`, `16 → 17 Failure`, `17 → 4 True`, `17 → 18 False` และ `17 → 18 Failure`

ถ้า ThingsBoard ไม่รับ configuration ของ node 16 ตอนบันทึก (Task 4 รันจริง) ให้สร้าง node นี้ในหน้า rule chain ของ ThingsBoard หนึ่งครั้ง export ออกมา แล้วคัดลอก configuration ของ node นั้นมาใส่ และบันทึกเป็น `Ruling:` ใน ledger

- [ ] **Step 1: เขียน test ที่ยังล้ม** ใน `thingsboard/test_rule_chains.py`
  - `test_device_rpc_passes_filter`: ทุกเส้นทางจาก node 0 ผ่าน connection `RPC Request to Device` ไปถึง node ชนิด `TbSendRPCRequestNode` ต้องผ่าน node "Direct RPC allowed?" ด้วย connection `True` และต้องไม่มี connection อื่นเข้า node RPC
  - `test_filter_list_matches_policy`: แยกรายการใน `var dangerous = [...]` ด้วย regex ได้เป็น set ที่เท่ากับ `dangerous_methods` และใน script มี `"test"` และ `"test_on"`
  - `test_fetch_failure_goes_to_filter`: node 16 มี connection `Failure` ไปที่ node 17 (Review Focus 1)
  - `test_fetch_uses_policy_relation`: `relationType` ของ node 16 เท่ากับ `shop.relation_type`
  - `test_preflight_clean`: `tb_setup.preflight() == []`

- [ ] **Step 2: รันแล้วต้องล้ม**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: FAIL ที่ `test_device_rpc_passes_filter` และ `test_filter_list_matches_policy`

- [ ] **Step 3: แก้ `fishpump.json` ตามที่ระบุข้างบน** ห้ามเปลี่ยน node 0–15 ที่มีอยู่ และห้ามเปลี่ยน index ของ node เหล่านั้น

- [ ] **Step 4: รันแล้วต้องผ่าน**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v` และ `python thingsboard/tb_setup.py --check`
Expected: `OK` และ `check OK`

- [ ] **Step 5: Commit**

```bash
git add thingsboard/rule_chains/fishpump.json thingsboard/test_rule_chains.py
git commit -m "feat(tb): drop dangerous direct RPC unless the shop asset allows it"
```

---

### Task 3: rule chain `fishpump_shop` (ทางของร้าน)

**Files:**
- Create: `thingsboard/rule_chains/fishpump_shop.json` (รูปแบบเดียวกับ `fishpump.json`: `{"ruleChain": {...}, "metadata": {...}}`)
- Test: `thingsboard/test_rule_chains.py` (เพิ่มเทสต์)

**Interfaces:**
- Consumes: ชื่อใน `rpc_policy.json` ส่วน `shop` (Task 1) และ helper ใน test (Task 2)
- Produces: สัญญาของ `apply_request` และ `apply_result` ที่แอปในแผน 3 ใช้
  - `apply_request` (JSON object): `{"request_id": string, "method": string, "params": object}`
  - `apply_result` (JSON object): `{"request_id": string|null, "method": string|null, "ok": bool, "reply": object|null, "error": string|null}`
    - `error` เป็น error code ของบอร์ด หรือหนึ่งใน `"bad_request"`, `"no_board"`, `"board_offline"`, `"no_reply"`, `"rpc_failed"`

node (index):
- **0 `TbMsgTypeSwitchNode`**
- **1 `TbJsSwitchNode` "Check apply_request":** รับขา `Attributes Updated` คืน `["ignore"]` ถ้า `metadata.scope != "SERVER_SCOPE"` หรือไม่มี key `apply_request` คืน `["valid"]` ถ้า `request_id` และ `method` เป็น string ที่ไม่ว่าง ส่วนกรณีอื่นคืน `["invalid"]`
- **2 `TbTransformMsgNode` "Build RPC":** ใส่ `metadata.fp_request_id` และ `metadata.fp_method` แล้วคืน `{msg: {method, params (ถ้าไม่มีให้เป็น {})}, metadata, msgType: "RPC_CALL_FROM_SERVER_TO_DEVICE"}`
- **3 `org.thingsboard.rule.engine.transform.TbChangeOriginatorNode` "To board":** `originatorSource = "RELATED"`, relationsQuery ทิศ `FROM` relation `fishpump_board` ไปที่ `DEVICE` ระดับ 1
- **4 `TbSendRPCRequestNode` "Send to board":** `timeoutInSeconds = 20`
- **5 `TbTransformMsgNode` "Result from reply":** `ok = msg.ok == true`, `reply = msg`, `error = msg.error` (ถ้ามี)
- **6 `TbTransformMsgNode` "Result from RPC failure":** `msg.error` ของ ThingsBoard เป็น `TIMEOUT` ให้เป็น `"no_reply"` เป็น `NO_ACTIVE_CONNECTION` ให้เป็น `"board_offline"` ส่วนค่าอื่นให้เป็น `"rpc_failed"` และ `ok = false`
- **7 `TbChangeOriginatorNode` "Back to shop asset":** relationsQuery ทิศ `TO` relation `fishpump_board` ไปที่ `ASSET`
- **8 `TbTransformMsgNode` "Result bad_request":** `request_id` เอามาจาก request ถ้าเป็น string
- **9 `TbTransformMsgNode` "Result no_board"**
- **10 `TbMsgAttributesNode` "Save apply_result":** `scope = "SERVER_SCOPE"`, `sendAttributesUpdatedNotification = false`, `updateAttributesOnlyOnValueChange = false`

transform ที่สร้างผล (5, 6, 8, 9) คืน `{msg: {"apply_result": {...}}, metadata, msgType: "POST_ATTRIBUTES_REQUEST"}` และเอา `request_id`/`method` มาจาก `metadata.fp_request_id`/`fp_method`

connection:
- `0 → 1 Attributes Updated`
- `1 → 2 valid`, `1 → 8 invalid`, `1 → 8 Failure`
- `2 → 3 Success`
- `3 → 4 Success`, `3 → 9 Failure`
- `4 → 5 Success`, `4 → 6 Failure`
- `5 → 7 Success`, `6 → 7 Success`
- `7 → 10 Success`, `8 → 10 Success`, `9 → 10 Success`

- [ ] **Step 1: เขียน test ที่ยังล้ม**
  - `test_shop_rpc_only_after_valid`: ทุกเส้นทางจาก node 0 ไปถึง `TbSendRPCRequestNode` ต้องผ่าน connection `valid` ของ node 1 และผ่าน node "To board"
  - `test_shop_every_outcome_saves_result`: node 5, 6, 8 และ 9 ไปถึง node 10 ได้ทุกตัว และ node 1 มีขา `invalid` และ `Failure` (Review Focus 2)
  - `test_shop_save_does_not_loop`: node 10 มี `scope == "SERVER_SCOPE"` และ `sendAttributesUpdatedNotification is False`
  - `test_shop_relation_names`: node "To board" มีทิศ `FROM` และ "Back to shop asset" มีทิศ `TO` ทั้งสองใช้ `shop.relation_type`
  - `test_shop_keys_match_policy`: ใน script ของ node 1 มี `request_key` และใน script ของ node 5, 6, 8, 9 มี `result_key`
  - `test_shop_preflight_clean`: `tb_setup.check_text_fields` กับไฟล์ใหม่ต้องได้ `[]`

- [ ] **Step 2: รันแล้วต้องล้ม** (ไม่มีไฟล์)

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: ERROR `FileNotFoundError` ที่ `fishpump_shop.json`

- [ ] **Step 3: สร้าง `fishpump_shop.json` ตามรายการ node และ connection ข้างบน** ใช้ TBEL ทุก script และให้ `ruleChain.name` เป็น `"fishpump_shop"`

- [ ] **Step 4: รันแล้วต้องผ่าน**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
git add thingsboard/rule_chains/fishpump_shop.json thingsboard/test_rule_chains.py
git commit -m "feat(tb): fishpump_shop rule chain relays apply_request to the board"
```

---

### Task 4: `tb_setup.py` สร้าง asset ของร้านต่อบอร์ด และตั้ง inactivityTimeout

**Files:**
- Modify: `thingsboard/tb_setup.py`
- Test: `thingsboard/test_tb_setup.py`

**Interfaces:**
- Consumes: `rpc_policy.load()` และ `fishpump_shop.json`
- Produces (ใน `tb_setup.py`):
  - `ensure_rule_chain(api, name: str, path: Path) -> dict` แทนตัวเดิมที่ไม่มีพารามิเตอร์ ตัวเดิมเรียกด้วย `RULE_CHAIN_NAME, RULE_CHAIN_FILE`
  - `ensure_asset_profile(api, rc_id: dict) -> dict` ใช้ชื่อ `fishpump_shop`, `defaultRuleChainId = rc_id` และ `defaultQueueName = QUEUE_NAME` ผ่าน `GET /api/assetProfiles` / `POST /api/assetProfile`
  - `shop_asset_name(device_name: str) -> str` คืนค่าเป็น `"shop-" + device_name`
  - `ensure_shop_asset(api, device: dict, profile: dict) -> dict` ทำทีละขั้นดังนี้ และพิมพ์หนึ่งบรรทัดต่อ device
    1. หา asset ด้วย `GET /api/tenant/assets?assetName=` ถ้าไม่มีให้สร้างด้วย `POST /api/asset`
    2. ถ้า asset มี `customerId` ที่ไม่ใช่ `NULL_UUID` ให้พิมพ์ `WARN` แล้วเรียก `DELETE /api/customer/asset/{id}`
    3. ถ้ายังไม่มี relation `fishpump_board` จาก asset ไป device ให้สร้างด้วย `POST /api/relation` (ตรวจด้วย `GET /api/relation?...` ที่ ok404)
    4. ถ้า asset ยังไม่มี `bench_allow_direct` ใน server attribute ให้เขียนเป็น `false`
    5. เขียน server attribute `inactivityTimeout = 180000` บน device ทุกครั้ง
  - `ensure_all_shop_assets(api, profile_id: dict, asset_profile: dict) -> list[dict]` วนทุก device ของ profile `fishpump` ผ่าน `GET /api/tenant/deviceInfos?deviceProfileId=` ทีละหน้า
  - `set_bench_direct(api, device_name: str, allow: bool)` เขียน `bench_allow_direct` บน asset ของ device นั้น
  - CLI: `--allow-direct DEVICE_NAME` และ `--block-direct DEVICE_NAME` ใช้หลังขั้น asset (ใช้ได้ทีละอย่าง)
- `preflight()` ต้องตรวจทั้ง `fishpump.json` และ `fishpump_shop.json`
- `main()` ต้องเรียกตามลำดับ: rule chain `fishpump` → rule chain `fishpump_shop` → profile → asset profile → customer → dashboard → shop assets → `--assign` → `--allow-direct`/`--block-direct`

- [ ] **Step 1: เขียน test ที่ยังล้ม** ใน `thingsboard/test_tb_setup.py` ด้วย `FakeApi` ในไฟล์ test ที่เก็บ entity ไว้ใน dict และบันทึกทุก `post`/`delete` ลง list
  - `test_shop_asset_created_once`: เรียก `ensure_shop_asset` สองครั้งกับ device เดิม ต้องมี `POST /api/asset` ครั้งเดียว และ `POST /api/relation` ครั้งเดียว
  - `test_bench_flag_not_overwritten`: ถ้า asset มี `bench_allow_direct = true` อยู่แล้ว `ensure_shop_asset` ต้องไม่เขียน key นี้
  - `test_inactivity_timeout_set`: ต้องมีการ post `{"inactivityTimeout": 180000}` ไปที่ `/api/plugins/telemetry/DEVICE/<id>/attributes/SERVER_SCOPE`
  - `test_customer_asset_pulled_back`: ถ้า asset มี `customerId` ของลูกค้า ต้องมี `DELETE /api/customer/asset/<id>` (Review Focus 5)
  - `test_set_bench_direct`: `set_bench_direct(api, "fishpump-x", True)` ต้องเขียน `{"bench_allow_direct": true}` ไปที่ asset `shop-fishpump-x` และถ้าไม่มี asset ต้อง raise `TbError`
  - `test_preflight_covers_shop_chain`: เอา `<` ใส่ `additionalInfo.description` ของ shop chain ด้วย monkeypatch ของตัวอ่านไฟล์ แล้ว `preflight()` ต้องไม่ว่าง

- [ ] **Step 2: รันแล้วต้องล้ม**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: ERROR/FAIL ที่ `AttributeError: module 'tb_setup' has no attribute 'ensure_shop_asset'`

- [ ] **Step 3: แก้ `tb_setup.py` ตาม Interfaces** โดยเก็บ REST path ใหม่เป็นค่าคงที่ `P_...` ด้านบนตามแบบเดิม และแก้ docstring บนสุดให้มีตัวอย่าง `--allow-direct`

- [ ] **Step 4: รันแล้วต้องผ่าน**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v` และ `python thingsboard/tb_setup.py --check`
Expected: `OK` และ `check OK`

- [ ] **Step 5: Commit**

```bash
git add thingsboard/tb_setup.py thingsboard/test_tb_setup.py
git commit -m "feat(tb): tb_setup creates shop assets, relation and inactivityTimeout"
```

---

### Task 5: `mqtt_mini` รับ RPC ได้ (บอร์ดปลอม)

**Files:**
- Modify: `thingsboard/mqtt_mini.py`
- Test: `selftest()` ในไฟล์เดียวกัน (มีอยู่แล้ว) และ `thingsboard/test_mqtt_mini.py` ที่เรียก `selftest()` ให้ CI รัน

**Interfaces:**
- Produces:
  - `build_subscribe(packet_id: int, topic: str) -> bytes` (QoS 0)
  - `read_packet(recv) -> tuple[int, bytes]` คืน (ชนิด, body) โดย `recv(n)` อ่านได้ n ไบต์พอดี
  - `parse_publish(first_byte: int, body: bytes) -> tuple[str, bytes]` (QoS 0 เท่านั้น ถ้า QoS > 0 ให้ข้าม packet id)
  - `MqttClient.subscribe(topic)` รอ SUBACK
  - `MqttClient.recv_publish(timeout: float) -> tuple[str, bytes] | None` ข้าม packet ที่ไม่ใช่ PUBLISH
  - ข้อความ error ใน `_recv_exact` ต้องไม่บอกว่าเป็น CONNACK ทุกครั้ง ให้รับชื่อสิ่งที่รอเป็นพารามิเตอร์

- [ ] **Step 1: เพิ่ม check ใน `selftest()` ที่ยังล้ม**
  - `build_subscribe(1, "v1/devices/me/rpc/request/+")` ต้องเท่ากับ `b"\x82\x20\x00\x01\x00\x1bv1/devices/me/rpc/request/+\x00"`
  - `parse_publish(0x30, b"\x00\x03a/b" + b"hi")` ต้องได้ `("a/b", b"hi")`
  - `read_packet` กับ bytes `b"\x90\x03\x00\x01\x00"` ต้องได้ `(0x90, b"\x00\x01\x00")`

  และสร้าง `test_mqtt_mini.py` ที่ assert `mqtt_mini.selftest() == 0`

- [ ] **Step 2: รันแล้วต้องล้ม**

Run: `python thingsboard/mqtt_mini.py --selftest`
Expected: `AttributeError` หรือ `FAIL`

- [ ] **Step 3: เขียนฟังก์ชันตาม Interfaces**

- [ ] **Step 4: รันแล้วต้องผ่าน**

Run: `python thingsboard/mqtt_mini.py --selftest` และ `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: ไม่มี `FAIL` และ `OK`

- [ ] **Step 5: Commit**

```bash
git add thingsboard/mqtt_mini.py thingsboard/test_mqtt_mini.py
git commit -m "feat(tb): mqtt_mini subscribe and receive for a fake board"
```

---

### Task 6: `rpc_guard_test.py` ทดสอบกับ ThingsBoard จริงด้วยบอร์ดปลอม

**Files:**
- Create: `thingsboard/rpc_guard_test.py`
- Test: `--selftest` ในไฟล์เดียวกัน และ `thingsboard/test_rpc_guard.py` ที่เรียก selftest

**Interfaces:**
- Consumes:
  - `tb_api` (`connect_from_args`, `rpc_twoway`)
  - `tb_setup.ensure_rule_chain`, `ensure_asset_profile`, `ensure_shop_asset` และ `set_bench_direct`
  - `mqtt_mini.MqttClient.subscribe` และ `recv_publish`
  - `rpc_policy.load()`
- Produces: CLI `python thingsboard/rpc_guard_test.py --user <อีเมล> [--customer-user <อีเมลลูกค้า>] [--keep] [--selftest]` พิมพ์ `PASS`/`FAIL`/`SKIP` ทีละเคส แล้วจบด้วย `summary: N PASS, N FAIL, N SKIP` และคืน exit 1 ถ้ามี FAIL

ขั้นเตรียม:
- สร้าง device `fishpump-guardtest` ที่ใช้ profile `fishpump` ถ้ายังไม่มี (แบบ `tb_event_test.py`) แล้วเรียก `ensure_shop_asset` ให้ device นี้
- **บอร์ดปลอม:** thread ที่ต่อ MQTT ด้วย access token ของ device นี้ แล้ว subscribe `v1/devices/me/rpc/request/+` ทุก request ที่ได้รับ เก็บ `(method, params)` ลง list แล้วตอบ `{"ok": true, "echo": method}` ที่ `v1/devices/me/rpc/response/<id>`
- **ค่าคงที่:** `DIRECT_TIMEOUT_MS = 5000` ใช้กับ `rpc_twoway` ส่วน `RESULT_WAIT_S = 30` คือเวลาที่ poll `apply_result`

เคส:
- **G1:** `protection.get` ทางตรงต้องได้ HTTP 200 และบอร์ดปลอมได้รับ
- **G2a:** ทุก method ใน `dangerous_methods` ทางตรงต้องไม่ถึงบอร์ดปลอม (HTTP ไม่ใช่ 200 ภายใน `DIRECT_TIMEOUT_MS`)
- **G2b:** ลบ relation ชั่วคราว แล้ว `protection.set` ทางตรงต้องไม่ถึงบอร์ด จากนั้นใส่ relation กลับ (Review Focus 1)
- **G2c:** `cooling.mode.set {"mode": "test"}` และ `{"mode": "test_on"}` ทางตรงต้องไม่ถึงบอร์ด
- **G2d:** `cooling.mode.set {"mode": "auto"}` ทางตรงต้องถึงบอร์ด (Review Focus 4)
- **G3:** เขียน `apply_request` แบบ `protection.set` ที่ `request_id = "g3-<สุ่ม>"` แล้วต้องได้ `apply_result` ที่ `request_id` ตรงกัน `ok == true` และบอร์ดปลอมได้รับ `protection.set`
- **G4:** ตัด MQTT ของบอร์ดปลอมแล้วส่ง `apply_request` ต้องได้ `apply_result` ที่ `request_id` ตรงกัน `ok == false` และ `error` เป็น `board_offline` หรือ `no_reply` จากนั้นต่อ MQTT กลับ (Review Focus 3)
- **G5:** `apply_request = {"request_id": "g5", "params": {}}` (ไม่มี method) ต้องได้ `error == "bad_request"` (Review Focus 2)
- **G6:** `set_bench_direct(..., True)` แล้ว `protection.set` ทางตรงต้องถึงบอร์ด จากนั้นใน `finally` ต้องคืนเป็น `False`
- **G7:** ทำเมื่อมี `--customer-user` เท่านั้น ถ้าไม่มีให้เป็น `SKIP`
  - มอบ device ให้ customer `ลูกค้าทดสอบ` แล้วล็อกอินเป็นลูกค้า โดยใช้ env `TB_CUSTOMER_PASSWORD` หรือ getpass
  - `GET /api/asset/<id>` ต้องได้ 403 หรือ 404
  - POST server attribute ไปที่ asset ต้องไม่สำเร็จ
  - `protection.set` ทางตรงต้องไม่ถึงบอร์ด
  - `pump.start` ทางตรงต้องถึงบอร์ด
  - สุดท้ายถอนการมอบ device ใน `finally`
- **การเก็บกวาด:** ถ้าไม่ใส่ `--keep` ให้ลบ device และ asset ของ guardtest ตอนจบ

- [ ] **Step 1: เขียน selftest ที่ยังล้ม** ทดสอบ helper บริสุทธิ์ `result_matches(attr_value, request_id) -> bool` และ `rpc_reached(log, method, since_index) -> bool`
  - `result_matches({"request_id": "a", "ok": True}, "a")` ต้องได้ `True`
  - ค่าที่เป็น JSON string และ `request_id` ตรงกัน ต้องได้ `True`
  - `None` หรือ `request_id` ไม่ตรง ต้องได้ `False`

  และสร้าง `test_rpc_guard.py` ที่ assert `rpc_guard_test.selftest() == 0`

- [ ] **Step 2: รันแล้วต้องล้ม**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: ERROR `No module named 'rpc_guard_test'`

- [ ] **Step 3: เขียน `rpc_guard_test.py` ตาม Interfaces และเคส G1–G7**

- [ ] **Step 4: รันแล้วต้องผ่าน (ออฟไลน์)**

Run: `python thingsboard/rpc_guard_test.py --selftest` และ `python -m unittest discover -s thingsboard -p "test_*.py" -v`
Expected: `SELFTEST PASS` และ `OK`

- [ ] **Step 5: Commit**

```bash
git add thingsboard/rpc_guard_test.py thingsboard/test_rpc_guard.py
git commit -m "test(tb): live rpc guard test with a fake MQTT board"
```

---

### Task 7: เอกสารและคู่มือทดสอบ bench

**Files:**
- Modify: `thingsboard/README.md` (ส่วนไฟล์ วิธีรัน และลำดับการทำงานของ rule chain)
- Modify: `docs/firmware/device-api.md` ข้อ 10 Known limits
- Create: `docs/bench-test-tb-shop-path.md`

**Interfaces:**
- Consumes: CLI ของ Task 4 และ Task 6

- [ ] **Step 1: แก้ `thingsboard/README.md`**
  - **ส่วนไฟล์:** เพิ่ม `rpc_policy.json`, `fishpump_shop.json` และ `rpc_guard_test.py`
  - **วิธีรัน:** เพิ่ม `--allow-direct` / `--block-direct`
  - **ลำดับการทำงาน:** เพิ่มย่อหน้าเรื่องทางตรงที่ถูกทิ้ง และทางของร้าน (`apply_request` → `apply_result` พร้อมรายการ `error`)
  - **ข้อสังเกต:** dashboard ทดสอบและ `parity_test.py` จะใช้คำสั่งอันตรายได้เฉพาะบอร์ดที่ `--allow-direct`

- [ ] **Step 2: แก้ `docs/firmware/device-api.md` ข้อ 10** เพิ่มหนึ่งข้อว่า rule chain `fishpump` ทิ้งคำสั่งในรายการ `thingsboard/rpc_policy.json` ที่ส่งทางตรง ผู้เรียกจะเห็นแค่หมดเวลา ส่วนแอปต้องใช้ทางของร้าน

- [ ] **Step 3: เขียน `docs/bench-test-tb-shop-path.md`** เป็นภาษาไทยตามรูปแบบของ `docs/bench-test-firmware-0.5.md` ข้อละ "ทำ / ต้องเห็น / ผ่าน บันทึก"
  - **T1:** รัน `python thingsboard/tb_setup.py --user <อีเมล> --allow-direct fishpump-1de564`
    - **ต้องเห็น:** rule chain ทั้งสองบันทึกได้ asset `shop-fishpump-1de564` ไม่ได้มอบให้ลูกค้า และ device มี `inactivityTimeout` = 180000
  - **T2:** รัน `python thingsboard/rpc_guard_test.py --user <อีเมล> --customer-user <อีเมลลูกค้าทดสอบ>`
    - **ต้องเห็น:** `0 FAIL`
  - **T3:** บอร์ดจริงที่ `--block-direct`
    - ส่ง `protection.set` ทางตรงแล้วต้องหมดเวลา และ `protection.get` ต้องได้ค่าเดิม
    - เขียน `apply_request` แบบ `protection.set` ค่าเดิมที่ asset แล้วต้องได้ `apply_result` ที่ `ok` เป็น true
  - **T4:** ถอดไฟบอร์ด แล้ว `active` ใน ThingsBoard ต้องเป็น false ภายในราว 3–4 นาที
  - **T5:** `--allow-direct fishpump-1de564` แล้ว `parity_test.py` ต้องได้ `0 FAIL`
  - **ท้ายคู่มือ:** คืนสภาพบอร์ดทดสอบเป็น `--allow-direct`

- [ ] **Step 4: ตรวจทั้งหมด**

Run: `python -m unittest discover -s thingsboard -p "test_*.py" -v`, `python thingsboard/tb_setup.py --check` และ `for f in firmware/scripts/verify_*.py; do python "$f" || exit 1; done` (bash)
Expected: `OK`, `check OK` และ verify ทุกตัวไม่ล้ม

- [ ] **Step 5: Commit**

```bash
git add thingsboard/README.md docs/firmware/device-api.md docs/bench-test-tb-shop-path.md
git commit -m "docs(tb): shop path, direct RPC drop and bench checklist"
```

---

## หลังจบแผน

- **ทดสอบบน ThingsBoard จริง:** T1–T5 ใน `docs/bench-test-tb-shop-path.md` ต้องใช้รหัสผ่าน ThingsBoard และรหัสผ่านของลูกค้าทดสอบ
  - Claude รันคำสั่งในแท็บเทอร์มินัลได้ แต่ผู้ใช้ต้องพิมพ์รหัสผ่านเอง
  - Claude ห้ามรัน `tb_setup.py` กับ ThingsBoard จริงโดยไม่ถามก่อน เพราะคำสั่งนี้แก้ rule chain ที่บอร์ดจริงใช้อยู่
- **ยังไม่อยู่ในแผนนี้:**
  - การตั้ง mail server ของ ThingsBoard ซึ่งผู้ใช้ทำเอง
  - user settings (ธีม ขนาดตัวหนังสือ ค่าไฟต่อหน่วย) ซึ่งเป็นค่าของผู้ใช้แต่ละคน แอปในแผน 3 เขียนเองตอนผู้ใช้กดตั้ง `tb_setup.py` จึงไม่ต้องสร้าง (ผมตัดสินเองจาก spec ข้อ 7 แถว ง1–ง6)
  - ชื่อร้านและเบอร์ช่างใน attribute ของ Customer ซึ่งแอปของเจ้าของร้านเขียน (spec ข้อ 14.3)
  - แอปในแผน 3 ซึ่งจะอ่าน `rpc_policy.json` และใช้สัญญา `apply_request` / `apply_result` จาก Task 3
