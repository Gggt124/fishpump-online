#pragma once

#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PUMP_CONTROL_RELAY_ACTIVE_HIGH = 0,
  PUMP_CONTROL_RELAY_ACTIVE_LOW,
} pump_control_relay_polarity_t;

typedef enum {
  PUMP_CONTROL_FLOAT_UNKNOWN = 0,
  PUMP_CONTROL_FLOAT_OFF,
  PUMP_CONTROL_FLOAT_ON,
} pump_control_float_state_t;

typedef enum {
  PUMP_CONTROL_SET_1 = 1,
  PUMP_CONTROL_SET_2 = 2,
} pump_control_active_set_t;

typedef enum {
  PUMP_CONTROL_PHASE_IDLE = 0,
  PUMP_CONTROL_PHASE_ON,
  PUMP_CONTROL_PHASE_OFF,
} pump_control_timer_phase_t;

typedef enum {
  PUMP_CONTROL_START_PHASE_ON = 0,
  PUMP_CONTROL_START_PHASE_OFF,
} pump_control_start_phase_t;

typedef struct {
  uint32_t on_sec;
  uint32_t off_sec;
  pump_control_start_phase_t start_phase;
} pump_control_timer_set_config_t;

typedef struct {
  pump_control_timer_set_config_t water_set1;
  pump_control_timer_set_config_t water_set2;
  pump_control_timer_set_config_t wash_timer;
  bool water_auto_start;
  bool wash_auto_start;
  uint32_t min_dwell_sec;
} pump_control_config_update_t;

typedef struct {
  bool running;
  bool relay_energized;
  pump_control_float_state_t float_state;
  pump_control_active_set_t active_set;
  pump_control_timer_phase_t phase;
  uint32_t countdown_sec;
  uint32_t total_duration_sec;
  uint32_t cooldown_remaining_sec;
  bool auto_start;
} pump_control_water_status_t;

typedef struct {
  bool running;
  bool relay_energized;
  pump_control_timer_phase_t phase;
  uint32_t countdown_sec;
  uint32_t total_duration_sec;
  bool auto_start;
} pump_control_wash_status_t;

typedef struct {
  bool initialized;
  bool fault;
  pump_control_water_status_t water;
  pump_control_wash_status_t wash;
} pump_control_status_t;

typedef struct {
  gpio_num_t float_gpio;
  gpio_num_t relay1_gpio;
  gpio_num_t relay2_gpio;
  bool relay1_active_low;
  bool relay2_active_low;
  bool allow_auto_start;
  bool allow_runtime_control;
  uint32_t min_dwell_sec;
  pump_control_timer_set_config_t water_set1;
  pump_control_timer_set_config_t water_set2;
  pump_control_timer_set_config_t wash_timer;
  bool water_auto_start;
  bool wash_auto_start;
} pump_control_init_context_t;

bool pump_control_init(const pump_control_init_context_t *context);
bool pump_control_start_water(void);
bool pump_control_stop_water(void);
bool pump_control_start_wash(void);
bool pump_control_stop_wash(void);
bool pump_control_get_status(pump_control_status_t *out);
bool pump_control_update_config(const pump_control_config_update_t *update);

/** Optional callback invoked after water/wash runtime state changes. */
typedef void (*pump_control_state_changed_cb_t)(void);
void pump_control_set_state_changed_callback(
    pump_control_state_changed_cb_t cb);

#ifdef __cplusplus
}
#endif
