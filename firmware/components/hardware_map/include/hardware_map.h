#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  HARDWARE_ROLE_FLOAT_INPUT = 0,
  HARDWARE_ROLE_PUMP_RELAY_1,
  HARDWARE_ROLE_PUMP_RELAY_2,
  HARDWARE_ROLE_DS18B20_DATA,
  HARDWARE_ROLE_COOLING_RELAY,
  HARDWARE_ROLE_COUNT,
} hardware_role_t;

/* Role convenience aliases */
#define HARDWARE_ROLE_FLOAT HARDWARE_ROLE_FLOAT_INPUT
#define HARDWARE_ROLE_PUMP1 HARDWARE_ROLE_PUMP_RELAY_1
#define HARDWARE_ROLE_PUMP2 HARDWARE_ROLE_PUMP_RELAY_2
#define HARDWARE_ROLE_TEMP_DATA HARDWARE_ROLE_DS18B20_DATA
#define HARDWARE_ROLE_COOLING HARDWARE_ROLE_COOLING_RELAY

typedef enum {
  HARDWARE_PIN_INVALID = -1,
  HARDWARE_PIN_GPIO0 = 0,
  HARDWARE_PIN_GPIO1,
  HARDWARE_PIN_GPIO2,
  HARDWARE_PIN_GPIO3,
  HARDWARE_PIN_GPIO4,
  HARDWARE_PIN_GPIO5,
  HARDWARE_PIN_GPIO6,
  HARDWARE_PIN_GPIO7,
  HARDWARE_PIN_GPIO8,
  HARDWARE_PIN_GPIO9,
  HARDWARE_PIN_GPIO10,
  HARDWARE_PIN_GPIO11,
  HARDWARE_PIN_GPIO12,
  HARDWARE_PIN_GPIO13,
  HARDWARE_PIN_GPIO14,
  HARDWARE_PIN_GPIO15,
  HARDWARE_PIN_GPIO16,
  HARDWARE_PIN_GPIO17,
  HARDWARE_PIN_GPIO18,
  HARDWARE_PIN_GPIO19,
  HARDWARE_PIN_GPIO20,
  HARDWARE_PIN_GPIO21,
  HARDWARE_PIN_GPIO22,
  HARDWARE_PIN_GPIO23,
  HARDWARE_PIN_GPIO24,
  HARDWARE_PIN_GPIO25,
  HARDWARE_PIN_GPIO26,
  HARDWARE_PIN_GPIO27,
  HARDWARE_PIN_GPIO28,
  HARDWARE_PIN_GPIO29,
  HARDWARE_PIN_GPIO30,
  HARDWARE_PIN_GPIO31,
  HARDWARE_PIN_GPIO32,
  HARDWARE_PIN_GPIO33,
  HARDWARE_PIN_GPIO34,
  HARDWARE_PIN_GPIO35,
  HARDWARE_PIN_GPIO36,
  HARDWARE_PIN_GPIO37,
  HARDWARE_PIN_GPIO38,
  HARDWARE_PIN_GPIO39,
  HARDWARE_PIN_GPIO40,
  HARDWARE_PIN_GPIO41,
  HARDWARE_PIN_GPIO42,
  HARDWARE_PIN_GPIO43,
  HARDWARE_PIN_GPIO44,
  HARDWARE_PIN_GPIO45,
  HARDWARE_PIN_GPIO46,
  HARDWARE_PIN_GPIO47,
  HARDWARE_PIN_GPIO48,
  HARDWARE_PIN_COUNT,
} hardware_pin_t;

typedef enum {
  HARDWARE_PIN_CAP_NONE = 0,
  HARDWARE_PIN_CAP_INPUT = 1u << 0,
  HARDWARE_PIN_CAP_OUTPUT = 1u << 1,
  HARDWARE_PIN_CAP_INTERNAL_PULL = 1u << 2,
} hardware_pin_capability_t;

typedef enum {
  HARDWARE_RELAY_ACTIVE_HIGH = 0,
  HARDWARE_RELAY_ACTIVE_LOW,
} hardware_relay_polarity_t;

typedef enum {
  HARDWARE_TIMER_START_PHASE_ON = 0,
  HARDWARE_TIMER_START_PHASE_OFF,
} hardware_timer_start_phase_t;

typedef enum {
  HARDWARE_COOLING_MODE_AUTO = 0,
  HARDWARE_COOLING_MODE_FORCE_OFF,
  HARDWARE_COOLING_MODE_TEST_ON,
} hardware_cooling_mode_t;

typedef struct {
  gpio_num_t float_input_gpio;
  gpio_num_t pump_relay1_gpio;
  gpio_num_t pump_relay2_gpio;
  gpio_num_t ds18b20_data_gpio;
  gpio_num_t cooling_relay_gpio;
} hardware_map_t;

typedef struct {
  hardware_relay_polarity_t pump_relay1;
  hardware_relay_polarity_t pump_relay2;
  hardware_relay_polarity_t cooling_relay;
} hardware_relay_polarity_map_t;

typedef struct {
  hardware_pin_t pin;
  gpio_num_t gpio;
  hardware_pin_capability_t capabilities;
  const char *label;
} hardware_pin_descriptor_t;

typedef struct {
  hardware_role_t role;
  hardware_pin_t pin;
  gpio_num_t gpio;
  bool input_capable;
  bool output_capable;
  bool internal_pull_capable;
  bool is_default;
  const char *label;
} hardware_gpio_option_t;

/**
 * Initialize active hardware map from NVS or defaults.
 */
esp_err_t hardware_map_init(void);

/**
 * Get active GPIO pin number assigned to the given role.
 */
gpio_num_t hardware_map_get_active_pin(hardware_role_t role);

/**
 * Check if the given GPIO is within the ESP32-S3 safe application pool
 * (GPIO 1, 2, 5, 6, 7, 8, 15, 16, 17, 18).
 */
bool hardware_map_is_pin_allowed(gpio_num_t pin);

/**
 * Default pin and polarity mappings for ESP32-S3.
 */
hardware_map_t hardware_map_defaults(void);
hardware_relay_polarity_map_t hardware_map_default_polarities(void);

gpio_num_t hardware_map_pin_to_gpio(hardware_pin_t pin);
bool hardware_map_pin_valid(hardware_pin_t pin);
const hardware_pin_descriptor_t *
hardware_map_pin_descriptor(hardware_pin_t pin);
bool hardware_map_pin_is_reserved(hardware_pin_t pin);

const char *hardware_map_role_name(hardware_role_t role);
const char *hardware_map_polarity_name(hardware_relay_polarity_t polarity);
const char *
hardware_map_timer_start_phase_name(hardware_timer_start_phase_t phase);
const char *hardware_map_cooling_mode_name(hardware_cooling_mode_t mode);

bool hardware_map_role_valid(hardware_role_t role);
bool hardware_map_polarity_valid(hardware_relay_polarity_t polarity);
bool hardware_map_timer_start_phase_valid(hardware_timer_start_phase_t phase);
bool hardware_map_cooling_mode_valid(hardware_cooling_mode_t mode);

const hardware_gpio_option_t *
hardware_map_options_for_role(hardware_role_t role, size_t *count);
bool hardware_map_gpio_allowed_for_role(hardware_role_t role, gpio_num_t gpio);
bool hardware_map_validate(const hardware_map_t *map);
bool hardware_map_equal(const hardware_map_t *a, const hardware_map_t *b);
bool hardware_map_reboot_required(const hardware_map_t *active,
                                  const hardware_map_t *pending);

#ifdef __cplusplus
}
#endif
