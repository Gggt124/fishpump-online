#include "hardware_map.h"
#include "nvs_store.h"

#if __has_include("app_config.h")
#include "app_config.h"
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

_Static_assert(HARDWARE_ROLE_FLOAT == HARDWARE_ROLE_FLOAT_INPUT, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_PUMP1 == HARDWARE_ROLE_PUMP_RELAY_1, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_PUMP2 == HARDWARE_ROLE_PUMP_RELAY_2, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_TEMP_DATA == HARDWARE_ROLE_DS18B20_DATA, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_COOLING == HARDWARE_ROLE_COOLING_RELAY, "Role alias mismatch");

/* Only pins described below are considered reusable application GPIOs.
 *
 * ESP32-S3 deliberately excludes:
 * - GPIO 0, 3, 45, 46: strapping pins
 * - GPIO 4: weak pull-up during reset
 * - GPIO 9-14: TFT wiring
 * - GPIO 19-20: native USB D-/D+
 * - GPIO 21, 38-42, 47: USB OTG download/boot-mode signals
 * - GPIO 27-37: WROOM-1 Octal Flash/PSRAM connections
 * - GPIO 43-44: UART0 console
 * - GPIO 48: DevKitC-1 RGB LED
 *
 * Treating the application pool as an allowlist is intentional. A GPIO that
 * is technically routable through the GPIO matrix is not automatically safe
 * to expose as a user-selectable hardware role.
 *
 * ESP32-S3 safe pool: GPIO 1, 2, 5, 6, 7, 8, 15, 16, 17, 18.
 */

static const hardware_pin_descriptor_t s_pin_descriptors[] = {
    {HARDWARE_PIN_GPIO1, GPIO_NUM_1,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO1"},
    {HARDWARE_PIN_GPIO2, GPIO_NUM_2,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO2"},
    {HARDWARE_PIN_GPIO5, GPIO_NUM_5,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO5"},
    {HARDWARE_PIN_GPIO6, GPIO_NUM_6,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO6"},
    {HARDWARE_PIN_GPIO7, GPIO_NUM_7,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO7"},
    {HARDWARE_PIN_GPIO8, GPIO_NUM_8,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO8"},
    {HARDWARE_PIN_GPIO15, GPIO_NUM_15,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO15"},
    {HARDWARE_PIN_GPIO16, GPIO_NUM_16,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO16"},
    {HARDWARE_PIN_GPIO17, GPIO_NUM_17,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO17"},
    {HARDWARE_PIN_GPIO18, GPIO_NUM_18,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO18"},
};

#define HARDWARE_GPIO_OPTION(role, pin, input, output, pull, is_default)       \
  {role,                                                                       \
   HARDWARE_PIN_GPIO##pin,                                                     \
   (gpio_num_t)HARDWARE_PIN_GPIO##pin,                                         \
   input,                                                                      \
   output,                                                                     \
   pull,                                                                       \
   is_default,                                                                 \
   "GPIO" #pin}

static const hardware_gpio_option_t s_float_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 1, true, true, true, false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 2, true, true, true, false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 16, true, true, true, true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_FLOAT_INPUT, 18, true, true, true,
                         false),
};

static const hardware_gpio_option_t s_pump_relay1_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 5, true, true, true, true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 6, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 7, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 8, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_1, 18, true, true, true,
                         false),
};

static const hardware_gpio_option_t s_pump_relay2_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 5, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 6, true, true, true, true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 7, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 8, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_PUMP_RELAY_2, 18, true, true, true,
                         false),
};

static const hardware_gpio_option_t s_ds18b20_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 1, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 2, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 7, true, true, true,
                         true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_DS18B20_DATA, 18, true, true, true,
                         false),
};

static const hardware_gpio_option_t s_cooling_relay_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 5, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 6, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 7, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 8, true, true, true,
                         true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_RELAY, 18, true, true, true,
                         false),
};

#undef HARDWARE_GPIO_OPTION

static hardware_map_t s_active_hardware_map;
static bool s_hardware_map_initialized = false;

#ifndef APP_TEMPLATE_HW_DEFAULT_FLOAT_GPIO
#define APP_TEMPLATE_HW_DEFAULT_FLOAT_GPIO GPIO_NUM_16
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_GPIO
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_GPIO GPIO_NUM_5
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_GPIO
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_GPIO GPIO_NUM_6
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO
#define APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO GPIO_NUM_7
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO
#define APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO GPIO_NUM_8
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW true
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW
#define APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW true
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW
#define APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW true
#endif

esp_err_t hardware_map_init(void) {
  s_active_hardware_map = hardware_map_defaults();

  nvs_store_hardware_map_load_status_t status =
      nvs_store_load_hardware_map(&s_active_hardware_map);
  if (status != NVS_STORE_HARDWARE_MAP_LOADED ||
      !hardware_map_validate(&s_active_hardware_map)) {
    s_active_hardware_map = hardware_map_defaults();
  }

  s_hardware_map_initialized = true;
  return ESP_OK;
}

gpio_num_t hardware_map_get_active_pin(hardware_role_t role) {
  if (!s_hardware_map_initialized) {
    hardware_map_init();
  }
  switch (role) {
  case HARDWARE_ROLE_FLOAT_INPUT:
    return s_active_hardware_map.float_input_gpio;
  case HARDWARE_ROLE_PUMP_RELAY_1:
    return s_active_hardware_map.pump_relay1_gpio;
  case HARDWARE_ROLE_PUMP_RELAY_2:
    return s_active_hardware_map.pump_relay2_gpio;
  case HARDWARE_ROLE_DS18B20_DATA:
    return s_active_hardware_map.ds18b20_data_gpio;
  case HARDWARE_ROLE_COOLING_RELAY:
    return s_active_hardware_map.cooling_relay_gpio;
  default:
    return GPIO_NUM_NC;
  }
}

bool hardware_map_is_pin_allowed(gpio_num_t pin) {
  switch (pin) {
  case GPIO_NUM_1:
  case GPIO_NUM_2:
  case GPIO_NUM_5:
  case GPIO_NUM_6:
  case GPIO_NUM_7:
  case GPIO_NUM_8:
  case GPIO_NUM_15:
  case GPIO_NUM_16:
  case GPIO_NUM_17:
  case GPIO_NUM_18:
    return true;
  default:
    return false;
  }
}

gpio_num_t hardware_map_pin_to_gpio(hardware_pin_t pin) {
  return hardware_map_pin_valid(pin) ? (gpio_num_t)pin : GPIO_NUM_NC;
}

bool hardware_map_pin_valid(hardware_pin_t pin) {
  return pin > HARDWARE_PIN_INVALID && pin < HARDWARE_PIN_COUNT;
}

const hardware_pin_descriptor_t *
hardware_map_pin_descriptor(hardware_pin_t pin) {
  if (!hardware_map_pin_valid(pin)) {
    return NULL;
  }

  for (size_t i = 0; i < ARRAY_SIZE(s_pin_descriptors); i++) {
    if (s_pin_descriptors[i].pin == pin) {
      return &s_pin_descriptors[i];
    }
  }

  return NULL;
}

bool hardware_map_pin_is_reserved(hardware_pin_t pin) {
  /*
   * This is an allowlist policy: an unlisted pin is reserved or unsupported
   * for application roles, even if the silicon GPIO matrix can route it.
   */
  return hardware_map_pin_descriptor(pin) == NULL;
}

hardware_map_t hardware_map_defaults(void) {
  return (hardware_map_t){
      .float_input_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_FLOAT_GPIO,
      .pump_relay1_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_GPIO,
      .pump_relay2_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_GPIO,
      .ds18b20_data_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO,
      .cooling_relay_gpio =
          (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO,
  };
}

hardware_relay_polarity_map_t hardware_map_default_polarities(void) {
  return (hardware_relay_polarity_map_t){
      .pump_relay1 = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW
                         ? HARDWARE_RELAY_ACTIVE_LOW
                         : HARDWARE_RELAY_ACTIVE_HIGH,
      .pump_relay2 = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW
                         ? HARDWARE_RELAY_ACTIVE_LOW
                         : HARDWARE_RELAY_ACTIVE_HIGH,
      .cooling_relay = APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW
                           ? HARDWARE_RELAY_ACTIVE_LOW
                           : HARDWARE_RELAY_ACTIVE_HIGH,
  };
}

bool hardware_map_role_valid(hardware_role_t role) {
  return role >= HARDWARE_ROLE_FLOAT_INPUT && role < HARDWARE_ROLE_COUNT;
}

bool hardware_map_polarity_valid(hardware_relay_polarity_t polarity) {
  return polarity == HARDWARE_RELAY_ACTIVE_HIGH ||
         polarity == HARDWARE_RELAY_ACTIVE_LOW;
}

bool hardware_map_timer_start_phase_valid(hardware_timer_start_phase_t phase) {
  return phase == HARDWARE_TIMER_START_PHASE_ON ||
         phase == HARDWARE_TIMER_START_PHASE_OFF;
}

bool hardware_map_cooling_mode_valid(hardware_cooling_mode_t mode) {
  return mode == HARDWARE_COOLING_MODE_AUTO ||
         mode == HARDWARE_COOLING_MODE_FORCE_OFF ||
         mode == HARDWARE_COOLING_MODE_TEST_ON;
}

static hardware_pin_capability_t
required_capabilities_for_role(hardware_role_t role) {
  switch (role) {
  case HARDWARE_ROLE_FLOAT_INPUT:
    return HARDWARE_PIN_CAP_INPUT;
  case HARDWARE_ROLE_PUMP_RELAY_1:
  case HARDWARE_ROLE_PUMP_RELAY_2:
  case HARDWARE_ROLE_COOLING_RELAY:
    return HARDWARE_PIN_CAP_OUTPUT;
  case HARDWARE_ROLE_DS18B20_DATA:
    return HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT;
  default:
    return HARDWARE_PIN_CAP_NONE;
  }
}

const char *hardware_map_role_name(hardware_role_t role) {
  switch (role) {
  case HARDWARE_ROLE_FLOAT_INPUT:
    return "float_input";
  case HARDWARE_ROLE_PUMP_RELAY_1:
    return "pump_relay_1";
  case HARDWARE_ROLE_PUMP_RELAY_2:
    return "pump_relay_2";
  case HARDWARE_ROLE_DS18B20_DATA:
    return "ds18b20_data";
  case HARDWARE_ROLE_COOLING_RELAY:
    return "cooling_relay";
  default:
    return "unknown";
  }
}

const char *hardware_map_polarity_name(hardware_relay_polarity_t polarity) {
  switch (polarity) {
  case HARDWARE_RELAY_ACTIVE_HIGH:
    return "active_high";
  case HARDWARE_RELAY_ACTIVE_LOW:
    return "active_low";
  default:
    return "unknown";
  }
}

const char *
hardware_map_timer_start_phase_name(hardware_timer_start_phase_t phase) {
  switch (phase) {
  case HARDWARE_TIMER_START_PHASE_ON:
    return "on";
  case HARDWARE_TIMER_START_PHASE_OFF:
    return "off";
  default:
    return "unknown";
  }
}

const char *hardware_map_cooling_mode_name(hardware_cooling_mode_t mode) {
  switch (mode) {
  case HARDWARE_COOLING_MODE_AUTO:
    return "auto";
  case HARDWARE_COOLING_MODE_FORCE_OFF:
    return "force_off";
  case HARDWARE_COOLING_MODE_TEST_ON:
    return "test_on";
  default:
    return "unknown";
  }
}

const hardware_gpio_option_t *
hardware_map_options_for_role(hardware_role_t role, size_t *count) {
  if (count) {
    *count = 0;
  }

  switch (role) {
  case HARDWARE_ROLE_FLOAT_INPUT:
    if (count) {
      *count = ARRAY_SIZE(s_float_options);
    }
    return s_float_options;
  case HARDWARE_ROLE_PUMP_RELAY_1:
    if (count) {
      *count = ARRAY_SIZE(s_pump_relay1_options);
    }
    return s_pump_relay1_options;
  case HARDWARE_ROLE_PUMP_RELAY_2:
    if (count) {
      *count = ARRAY_SIZE(s_pump_relay2_options);
    }
    return s_pump_relay2_options;
  case HARDWARE_ROLE_DS18B20_DATA:
    if (count) {
      *count = ARRAY_SIZE(s_ds18b20_options);
    }
    return s_ds18b20_options;
  case HARDWARE_ROLE_COOLING_RELAY:
    if (count) {
      *count = ARRAY_SIZE(s_cooling_relay_options);
    }
    return s_cooling_relay_options;
  default:
    return NULL;
  }
}

bool hardware_map_gpio_allowed_for_role(hardware_role_t role, gpio_num_t gpio) {
  size_t count = 0;
  const hardware_gpio_option_t *options =
      hardware_map_options_for_role(role, &count);
  if (!options) {
    return false;
  }

  hardware_pin_capability_t required = required_capabilities_for_role(role);
  for (size_t i = 0; i < count; i++) {
    const hardware_pin_descriptor_t *descriptor =
        hardware_map_pin_descriptor(options[i].pin);
    if (!descriptor || hardware_map_pin_is_reserved(options[i].pin)) {
      continue;
    }
    if ((descriptor->capabilities & required) != required) {
      continue;
    }
    if (options[i].gpio == hardware_map_pin_to_gpio(options[i].pin) &&
        options[i].gpio == gpio) {
      return true;
    }
  }
  return false;
}

bool hardware_map_validate(const hardware_map_t *map) {
  if (!map) {
    return false;
  }

  gpio_num_t gpios[HARDWARE_ROLE_COUNT] = {
      map->float_input_gpio,  map->pump_relay1_gpio,   map->pump_relay2_gpio,
      map->ds18b20_data_gpio, map->cooling_relay_gpio,
  };

  for (int role = HARDWARE_ROLE_FLOAT_INPUT; role < HARDWARE_ROLE_COUNT;
       role++) {
    if (!hardware_map_gpio_allowed_for_role((hardware_role_t)role,
                                            gpios[role])) {
      return false;
    }
    for (int other = role + 1; other < HARDWARE_ROLE_COUNT; other++) {
      if (gpios[role] == gpios[other]) {
        return false;
      }
    }
  }

  return true;
}

bool hardware_map_equal(const hardware_map_t *a, const hardware_map_t *b) {
  return a && b && a->float_input_gpio == b->float_input_gpio &&
         a->pump_relay1_gpio == b->pump_relay1_gpio &&
         a->pump_relay2_gpio == b->pump_relay2_gpio &&
         a->ds18b20_data_gpio == b->ds18b20_data_gpio &&
         a->cooling_relay_gpio == b->cooling_relay_gpio;
}

bool hardware_map_reboot_required(const hardware_map_t *active,
                                  const hardware_map_t *pending) {
  return active && pending && !hardware_map_equal(active, pending);
}
