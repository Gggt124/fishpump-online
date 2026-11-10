# PT100 MAX31865 & Inverter PWM-to-VDC Cooling Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace DS18B20 1-Wire temperature sensor and on/off relay cooling control with MAX31865 SPI PT100 RTD sensor and ESP32-S3 LEDC hardware PWM driving a 0–10V DC inverter speed modulation module, with a multi-point rule engine supporting Linear Interpolation and Step modes.

**Architecture:** A modular embedded C implementation on ESP32-S3:
- Dedicated SPI2 hardware driver interfacing with the MAX31865 RTD converter on GPIO 10, 11, 12, 13 (with automatic fault clearing and fail-safe shutdown).
- Dedicated LEDC timer PWM driver modulating GPIO 8 (1 kHz, 10-bit resolution) for a 0–10V DAC module.
- Multi-point rule evaluator (up to 8 points) supporting both piecewise linear interpolation and discrete steps, with compressor anti-short-cycle lockout and soft slew rate control.
- Updated NVS storage, REST API (`/api/cooling/*`), real-time SSE telemetry, and an interactive Web UI dashboard with SVG curve preview.
- Strict GPIO protection preserving pins for future RS485 and dual PZEM-004T devices.

**Tech Stack:** ESP-IDF v5.x (C), FreeRTOS, `esp-idf-lib/max31865`, ESP32-S3 `driver/ledc`, `driver/spi_master`, cJSON, HTML5/CSS/Vanilla JS (Web SPA).

---

## Global Constraints

- **Sensor Pins (SPI2 Native):** CS = GPIO 10, MOSI = GPIO 11, SCK = GPIO 12, MISO = GPIO 13.
- **Cooling Output:** PWM on GPIO 8 (LEDC Timer 0, Channel 0, 1 kHz, 10-bit).
- **Reserved Unchanged Pins:** Float Switch = GPIO 16, Pump Relay 1 = GPIO 5, Pump Relay 2 = GPIO 6.
- **Reserved Future Pins:** GPIO 1, 2 (RS485), GPIO 17, 18 (dual PZEM-004T shared UART bus), GPIO 7 (RS485 DE/RE or spare).
- **Max Rule Points:** 8 points, strictly sorted by temperature $T_0 < T_1 < \dots < T_{N-1}$.
- **Voltage Range:** 0 mV to 10,000 mV ($0.0\,\text{V}$ to $10.0\,\text{V}$ DC).
- **Fail-Safe Behavior:** On sensor disconnect or fault, PWM duty immediately drops to 0% ($0.0\,\text{V}$).

---

### Task 1: Hardware Pinout & Data Model Updates in `hardware_map` & `app_config`

**Files:**
- Modify: `components/hardware_map/include/hardware_map.h`
- Modify: `components/hardware_map/hardware_map.c`
- Modify: `components/app_config/include/app_config.h`

**Interfaces:**
- Consumes: ESP-IDF `gpio_num_t`.
- Produces: `HARDWARE_ROLE_COOLING_PWM` (aliased to GPIO 8), MAX31865 SPI pin definitions (`APP_TEMPLATE_MAX31865_CS_GPIO 10`, etc.).

- [ ] **Step 1: Update `app_config.h` with MAX31865 SPI and Cooling PWM definitions**
Add default pins and initial settings:
```c
#define APP_TEMPLATE_MAX31865_SPI_HOST SPI2_HOST
#define APP_TEMPLATE_MAX31865_CS_GPIO 10
#define APP_TEMPLATE_MAX31865_MOSI_GPIO 11
#define APP_TEMPLATE_MAX31865_CLK_GPIO 12
#define APP_TEMPLATE_MAX31865_MISO_GPIO 13
#define APP_TEMPLATE_COOLING_PWM_GPIO 8

#define APP_TEMPLATE_COOLING_DEFAULT_P0_TEMP_X10 240
#define APP_TEMPLATE_COOLING_DEFAULT_P0_MV 5000
#define APP_TEMPLATE_COOLING_DEFAULT_P1_TEMP_X10 350
#define APP_TEMPLATE_COOLING_DEFAULT_P1_MV 10000
#define APP_TEMPLATE_COOLING_MIN_OFF_SEC 180
#define APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC 30
```

- [ ] **Step 2: Update `hardware_map.h` and `hardware_map.c`**
Update `hardware_role_t` and `hardware_map_t` to replace or re-label `HARDWARE_ROLE_DS18B20_DATA` with `HARDWARE_ROLE_MAX31865_CS` (or general MAX31865 role) and `HARDWARE_ROLE_COOLING_RELAY` with `HARDWARE_ROLE_COOLING_PWM`.
Ensure the safe pin validator allows GPIO 8 for Cooling PWM, and pins 10, 11, 12, 13 for SPI2.

- [ ] **Step 3: Verify build of `hardware_map`**
Run: `idf.py reconfigure` to verify configuration syntax.

- [ ] **Step 4: Commit Task 1**
```bash
git add components/hardware_map components/app_config
git commit -m "feat(hardware_map): add max31865 spi pins and cooling pwm gpio definitions"
```

---

### Task 2: Multi-Point Rule Curve Engine & Data Structures

**Files:**
- Modify: `components/cooling_control/include/cooling_control.h`
- Create: `components/cooling_control/cooling_rule_engine.h`
- Create: `components/cooling_control/cooling_rule_engine.c`

**Interfaces:**
- Produces:
  ```c
  #define COOLING_MAX_RULE_POINTS 8

  typedef enum {
    COOLING_CURVE_LINEAR = 0,
    COOLING_CURVE_STEP = 1,
  } cooling_curve_type_t;

  typedef struct {
    int32_t temp_c_x10;
    uint16_t output_mv;
  } cooling_rule_point_t;

  typedef struct {
    bool auto_enable;
    cooling_curve_type_t curve_type;
    uint8_t point_count;
    cooling_rule_point_t points[COOLING_MAX_RULE_POINTS];
    uint32_t min_off_sec;
    uint32_t test_timeout_sec;
  } cooling_settings_t;

  uint16_t cooling_evaluate_rule_curve(const cooling_settings_t *settings, float temp_c);
  bool cooling_validate_settings(const cooling_settings_t *settings);
  void cooling_settings_set_defaults(cooling_settings_t *settings);
  ```

- [ ] **Step 1: Write `cooling_rule_engine.h` with type definitions and function declarations**
Include curve types, point definitions, and validation helpers.

- [ ] **Step 2: Implement `cooling_rule_engine.c`**
Implement:
1. `cooling_settings_set_defaults`: Sets 2 points: $24.0^\circ\text{C} \rightarrow 5000\,\text{mV}$, $35.0^\circ\text{C} \rightarrow 10000\,\text{mV}$, curve type `COOLING_CURVE_LINEAR`, `min_off_sec = 180`.
2. `cooling_validate_settings`: Checks $2 \le \text{point\_count} \le 8$, strictly ascending temperatures ($T_0 < T_1 < \dots$), and outputs $\le 10000\,\text{mV}$.
3. `cooling_evaluate_rule_curve`:
   - If $T < T_0 \rightarrow$ return $0\,\text{mV}$.
   - If $T \ge T_{N-1} \rightarrow$ return $V_{N-1}$.
   - If $T_i \le T < T_{i+1}$:
     - If `COOLING_CURVE_LINEAR`:
       $$V = V_i + \frac{(T - T_i) \times (V_{i+1} - V_i)}{T_{i+1} - T_i}$$
     - If `COOLING_CURVE_STEP`:
       $$V = V_i$$

- [ ] **Step 3: Update `cooling_control.h` to incorporate `cooling_settings_t` and new status fields**
Include:
- `uint16_t output_mv`
- `float duty_percent`
- `cooling_curve_type_t curve_type`
- `uint8_t point_count`
- `cooling_rule_point_t points[COOLING_MAX_RULE_POINTS]`

- [ ] **Step 4: Commit Task 2**
```bash
git add components/cooling_control
git commit -m "feat(cooling_control): implement multi-point linear and step rule engine"
```

---

### Task 3: MAX31865 SPI PT100 Driver Integration

**Files:**
- Modify: `components/cooling_control/CMakeLists.txt`
- Modify: `components/cooling_control/idf_component.yml`
- Create: `components/cooling_control/cooling_sensor_pt100.h`
- Create: `components/cooling_control/cooling_sensor_pt100.c`

**Interfaces:**
- Consumes: `esp-idf-lib/max31865`, `driver/spi_master.h`.
- Produces:
  ```c
  esp_err_t cooling_sensor_pt100_init(spi_host_device_t host, gpio_num_t cs_pin,
                                      gpio_num_t mosi_pin, gpio_num_t clk_pin,
                                      gpio_num_t miso_pin);
  esp_err_t cooling_sensor_pt100_read(float *out_temp_c, uint8_t *out_fault_status);
  esp_err_t cooling_sensor_pt100_deinit(void);
  ```

- [ ] **Step 1: Update `components/cooling_control/idf_component.yml` and `CMakeLists.txt`**
Add `esp-idf-lib/max31865: '*'` dependency to `components/cooling_control/idf_component.yml`.
Add `cooling_sensor_pt100.c` and `cooling_rule_engine.c` to `CMakeLists.txt`.

- [ ] **Step 2: Implement `cooling_sensor_pt100.c`**
Implement:
1. `cooling_sensor_pt100_init`:
   - Initialize SPI bus on `host` (e.g. `SPI2_HOST`) with MOSI, MISO, CLK.
   - Call `max31865_init_desc(&s_dev, host, MAX31865_MAX_CLOCK_SPEED_HZ, cs_pin)`.
   - Set parameters: standard ITS-90, 3-wire RTD, $R_{\text{ref}} = 430.0\,\Omega$, $R_{\text{nominal}} = 100.0\,\Omega$.
   - Call `max31865_set_config(&s_dev, &config)` with single-shot or continuous mode, 50Hz filter, and $V_{\text{bias}}$ enabled.
2. `cooling_sensor_pt100_read`:
   - Call `max31865_measure(&s_dev, out_temp_c)`.
   - If error returned, read fault register using `max31865_get_fault_status`, clear fault status with `max31865_clear_fault_status`, and reapply config if needed.
3. `cooling_sensor_pt100_deinit`: Clean up SPI device and bus.

- [ ] **Step 3: Commit Task 3**
```bash
git add components/cooling_control
git commit -m "feat(cooling_control): implement max31865 spi pt100 driver"
```

---

### Task 4: LEDC PWM Output Driver & Safety Slew Rate

**Files:**
- Create: `components/cooling_control/cooling_pwm_output.h`
- Create: `components/cooling_control/cooling_pwm_output.c`

**Interfaces:**
- Consumes: ESP-IDF `driver/ledc.h`.
- Produces:
  ```c
  esp_err_t cooling_pwm_output_init(gpio_num_t pwm_gpio);
  esp_err_t cooling_pwm_output_set_millivolts(uint16_t mv);
  uint16_t cooling_pwm_output_get_current_millivolts(void);
  float cooling_pwm_output_get_duty_percent(void);
  esp_err_t cooling_pwm_output_stop(void);
  ```

- [ ] **Step 1: Write `cooling_pwm_output.h`**
Define initialization parameters, constants ($1000\,\text{Hz}$, 10-bit resolution, GPIO 8).

- [ ] **Step 2: Implement `cooling_pwm_output.c`**
Implement:
1. `cooling_pwm_output_init`:
   - Pre-configure GPIO pin LOW to prevent power-up glitch.
   - Configure LEDC timer: `LEDC_LOW_SPEED_MODE`, `LEDC_TIMER_0`, 1 kHz frequency, 10-bit resolution (`LEDC_TIMER_10_BIT`).
   - Configure LEDC channel: `LEDC_CHANNEL_0`, GPIO 8, initial duty 0.
2. `cooling_pwm_output_set_millivolts`:
   - Clamp input to $0 \le mv \le 10000$.
   - Calculate duty: $\text{duty} = \mathrm{round}\left( \frac{mv}{10000.0} \times 1023 \right)$.
   - Apply duty via `ledc_set_duty` and `ledc_update_duty`.
3. Slew rate limiter helper to smoothly step voltage when required.

- [ ] **Step 3: Commit Task 4**
```bash
git add components/cooling_control
git commit -m "feat(cooling_control): implement ledc pwm driver for 0-10v inverter control"
```

---

### Task 5: Refactor `cooling_control` Coordinator & Task Loop

**Files:**
- Modify: `components/cooling_control/cooling_control.c`
- Modify: `components/cooling_control/include/cooling_control.h`

**Interfaces:**
- Consumes: `cooling_sensor_pt100.h`, `cooling_pwm_output.h`, `cooling_rule_engine.h`.
- Produces:
  ```c
  bool cooling_control_init(const cooling_control_config_t *config);
  bool cooling_control_apply_config(const cooling_control_config_t *config);
  bool cooling_control_get_status(cooling_control_status_t *out);
  bool cooling_control_set_mode(cooling_control_mode_t mode);
  bool cooling_control_start_test(uint16_t test_mv);
  bool cooling_control_stop(void);
  ```

- [ ] **Step 1: Remove legacy 1-Wire & Relay code from `cooling_control.c`**
Replace `ds18b20` and `relay_safe_init` references with `cooling_sensor_pt100` and `cooling_pwm_output`.

- [ ] **Step 2: Implement FreeRTOS control task loop in `cooling_control.c`**
Every 1000–2000 ms:
1. Read PT100 temperature via `cooling_sensor_pt100_read`.
2. Check sensor status. If $\ge 3$ consecutive failures:
   - Mark `COOLING_CONTROL_SENSOR_FAULT`.
   - Set output to $0\,\text{mV}$ immediately.
3. If sensor OK and in `AUTO` mode:
   - Evaluate target voltage via `cooling_evaluate_rule_curve(&s_settings, temp_c)`.
   - If target $> 0\,\text{mV}$ but compressor lockout is active:
     - Keep output at $0\,\text{mV}$, record `BLOCKED_COMPRESSOR_LOCKOUT`.
   - Else:
     - Apply target voltage via `cooling_pwm_output_set_millivolts`.
     - If output transitions from $>0\,\text{mV}$ to $0\,\text{mV}$, trigger `lockout_remaining_sec = s_settings.min_off_sec`.
4. If in `FORCE_OFF` mode:
   - Output $0\,\text{mV}$.
5. If in `TEST` mode:
   - Output requested test voltage, decrement `test_remaining_sec`. If 0, return to previous mode.
6. Publish telemetry via `telemetry_hub_publish`.

- [ ] **Step 3: Commit Task 5**
```bash
git add components/cooling_control
git commit -m "refactor(cooling_control): integrate pt100 sensor and pwm inverter coordinator"
```

---

### Task 6: NVS Store Schema Updates & Persistence Migration

**Files:**
- Modify: `components/nvs_store/include/nvs_store.h`
- Modify: `components/nvs_store/nvs_store.c`

**Interfaces:**
- Consumes: `cooling_settings_t`.
- Produces:
  ```c
  nvs_store_cooling_settings_load_status_t nvs_store_load_cooling_settings(cooling_settings_t *out_settings);
  bool nvs_store_save_cooling_settings(const cooling_settings_t *settings);
  void nvs_store_cooling_settings_defaults(cooling_settings_t *out_settings);
  ```

- [ ] **Step 1: Update `nvs_store.h`**
Replace old `threshold_c_x10` and `hysteresis_c_x10` with `cooling_settings_t` struct (curve type, point count, points array, min off time).

- [ ] **Step 2: Update `nvs_store.c`**
Implement serialization/deserialization for the points array under key `"cool_rules"` or individual fields in NVS namespace `"cooling_cfg"`.
Provide fallback to defaults if legacy keys are found or if data is corrupt.

- [ ] **Step 3: Commit Task 6**
```bash
git add components/nvs_store
git commit -m "feat(nvs_store): update cooling persistence schema for multi-point rules"
```

---

### Task 7: REST API & Telemetry Hub Updates in `web_server` & `app_main`

**Files:**
- Modify: `components/telemetry_hub/include/telemetry_hub.h`
- Modify: `main/web_server.c`
- Modify: `main/app_main.c`

**Interfaces:**
- Consumes: `cooling_control.h`, `nvs_store.h`, `cJSON`.
- Produces: Updated JSON handlers for `/api/cooling/status`, `/api/cooling/config`, `/api/cooling/mode`, and SSE event `cooling`.

- [ ] **Step 1: Update `telemetry_hub.h`**
Update `cooling_telemetry_t` struct:
```c
typedef struct {
  float current_temp_c;
  bool is_sensor_valid;
  uint16_t output_mv;
  float duty_percent;
  uint32_t lockout_remaining_sec;
  uint8_t mode;
} telemetry_cooling_t;
```

- [ ] **Step 2: Update `web_server.c` REST endpoints**
1. `handle_api_cooling_status`: Output JSON containing `temperature_c`, `output_volts`, `duty_percent`, `sensor_state`, `mode`, `lockout_active`, `lockout_remaining_sec`.
2. `handle_api_cooling_config_get`: Output JSON with `auto_enable`, `curve_type`, `point_count`, `points` array, `min_off_sec`.
3. `handle_api_cooling_config_post`: Parse incoming JSON containing `curve_type`, `points` array (`temp_c`, `output_volts`), validate rules, save to NVS, and apply live to `cooling_control`.
4. `handle_api_cooling_mode_post`: Handle `auto`, `force_off`, and `test` (with optional `test_volts`).
5. Update SSE event formatter in `web_server.c` for `TELEMETRY_EVT_COOLING`.

- [ ] **Step 3: Update `app_main.c`**
Update startup diagnostics and initialization calls to pass `cooling_control_config_t` with MAX31865 SPI pins and GPIO 8 PWM.

- [ ] **Step 4: Commit Task 7**
```bash
git add components/telemetry_hub main/web_server.c main/app_main.c
git commit -m "feat(web_server): update cooling api and sse telemetry for inverter control"
```

---

### Task 8: Web Dashboard UI & Rule Settings Modal

**Files:**
- Modify: `main/static/index.html`
- Modify: `main/static/app.js`
- Modify: `main/static/style.css`

**Interfaces:**
- Consumes: REST API `/api/cooling/*` and SSE `/api/events`.

- [ ] **Step 1: Update Dashboard Cooling Card in `index.html`**
Replace Relay On/Off indicator with:
- PT100 temperature readout (e.g. `28.52 °C`).
- Inverter output voltage badge (e.g. `7.05 V`) and duty cycle percentage (`70.5%`).
- Progress bar / gauge showing current 0–10V output level.
- Status indicator (`ACTIVE`, `STANDBY`, `LOCKOUT`, `FAULT`).

- [ ] **Step 2: Add Cooling Rule Configuration Modal in `index.html`**
Include:
- Radio buttons for Curve Type: `Linear Interpolation` and `Discrete Step`.
- Dynamic Table for Rule Points (columns: Index, Temperature °C, Target Voltage V, Action).
- "Add Point" button (up to 8 points).
- Live SVG Preview Graph showing the plotted curve.
- Compressor Lockout (seconds) input.

- [ ] **Step 3: Implement UI logic and SVG graphing in `app.js`**
Add event listeners to redraw the SVG curve whenever points are added, deleted, or modified.
Implement API communication: fetch config on modal open, POST updated config on save.
Update live telemetry handler to render live temperature and voltage gauge without reloading.

- [ ] **Step 4: Regenerate compressed web assets**
Run: `python scripts/optimize_assets.py` to compress static assets to `static_compressed`.

- [ ] **Step 5: Commit Task 8**
```bash
git add main/static main/static_compressed
git commit -m "feat(ui): add inverter cooling card, multi-point rule editor, and svg preview"
```

---

### Task 9: Firmware Build, Verification & Regression Testing

**Files:**
- Verify: Full firmware build and image creation.

- [ ] **Step 1: Build firmware target**
Run: `idf.py build`
Expected: Clean build with 0 compilation errors and 0 link errors.

- [ ] **Step 2: Verify binary size & flash allocation**
Check that app partition easily fits within ESP32-S3 16MB flash layout.

- [ ] **Step 3: Verification with test scripts / checks**
Verify configuration validation logic, boundary conditions, and memory safety.

- [ ] **Step 4: Commit Task 9 & Finalization**
```bash
git commit --allow-empty -m "chore: verify firmware build for pt100 inverter cooling control"
```
