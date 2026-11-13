# PT100 MAX31865 Temperature Sensing & Inverter PWM-to-VDC Cooling Control Design

**Date:** 2026-11-10  
**Status:** Approved  
**Target Component:** `components/cooling_control`, `components/hardware_map`, `components/nvs_store`, `main/web_server`, `main/static`

---

## 1. Executive Summary & Objectives

### 1.1 Background
The FishPump system previously used a DS18B20 1-Wire temperature sensor and an on/off mechanical relay (bang-bang thermostat) for cooling control. For inverter-driven chillers and cooling units, on/off relay switching is inappropriate and causes severe mechanical and electrical wear. Inverters accept analog voltage inputs (typically 0–10V DC) to modulate compressor speed proportionally. Additionally, high-precision industrial PT100 RTD temperature sensors provide far greater reliability, response time, and accuracy across marine/aquarium environments compared to DS18B20.

### 1.2 Core Objectives
1. **PT100 Temperature Sensing:** Integrate the MAX31865 RTD-to-Digital SPI converter using proven 3-wire RTD configurations on hardware SPI2 (FSPI).
2. **PWM-to-VDC Inverter Speed Output:** Replace mechanical relay control with ESP32-S3 hardware LEDC PWM driving an external PWM-to-0..10V DAC module on GPIO 8.
3. **Multi-Point Rule Engine:** Support up to 8 user-configurable control points $(T_i, V_i)$ with runtime selection between:
   - **Linear Interpolation:** Smooth ramp between setpoints, avoiding sudden compressor rpm jumps.
   - **Step / Discrete Stages:** Fixed voltage plateaus with hysteresis.
4. **Safety & Protection:** Implement sensor loss/fault immediate shutdown (0V), soft slew rate limiting, and compressor anti-short-cycle lockout delay.
5. **Conflict-Free GPIO Allocation:** Ensure all pin assignments preserve reserved lines for planned dual PZEM-004T power meters (single shared UART bus) and RS485 communication.
6. **Web Dashboard & API:** Provide interactive rule curve configuration, live voltage/temperature telemetry via SSE, and manual test features.

---

## 2. Hardware Architecture & Pin Budget

### 2.1 ESP32-S3 Pin Allocation Matrix

> **Note (2026-11-13):** the planned UART / RS485 pins in this table (GPIO 1/2, 17/18, 7) and the UART0 console row are superseded by `2026-11-13-pzem-ph-sensor-integration-design.md` §3.1. The new layout is UART0 4/42, UART1 40/41 and UART2 21/14, and the console moves to USB-Serial-JTAG.
All pins are strictly assigned to avoid strapping pins, Octal Flash/PSRAM lines (GPIO 27–37), USB console, or planned peripherals:

| Peripheral Role | GPIO Pin | Bus / Signal | Description |
| :--- | :--- | :--- | :--- |
| **MAX31865 CS** | **GPIO 10** | FSPI CS | Chip Select (Active Low) |
| **MAX31865 MOSI** | **GPIO 11** | FSPI MOSI | SPI Data Output from ESP32-S3 |
| **MAX31865 SCK** | **GPIO 12** | FSPI SCK | SPI Clock |
| **MAX31865 MISO** | **GPIO 13** | FSPI MISO | SPI Data Input to ESP32-S3 |
| **Cooling PWM Out**| **GPIO 8** | LEDC Timer 0 | PWM Output to 0-10V Converter |
| **Pump 1 Relay** | **GPIO 5** | GPIO Out | Water circulation pump 1 relay |
| **Pump 2 Relay** | **GPIO 6** | GPIO Out | Water circulation pump 2 relay |
| **Float Switch** | **GPIO 16** | GPIO In (Ext Pullup) | Water level basin sensor |
| *Freed / Spare* | **GPIO 7** | General I/O | Formerly DS18B20; reserved for RS485 DE/RE |
| *Reserved (Future)*| **GPIO 1, 2** | UART2 (TX/RX) | Reserved for RS485 serial communication |
| *Reserved (Future)*| **GPIO 17, 18** | UART1 (TX/RX) | Reserved for 2x PZEM-004T (shared bus, dual address) |
| *System Reserved* | GPIO 0 | Boot Button | Boot strapping |
| *System Reserved* | GPIO 38, 39 | Ext Button, LED | Hardware UI Recovery & Status |
| *System Reserved* | GPIO 43, 44 | UART0 | Console debug serial |
| *System Reserved* | GPIO 48 | WS2812 | Onboard RGB status LED |

---

## 3. Sensor Subsystem: MAX31865 & PT100

### 3.1 Device Configuration
* **IDF Component:** `esp-idf-lib/max31865`
* **SPI Host:** `SPI2_HOST` (FSPI) with `SPI_DMA_CH_AUTO`
* **Clock Frequency:** 1 MHz (compatible with long probe leads and breakout boards)
* **RTD Standard:** ITS-90 (`MAX31865_ITS90`)
* **Connection Mode:** 3-Wire (`MAX31865_3WIRE`)
* **Reference Resistor ($R_{\text{ref}}$):** $430.0\,\Omega$ ($0.1\%$ precision resistor on board)
* **Nominal Resistor ($R_0$):** $100.0\,\Omega$ (PT100 platinum RTD)
* **Mains Filter:** 50 Hz rejection (`MAX31865_FILTER_50HZ`)

### 3.2 Polling & Fault Handling State Machine
1. Polling interval: 1,000 ms to 2,000 ms in `cooling_control_task`.
2. On each cycle, `max31865_measure(&dev, &temp_c)` is invoked.
3. If successful:
   - Clear consecutive failure counter.
   - Temperature is valid; publish to telemetry.
4. If failed:
   - Read fault register using `max31865_get_fault_status`.
   - Distinguish fault conditions (RTD High/Low threshold, VREFIN/RTDIN out of range, Overvoltage/Undervoltage).
   - Clear fault register (`max31865_clear_fault_status`) and reapply configuration if corrupted.
   - If $\ge 3$ consecutive failures occur:
     - Transition sensor state to `COOLING_CONTROL_SENSOR_FAULT`.
     - **Safety Action:** Immediately drive PWM output to $0.0\,\text{V}$ (0% duty cycle).
     - Broadcast fault alarm event via `telemetry_hub`.

---

## 4. Actuator Subsystem: LEDC PWM to 0-10V DC

### 4.1 LEDC Configuration
* **Speed Mode:** `LEDC_LOW_SPEED_MODE`
* **Timer:** `LEDC_TIMER_0`
* **Channel:** `LEDC_CHANNEL_0` on GPIO 8
* **Frequency:** 1 kHz (standard for hardware RC / active integrator PWM-to-VDC converter modules)
* **Resolution:** 10-bit (`LEDC_TIMER_10_BIT`), providing 1,024 discrete steps ($0$ to $1023$).
  - Effective resolution: $\frac{10.0\,\text{V}}{1023} \approx 9.77\,\text{mV}$ per step.
* **Boot Clamp:** GPIO 8 is clamped LOW before LEDC driver starts to prevent startup glitches.

### 4.2 Output Voltage Calculation
Given target voltage $V_{\text{target}}$ in millivolts ($0 \le V_{\text{target}} \le 10000$):
$$\text{Duty}_{\text{raw}} = \mathrm{round}\left( \frac{V_{\text{target}}}{10000.0} \times 1023 \right)$$

---

## 5. Multi-Point Rule Engine & Control Algorithms

### 5.1 Data Model
```c
#define COOLING_MAX_RULE_POINTS 8

typedef enum {
  COOLING_CURVE_LINEAR = 0,  // Piecewise linear interpolation
  COOLING_CURVE_STEP = 1,    // Discrete step jumps
} cooling_curve_type_t;

typedef struct {
  int32_t temp_c_x10;        // e.g., 240 = 24.0°C, 350 = 35.0°C
  uint16_t output_mv;        // e.g., 5000 = 5.0V, 10000 = 10.0V
} cooling_rule_point_t;

typedef struct {
  bool auto_enable;
  cooling_curve_type_t curve_type;
  uint8_t point_count;       // 2 to 8 points
  cooling_rule_point_t points[COOLING_MAX_RULE_POINTS];
  uint32_t min_off_sec;      // Compressor lockout delay (default: 180s)
  uint32_t test_timeout_sec; // Test mode duration (default: 30s)
} cooling_settings_t;
```

### 5.2 Default Factory Settings
* **Point Count:** 2
* **Point 0:** $24.0^\circ\text{C} \rightarrow 5,000\,\text{mV}$ ($5.0\,\text{V}$)
* **Point 1:** $35.0^\circ\text{C} \rightarrow 10,000\,\text{mV}$ ($10.0\,\text{V}$)
* **Min Off Sec:** 180 seconds

### 5.3 Curve Evaluation Rules
Given sorted points $(T_0, V_0), (T_1, V_1), \dots, (T_{N-1}, V_{N-1})$ with $T_0 < T_1 < \dots < T_{N-1}$:
1. **Below Lower Bound ($T < T_0$):**
   - Output = $0\,\text{mV}$ ($0.0\,\text{V}$). Inverter stops/standby.
   - If output transitioned from $>0\,\text{V}$ to $0\,\text{V}$, start the `min_off_sec` compressor anti-short-cycle timer.
2. **Above Upper Bound ($T \ge T_{N-1}$):**
   - Output clamped at $V_{N-1}$ (typically $10,000\,\text{mV} = 10.0\,\text{V}$).
3. **In-Range ($T_i \le T < T_{i+1}$):**
   - **Linear Mode (`COOLING_CURVE_LINEAR`):**
     $$V(T) = V_i + \frac{(T - T_i) \times (V_{i+1} - V_i)}{T_{i+1} - T_i}$$
   - **Step Mode (`COOLING_CURVE_STEP`):**
     $$V(T) = V_i$$
     *(with $0.2^\circ\text{C}$ debounce hysteresis across threshold boundaries to avoid rapid stepping).*

### 5.4 Safety Guards
* **Soft Slew Rate:** Voltage rate-of-change is limited to $\le 1.0\,\text{V/sec}$ during normal transitions to prevent mechanical stress on inverter compressors.
* **Compressor Lockout (`min_off_sec`):** If the inverter shuts down to $0\,\text{V}$, it cannot re-energize above $0\,\text{V}$ until the lockout timer expires, even if water temperature rises.
* **Sensor Loss Protection:** If PT100 reading is invalid, bypass slew rate and immediately set PWM to 0% ($0.0\,\text{V}$).

---

## 6. NVS Persistence & Migration

* Stored under NVS namespace `cooling_cfg`.
* Schema validation ensures:
  - `point_count` between 2 and 8.
  - Temperature points strictly ascending: $T_0 < T_1 < \dots < T_{N-1}$.
  - Millivolt outputs bounded: $0 \le V_i \le 10000$.
  - Legacy relay settings in NVS gracefully migrate to the default 2-point inverter profile.

---

## 7. Web API & Telemetry Specifications

### 7.1 REST Endpoints
* **`GET /api/cooling/status`**
  ```json
  {
    "initialized": true,
    "temperature_c": 28.50,
    "sensor_state": "ok",
    "fault_code": "none",
    "output_volts": 7.05,
    "duty_percent": 70.5,
    "mode": "auto",
    "auto_enable": true,
    "curve_type": "linear",
    "lockout_active": false,
    "lockout_remaining_sec": 0,
    "test_remaining_sec": 0
  }
  ```
* **`GET /api/cooling/config`**
  Returns persisted `cooling_settings_t` in JSON format.
* **`POST /api/cooling/config`**
  Validates and saves new points array, curve type, and lockout timers to NVS; reconfigures runtime instantly.
* **`POST /api/cooling/mode`**
  Allows switching between `auto`, `force_off`, and `test` (with temporary voltage override).

### 7.2 Telemetry (SSE `/api/events`)
Pushes event `cooling` every 1–2 seconds with `current_temp_c`, `output_volts`, `is_sensor_valid`, and `lockout_remaining_sec`.

---

## 8. Web UI Dashboard & Settings Modal

1. **Dashboard Cooling Card:**
   - Real-time PT100 temperature display with $0.01^\circ\text{C}$ precision.
   - Dual-indicator output display: Output Voltage (e.g., $7.05\,\text{V}$) and Duty Cycle ($70.5\%$).
   - Dynamic gauge/progress bar with status badge (`ACTIVE`, `STANDBY`, `LOCKOUT`, `FAULT`).
2. **Cooling Settings Modal:**
   - Segmented toggle for Curve Type (`Linear Interpolation` vs `Step Stages`).
   - Dynamic Rule Table: Add/Delete rows (2 to 8 rows), inputs for Temperature ($^\circ\text{C}$) and Target Voltage ($\text{V}$).
   - SVG Curve Visualizer: Renders live preview of the piece-wise curve before saving.
   - Inputs for Compressor Lockout time (seconds) and Test Timeout.

---

## 9. Verification & Testing Strategy

1. **Hardware SPI & Sensor Verification:**
   - Verify SPI communication on pins 10, 11, 12, 13 with MAX31865 module.
   - Test fault detection by disconnecting PT100 lead (verify instant transition to `SENSOR_FAULT` and 0V PWM).
2. **PWM & 0-10V Output Verification:**
   - Measure GPIO 8 PWM duty cycle with oscilloscope / logic analyzer.
   - Measure 0-10V converter output voltage with digital multimeter across test setpoints ($0\,\text{V}$, $2.5\,\text{V}$, $5.0\,\text{V}$, $7.5\,\text{V}$, $10.0\,\text{V}$).
3. **Algorithm Unit Tests:**
   - Test piecewise interpolation math across negative and positive temperature intervals.
   - Test step mode threshold hysteresis.
   - Test anti-short-cycle lockout timer countdown.
4. **End-to-End Web UI Tests:**
   - Save custom multi-point table via Web UI, reboot ESP32-S3, and confirm settings persist in NVS.
   - Verify SSE updates reflect live temperature and voltage changes in real time.

