# เฟิร์มแวร์: ขีดปลอดภัยกันปั๊มเดินแห้ง และหยุดปั๊มแบบตั้งเวลา Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** แคบขีดของการกันปั๊มเดินแห้ง ไม่ปิดการกันเองเมื่อค่าเก่านอกช่วง บอกขีดให้แอปรู้ และให้หยุดปั๊มแบบเปิดกลับเองได้ (F1–F3)

**Architecture:** ตรรกะใหม่ทั้งหมดอยู่ในคอมโพเนนต์บริสุทธิ์ที่ทดสอบใน QEMU selftest ได้ ได้แก่ `dry_run_logic` ที่มีอยู่ และคอมโพเนนต์ใหม่ `pump_resume` ส่วน glue ที่แตะ NVS, FreeRTOS และ device_api ไม่มีตรรกะของตัวเอง และตรวจบน bench

**Tech Stack:** ESP-IDF v6.0.1 (C), cJSON, QEMU selftest (`firmware/test/sensor_selftest`)

**Spec:** `docs/superpowers/specs/2026-11-24-frontend-app-design.md` ข้อ 13 (F1, F2, F3) และข้อ 2 (สภาพปัจจุบันของบอร์ด)

## Global Constraints

- ขีดใหม่: `prime_sec` 5..120, `dry_trip_sec` 3..30, `retry_wait_min` 1..60, `max_retries` 0..5 (คงเดิม) เพดานของการรอแบบทวีคูณคือ 60 นาที
- ผลรวม `prime_sec` + `dry_trip_sec` ไม่เกิน 150 ช่วงข้างบนบังคับไว้แล้ว (120 + 30) จึงไม่ต้องตรวจซ้ำ
- ค่าเก่านอกช่วงต้องถูกบีบเข้าขอบ และคงค่า `enabled` เดิม ห้ามตกไปใช้ค่าเริ่มต้นซึ่งปิดการกันปั๊มเดินแห้ง
- `resume_after_min` เป็นจำนวนเต็ม 5..120 ใช้ได้กับ `pump.control` ที่ `action` เป็น `"stop"` และ `system` เป็น `"water"` หรือ `"all"` เท่านั้น
- ไม่เก็บเวลาเปิดกลับข้ามการรีสตาร์ต
- เปิดกลับผ่าน `pump.start` เท่านั้น เพื่อให้การตรวจกันปั๊มเดินแห้ง (`pump_start_blocked`) ยังทำงาน
- selftest ทั้งชุดต้องจบด้วย `SELFTEST PASS`: `firmware/test/sensor_selftest/run_selftest.ps1`
- ห้าม stage `firmware/.vscode/settings.json` และ `docs/bench-test-alarms.md`
- commit ลงท้ายด้วย `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` ห้าม push จนกว่าผู้ใช้สั่ง

## Review Focus

1. **บอร์ดลูกค้าที่เก็บ `prime_sec` = 300 แล้วอัปเดตเป็นรุ่นใหม่:** การกันปั๊มเดินแห้งต้องยังเปิดอยู่ ค่าถูกบีบเป็น 120 และ `config_source` เป็น `"clamped"` ทดสอบที่ Task 2 (ตรรกะ) และ bench B2
2. **ตัวจับเวลาเปิดกลับข้ามจุดที่ millis วนรอบ (0xFFFFFFFF):** ต้องเปิดตรงเวลาและเปิดครั้งเดียว ทดสอบที่ Task 4
3. **ถึงเวลาเปิดกลับตอนปั๊มถูกล็อกหรือกำลังรอ:** ปั๊มต้องไม่เปิด และตัวจับเวลาต้องหายไป ไม่ลองซ้ำ ตรวจที่ bench B5 เพราะผ่าน `pump_start_blocked` ตัวเดิม
4. **`pump.control` ที่มี `resume_after_min` แต่ไม่มี `action`:** ค่าเริ่มต้นของ action คือ start จึงต้องได้ 400 `invalid_resume` ไม่ใช่เปิดปั๊ม ทดสอบที่ Task 4
5. **หยุดแบบตั้งเวลาจากแอป แล้วมีคนกดเปิดหรือหยุดจากหน้าเว็บของบอร์ด:** ตัวจับเวลาต้องถูกยกเลิก และไม่มีการเปิดเองทีหลัง ทดสอบที่ Task 4 (cancel) และ bench B6

---

### Task 1: ขีดใหม่ของการกันปั๊มเดินแห้ง

**Files:**
- Modify: `firmware/components/dry_run_logic/include/dry_run_logic.h:6-12`
- Test: `firmware/test/sensor_selftest/main/test_dry_run.c:13-27`

**Interfaces:**
- Produces: `DRY_RUN_PRIME_SEC_MIN 5`, `DRY_RUN_PRIME_SEC_MAX 120`, `DRY_RUN_TRIP_SEC_MIN 3`, `DRY_RUN_TRIP_SEC_MAX 30`, `DRY_RUN_WAIT_MIN_MIN 1`, `DRY_RUN_WAIT_MIN_MAX 60`, `DRY_RUN_RETRIES_MAX 5` (ชื่อเดิม ค่าใหม่)

- [ ] **Step 1: แก้ test ใน `test_dry_run_config()` ให้ตรงขีดใหม่**

```c
c.prime_sec = 4;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
c.prime_sec = 5;   CHECK(dry_run_config_validate(&c, &bad));
c.prime_sec = 120; CHECK(dry_run_config_validate(&c, &bad));
c.prime_sec = 121; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
dry_run_config_defaults(&c); c.dry_trip_sec = 30; CHECK(dry_run_config_validate(&c, &bad));
dry_run_config_defaults(&c); c.dry_trip_sec = 31; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "dry_trip_sec") == 0);
dry_run_config_defaults(&c); c.retry_wait_min = 60; CHECK(dry_run_config_validate(&c, &bad));
dry_run_config_defaults(&c); c.retry_wait_min = 61; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "retry_wait_min") == 0);
/* backoff cap is now 60 min */
dry_run_config_defaults(&c); c.retry_wait_min = 40; CHECK(dry_run_wait_ms(&c, 2) == 60u * 60000u);
c.retry_wait_min = 60; CHECK(dry_run_wait_ms(&c, 5) == 60u * 60000u);
CHECK(dry_run_suggest_prime_sec(1000000) == 120);  /* clamp */
```

ลบบรรทัดเดิมที่ใช้ 601, 240 และ 600 ส่วนบรรทัดเดิมของ 0, 2, 0 และ 6 เก็บไว้ได้ ค่าเริ่มต้น (15, 10, 5, 3) ยังอยู่ในช่วง

- [ ] **Step 2: รัน selftest ให้เห็นว่าล้ม**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST FAIL` โดยมี CHECK ที่ล้มอยู่ในบรรทัดที่เพิ่งแก้ใน `test_dry_run.c`

- [ ] **Step 3: แก้ค่าคงที่ใน `dry_run_logic.h` ตาม Interfaces**

`dry_run_wait_ms` และ `dry_run_suggest_prime_sec` ใช้ค่าคงที่เดิมอยู่แล้ว จึงไม่ต้องแก้

- [ ] **Step 4: รัน selftest อีกครั้ง**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST PASS`

ถ้า `test_dry_run_machine` ล้มเพราะเวลาในตัวอย่างเกินขีดใหม่ ให้แก้ตัวอย่างนั้นแล้วบันทึกเป็น ruling

- [ ] **Step 5: Commit**

```bash
git add firmware/components/dry_run_logic/include/dry_run_logic.h firmware/test/sensor_selftest/main/test_dry_run.c
git commit -m "firmware: narrow dry-run limits (prime 5-120 s, trip 3-30 s, wait 1-60 min)"
```

### Task 2: บีบค่าเก่าแทนการตกไปใช้ค่าเริ่มต้น และบอกที่มาของค่า

**Files:**
- Modify: `firmware/components/dry_run_logic/include/dry_run_logic.h`, `firmware/components/dry_run_logic/dry_run_logic.c`
- Modify: `firmware/components/nvs_store/include/nvs_store.h`, `firmware/components/nvs_store/nvs_store.c:2010-2054`
- Modify: `firmware/components/dry_run_guard/include/dry_run_guard.h:23-30`, `firmware/components/dry_run_guard/dry_run_guard.c:319-330, 397-…`
- Modify: `firmware/main/dapi_protection.c:21-49`
- Test: `firmware/test/sensor_selftest/main/test_dry_run.c`

**Interfaces:**
- Consumes: ค่าคงที่จาก Task 1
- Produces:
  - `typedef enum { DRY_RUN_CFG_STORED = 0, DRY_RUN_CFG_CLAMPED, DRY_RUN_CFG_DEFAULTS, DRY_RUN_CFG_DEFAULTS_AFTER_ERROR } dry_run_cfg_source_t;`
  - `bool dry_run_config_clamp(dry_run_config_t *cfg);` บีบแต่ละช่องเข้าขอบที่ใกล้ที่สุด ไม่แตะ `enabled` และ `retry_backoff` คืน true เมื่อมีค่าเปลี่ยน
  - `const char *dry_run_cfg_source_name(uint8_t source);` คืน `"stored"`, `"clamped"`, `"defaults"`, `"defaults_after_error"` ค่าอื่นได้ `"?"`
  - `bool nvs_store_load_dry_run(dry_run_config_t *cfg, bool *locked, dry_run_cfg_source_t *source);`
  - `dry_run_guard_status_t.cfg_source` (`uint8_t`)
  - `protection.get` มี `"config_source":"<name>"` ที่ระดับบนสุด และจึงมีอยู่ใน attribute `cfg_protection` ด้วย

- [ ] **Step 1: เขียน test `test_dry_run_config()` ส่วน clamp และชื่อที่มาของค่า**

```c
dry_run_config_t k;
dry_run_config_defaults(&k); k.enabled = true;
k.prime_sec = 300; k.dry_trip_sec = 90; k.retry_wait_min = 240; k.max_retries = 9;
CHECK(dry_run_config_clamp(&k));
CHECK(k.enabled && k.prime_sec == 120 && k.dry_trip_sec == 30 && k.retry_wait_min == 60 && k.max_retries == 5);
CHECK(dry_run_config_validate(&k, &bad));
k.prime_sec = 1; k.dry_trip_sec = 0; k.retry_wait_min = 0;
CHECK(dry_run_config_clamp(&k) && k.prime_sec == 5 && k.dry_trip_sec == 3 && k.retry_wait_min == 1);
dry_run_config_defaults(&k);
CHECK(!dry_run_config_clamp(&k));                       /* already valid: unchanged */
CHECK(strcmp(dry_run_cfg_source_name(DRY_RUN_CFG_CLAMPED), "clamped") == 0);
CHECK(strcmp(dry_run_cfg_source_name(DRY_RUN_CFG_DEFAULTS_AFTER_ERROR), "defaults_after_error") == 0);
CHECK(strcmp(dry_run_cfg_source_name(42), "?") == 0);
```

- [ ] **Step 2: รัน selftest ให้เห็นว่าล้ม**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: build ล้ม เพราะยังไม่มี `dry_run_config_clamp` (นับเป็น RED)

- [ ] **Step 3: เขียน `dry_run_config_clamp` และ `dry_run_cfg_source_name` ใน `dry_run_logic.c`**

- [ ] **Step 4: รัน selftest**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST PASS`

- [ ] **Step 5: ต่อ glue (ไม่มีตรรกะใหม่ ตรวจบน bench)**

`nvs_store_load_dry_run` ตั้ง `*source` ดังนี้:

| กรณี | `*source` |
|---|---|
| ไม่เคยบันทึก (namespace หรือ key ไม่มี) | `DEFAULTS` |
| blob ถูกต้องและผ่านการตรวจ | `STORED` |
| blob ถูกต้องแต่ไม่ผ่าน → `dry_run_config_clamp` แล้วใช้ค่าที่บีบ | `CLAMPED` |
| version หรือความยาวผิด หรืออ่านล้ม | `DEFAULTS_AFTER_ERROR` |

- ไม่เขียนค่าที่บีบกลับลง NVS เพื่อให้ `"clamped"` ยังแสดงจนกว่าช่างจะบันทึกใหม่
- log ของกรณีบีบคือ `Dry-run settings out of range (<field>); clamped`
- `dry_run_guard` เก็บที่มาไว้ใน `s_cfg_source` และเปลี่ยนเป็น `STORED` เมื่อ `dry_run_guard_set_config` บันทึกสำเร็จ
- `dry_run_guard_get_status` คัดลอกค่านี้ลง `cfg_source`
- `protection_status` เพิ่ม `cJSON_AddStringToObject(root, "config_source", dry_run_cfg_source_name(gs.cfg_source))`
- แก้ทุกที่ที่เรียก `nvs_store_load_dry_run` ให้ตรง signature ใหม่ (ค้นด้วย grep)

- [ ] **Step 6: build เฟิร์มแวร์**

Run: `idf.py -C firmware build`
Expected: build ผ่าน ไม่มี warning ใหม่

- [ ] **Step 7: Commit**

```bash
git add firmware/components/dry_run_logic firmware/components/nvs_store firmware/components/dry_run_guard firmware/main/dapi_protection.c firmware/test/sensor_selftest/main/test_dry_run.c
git commit -m "firmware: clamp out-of-range dry-run settings instead of disabling protection"
```

### Task 3: บอร์ดบอกขีดของตัวเองใน `protection.get`

**Files:**
- Modify: `firmware/components/dry_run_logic/include/dry_run_logic.h`, `firmware/components/dry_run_logic/dry_run_logic.c`
- Modify: `firmware/main/dapi_protection.c:21-49`
- Test: `firmware/test/sensor_selftest/main/test_dry_run.c`

**Interfaces:**
- Produces:
  - `typedef struct { uint16_t prime_min, prime_max, trip_min, trip_max, wait_min_min, wait_min_max; uint8_t retries_max; } dry_run_limits_t;`
  - `void dry_run_limits(dry_run_limits_t *out);`
  - `protection.get` มี `"limits":{"prime_sec":[5,120],"dry_trip_sec":[3,30],"retry_wait_min":[1,60],"max_retries":[0,5]}`

- [ ] **Step 1: เขียน test**

```c
dry_run_limits_t L;
dry_run_limits(&L);
CHECK(L.prime_min == 5 && L.prime_max == 120 && L.trip_min == 3 && L.trip_max == 30 &&
      L.wait_min_min == 1 && L.wait_min_max == 60 && L.retries_max == 5);
```

- [ ] **Step 2: รัน selftest ให้เห็นว่าล้ม**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: build ล้ม เพราะยังไม่มี `dry_run_limits`

- [ ] **Step 3: เขียน `dry_run_limits` จากค่าคงที่ของ Task 1 แล้วให้ `protection_status` เพิ่ม `limits` ตามรูปใน Interfaces**

array ของแต่ละช่องคือ `[min, max]`

- [ ] **Step 4: รัน selftest และ build**

Run: `firmware/test/sensor_selftest/run_selftest.ps1` แล้ว `idf.py -C firmware build`
Expected: `SELFTEST PASS` และ build ผ่าน

- [ ] **Step 5: Commit**

```bash
git add firmware/components/dry_run_logic firmware/main/dapi_protection.c firmware/test/sensor_selftest/main/test_dry_run.c
git commit -m "firmware: report dry-run limits and config source in protection.get"
```

### Task 4: คอมโพเนนต์ `pump_resume` (ตรรกะบริสุทธิ์)

**Files:**
- Create: `firmware/components/pump_resume/CMakeLists.txt`, `firmware/components/pump_resume/include/pump_resume.h`, `firmware/components/pump_resume/pump_resume.c`
- Create: `firmware/test/sensor_selftest/main/test_pump_resume.c`
- Modify:
  - `firmware/test/sensor_selftest/main/selftest_main.c` (เรียก `test_pump_resume()` ต่อจาก `test_dry_run_machine()`)
  - `firmware/test/sensor_selftest/main/selftest.h` (ประกาศฟังก์ชัน)
  - `firmware/test/sensor_selftest/main/CMakeLists.txt` (เพิ่มไฟล์และ `pump_resume` ใน REQUIRES)

**Interfaces:**
- Produces (`pump_resume.h`):

```c
#define PUMP_RESUME_MIN_MIN 5
#define PUMP_RESUME_MIN_MAX 120
typedef struct { bool armed; uint32_t due_ms; } pump_resume_t;
typedef enum { PUMP_RESUME_PARAM_NONE = 0, PUMP_RESUME_PARAM_OK, PUMP_RESUME_PARAM_BAD_COMBO, PUMP_RESUME_PARAM_OUT_OF_RANGE } pump_resume_param_t;
/* has_field: the key is present. is_number: cJSON_IsNumber. is_stop/is_water: the parsed pump.control action/system. */
pump_resume_param_t pump_resume_check_param(bool has_field, bool is_number, double value,
                                            bool is_stop, bool is_water, uint16_t *minutes);
void pump_resume_arm(pump_resume_t *r, uint32_t now_ms, uint16_t minutes);
void pump_resume_cancel(pump_resume_t *r);
uint32_t pump_resume_remaining_sec(const pump_resume_t *r, uint32_t now_ms); /* 0 when not armed; rounds up */
bool pump_resume_take_due(pump_resume_t *r, uint32_t now_ms);                /* true once, then disarmed */
```

- [ ] **Step 1: เขียน `test_pump_resume()`**

```c
uint16_t m = 0;
CHECK(pump_resume_check_param(false, false, 0, true, true, &m) == PUMP_RESUME_PARAM_NONE);
CHECK(pump_resume_check_param(true, true, 15, true, true, &m) == PUMP_RESUME_PARAM_OK && m == 15);
CHECK(pump_resume_check_param(true, true, 5, true, true, &m) == PUMP_RESUME_PARAM_OK);
CHECK(pump_resume_check_param(true, true, 120, true, true, &m) == PUMP_RESUME_PARAM_OK);
CHECK(pump_resume_check_param(true, true, 4, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
CHECK(pump_resume_check_param(true, true, 121, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
CHECK(pump_resume_check_param(true, true, 15.5, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
CHECK(pump_resume_check_param(true, false, 0, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
CHECK(pump_resume_check_param(true, true, 15, false, true, &m) == PUMP_RESUME_PARAM_BAD_COMBO); /* start, incl. default action */
CHECK(pump_resume_check_param(true, true, 15, true, false, &m) == PUMP_RESUME_PARAM_BAD_COMBO); /* wash only */

pump_resume_t r = {0};
CHECK(pump_resume_remaining_sec(&r, 1000) == 0 && !pump_resume_take_due(&r, 1000));
pump_resume_arm(&r, 1000, 15);
CHECK(pump_resume_remaining_sec(&r, 1000) == 900);
CHECK(pump_resume_remaining_sec(&r, 1500) == 900);          /* rounds up */
CHECK(!pump_resume_take_due(&r, 1000 + 899999u));
CHECK(pump_resume_take_due(&r, 1000 + 900000u));
CHECK(!r.armed && !pump_resume_take_due(&r, 1000 + 900001u)); /* once */
pump_resume_arm(&r, 0, 30); pump_resume_cancel(&r);
CHECK(!pump_resume_take_due(&r, 30u * 60000u) && pump_resume_remaining_sec(&r, 0) == 0);
/* millis wrap */
pump_resume_arm(&r, 0xFFFFF000u, 5);
CHECK(!pump_resume_take_due(&r, 0xFFFFFFFFu));
CHECK(pump_resume_remaining_sec(&r, 0x00000F00u) == 293u);     /* 7936 ms gone, 292.064 s left, rounds up */
CHECK(pump_resume_take_due(&r, 0xFFFFF000u + 300000u));
```

- [ ] **Step 2: รัน selftest ให้เห็นว่าล้ม**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: build ล้ม เพราะยังไม่มีคอมโพเนนต์ `pump_resume`

- [ ] **Step 3: เขียน `pump_resume.c`**

- เทียบเวลาด้วย `(int32_t)(now_ms - due_ms) >= 0` เพื่อให้ปลอดภัยเมื่อ millis วนรอบ
- `pump_resume_check_param`:
  - คืน `NONE` เมื่อไม่มี field
  - ถ้ามี field ตรวจตามลำดับ: action ต้องเป็น stop และ system ต้องเป็น water มิฉะนั้นได้ `BAD_COMBO`
  - จากนั้นต้องเป็นตัวเลขจำนวนเต็มที่ finite อยู่ใน 5..120 มิฉะนั้นได้ `OUT_OF_RANGE`

- [ ] **Step 4: รัน selftest**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST PASS`

- [ ] **Step 5: Commit**

```bash
git add firmware/components/pump_resume firmware/test/sensor_selftest/main
git commit -m "firmware: add pump_resume logic for timed pump stop"
```

### Task 5: ต่อการหยุดแบบตั้งเวลาเข้ากับ `pump.control` และ telemetry

**Files:**
- Create: `firmware/main/pump_resume_service.c`, `firmware/main/pump_resume_service.h`
- Modify:
  - `firmware/main/CMakeLists.txt` (เพิ่มไฟล์และ `pump_resume`)
  - `firmware/main/app_main.c` (เรียก `pump_resume_service_init()` หลังลงทะเบียน device_api)
  - `firmware/main/dapi_pump.c:886-995` และตัวสร้างสถานะ `api_pump_add_status_fields_gen` / cJSON ที่บรรทัด ~254 และ ~369
- Modify: `firmware/components/uplink_format/uplink_format.c:366-369` และ `firmware/components/uplink_format/include/uplink_format.h:22`
- Test: `firmware/test/sensor_selftest/main/test_uplink_state.c` และ `test_uplink_format.c` (ตรงที่ตรวจ `contract_version`)

**Interfaces:**
- Consumes: `pump_resume.h` จาก Task 4
- Produces:
  - `void pump_resume_service_init(void);`
  - `void pump_resume_service_arm(uint16_t minutes);`
  - `void pump_resume_service_cancel(void);`
  - `uint32_t pump_resume_service_remaining_sec(void);`
  - `pump.status` → `system_water.resume_in_sec`
  - status telemetry `pump_resume_sec` (ไม่ watched)
  - `UPLINK_CONTRACT_VERSION 2`

- [ ] **Step 1: เขียน test ฝั่ง uplink**

- ใน `test_uplink_state.c` เพิ่ม `"resume_in_sec":600` ใน `system_water` ของตัวอย่าง `pump.status` แล้วตรวจ `CHECK(cJSON_IsNumber(get(out, "pump_resume_sec")) && get(out, "pump_resume_sec")->valuedouble == 600);`
- ทุกที่ที่ตรวจ `contract_version == 1` ให้เปลี่ยนเป็น 2
- ถ้ามี test ที่นับจำนวน key ของ status ให้เพิ่มอีก 1

- [ ] **Step 2: รัน selftest ให้เห็นว่าล้ม**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST FAIL` ที่ `pump_resume_sec` และ `contract_version`

- [ ] **Step 3: เพิ่มแถว `{UPLINK_SRC_PUMP, "system_water.resume_in_sec", "pump_resume_sec", false}` ต่อจาก `pump_cooldown_sec` แล้วตั้ง `UPLINK_CONTRACT_VERSION 2`**

- [ ] **Step 4: รัน selftest**

Run: `firmware/test/sensor_selftest/run_selftest.ps1`
Expected: `SELFTEST PASS`

- [ ] **Step 5: เขียน `pump_resume_service.c`**

- task เดียว stack 3072 ไบต์ ตื่นทุก 1 วินาที ถือ `pump_resume_t` ไว้ใต้ mutex
- เมื่อ `pump_resume_take_due` คืน true ให้เรียก `device_api_call("pump.start", NULL, DAPI_SRC_HTTP, &reply)` นอก mutex
- log ผลเป็น `timed stop over: pump.start -> <status>` แล้ว `cJSON_Delete(reply)`
- ถ้า start ถูกปฏิเสธ (เช่น `dry_run_locked`) ไม่ลองซ้ำ

- [ ] **Step 6: แก้ `dapi_pump.c`**

`pump.control`:
- อ่าน `resume_after_min` แล้วเรียก `pump_resume_check_param` หลังรู้ `is_stop` และ `do_water`
- `BAD_COMBO` ตอบ 400 `dapi_error("invalid_resume", "resume_after_min needs action stop and system water or all")`
- `OUT_OF_RANGE` ตอบ 400 `dapi_error("value_out_of_range", "resume_after_min")`
- ทั้งสองกรณีตอบก่อนแตะปั๊ม
- เมื่อ `do_water`:
  - เรียก `pump_resume_service_cancel()` ก่อนสั่ง
  - ถ้าผลเป็น `OK` และหยุดน้ำสำเร็จ ให้เรียก `pump_resume_service_arm(minutes)`

`pump.start` และ `pump.stop`: เรียก `pump_resume_service_cancel()` ก่อนสั่ง

ตัวสร้างสถานะทั้งสองตัวเพิ่ม `resume_in_sec` = `pump_resume_service_remaining_sec()` ใน `system_water`

- [ ] **Step 7: build**

Run: `idf.py -C firmware build`
Expected: build ผ่าน

- [ ] **Step 8: Commit**

```bash
git add firmware/main firmware/components/uplink_format firmware/test/sensor_selftest/main
git commit -m "firmware: timed pump stop via pump.control resume_after_min"
```

### Task 6: เอกสาร เวอร์ชัน และ checklist สำหรับ bench

**Files:**
- Modify: `docs/firmware/device-api.md`
  - ข้อ 3: ขีดใหม่, `limits`, `config_source`
  - ข้อ 4: `resume_after_min`, `invalid_resume`, `resume_in_sec`
  - ข้อ 11.2: แถว `pump_resume_sec`, `contract_version` 2
- Modify: `firmware/version.txt` (0.4.0 → 0.5.0 ตาม `docs/release/release.md`)
- Create: `docs/bench-test-firmware-0.5.md`

- [ ] **Step 1: แก้ `device-api.md` ให้ตรงกับโค้ดของ Task 1–5**

ทุกค่าต้องคัดจากโค้ด ไม่คัดจากแผนนี้

- [ ] **Step 2: เปลี่ยนเวอร์ชันเป็น 0.5.0 แล้วรัน selftest และ build อีกรอบ**

Run: `firmware/test/sensor_selftest/run_selftest.ps1` แล้ว `idf.py -C firmware build`
Expected: `SELFTEST PASS` และ build ผ่าน

- [ ] **Step 3: เขียน checklist ใน `docs/bench-test-firmware-0.5.md`**

ผู้ใช้เป็นคนทดสอบ แต่ละข้อบอกขั้นตอนและผลที่ต้องเห็น

| ข้อ | ทดสอบ |
|---|---|
| B1 | ตั้ง `prime_sec` = 121 และ `dry_trip_sec` = 31 จากหน้าเว็บของบอร์ดและจาก RPC ต้องได้ 400 `value_out_of_range` |
| B2 | บอร์ดที่รุ่น 0.4.0 ตั้ง `prime_sec` = 300 ไว้ แฟลชรุ่นใหม่แล้ว `protection.get` ต้องได้ `enabled` เดิม, `prime_sec` = 120 และ `config_source` = `"clamped"` จากนั้นบันทึกใหม่ต้องได้ `"stored"` |
| B3 | `protection.get` และ attribute `cfg_protection` มี `limits` |
| B4 | `pump.control` stop พร้อม `resume_after_min` = 5 ต้องได้ `pump_resume_sec` ลดลงใน ThingsBoard และปั๊มเปิดเองเมื่อครบ 5 นาที |
| B5 | ระหว่างหยุดแบบตั้งเวลา ทำให้ปั๊มถูกล็อก (ถอดสายสวิตช์น้ำไหล) เมื่อครบเวลาปั๊มต้องไม่เปิด และ log ต้องมี `timed stop over` |
| B6 | หยุดแบบตั้งเวลาแล้วกดเปิดจากหน้าเว็บของบอร์ด `resume_in_sec` ต้องกลับเป็น 0 |
| B7 | รีสตาร์ตบอร์ดระหว่างหยุดแบบตั้งเวลา ปั๊มต้องทำตาม `water_auto_start` |
| B8 | รัน `python thingsboard/parity_test.py` บน bench แล้วผลตรงกันทั้งสองทาง |

- [ ] **Step 4: Commit**

```bash
git add docs/firmware/device-api.md firmware/version.txt docs/bench-test-firmware-0.5.md
git commit -m "docs: device API and bench checklist for firmware 0.5.0"
```
