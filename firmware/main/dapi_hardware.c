#include "dapi_hardware.h"

#include <stdlib.h>

#include "api_json.h"
#include "app_config.h"
#include "device_api.h"
#include "hardware_map.h"
#include "nvs_store.h"

/* Hardware map methods: the code that used to run inside the /api/hardware/map
 * handlers after the request body was parsed. Replies keep the exact bytes of
 * the old handlers. Saving a map needs a reboot to apply, so hardware.map.set
 * is local-only: ThingsBoard can read the map but never change it. */

static const char *api_pending_hardware_map_status_name(
    nvs_store_pending_hardware_map_load_status_t status) {
  switch (status) {
  case NVS_STORE_PENDING_HARDWARE_MAP_LOADED:
    return "loaded";
  case NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND:
    return "not_found";
  case NVS_STORE_PENDING_HARDWARE_MAP_INVALID:
    return "invalid";
  case NVS_STORE_PENDING_HARDWARE_MAP_ERROR:
  default:
    return "error";
  }
}

static const char *api_hardware_map_field_for_role(hardware_role_t role) {
  switch (role) {
  case HARDWARE_ROLE_FLOAT_INPUT:
    return "float_input_gpio";
  case HARDWARE_ROLE_PUMP_RELAY_1:
    return "pump_relay1_gpio";
  case HARDWARE_ROLE_PUMP_RELAY_2:
    return "pump_relay2_gpio";
  case HARDWARE_ROLE_DS18B20_DATA:
    return "ds18b20_gpio";
  case HARDWARE_ROLE_COOLING_RELAY:
    return "cooling_relay_gpio";
  default:
    return "unknown";
  }
}

static bool api_hardware_map_add_json(cJSON *root, const char *name,
                                      const hardware_map_t *map) {
  if (!root || !name || !map) {
    return false;
  }

  cJSON *obj = cJSON_AddObjectToObject(root, name);
  if (!obj) {
    return false;
  }

  cJSON_AddNumberToObject(obj, "float_input_gpio", map->float_input_gpio);
  cJSON_AddNumberToObject(obj, "pump_relay1_gpio", map->pump_relay1_gpio);
  cJSON_AddNumberToObject(obj, "pump_relay2_gpio", map->pump_relay2_gpio);
  cJSON_AddNumberToObject(obj, "ds18b20_gpio", map->ds18b20_data_gpio);
  cJSON_AddNumberToObject(obj, "cooling_relay_gpio", map->cooling_relay_gpio);
  return true;
}

static bool api_hardware_options_add_role(cJSON *options_root,
                                          hardware_role_t role) {
  if (!options_root || !hardware_map_role_valid(role)) {
    return false;
  }

  size_t option_count = 0;
  const hardware_gpio_option_t *options =
      hardware_map_options_for_role(role, &option_count);
  if (!options) {
    return false;
  }

  cJSON *arr = cJSON_AddArrayToObject(options_root,
                                      api_hardware_map_field_for_role(role));
  if (!arr) {
    return false;
  }

  for (size_t i = 0; i < option_count; i++) {
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      return false;
    }
    cJSON_AddNumberToObject(item, "pin_id", (double)options[i].pin);
    cJSON_AddNumberToObject(item, "gpio", options[i].gpio);
    cJSON_AddStringToObject(item, "label", options[i].label);
    cJSON_AddStringToObject(item, "role",
                            hardware_map_role_name(options[i].role));
    cJSON_AddBoolToObject(item, "input_capable", options[i].input_capable);
    cJSON_AddBoolToObject(item, "output_capable", options[i].output_capable);
    cJSON_AddBoolToObject(item, "internal_pull_capable",
                          options[i].internal_pull_capable);
    cJSON_AddBoolToObject(item, "is_default", options[i].is_default);
    if (!cJSON_AddItemToArray(arr, item)) {
      cJSON_Delete(item);
      return false;
    }
  }

  return true;
}

static bool api_hardware_map_add_options(cJSON *root) {
  if (!root) {
    return false;
  }

  cJSON *options = cJSON_AddObjectToObject(root, "options");
  if (!options) {
    return false;
  }

  for (hardware_role_t role = HARDWARE_ROLE_FLOAT_INPUT;
       role < HARDWARE_ROLE_COUNT; role = (hardware_role_t)(role + 1)) {
    if (!api_hardware_options_add_role(options, role)) {
      return false;
    }
  }

  return true;
}

/* Report firmware-locked GPIO pins (buttons, LEDs) and board identity to
 * the web UI so the hardware page is not hardcoded to one board. Pin values
 * come from existing compile-time macros in app_config.h (not new assignments).
 */
static bool api_hardware_add_board_info(cJSON *root) {
  if (!root)
    return false;

  cJSON *board = cJSON_AddObjectToObject(root, "board");
  if (!board)
    return false;
  cJSON_AddStringToObject(board, "target", APP_TEMPLATE_BOARD_TARGET);
  cJSON_AddStringToObject(board, "name", APP_TEMPLATE_BOARD_NAME);
  cJSON_AddNumberToObject(board, "flash_mb",
                          (double)APP_TEMPLATE_BOARD_FLASH_MB);
  cJSON_AddNumberToObject(board, "psram_mb",
                          (double)APP_TEMPLATE_BOARD_PSRAM_MB);

  cJSON *locked = cJSON_AddObjectToObject(root, "locked_pins");
  if (!locked)
    return false;
  cJSON_AddNumberToObject(locked, "boot_btn", (double)APP_CONFIG_BOOT_BTN_GPIO);
  cJSON_AddNumberToObject(locked, "ext_btn", (double)APP_CONFIG_EXT_BTN_GPIO);
  cJSON_AddNumberToObject(locked, "ext_led", (double)APP_CONFIG_EXT_LED_GPIO);
  cJSON_AddNumberToObject(locked, "rgb_led", (double)APP_CONFIG_RGB_LED_GPIO);

  return true;
}

/* Fixed sensor UART pins and fixed inputs (read-only on the Hardware page). */
static bool api_hardware_add_fixed_bus_pins(cJSON *root) {
  size_t count = 0;
  const hardware_bus_pin_t *bus = hardware_map_fixed_bus_pins(&count);
  cJSON *arr = cJSON_AddArrayToObject(root, "fixed_bus_pins");
  if (!arr) {
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      return false;
    }
    cJSON_AddStringToObject(item, "id", bus[i].id);
    cJSON_AddStringToObject(item, "label", bus[i].label);
    cJSON_AddNumberToObject(item, "uart", bus[i].uart_port);
    cJSON_AddNumberToObject(item, "tx", bus[i].tx_gpio);
    cJSON_AddNumberToObject(item, "rx", bus[i].rx_gpio);
    cJSON_AddNumberToObject(item, "baud", bus[i].baud);
    cJSON_AddItemToArray(arr, item);
  }

  size_t in_count = 0;
  const hardware_fixed_input_t *in = hardware_map_fixed_inputs(&in_count);
  cJSON *inputs = cJSON_AddArrayToObject(root, "fixed_inputs");
  if (!inputs) {
    return false;
  }
  for (size_t i = 0; i < in_count; i++) {
    cJSON *item = cJSON_CreateObject();
    if (!item) {
      return false;
    }
    cJSON_AddStringToObject(item, "id", in[i].id);
    cJSON_AddStringToObject(item, "label", in[i].label);
    cJSON_AddNumberToObject(item, "gpio", in[i].gpio);
    cJSON_AddStringToObject(item, "wiring", in[i].wiring);
    cJSON_AddItemToArray(inputs, item);
  }
  return true;
}

static bool api_hardware_required_gpio(const cJSON *root, const char *name,
                                       hardware_role_t role, gpio_num_t *out,
                                       const char **error_code,
                                       const char **error_message) {
  const cJSON *item = cJSON_GetObjectItem(root, name);
  if (!item) {
    *error_code = "missing_field";
    *error_message = name;
    return false;
  }
  if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }

  int gpio = item->valueint;
  if ((double)gpio != item->valuedouble) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }
  if (!hardware_map_gpio_allowed_for_role(role, (gpio_num_t)gpio)) {
    *error_code = "gpio_not_allowed";
    *error_message = name;
    return false;
  }

  *out = (gpio_num_t)gpio;
  return true;
}

static bool api_hardware_parse_map_payload(const cJSON *root,
                                           hardware_map_t *map,
                                           const char **error_code,
                                           const char **error_message) {
  if (!root || !map) {
    *error_code = "invalid_json";
    *error_message = "Expected a JSON object";
    return false;
  }

  const cJSON *map_obj = cJSON_GetObjectItem(root, "map");
  if (!map_obj || !cJSON_IsObject(map_obj)) {
    *error_code = "missing_field";
    *error_message = "map";
    return false;
  }

  const cJSON *confirm = cJSON_GetObjectItem(root, "confirm_reboot_required");
  if (!confirm || !cJSON_IsBool(confirm) || !cJSON_IsTrue(confirm)) {
    *error_code = "confirmation_required";
    *error_message = "confirm_reboot_required";
    return false;
  }

  if (!api_hardware_required_gpio(
          map_obj, "float_input_gpio", HARDWARE_ROLE_FLOAT_INPUT,
          &map->float_input_gpio, error_code, error_message) ||
      !api_hardware_required_gpio(
          map_obj, "pump_relay1_gpio", HARDWARE_ROLE_PUMP_RELAY_1,
          &map->pump_relay1_gpio, error_code, error_message) ||
      !api_hardware_required_gpio(
          map_obj, "pump_relay2_gpio", HARDWARE_ROLE_PUMP_RELAY_2,
          &map->pump_relay2_gpio, error_code, error_message) ||
      !api_hardware_required_gpio(
          map_obj, "ds18b20_gpio", HARDWARE_ROLE_DS18B20_DATA,
          &map->ds18b20_data_gpio, error_code, error_message) ||
      !api_hardware_required_gpio(
          map_obj, "cooling_relay_gpio", HARDWARE_ROLE_COOLING_RELAY,
          &map->cooling_relay_gpio, error_code, error_message)) {
    return false;
  }

  if (!hardware_map_validate(map)) {
    *error_code = "invalid_hardware_map";
    *error_message = "map";
    return false;
  }

  return true;
}

/* The map document: active and pending maps, options, board info, fixed pins.
 * NULL *out on allocation failure (the caller answers the memory 500). */
static dapi_status_t hardware_map_reply(cJSON **out) {
  hardware_map_t active = hardware_map_defaults();
  hardware_map_t pending = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t active_status = nvs_store_load_hardware_map(&active);
  nvs_store_pending_hardware_map_load_status_t pending_status = nvs_store_load_pending_hardware_map(&pending);

  bool pending_valid = pending_status == NVS_STORE_PENDING_HARDWARE_MAP_LOADED;
  bool reboot_required = false;
  bool reboot_status_ok = nvs_store_hardware_reboot_required(&reboot_required);
  if (!pending_valid) {
    reboot_required = false;
  }

  cJSON *root = cJSON_CreateObject();
  *out = NULL;
  if (!root) {
    return DAPI_INTERNAL;
  }

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "active_status", api_hardware_map_status_name(active_status));
  cJSON_AddStringToObject(root, "pending_status", api_pending_hardware_map_status_name(pending_status));
  cJSON_AddBoolToObject(root, "pending_valid", pending_valid);
  cJSON_AddBoolToObject(root, "reboot_required", reboot_status_ok && reboot_required);
  bool built = api_hardware_map_add_json(root, "active", &active);
  if (built) {
    if (pending_valid) {
      built = api_hardware_map_add_json(root, "pending", &pending);
    } else {
      cJSON_AddNullToObject(root, "pending");
    }
  }
  built = built && api_hardware_map_add_options(root) && api_hardware_add_board_info(root) &&
          api_hardware_add_fixed_bus_pins(root);
  if (!built) {
    cJSON_Delete(root);
    return DAPI_INTERNAL;
  }
  *out = root;
  return DAPI_OK;
}

/* hardware.map.get */
static dapi_status_t dapi_hardware_map_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  return hardware_map_reply(out);
}

/* hardware.map.set - validate, save the pending map for the next reboot, reply with the map. */
static dapi_status_t dapi_hardware_map_set(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  hardware_map_t submitted = hardware_map_defaults();
  const char *error_code = NULL;
  const char *error_message = NULL;
  bool valid = api_hardware_parse_map_payload(params, &submitted, &error_code, &error_message);
  if (!valid) {
    *out = dapi_error(error_code, error_message);
    return DAPI_BAD_REQUEST;
  }

  hardware_map_t active = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t active_status = nvs_store_load_hardware_map(&active);
  if (active_status == NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR) {
    *out = dapi_error("hardware_map_load_failed", "Could not load active hardware map");
    return DAPI_INTERNAL;
  }

  bool saved = hardware_map_equal(&active, &submitted) ? nvs_store_clear_pending_hardware_map()
                                                       : nvs_store_save_pending_hardware_map(&submitted);
  if (!saved) {
    *out = dapi_error("pending_save_failed", "Could not save pending hardware map");
    return DAPI_INTERNAL;
  }

  return hardware_map_reply(out);
}

static const dapi_method_t s_hardware_methods[] = {
    {"hardware.map.get", DAPI_F_READ, 0, dapi_hardware_map_get},
    {"hardware.map.set", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY, HARDWARE_MAP_BODY_BYTES, dapi_hardware_map_set},
};

void dapi_hardware_register(void) {
  (void)device_api_register(s_hardware_methods, sizeof(s_hardware_methods) / sizeof(s_hardware_methods[0]));
}
