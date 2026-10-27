#pragma once

/*
 * Application and board configuration for ESP32-S3.
 *
 * Board: ESP32-S3-DevKitC-1 WROOM-1-N16R8 (16MB Quad/Octal Flash, 8MB Octal PSRAM)
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TEMPLATE_NAME "Fish Pump Relay Timer Control"
#ifndef APP_TEMPLATE_FIRMWARE_VERSION
#define APP_TEMPLATE_FIRMWARE_VERSION "v0.3.2"
#endif
#define APP_TEMPLATE_PHASE_LABEL "Wi-Fi Setup and Control Dashboard"

#define APP_TEMPLATE_AP_SSID "FishPump-Setup"
#define APP_TEMPLATE_AP_CHANNEL 1
#define APP_TEMPLATE_AP_MAX_CONN 4
#define APP_TEMPLATE_WIFI_SCAN_MAX 20
#define APP_TEMPLATE_WIFI_MAX_TX_POWER_QDBM 80
#define APP_TEMPLATE_WIFI_SCAN_ACTIVE_MIN_MS 30
#define APP_TEMPLATE_WIFI_SCAN_ACTIVE_MAX_MS 60
#define APP_TEMPLATE_WIFI_SCAN_HOME_DWELL_MS 30

#define APP_TEMPLATE_MDNS_HOSTNAME "fishpump"
#define APP_TEMPLATE_MDNS_INSTANCE_NAME "Fish Pump Dashboard"

#define APP_TEMPLATE_DEFAULT_USERNAME "admin"
#define APP_TEMPLATE_DEFAULT_PASSWORD "admin123"

#define APP_TEMPLATE_SESSION_MAX_AGE_SEC 2592000
#define APP_TEMPLATE_LOGIN_RATE_LIMIT_MAX 5
#define APP_TEMPLATE_LOGIN_RATE_LIMIT_SEC 30

#define APP_TEMPLATE_AP_STOP_TMO_DEFAULT_MS 60000
#define APP_TEMPLATE_AP_AUTO_STOP_DEFAULT true

#define APP_TEMPLATE_HTTP_MAX_URI_HANDLERS 64
#define APP_TEMPLATE_HTTP_MAX_OPEN_SOCKETS 7
#define APP_TEMPLATE_HTTP_RECV_TIMEOUT_SEC 8
#define APP_TEMPLATE_HTTP_SEND_TIMEOUT_SEC 4
#define APP_TEMPLATE_HTTP_STATIC_CHUNK_BYTES 2048
#define APP_TEMPLATE_HTTP_STATIC_MAX_DURATION_MS 10000
#define APP_TEMPLATE_HTTP_KEEP_ALIVE_IDLE_SEC 15
#define APP_TEMPLATE_HTTP_KEEP_ALIVE_INTERVAL_SEC 3
#define APP_TEMPLATE_HTTP_KEEP_ALIVE_COUNT 3
#define APP_TEMPLATE_HTTP_SLOW_REQUEST_MS 1000
#define APP_TEMPLATE_MAIN_WDT_TIMEOUT_MS 10000
#define APP_TEMPLATE_HTTP_HUNG_CONSECUTIVE_FAILURES 3
#define APP_TEMPLATE_STATUS_LOG_INTERVALS 6

/* FLOAT SWITCH PULL-UP NOTE: ESP32-S3 internal pull-up (~45kOhm) may be
 * insufficient to reject EMI from the pump's electromagnetic field.
 * Recommended: add a 4.7kOhm external pull-up resistor between FLOAT_GPIO
 * and 3.3V. */
#define APP_TEMPLATE_PUMP_FLOAT_GPIO 16
#define APP_TEMPLATE_PUMP_RELAY_GPIO 5
#define APP_TEMPLATE_PUMP_FLOAT_ACTIVE_LOW true
#define APP_TEMPLATE_PUMP_RELAY_ACTIVE_LOW true

/* ESP32-S3 WROOM-1-N16R8: safe application GPIO pool defaults.
 * Excludes USB/UART/strapping/Octal Flash & PSRAM pins. */
#define APP_TEMPLATE_HW_DEFAULT_FLOAT_GPIO 16
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_GPIO 5
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_GPIO 6
#define APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO 7
#define APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO 8

#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW true
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW true
#define APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW true
#define APP_TEMPLATE_PUMP_TIMER_START_ON true
#define APP_TEMPLATE_COOLING_THRESHOLD_C_X10 300
#define APP_TEMPLATE_COOLING_HYSTERESIS_C_X10 10
#define APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT false
#define APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC 10
#define APP_TEMPLATE_COOLING_MIN_OFF_SEC 180

#define APP_TEMPLATE_PUMP_TIMER1_ON_SEC 20
#define APP_TEMPLATE_PUMP_TIMER1_OFF_SEC 60
#define APP_TEMPLATE_PUMP_TIMER2_ON_SEC 10
#define APP_TEMPLATE_PUMP_TIMER2_OFF_SEC 180
#define APP_TEMPLATE_PUMP_TIMER_MIN_SEC 5
#define APP_TEMPLATE_PUMP_TIMER_MAX_SEC 86400
#define APP_TEMPLATE_PUMP_AUTO_START_DEFAULT false
#define APP_TEMPLATE_PUMP_FLOAT_DEBOUNCE_MS 2000

#define APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_SEC 30
#define APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MIN_SEC 0
#define APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC 3600

/* Master enable flag for TFT Display hardware & background tasks.
 * Set to 1 to enable TFT display, or 0 to completely disable TFT hardware
 * operation while keeping all main code intact. */
#define APP_CONFIG_TFT_ENABLED 1

/* Master enable flag for relay fault check & alert notification.
 * Set to 1 to enable relay fault lockouts, or 0 to completely disable
 * relay fault lockouts and allow normal system operation without relays. */
#define APP_CONFIG_RELAY_FAULT_ENABLED 0

/* TFT Display GPIO -- ESP32-S3 SPI2_HOST (FSPI) on safe pins.
 * Octal Flash: GPIO 27-32 internal; Octal PSRAM: GPIO 33-37 internal (DO NOT USE).
 * EMI NOTE: For production environments, add an EXTERNAL 10k pull-up from
 * APP_TEMPLATE_TFT_DC_GPIO to 3V3. */
#define APP_TEMPLATE_TFT_CS_GPIO 10
#define APP_TEMPLATE_TFT_RESET_GPIO 14
#define APP_TEMPLATE_TFT_DC_GPIO 13
#define APP_TEMPLATE_TFT_MOSI_GPIO 11
#define APP_TEMPLATE_TFT_SCK_GPIO 12
#define APP_TEMPLATE_TFT_LED_GPIO 9

// Enable ST7789 panel driver instead of ILI9341 driver
#define APP_TEMPLATE_TFT_PANEL_ST7789

// ST7789 color order: LCD_RGB_ELEMENT_ORDER_RGB (0) or LCD_RGB_ELEMENT_ORDER_BGR (1)
#define APP_TEMPLATE_TFT_RGB_ELE_ORDER LCD_RGB_ELEMENT_ORDER_RGB

// ST7789 color inversion: false = normal, true = invert all colors
#define APP_TEMPLATE_TFT_INVERT_COLOR false

// ST7789 SPI clock: 40MHz minimizes EMI exposure window per frame.
#define APP_TEMPLATE_TFT_SPI_CLK_HZ (40 * 1000 * 1000)

// SoftAP recovery and rollback timing constants
#define APP_CONFIG_AP_RECOVERY_TIMEOUT_MS 300000
#define APP_CONFIG_AP_IDLE_TIMEOUT_MS 600000
#define APP_CONFIG_ROLLBACK_WIFI_TIMEOUT_MS 30000
#define APP_CONFIG_ROLLBACK_CONFIRM_TIMEOUT_MS 180000

/* Hardware recovery buttons and LEDs GPIO mapping for ESP32-S3.
 * GPIO 0 is boot-mode strapping (hold LOW on power-up for Download Mode).
 * GPIO 38/39 safely away from Octal PSRAM range (GPIO 33-37).
 * WS2812 RGB LED is on GPIO 48 on ESP32-S3 DevKitC-1. */
#define APP_CONFIG_BOOT_BTN_GPIO 0
#define APP_CONFIG_EXT_BTN_GPIO 38
#define APP_CONFIG_EXT_LED_GPIO 39
#define APP_CONFIG_RGB_LED_GPIO 48

/* Board identity reported to the web UI via /api/hardware/map and /api/status. */
#define APP_TEMPLATE_BOARD_TARGET "esp32s3"
#define APP_TEMPLATE_BOARD_NAME "ESP32-S3-DevKitC-1 WROOM-1-N16R8"
#define APP_TEMPLATE_BOARD_FLASH_MB 16
#define APP_TEMPLATE_BOARD_PSRAM_MB 8

// 32-byte (256-bit) AES key in hex format for Pre-Encrypted OTA.
// Defined in secrets.h (generated by scripts/generate_secrets.ps1, gitignored).
#if __has_include("secrets.h")
#include "secrets.h"
#elif __has_include("app_config/secrets.h")
#include "app_config/secrets.h"
#else
#ifndef APP_CONFIG_OTA_ENCRYPTION_KEY
#define APP_CONFIG_OTA_ENCRYPTION_KEY "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#endif

// Critical OOM thresholds (below these = add a strike)
#define APP_CONFIG_OOM_MIN_FREE_HEAP_BYTES 10240    // 10KB
#define APP_CONFIG_OOM_MIN_LARGEST_BLOCK_BYTES 5120 // 5KB

// Recovery thresholds (must exceed BOTH of these to reset strikes to 0)
#define APP_CONFIG_OOM_RECOVER_FREE_HEAP_BYTES 15360    // 15KB
#define APP_CONFIG_OOM_RECOVER_LARGEST_BLOCK_BYTES 8192 // 8KB

#define APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART 3 // 15 seconds (3 * 5s loop)

/* Wi-Fi STA long-term retry hardening constants. */
#define APP_CONFIG_STA_COOLDOWN_THRESHOLD 10       /* consecutive failures before long cooldown */
#define APP_CONFIG_STA_COOLDOWN_PERIOD_SEC 1800   /* 30-minute cooldown retry interval */
#define APP_CONFIG_STA_REBOOT_THRESHOLD 720       /* total lifetime attempts before esp_restart() */

#ifdef __cplusplus
}
#endif
