#include "hardware_map.h"
#include "nvs_store.h"

#if __has_include("app_config.h")
#include "app_config.h"
#endif

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

_Static_assert(HARDWARE_ROLE_FLOAT == HARDWARE_ROLE_FLOAT_INPUT, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_PUMP1 == HARDWARE_ROLE_PUMP_RELAY_1, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_PUMP2 == HARDWARE_ROLE_PUMP_RELAY_2, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_TEMP_DATA == HARDWARE_ROLE_MAX31865_CS, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_COOLING == HARDWARE_ROLE_COOLING_PWM, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_DS18B20_DATA == HARDWARE_ROLE_MAX31865_CS, "Role alias mismatch");
_Static_assert(HARDWARE_ROLE_COOLING_RELAY == HARDWARE_ROLE_COOLING_PWM, "Role alias mismatch");

/* Only pins described below are considered reusable application GPIOs.
 *
 * ESP32-S3 deliberately excludes:
 * - GPIO 0, 3, 45, 46: strapping pins
 * - GPIO 4, 42: fixed sensor UART0 TX/RX, PZEM-004T pump 1
 *   (see hardware_map_fixed_bus_pins)
 * - GPIO 40, 41: fixed sensor UART1 TX/RX, PZEM-004T pump 2
 * - GPIO 21, 14: fixed sensor UART2 TX/RX, RS485 SEN0708 pH
 * - GPIO 9: fixed flow-switch input (see hardware_map_fixed_inputs)
 * - GPIO 10, 11, 12, 13: Native SPI2 (MAX31865 PT100 RTD)
 * - GPIO 19-20: native USB D-/D+ (console runs on USB-Serial-JTAG)
 * - GPIO 38, 39: external recovery button / status LED
 * - GPIO 47: unassigned
 * - GPIO 27-37: WROOM-1 Octal Flash/PSRAM connections
 * - GPIO 43-44: ROM bootloader UART0 default pins (boot log only)
 * - GPIO 48: DevKitC-1 RGB LED
 *
 * Treating the application pool as an allowlist is intentional. A GPIO that
 * is technically routable through the GPIO matrix is not automatically safe
 * to expose as a user-selectable hardware role.
 *
 * ESP32-S3 safe pool: GPIO 1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18.
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
    {HARDWARE_PIN_GPIO10, GPIO_NUM_10,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO10"},
    {HARDWARE_PIN_GPIO11, GPIO_NUM_11,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO11"},
    {HARDWARE_PIN_GPIO12, GPIO_NUM_12,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO12"},
    {HARDWARE_PIN_GPIO13, GPIO_NUM_13,
     HARDWARE_PIN_CAP_INPUT | HARDWARE_PIN_CAP_OUTPUT |
         HARDWARE_PIN_CAP_INTERNAL_PULL,
     "GPIO13"},
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

static const hardware_gpio_option_t s_max31865_cs_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 1, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 2, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 7, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 10, true, true, true,
                         true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_MAX31865_CS, 18, true, true, true,
                         false),
};

static const hardware_gpio_option_t s_cooling_pwm_options[] = {
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 5, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 6, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 7, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 8, true, true, true,
                         true),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 15, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 16, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 17, true, true, true,
                         false),
    HARDWARE_GPIO_OPTION(HARDWARE_ROLE_COOLING_PWM, 18, true, true, true,
                         false),
};

#define s_ds18b20_options s_max31865_cs_options
#define s_cooling_relay_options s_cooling_pwm_options

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

#ifndef APP_TEMPLATE_MAX31865_CS_GPIO
#define APP_TEMPLATE_MAX31865_CS_GPIO GPIO_NUM_10
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_MAX31865_CS_GPIO
#define APP_TEMPLATE_HW_DEFAULT_MAX31865_CS_GPIO APP_TEMPLATE_MAX31865_CS_GPIO
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO
#define APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO APP_TEMPLATE_HW_DEFAULT_MAX31865_CS_GPIO
#endif

#ifndef APP_TEMPLATE_COOLING_PWM_GPIO
#define APP_TEMPLATE_COOLING_PWM_GPIO GPIO_NUM_8
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_COOLING_PWM_GPIO
#define APP_TEMPLATE_HW_DEFAULT_COOLING_PWM_GPIO APP_TEMPLATE_COOLING_PWM_GPIO
#endif

#ifndef APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO
#define APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO APP_TEMPLATE_HW_DEFAULT_COOLING_PWM_GPIO
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
  case HARDWARE_ROLE_MAX31865_CS:
    return s_active_hardware_map.max31865_cs_gpio;
  case HARDWARE_ROLE_COOLING_PWM:
    return s_active_hardware_map.cooling_pwm_gpio;
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
  case GPIO_NUM_10:
  case GPIO_NUM_11:
  case GPIO_NUM_12:
  case GPIO_NUM_13:
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

static const hardware_bus_pin_t s_fixed_bus_pins[HARDWARE_BUS_COUNT] = {
    [HARDWARE_BUS_PZEM_PUMP1] = {0, GPIO_NUM_4, GPIO_NUM_42, 9600, 0x01,
                                 "pzem_pump1", "PZEM-004T ปั๊ม 1"},
    [HARDWARE_BUS_PZEM_PUMP2] = {1, GPIO_NUM_40, GPIO_NUM_41, 9600, 0x01,
                                 "pzem_pump2", "PZEM-004T ปั๊ม 2"},
    [HARDWARE_BUS_RS485_PH] = {2, GPIO_NUM_21, GPIO_NUM_14, 4800, 0x01,
                               "rs485_ph", "RS485 pH (SEN0708)"},
};

const hardware_bus_pin_t *hardware_map_fixed_bus_pins(size_t *count) {
  if (count) {
    *count = HARDWARE_BUS_COUNT;
  }
  return s_fixed_bus_pins;
}

static const hardware_fixed_input_t s_fixed_inputs[] = {
    {HARDWARE_FLOW_SWITCH_GPIO, "flow_switch", "Flow switch (น้ำไหล)",
     "GPIO9 → สวิตช์ (NO) → GND"},
};

const hardware_fixed_input_t *hardware_map_fixed_inputs(size_t *count) {
  if (count) {
    *count = ARRAY_SIZE(s_fixed_inputs);
  }
  return s_fixed_inputs;
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
      .max31865_cs_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_MAX31865_CS_GPIO,
      .cooling_pwm_gpio =
          (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_COOLING_PWM_GPIO,
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
  case HARDWARE_ROLE_COOLING_PWM:
    return HARDWARE_PIN_CAP_OUTPUT;
  case HARDWARE_ROLE_MAX31865_CS:
    return HARDWARE_PIN_CAP_OUTPUT;
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
  case HARDWARE_ROLE_MAX31865_CS:
    return "max31865_cs";
  case HARDWARE_ROLE_COOLING_PWM:
    return "cooling_pwm";
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
  case HARDWARE_ROLE_MAX31865_CS:
    if (count) {
      *count = ARRAY_SIZE(s_max31865_cs_options);
    }
    return s_max31865_cs_options;
  case HARDWARE_ROLE_COOLING_PWM:
    if (count) {
      *count = ARRAY_SIZE(s_cooling_pwm_options);
    }
    return s_cooling_pwm_options;
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
      map->max31865_cs_gpio,  map->cooling_pwm_gpio,
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
         a->max31865_cs_gpio == b->max31865_cs_gpio &&
         a->cooling_pwm_gpio == b->cooling_pwm_gpio;
}

bool hardware_map_reboot_required(const hardware_map_t *active,
                                  const hardware_map_t *pending) {
  return active && pending && !hardware_map_equal(active, pending);
}
