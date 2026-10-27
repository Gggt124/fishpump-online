#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screen dimensions in landscape mode
#define TFT_WIDTH 320
#define TFT_HEIGHT 240

// Predefined 16-bit RGB565 color constants (native host byte order)
// Byte swapping for SPI transmission is handled manually via SWAP_BYTES in draw
// routines ST7789 driver: see APP_TEMPLATE_TFT_RGB_ELE_ORDER and
// APP_TEMPLATE_TFT_INVERT_COLOR in app_config.h
#define TFT_COLOR_BLACK 0x0000
#define TFT_COLOR_WHITE 0xFFFF
#define TFT_COLOR_RED 0xF800
#define TFT_COLOR_GREEN                                                        \
  0x07E2 /* was 0x07E0: low byte 0xE0 == ST7789 GAMCTRP1, EMI gamma-corruption \
            hazard */
#define TFT_COLOR_BLUE 0x001F
#define TFT_COLOR_YELLOW                                                       \
  0xFFE2 /* was 0xFFE0: low byte 0xE0 == ST7789 GAMCTRP1, EMI gamma-corruption \
            hazard */
#define TFT_COLOR_CYAN 0x07FF
#define TFT_COLOR_MAGENTA 0xF81F
#define TFT_COLOR_ORANGE                                                       \
  0xFD23 /* was 0xFD20: low byte 0x20 == ST7789 INVOFF (wire 0x20,0xFD) */
#define TFT_COLOR_GRAY 0x7BEF
#define TFT_COLOR_DARK_NAVY                                                    \
  0x0823 /* was 0x0821: low byte 0x21 == ST7789 INVON, caused EMI-triggered    \
            color inversion */
#define TFT_COLOR_DARK_PANEL 0x18E3

/* ── Compile-time EMI guard ─────────────────────────────────────
 * A color value's two on-wire bytes must NOT collide with any high-impact
 * ST7789 command. If the DC line glitches LOW during a RAMWR data phase
 * (EMI from pump relay / Wi-Fi TX / S3 Octal PSRAM), a data byte that
 * matches a command byte is misinterpreted as that command — causing
 * spontaneous color inversion (0x21 INVON), gamma corruption (0xE0/0xE1),
 * sleep entry (0x10-0x13), display off (0x28), etc. The fix is two-layer:
 *   (1) keep all palette colors off command bytes (this guard), and
 *   (2) bias the DC line HIGH between transactions (internal pull-up) —
 *       see tft_display_init() in tft_display.c.
 * See: docs/superpowers/plans/2026-07-12-tft-color-invert-fix.md
 */
#define TFT_BYTE_IS_PANEL_CMD(b)                                               \
  ((b) == 0x01 || (b) == 0x10 || (b) == 0x11 || (b) == 0x12 || (b) == 0x13 ||  \
   (b) == 0x20 || (b) == 0x21 || (b) == 0x22 || (b) == 0x28 || (b) == 0x29 ||  \
   (b) == 0x2A || (b) == 0x2B || (b) == 0x2C || (b) == 0x36 || (b) == 0xE0 ||  \
   (b) == 0xE1)

#define TFT_COLOR_IS_SAFE(c)                                                   \
  (!TFT_BYTE_IS_PANEL_CMD(((c) >> 8) & 0xFF) &&                                \
   !TFT_BYTE_IS_PANEL_CMD((c) & 0xFF))

_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_BLACK), "TFT_COLOR_BLACK unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_WHITE), "TFT_COLOR_WHITE unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_RED), "TFT_COLOR_RED unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_GREEN),
               "TFT_COLOR_GREEN bytes collide with an ST7789 command (EMI "
               "gamma-corruption hazard)");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_BLUE), "TFT_COLOR_BLUE unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_YELLOW),
               "TFT_COLOR_YELLOW bytes collide with an ST7789 command (EMI "
               "gamma-corruption hazard)");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_CYAN), "TFT_COLOR_CYAN unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_MAGENTA),
               "TFT_COLOR_MAGENTA unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_ORANGE),
               "TFT_COLOR_ORANGE bytes collide with an ST7789 command (EMI "
               "INVOFF hazard)");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_GRAY), "TFT_COLOR_GRAY unsafe");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_DARK_NAVY),
               "TFT_COLOR_DARK_NAVY bytes collide with an ST7789 command (EMI "
               "inversion hazard)");
_Static_assert(TFT_COLOR_IS_SAFE(TFT_COLOR_DARK_PANEL),
               "TFT_COLOR_DARK_PANEL unsafe");

/**
 * @brief Initialize the SPI bus and ILI9341 TFT display using esp_lcd.
 *        Renders the boot splash screen immediately upon success.
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t tft_display_init(void);

/**
 * @brief Clear the entire screen to a specified color.
 * @param color RGB565 color.
 */
void tft_clear(uint16_t color);

/**
 * @brief Draw a single character using the embedded 8x16 font.
 * @param x X coordinate (0 to TFT_WIDTH-1)
 * @param y Y coordinate (0 to TFT_HEIGHT-1)
 * @param c The character to render
 * @param color Foreground RGB565 color
 * @param bg_color Background RGB565 color
 */
void tft_draw_char(uint16_t x, uint16_t y, char c, uint16_t color,
                   uint16_t bg_color);

/**
 * @brief Draw a single character scaled by 2 (16x32 pixels) using the embedded
 * 8x16 font.
 * @param x X coordinate
 * @param y Y coordinate
 * @param c The character to render
 * @param color Foreground RGB565 color
 * @param bg_color Background RGB565 color
 */
void tft_draw_char_x2(uint16_t x, uint16_t y, char c, uint16_t color,
                      uint16_t bg_color);

/**
 * @brief Draw a string of characters.
 *        Safe bounds-checking will prevent overflow outside the screen.
 * @param x X coordinate
 * @param y Y coordinate
 * @param str The null-terminated string to render
 * @param color Foreground RGB565 color
 * @param bg_color Background RGB565 color
 */
void tft_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color,
                     uint16_t bg_color);

/**
 * @brief Draw a string of characters scaled by 2 (16x32 pixels).
 *        Safe bounds-checking will prevent overflow outside the screen.
 * @param x X coordinate
 * @param y Y coordinate
 * @param str The null-terminated string to render
 * @param color Foreground RGB565 color
 * @param bg_color Background RGB565 color
 */
void tft_draw_string_x2(uint16_t x, uint16_t y, const char *str, uint16_t color,
                        uint16_t bg_color);

/**
 * @brief Draw a fixed-size text widget in one SPI flush (8x16 font).
 *        Entire w×h region is filled with bg; str is left-aligned and clipped.
 *        Prefer this for runtime dynamic fields to avoid per-char ghosting.
 */
void tft_draw_string_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         const char *str, uint16_t color, uint16_t bg_color);

/**
 * @brief Draw a fixed-size text widget in one SPI flush (16x32 font).
 */
void tft_draw_string_x2_box(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            const char *str, uint16_t color, uint16_t bg_color);

/**
 * @brief Fill a rectangular area with a specified color.
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width of the rectangle
 * @param h Height of the rectangle
 * @param color RGB565 color
 */
void tft_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color);

/**
 * @brief Draw the static landscape layout skeleton of the status dashboard.
 */
void tft_display_draw_dashboard_skeleton(void);

/**
 * @brief Start the background periodic update FreeRTOS task.
 */
void tft_display_start_task(void);

/**
 * @brief Control TFT backlight brightness using LEDC PWM
 * @param percent 0-100% brightness level
 */
void tft_display_set_brightness(uint8_t percent);

/**
 * @brief Reset the backlight idle timer.
 * Call from BOTH hardware events (pump state change, float switch change, phase
 * transition) AND web API activity. Backlight dims to
 * APP_TEMPLATE_TFT_DIM_PERCENT after APP_TEMPLATE_TFT_DIM_TIMEOUT_MS of
 * inactivity from any source.
 *
 * Rationale: TFT is a 24/7 status display (shows IP/mDNS/pump state without
 * browser) — idle must be measured from real device activity, not only web
 * usage.
 */
void tft_display_reset_idle_timer(void);

/**
 * @brief Set the idle dim brightness level at runtime (persisted via NVS
 * separately). Call after loading from NVS on boot, or after user saves new
 * value via web API.
 * @param percent 0-100 brightness to apply when display enters idle state
 */
void tft_display_set_idle_dim_percent(uint8_t percent);

/**
 * @brief Get the current idle dim brightness level.
 * @return brightness percent (0-100)
 */
uint8_t tft_display_get_idle_dim_percent(void);

/** Screen saver config: dims after 5 min. Long because TFT is primary status
 * display. Burn-in risk is low: dynamic content (countdown, temp) changes every
 * second. Only static labels risk burn-in if pump is completely idle for hours.
 */
#define APP_TEMPLATE_TFT_DIM_PERCENT 70
#define APP_TEMPLATE_TFT_DIM_TIMEOUT_MS (5 * 60 * 1000)

/**
 * @brief Request panel recovery on the next display task cycle.
 *        Safe to call from any task/ISR context (lock-free atomic flag).
 *        Use after events that may cause EMI (relay switching, external
 * triggers). Recovery re-syncs panel registers only; dynamic widgets refresh
 * via dirty partial updates (no full-screen blink).
 */
void tft_display_request_resync(void);

/**
 * @brief Subscribe TFT display to telemetry_hub events for push-based redraw.
 *        Called automatically in tft_display_start_task(), but can also be
 *        invoked explicitly.
 * @return esp_err_t ESP_OK on success, or error code from telemetry_hub_subscribe
 */
esp_err_t tft_display_subscribe_telemetry(void);

#ifdef __cplusplus
}
#endif
