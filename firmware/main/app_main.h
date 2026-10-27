#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LED_STATE_OFF = 0,
  LED_STATE_ON,
  LED_STATE_STAGING_PENDING,
  LED_STATE_BTN_HOLD_SHORT,
  LED_STATE_RECOVERY_AP,
  LED_STATE_FACTORY_RESET
} app_led_state_t;

app_led_state_t app_get_led_state(void);
const char *app_led_state_name(app_led_state_t state);

/** Returns the restart detail string from the previous boot (e.g. "OOM",
 * "HTTP_HUNG"). Returns "" if no detail was recorded (normal power-on, etc.).
 */
const char *app_get_last_restart_detail(void);

/**
 * Safely initialize all relay GPIO pins to inactive level before driver init.
 */
void relay_safe_init_pins(void);

#ifdef __cplusplus
}
#endif
