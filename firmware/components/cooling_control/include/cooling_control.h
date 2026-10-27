#pragma once

#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  COOLING_CONTROL_RELAY_ACTIVE_HIGH = 0,
  COOLING_CONTROL_RELAY_ACTIVE_LOW,
} cooling_control_relay_polarity_t;

typedef enum {
  COOLING_CONTROL_MODE_AUTO = 0,
  COOLING_CONTROL_MODE_FORCE_OFF,
  COOLING_CONTROL_MODE_TEST_ON,
} cooling_control_mode_t;

typedef enum {
  COOLING_CONTROL_SENSOR_UNKNOWN = 0,
  COOLING_CONTROL_SENSOR_OK,
  COOLING_CONTROL_SENSOR_FAULT,
} cooling_control_sensor_state_t;

typedef enum {
  COOLING_CONTROL_FAULT_NONE = 0,
  COOLING_CONTROL_FAULT_READ_FAILED,
  COOLING_CONTROL_FAULT_OUT_OF_RANGE,
  COOLING_CONTROL_FAULT_CONFIG_INVALID,
} cooling_control_fault_code_t;

typedef enum {
  COOLING_CONTROL_BLOCKED_NONE = 0,
  COOLING_CONTROL_BLOCKED_COMPRESSOR_LOCKOUT,
  COOLING_CONTROL_BLOCKED_SENSOR_FAULT,
  COOLING_CONTROL_BLOCKED_FORCE_OFF,
  COOLING_CONTROL_BLOCKED_CONFIG_INVALID,
} cooling_control_blocked_reason_t;

typedef struct {
  gpio_num_t ds18b20_gpio;
  gpio_num_t cooling_relay_gpio;
  cooling_control_relay_polarity_t relay_polarity;
  int32_t threshold_c_x10;
  int32_t hysteresis_c_x10;
  bool auto_enable;
  bool allow_runtime_control;
  cooling_control_mode_t mode;
  uint32_t test_timeout_sec;
  uint32_t compressor_min_off_sec;
} cooling_control_config_t;

typedef struct {
  bool initialized;
  bool config_valid;
  bool fault;
  bool temperature_valid;
  bool relay_energized;
  bool cooling_demand;
  bool lockout_active;
  gpio_num_t ds18b20_gpio;
  gpio_num_t cooling_relay_gpio;
  cooling_control_mode_t mode;
  bool auto_enable;
  float temperature_c;
  int32_t threshold_c_x10;
  int32_t hysteresis_c_x10;
  cooling_control_sensor_state_t sensor_state;
  cooling_control_fault_code_t fault_code;
  cooling_control_blocked_reason_t blocked_reason;
  uint32_t lockout_remaining_sec;
  uint32_t test_remaining_sec;
} cooling_control_status_t;

cooling_control_config_t cooling_control_default_config(void);
bool cooling_control_init(const cooling_control_config_t *config);
bool cooling_control_apply_config(const cooling_control_config_t *config);
bool cooling_control_stop(void);
bool cooling_control_get_status(cooling_control_status_t *out);
bool cooling_control_set_mode(cooling_control_mode_t mode);
bool cooling_control_start_test(void);

#ifdef __cplusplus
}
#endif
