#pragma once

#include "cooling_rule_engine.h"
#include "driver/gpio.h"
#if __has_include("hal/spi_types.h")
#include "hal/spi_types.h"
#endif
#ifndef SPI2_HOST
#define SPI2_HOST 1
#endif
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
  /* Legacy pin fields preserved for backward compatibility */
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
  cooling_settings_t settings;

  /* MAX31865 SPI RTD Sensor & LEDC PWM Inverter output configuration */
  spi_host_device_t spi_host;
  gpio_num_t max31865_cs_gpio;
  gpio_num_t max31865_mosi_gpio;
  gpio_num_t max31865_clk_gpio;
  gpio_num_t max31865_miso_gpio;
  gpio_num_t cooling_pwm_gpio;
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

  /* Multi-point inverter rule engine status */
  uint16_t output_mv;
  float duty_percent;
  cooling_curve_type_t curve_type;
  uint8_t point_count;
  cooling_rule_point_t points[COOLING_MAX_RULE_POINTS];

  /* Hardware pinout info */
  gpio_num_t max31865_cs_gpio;
  gpio_num_t cooling_pwm_gpio;
} cooling_control_status_t;

cooling_control_config_t cooling_control_default_config(void);
bool cooling_control_init(const cooling_control_config_t *config);
bool cooling_control_apply_config(const cooling_control_config_t *config);
bool cooling_control_stop(void);
bool cooling_control_get_status(cooling_control_status_t *out);
bool cooling_control_set_mode(cooling_control_mode_t mode);
bool cooling_control_start_test(uint16_t test_mv);
bool cooling_control_start_test_default(void);

#if defined(__cplusplus)
inline bool cooling_control_start_test(void) {
  return cooling_control_start_test((uint16_t)COOLING_RULE_MAX_OUTPUT_MV);
}
#else
#define _CC_TEST_GET_MACRO(_0, _1, NAME, ...) NAME
#define cooling_control_start_test(...) \
    _CC_TEST_GET_MACRO(_0 __VA_OPT__(,) __VA_ARGS__, cooling_control_start_test, cooling_control_start_test_default)(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif
