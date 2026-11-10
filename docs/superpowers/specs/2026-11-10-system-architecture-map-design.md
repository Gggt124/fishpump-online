# FishPump ESP32-S3 Firmware Architecture & Data Flow Map Design

**Date:** 2026-11-10  
**Status:** Approved  
**Target Deliverable:** `map.html` at repository root (`c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`)

---

## 1. Goal & Overview

To create a standalone, interactive, visually rich HTML document (`map.html`) visualizing the internal architecture, task relationships, and data flow of the FishPump ESP32-S3 firmware.

The document enables both technical and non-technical stakeholders to understand:
- How components communicate with each other and with `app_main`.
- How sensor inputs (Float switch, DS18B20 1-Wire) flow through RTOS tasks into `telemetry_hub` and push out via Server-Sent Events (SSE) to the Web client.
- How control commands flow from Web UI into state machines and drive physical relays.
- How safety mechanisms operate (Relay glitch-free initialization, NVS staging rollback timers, Task Watchdog).

---

## 2. Technical Requirements

1. **Self-Contained File:**
   - Single standalone HTML file with inline CSS, SVG graphics, and vanilla JavaScript.
   - Zero external CDN or internet dependencies. Works 100% offline when opened locally in any modern browser.
2. **Visual & Motion Design:**
   - Dark mode engineering aesthetic (Slate/Navy background `#0b1120`, Cyan `#06b6d4`, Emerald `#10b981`, Amber `#f59e0b`, Purple `#a855f7`, Rose `#f43f5e`).
   - SVG vector paths with curved bezier lines connecting components.
   - Dynamic particle animation / dash-flow moving along connection paths indicating live data direction.
   - Smooth transitions, responsive SVG viewBox, and interactive zoom/pan or fit-to-screen controls.
3. **Bilingual Documentation Content:**
   - Primary explanations, component responsibilities, and tooltips in **Thai** (ภาษาไทย).
   - Technical terms (FreeRTOS, Mutex, Core 0/1, SSE, REST API, GPIO, 1-Wire, NVS, TWDT) preserved in standard **English**.

---

## 3. Architectural Structure & Components

The diagram is organized into four distinct architectural layers:

### Layer 1: Physical Hardware & I/O
- **Dual Water Pump Relays (GPIO 10, 11 default):** Switched power outputs driving water circulation pumps.
- **Cooling Compressor Relay (GPIO 12 default):** Power output driving the chiller/compressor.
- **Float Switch Level Sensor (GPIO 4 default):** Digital input detecting water basin low/high levels.
- **DS18B20 1-Wire Temperature Sensor (GPIO 5 default):** Digital bus measuring water temperature in real-time.
- **Physical Controls & Indicators:** Boot button (GPIO 0), External button (GPIO 21), Status LEDs (NeoPixel/GPIO).

### Layer 2: Real-time Control & FreeRTOS Tasks
- **`pump_control_task`:** Dual-pump alternating timer, wash cycle sequencer, float switch debounce, and minimum dwell protection state machine.
- **`cooling_control_task`:** Thermostat state machine, 1-Wire DS18B20 driver polling, hysteresis logic, and compressor anti-short-cycle lockout timer.
- **`hardware_ui_task` (Core 1):** Button hold duration measurement, recovery AP trigger (2-5s hold), and factory reset trigger (>5s hold).
- **`TWDT` (Task Watchdog Timer):** Hardware watchdog with 10-second timeout, monitoring `app_main` loop health and resetting CPU on lockup.

### Layer 3: System Core, Data Bus & Persistence
- **`telemetry_hub`:** In-memory publish/subscribe event broker protected by `s_telemetry_mutex`. Holds cached `telemetry_snapshot_t` and broadcasts events to registered callbacks.
- **`app_main`:** Core system coordinator on Core 0. Handles cold boot sequencing, 5-second periodic loop, TWDT feed, health telemetry publishing, and rollback timer management.
- **`nvs_store`:** Flash persistence engine. Single source of truth for pump timer settings, multi-profile Wi-Fi credentials, hardware GPIO mapping, staging rollback slots, and last reset reason records.
- **`session`:** Security system managing session tokens, expiration (30 min), client IP binding, and brute-force protection.
- **`hardware_map`:** Dynamic GPIO map validator preventing pin conflicts and validating assignments against safe ESP32-S3 pins.
- **`relay_safe_init`:** Pre-initialization driver clamping all relay control GPIOs to safe inactive levels before functional drivers configure them, preventing boot glitches.

### Layer 4: Connectivity & Presentation
- **`wifi_manager`:** ESP-IDF Wi-Fi netif abstraction running concurrent AP (FishPump_Setup) and Station mode with automatic reconnection and RSSI tracking.
- **`dns_server`:** UDP port 53 captive portal DNS redirecting queries to the AP IP (`192.168.4.1`).
- **`web_server`:** ESP-HTTPD server handling REST endpoints (`/api/status`, `/api/events`, `/api/settings/*`, etc.), serving embedded gzip-compressed assets, and managing active SSE clients.
- **Web SPA (Browser):** Single-page web application communicating via REST HTTP requests and streaming real-time status through `/api/events` (SSE).

---

## 4. Interactive Scenarios & Data Flows

The map includes a top scenario switcher to highlight specific data trajectories:

### Scenario 1: System Overview (Default)
Displays the full topology and baseline relationships across all 4 layers simultaneously.

### Scenario 2: Boot & Safe Hardware Initialization
1. `app_main` executes $\rightarrow$ `relay_safe_init_pins()` de-energizes all relay GPIOs immediately.
2. `nvs_store_init()` initializes flash $\rightarrow$ checks `s_last_restart_detail`.
3. Loads `hardware_map`, `pump_settings`, and `cooling_settings` from NVS.
4. Checks staging status; if staging config is unconfirmed, starts `s_confirm_timer` (180s) and `s_wifi_timer`.
5. Initializes TWDT (10s timeout) and configures relays safely via `relay_safe_configure()`.
6. Starts `cooling_control_task` and `pump_control_task`.
7. Initializes `session_init()`, `wifi_manager_init()`, starts `web_server()`, `mdns()`, `dns_server()`, and `hardware_ui_task`.

### Scenario 3: Real-Time Telemetry & Sensor Loop (In-Memory Pub/Sub)
1. `pump_control_task` evaluates float switch and pump timers $\rightarrow$ publishes `TELEMETRY_EVT_PUMP` and `TELEMETRY_EVT_FLOAT`.
2. `cooling_control_task` polls DS18B20 $\rightarrow$ publishes `TELEMETRY_EVT_COOLING`.
3. `app_main` 5s loop gathers heap, PSRAM, RSSI, uptime $\rightarrow$ publishes `TELEMETRY_EVT_HEALTH`.
4. `telemetry_hub_publish()` acquires `s_telemetry_mutex`, caches snapshot, and invokes registered callbacks.
5. `web_server_telemetry_cb` receives event $\rightarrow$ formats JSON payload $\rightarrow$ pushes to connected SSE clients via `/api/events`.

### Scenario 4: Web Command & Zero-Brick Staging Rollback
1. Web client issues POST request with session token.
2. `web_server` verifies token with `session_validate(token, client_ip)`.
3. Critical configuration change (Wi-Fi or Hardware Map) is written to `nvs_store` as pending staging.
4. `s_confirm_timer` starts (180s).
5. If confirmed by user before timeout $\rightarrow$ `nvs_store_confirm_staging()` permanently commits changes and cancels timer.
6. If timeout occurs $\rightarrow$ `nvs_store_rollback_staging()` reverts config and triggers clean reboot to prevent bricking.

---

## 5. UI Features & Inspector Specifications

- **Interactive Canvas:**
  - Responsive SVG with smooth zooming and panning.
  - Distinct component nodes with clear icons, title, layer badge, and status indicators.
  - Hover highlights corresponding input and output paths.
  - Click locks focus onto the selected component, dimming unaffected components.
- **Slide-Over Inspector Drawer:**
  - Header: Component title, C source filename, and layer badge.
  - Section 1: Detailed responsibility description in Thai.
  - Section 2: FreeRTOS Execution Specs (Core affinity, Task Priority, Stack Size, Loop Frequency).
  - Section 3: Data Inbound & Outbound summary (Sources, Targets, and Data Types).
  - Section 4: Key C API functions & data structures.
  - Section 5: Safety and error mitigation mechanisms.
- **Header Controls:**
  - Scenario buttons (Overview, Boot & Safe Init, Telemetry Loop, Staging Rollback).
  - Animation play/pause toggle.
  - Reset View button.
  - Layer filter checkboxes.

---

## 6. Verification Plan

1. Verify file exists at `c:\Users\Copter\OneDrive\Desktop\fishpump-online\map.html`.
2. Open in browser:
   - Check all 4 layers render correctly without visual overlap.
   - Verify all 4 scenarios switch active animations and path highlights accurately.
   - Test clicking on every component to ensure the Inspector Drawer opens with accurate data and C source references.
   - Verify particle motion runs smoothly with zero console errors.
   - Verify full offline operation with no external network requests.
