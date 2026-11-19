#include "dapi_cooling.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "api_json.h"
#include "cooling_control.h"
#include "device_api.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "hardware_map.h"
#include "nvs_store.h"

/* Cooling methods: the code that used to run inside the /api/cooling/ handlers
 * after the request body was parsed, plus the cooling status and config
 * documents. Replies keep the exact bytes of the old handlers. */

static const char *api_cooling_mode_name(cooling_control_mode_t mode) {
  switch (mode) {
  case COOLING_CONTROL_MODE_AUTO:
    return "auto";
  case COOLING_CONTROL_MODE_TEST_ON:
    return "test";
  case COOLING_CONTROL_MODE_FORCE_OFF:
  default:
    return "force_off";
  }
}

static const char *api_cooling_curve_type_name(cooling_curve_type_t curve_type) {
  switch (curve_type) {
  case COOLING_CURVE_STEP:
    return "step";
  case COOLING_CURVE_LINEAR:
  default:
    return "linear";
  }
}

static bool api_cooling_parse_curve_type(const cJSON *item,
                                         cooling_curve_type_t *curve_type) {
  if (!item || !cJSON_IsString(item) || !curve_type) {
    return false;
  }
  if (strcmp(item->valuestring, "linear") == 0) {
    *curve_type = COOLING_CURVE_LINEAR;
    return true;
  }
  if (strcmp(item->valuestring, "step") == 0) {
    *curve_type = COOLING_CURVE_STEP;
    return true;
  }
  return false;
}

static const char *api_cooling_settings_status_name(
    nvs_store_cooling_settings_load_status_t status) {
  switch (status) {
  case NVS_STORE_COOLING_SETTINGS_LOADED:
    return "loaded";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_MISSING:
    return "defaults_missing";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID:
    return "defaults_invalid";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR:
  default:
    return "defaults_error";
  }
}

static bool api_cooling_parse_mode(const cJSON *item,
                                   hardware_cooling_mode_t *mode) {
  if (!item || !cJSON_IsString(item) || !mode) {
    return false;
  }
  if (strcmp(item->valuestring, "auto") == 0) {
    *mode = HARDWARE_COOLING_MODE_AUTO;
    return true;
  }
  if (strcmp(item->valuestring, "force_off") == 0) {
    *mode = HARDWARE_COOLING_MODE_FORCE_OFF;
    return true;
  }
  if (strcmp(item->valuestring, "test_on") == 0 ||
      strcmp(item->valuestring, "test") == 0) {
    *mode = HARDWARE_COOLING_MODE_TEST_ON;
    return true;
  }
  return false;
}

static bool api_cooling_parse_polarity(const cJSON *item,
                                       hardware_relay_polarity_t *polarity) {
  if (!item || !cJSON_IsString(item) || !polarity) {
    return false;
  }
  if (strcmp(item->valuestring, "active_low") == 0) {
    *polarity = HARDWARE_RELAY_ACTIVE_LOW;
    return true;
  }
  if (strcmp(item->valuestring, "active_high") == 0) {
    *polarity = HARDWARE_RELAY_ACTIVE_HIGH;
    return true;
  }
  return false;
}

static cooling_control_mode_t
api_cooling_mode_from_hardware(hardware_cooling_mode_t mode) {
  switch (mode) {
  case HARDWARE_COOLING_MODE_AUTO:
    return COOLING_CONTROL_MODE_AUTO;
  case HARDWARE_COOLING_MODE_TEST_ON:
    return COOLING_CONTROL_MODE_TEST_ON;
  case HARDWARE_COOLING_MODE_FORCE_OFF:
  default:
    return COOLING_CONTROL_MODE_FORCE_OFF;
  }
}

static cooling_control_relay_polarity_t
api_cooling_polarity_from_hardware(hardware_relay_polarity_t polarity) {
  return polarity == HARDWARE_RELAY_ACTIVE_LOW
             ? COOLING_CONTROL_RELAY_ACTIVE_LOW
             : COOLING_CONTROL_RELAY_ACTIVE_HIGH;
}

static const char *
api_cooling_sensor_state_name(cooling_control_sensor_state_t state) {
  switch (state) {
  case COOLING_CONTROL_SENSOR_OK:
    return "ok";
  case COOLING_CONTROL_SENSOR_FAULT:
    return "fault";
  case COOLING_CONTROL_SENSOR_UNKNOWN:
  default:
    return "unknown";
  }
}

static const char *
api_cooling_fault_code_name(cooling_control_fault_code_t code) {
  switch (code) {
  case COOLING_CONTROL_FAULT_READ_FAILED:
    return "read_failed";
  case COOLING_CONTROL_FAULT_OUT_OF_RANGE:
    return "out_of_range";
  case COOLING_CONTROL_FAULT_CONFIG_INVALID:
    return "config_invalid";
  case COOLING_CONTROL_FAULT_NONE:
  default:
    return "none";
  }
}

static const char *
api_cooling_blocked_reason_name(cooling_control_blocked_reason_t reason) {
  switch (reason) {
  case COOLING_CONTROL_BLOCKED_COMPRESSOR_LOCKOUT:
    return "compressor_lockout";
  case COOLING_CONTROL_BLOCKED_SENSOR_FAULT:
    return "sensor_fault";
  case COOLING_CONTROL_BLOCKED_FORCE_OFF:
    return "force_off";
  case COOLING_CONTROL_BLOCKED_CONFIG_INVALID:
    return "config_invalid";
  case COOLING_CONTROL_BLOCKED_NONE:
  default:
    return "none";
  }
}

static bool api_cooling_add_status_fields_gen(json_gen_t *gen) {
  if (!gen) {
    return false;
  }

  cooling_control_status_t status = {0};
  if (!cooling_control_get_status(&status)) {
    return false;
  }

  json_gen_add_bool(gen, "initialized", status.initialized);
  json_gen_add_bool(gen, "config_valid", status.config_valid);
  json_gen_add_string(gen, "mode", api_cooling_mode_name(status.mode));
  json_gen_add_bool(gen, "auto_enable", status.auto_enable);
  json_gen_add_number(gen, "temperature_c", (double)(status.temperature_c));
  json_gen_add_bool(gen, "temperature_valid", status.temperature_valid);
  json_gen_add_string(gen, "sensor_state",
                      api_cooling_sensor_state_name(status.sensor_state));
  json_gen_add_bool(gen, "fault", status.fault);
  json_gen_add_string(gen, "fault_code",
                      api_cooling_fault_code_name(status.fault_code));
  json_gen_add_bool(gen, "relay_energized", status.relay_energized);
  json_gen_add_number(gen, "output_volts", (double)status.output_mv / 1000.0);
  json_gen_add_number(gen, "output_mv", (double)status.output_mv);
  json_gen_add_number(gen, "duty_percent", (double)status.duty_percent);
  json_gen_add_number(gen, "threshold_c",
                      (double)(status.threshold_c_x10) / 10.0);
  json_gen_add_number(gen, "hysteresis_c",
                      (double)(status.hysteresis_c_x10) / 10.0);
  json_gen_add_bool(gen, "lockout_active", status.lockout_active);
  json_gen_add_number(gen, "lockout_remaining_sec",
                      (double)(status.lockout_remaining_sec));
  json_gen_add_number(gen, "test_remaining_sec",
                      (double)(status.test_remaining_sec));
  json_gen_add_bool(gen, "cooling_demand", status.cooling_demand);
  json_gen_add_string(gen, "blocked_reason",
                      api_cooling_blocked_reason_name(status.blocked_reason));
  json_gen_add_number(gen, "ds18b20_gpio", (double)(status.ds18b20_gpio));
  json_gen_add_number(gen, "cooling_relay_gpio",
                      (double)(status.cooling_relay_gpio));
  json_gen_add_number(gen, "max31865_cs_gpio", (double)(status.max31865_cs_gpio));
  json_gen_add_number(gen, "cooling_pwm_gpio", (double)(status.cooling_pwm_gpio));
  return true;
}

static bool api_cooling_add_status_fields(cJSON *root) {
  if (!root) {
    return false;
  }

  cooling_control_status_t status = {0};
  if (!cooling_control_get_status(&status)) {
    return false;
  }

  cJSON_AddBoolToObject(root, "initialized", status.initialized);
  cJSON_AddBoolToObject(root, "config_valid", status.config_valid);
  cJSON_AddStringToObject(root, "mode", api_cooling_mode_name(status.mode));
  cJSON_AddBoolToObject(root, "auto_enable", status.auto_enable);
  cJSON_AddNumberToObject(root, "temperature_c", status.temperature_c);
  cJSON_AddBoolToObject(root, "temperature_valid", status.temperature_valid);
  cJSON_AddStringToObject(root, "sensor_state",
                          api_cooling_sensor_state_name(status.sensor_state));
  cJSON_AddBoolToObject(root, "fault", status.fault);
  cJSON_AddStringToObject(root, "fault_code",
                          api_cooling_fault_code_name(status.fault_code));
  cJSON_AddBoolToObject(root, "relay_energized", status.relay_energized);
  cJSON_AddNumberToObject(root, "output_volts", (double)status.output_mv / 1000.0);
  cJSON_AddNumberToObject(root, "output_mv", status.output_mv);
  cJSON_AddNumberToObject(root, "duty_percent", status.duty_percent);
  cJSON_AddNumberToObject(root, "threshold_c",
                          (double)status.threshold_c_x10 / 10.0);
  cJSON_AddNumberToObject(root, "hysteresis_c",
                          (double)status.hysteresis_c_x10 / 10.0);
  cJSON_AddBoolToObject(root, "lockout_active", status.lockout_active);
  cJSON_AddNumberToObject(root, "lockout_remaining_sec",
                          status.lockout_remaining_sec);
  cJSON_AddNumberToObject(root, "test_remaining_sec",
                          status.test_remaining_sec);
  cJSON_AddBoolToObject(root, "cooling_demand", status.cooling_demand);
  cJSON_AddStringToObject(
      root, "blocked_reason",
      api_cooling_blocked_reason_name(status.blocked_reason));
  cJSON_AddNumberToObject(root, "ds18b20_gpio", status.ds18b20_gpio);
  cJSON_AddNumberToObject(root, "cooling_relay_gpio",
                          status.cooling_relay_gpio);
  cJSON_AddNumberToObject(root, "max31865_cs_gpio", status.max31865_cs_gpio);
  cJSON_AddNumberToObject(root, "cooling_pwm_gpio", status.cooling_pwm_gpio);
  return true;
}

static bool
api_cooling_add_config_fields(cJSON *root,
                              const nvs_store_cooling_settings_t *settings,
                              nvs_store_cooling_settings_load_status_t status) {
  if (!root || !settings) {
    return false;
  }

  cJSON_AddStringToObject(root, "settings_status",
                          api_cooling_settings_status_name(status));
  cJSON_AddBoolToObject(root, "auto_enable", settings->auto_enable);
  cJSON_AddStringToObject(root, "curve_type",
                          api_cooling_curve_type_name(settings->curve_type));
  uint8_t count = settings->point_count;
  if (count < COOLING_MIN_RULE_POINTS || count > COOLING_MAX_RULE_POINTS) {
    count = 2;
  }
  cJSON_AddNumberToObject(root, "point_count", count);

  cJSON *pts_array = cJSON_AddArrayToObject(root, "points");
  if (pts_array) {
    for (uint8_t i = 0; i < count; i++) {
      cJSON *pt_obj = cJSON_CreateObject();
      if (pt_obj) {
        cJSON_AddNumberToObject(pt_obj, "temp_c",
                                (double)settings->points[i].temp_c_x10 / 10.0);
        cJSON_AddNumberToObject(pt_obj, "temp_c_x10",
                                settings->points[i].temp_c_x10);
        cJSON_AddNumberToObject(pt_obj, "output_volts",
                                (double)settings->points[i].output_mv / 1000.0);
        cJSON_AddNumberToObject(pt_obj, "output_mv",
                                settings->points[i].output_mv);
        cJSON_AddItemToArray(pts_array, pt_obj);
      }
    }
  }

  uint32_t min_off = settings->min_off_sec > 0
                         ? settings->min_off_sec
                         : settings->compressor_min_off_sec;
  cJSON_AddNumberToObject(root, "min_off_sec", min_off);
  cJSON_AddNumberToObject(root, "test_timeout_sec", settings->test_timeout_sec);
  cJSON_AddNumberToObject(root, "threshold_c",
                          (double)settings->threshold_c_x10 / 10.0);
  cJSON_AddNumberToObject(root, "threshold_c_x10", settings->threshold_c_x10);
  cJSON_AddNumberToObject(root, "hysteresis_c",
                          (double)settings->hysteresis_c_x10 / 10.0);
  cJSON_AddNumberToObject(root, "hysteresis_c_x10", settings->hysteresis_c_x10);
  cJSON_AddStringToObject(root, "mode",
                          hardware_map_cooling_mode_name(settings->mode));
  cJSON_AddNumberToObject(root, "compressor_min_off_sec", min_off);
  cJSON_AddStringToObject(root, "cooling_relay_polarity",
                          hardware_map_polarity_name(settings->relay_polarity));
  return true;
}

static bool api_cooling_add_limits(cJSON *root) {
  if (!root) {
    return false;
  }

  cJSON *limits = cJSON_AddObjectToObject(root, "limits");
  if (!limits) {
    return false;
  }
  cJSON_AddNumberToObject(limits, "threshold_c_x10_min", -550);
  cJSON_AddNumberToObject(limits, "threshold_c_x10_max", 1250);
  cJSON_AddNumberToObject(limits, "hysteresis_c_x10_min", 1);
  cJSON_AddNumberToObject(limits, "hysteresis_c_x10_max", 500);
  cJSON_AddNumberToObject(limits, "test_timeout_sec_min", 1);
  cJSON_AddNumberToObject(limits, "test_timeout_sec_max", 3600);
  cJSON_AddNumberToObject(limits, "compressor_min_off_sec_min", 120);
  cJSON_AddNumberToObject(limits, "compressor_min_off_sec_max", 86400);
  cJSON_AddNumberToObject(limits, "min_off_sec_min", 120);
  cJSON_AddNumberToObject(limits, "min_off_sec_max", 86400);
  cJSON_AddNumberToObject(limits, "min_points", COOLING_MIN_RULE_POINTS);
  cJSON_AddNumberToObject(limits, "max_points", COOLING_MAX_RULE_POINTS);
  cJSON_AddNumberToObject(limits, "max_output_volts", 10.0);
  cJSON_AddNumberToObject(limits, "max_output_mv", COOLING_RULE_MAX_OUTPUT_MV);

  cJSON *modes = cJSON_AddArrayToObject(root, "modes");
  cJSON *polarities = cJSON_AddArrayToObject(root, "relay_polarities");
  cJSON *curve_types = cJSON_AddArrayToObject(root, "curve_types");
  if (!modes || !polarities || !curve_types) {
    return false;
  }
  cJSON_AddItemToArray(modes, cJSON_CreateString("auto"));
  cJSON_AddItemToArray(modes, cJSON_CreateString("force_off"));
  cJSON_AddItemToArray(modes, cJSON_CreateString("test"));
  cJSON_AddItemToArray(modes, cJSON_CreateString("test_on"));
  cJSON_AddItemToArray(polarities, cJSON_CreateString("active_low"));
  cJSON_AddItemToArray(polarities, cJSON_CreateString("active_high"));
  cJSON_AddItemToArray(curve_types, cJSON_CreateString("linear"));
  cJSON_AddItemToArray(curve_types, cJSON_CreateString("step"));
  return true;
}

static void api_cooling_settings_to_runtime_config(
    const nvs_store_cooling_settings_t *settings,
    cooling_control_config_t *config) {
  if (!settings || !config) {
    return;
  }

  *config = cooling_control_default_config();
  hardware_map_t active_map = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t map_status =
      nvs_store_load_hardware_map(&active_map);
  if (map_status != NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR) {
    config->ds18b20_gpio = active_map.ds18b20_data_gpio;
    config->cooling_relay_gpio = active_map.cooling_relay_gpio;
    if (GPIO_IS_VALID_OUTPUT_GPIO(active_map.max31865_cs_gpio)) {
      config->max31865_cs_gpio = active_map.max31865_cs_gpio;
    }
    if (GPIO_IS_VALID_OUTPUT_GPIO(active_map.cooling_pwm_gpio)) {
      config->cooling_pwm_gpio = active_map.cooling_pwm_gpio;
    }
  }

  config->relay_polarity =
      api_cooling_polarity_from_hardware(settings->relay_polarity);
  config->threshold_c_x10 = settings->threshold_c_x10;
  config->hysteresis_c_x10 = settings->hysteresis_c_x10;
  config->auto_enable = settings->auto_enable;
  config->mode = api_cooling_mode_from_hardware(settings->mode);
  config->test_timeout_sec = settings->test_timeout_sec;
  uint32_t min_off = settings->min_off_sec > 0
                         ? settings->min_off_sec
                         : settings->compressor_min_off_sec;
  config->compressor_min_off_sec = min_off;

  /* Multi-point rule engine settings */
  config->settings.auto_enable = settings->auto_enable;
  config->settings.curve_type = settings->curve_type;
  config->settings.point_count = settings->point_count;
  memcpy(config->settings.points, settings->points, sizeof(config->settings.points));
  config->settings.min_off_sec = min_off;
  config->settings.test_timeout_sec = settings->test_timeout_sec;
}

static bool api_cooling_required_i32_range(const cJSON *root, const char *name,
                                           int32_t min_value, int32_t max_value,
                                           int32_t *out,
                                           const char **error_code,
                                           const char **error_message) {
  const cJSON *item = cJSON_GetObjectItem(root, name);
  if (!item) {
    *error_code = "missing_field";
    *error_message = name;
    return false;
  }
  if (!cJSON_IsNumber(item)) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }

  int32_t value = item->valueint;
  if ((double)value != item->valuedouble) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }
  if (value < min_value || value > max_value) {
    *error_code = "value_out_of_range";
    *error_message = name;
    return false;
  }

  *out = value;
  return true;
}

static bool api_cooling_required_u32_range(const cJSON *root, const char *name,
                                           uint32_t min_value,
                                           uint32_t max_value, uint32_t *out,
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

  uint32_t value = (uint32_t)item->valuedouble;
  if ((double)value != item->valuedouble) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }
  if (value < min_value || value > max_value) {
    *error_code = "value_out_of_range";
    *error_message = name;
    return false;
  }

  *out = value;
  return true;
}

static bool api_cooling_required_bool(const cJSON *root, const char *name,
                                      bool *out, const char **error_code,
                                      const char **error_message) {
  const cJSON *item = cJSON_GetObjectItem(root, name);
  if (!item) {
    *error_code = "missing_field";
    *error_message = name;
    return false;
  }
  if (!cJSON_IsBool(item)) {
    *error_code = "invalid_type";
    *error_message = name;
    return false;
  }

  *out = cJSON_IsTrue(item);
  return true;
}

static bool api_cooling_parse_config_payload(
    const cJSON *root, nvs_store_cooling_settings_t *settings,
    const char **error_code, const char **error_message) {
  if (!root || !settings) {
    *error_code = "invalid_json";
    *error_message = "Expected a JSON object";
    return false;
  }

  const cJSON *points_item = cJSON_GetObjectItem(root, "points");
  if (points_item) {
    if (!cJSON_IsArray(points_item)) {
      *error_code = "invalid_type";
      *error_message = "points must be an array";
      return false;
    }
    int pt_count = cJSON_GetArraySize(points_item);
    if (pt_count < COOLING_MIN_RULE_POINTS ||
        pt_count > COOLING_MAX_RULE_POINTS) {
      *error_code = "invalid_points_count";
      *error_message = "points array must have 2 to 8 points";
      return false;
    }

    cooling_rule_point_t parsed_points[COOLING_MAX_RULE_POINTS];
    memset(parsed_points, 0, sizeof(parsed_points));

    for (int i = 0; i < pt_count; i++) {
      const cJSON *pt_obj = cJSON_GetArrayItem(points_item, i);
      if (!pt_obj || !cJSON_IsObject(pt_obj)) {
        *error_code = "invalid_point";
        *error_message = "Each point must be an object";
        return false;
      }

      const cJSON *temp_c_item = cJSON_GetObjectItem(pt_obj, "temp_c");
      const cJSON *temp_x10_item = cJSON_GetObjectItem(pt_obj, "temp_c_x10");
      int32_t temp_c_x10 = 0;
      if (temp_c_item && cJSON_IsNumber(temp_c_item)) {
        temp_c_x10 = (int32_t)round(temp_c_item->valuedouble * 10.0);
      } else if (temp_x10_item && cJSON_IsNumber(temp_x10_item)) {
        temp_c_x10 = (int32_t)round(temp_x10_item->valuedouble);
      } else {
        *error_code = "missing_field";
        *error_message = "Point requires temp_c";
        return false;
      }

      if (temp_c_x10 < -500 || temp_c_x10 > 1500) {
        *error_code = "value_out_of_range";
        *error_message = "temp_c must be between -50.0 and 150.0 C";
        return false;
      }

      const cJSON *out_v_item = cJSON_GetObjectItem(pt_obj, "output_volts");
      const cJSON *out_mv_item = cJSON_GetObjectItem(pt_obj, "output_mv");
      uint16_t out_mv = 0;
      if (out_mv_item && cJSON_IsNumber(out_mv_item)) {
        if (out_mv_item->valuedouble < 0.0 ||
            out_mv_item->valuedouble > (double)COOLING_RULE_MAX_OUTPUT_MV) {
          *error_code = "value_out_of_range";
          *error_message = "output_mv must be between 0 and 10000";
          return false;
        }
        out_mv = (uint16_t)round(out_mv_item->valuedouble);
      } else if (out_v_item && cJSON_IsNumber(out_v_item)) {
        if (out_v_item->valuedouble < 0.0 || out_v_item->valuedouble > 10.0) {
          *error_code = "value_out_of_range";
          *error_message = "output_volts must be between 0.0 and 10.0";
          return false;
        }
        out_mv = (uint16_t)round(out_v_item->valuedouble * 1000.0);
      } else {
        *error_code = "missing_field";
        *error_message = "Point requires output_volts or output_mv";
        return false;
      }

      parsed_points[i].temp_c_x10 = temp_c_x10;
      parsed_points[i].output_mv = out_mv;
    }

    /* Optional curve_type */
    const cJSON *ct_item = cJSON_GetObjectItem(root, "curve_type");
    if (ct_item) {
      if (!api_cooling_parse_curve_type(ct_item, &settings->curve_type)) {
        *error_code = "invalid_curve_type";
        *error_message = "curve_type must be 'linear' or 'step'";
        return false;
      }
    }

    /* Optional min_off_sec */
    const cJSON *min_off_item =
        api_json_get_item(root, "min_off_sec", "compressor_min_off_sec");
    if (min_off_item && cJSON_IsNumber(min_off_item)) {
      if (min_off_item->valuedouble < 120.0 ||
          min_off_item->valuedouble > 86400.0) {
        *error_code = "hardware_safety";
        *error_message = "min_off_sec must be between 120 and 86400 sec";
        return false;
      }
      settings->min_off_sec = (uint32_t)min_off_item->valuedouble;
      settings->compressor_min_off_sec = settings->min_off_sec;
    }

    /* Optional auto_enable */
    const cJSON *ae_item = cJSON_GetObjectItem(root, "auto_enable");
    if (ae_item && cJSON_IsBool(ae_item)) {
      settings->auto_enable = cJSON_IsTrue(ae_item);
    }

    /* Optional test_timeout_sec */
    const cJSON *tt_item = cJSON_GetObjectItem(root, "test_timeout_sec");
    if (tt_item && cJSON_IsNumber(tt_item)) {
      if (tt_item->valuedouble < 1.0 || tt_item->valuedouble > 3600.0) {
        *error_code = "value_out_of_range";
        *error_message = "test_timeout_sec must be between 1 and 3600 sec";
        return false;
      }
      settings->test_timeout_sec = (uint32_t)tt_item->valuedouble;
    }

    /* Optional mode */
    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (mode) {
      if (!api_cooling_parse_mode(mode, &settings->mode)) {
        *error_code = "invalid_cooling_mode";
        *error_message = "mode";
        return false;
      }
      if (settings->mode == HARDWARE_COOLING_MODE_TEST_ON) {
        *error_code = "test_mode_not_persistent";
        *error_message = "Use /api/cooling/mode for runtime Test ON";
        return false;
      }
    }

    /* Optional polarity */
    const cJSON *polarity = cJSON_GetObjectItem(root, "cooling_relay_polarity");
    if (polarity) {
      if (!api_cooling_parse_polarity(polarity, &settings->relay_polarity)) {
        *error_code = "invalid_relay_polarity";
        *error_message = "cooling_relay_polarity";
        return false;
      }
    }

    /* Validate curve rule monotonicity and constraints */
    cooling_settings_t validate_test = {
        .auto_enable = settings->auto_enable,
        .curve_type = settings->curve_type,
        .point_count = (uint8_t)pt_count,
        .min_off_sec = settings->min_off_sec > 0 ? settings->min_off_sec : 180,
        .test_timeout_sec =
            settings->test_timeout_sec > 0 ? settings->test_timeout_sec : 30,
    };
    memcpy(validate_test.points, parsed_points, sizeof(parsed_points));
    if (!cooling_validate_settings(&validate_test)) {
      *error_code = "invalid_curve_points";
      *error_message =
          "Points must have strictly ascending temperatures (T0 < T1 < ... < "
          "Tn)";
      return false;
    }

    /* Commit parsed points to settings */
    settings->point_count = (uint8_t)pt_count;
    memcpy(settings->points, parsed_points, sizeof(settings->points));
    settings->threshold_c_x10 = parsed_points[0].temp_c_x10;
    settings->hysteresis_c_x10 =
        (pt_count > 1)
            ? (parsed_points[1].temp_c_x10 - parsed_points[0].temp_c_x10)
            : 10;
    return true;
  }

  /* Legacy parameter parsing fallback */
  if (!api_cooling_required_i32_range(root, "threshold_c_x10", -550, 1250,
                                      &settings->threshold_c_x10, error_code,
                                      error_message) ||
      !api_cooling_required_i32_range(root, "hysteresis_c_x10", 1, 500,
                                      &settings->hysteresis_c_x10, error_code,
                                      error_message) ||
      !api_cooling_required_bool(root, "auto_enable", &settings->auto_enable,
                                 error_code, error_message) ||
      !api_cooling_required_u32_range(root, "test_timeout_sec", 1, 3600,
                                      &settings->test_timeout_sec, error_code,
                                      error_message) ||
      !api_cooling_required_u32_range(root, "compressor_min_off_sec", 120,
                                      86400, &settings->compressor_min_off_sec,
                                      error_code, error_message)) {
    return false;
  }

  settings->min_off_sec = settings->compressor_min_off_sec;
  if (settings->compressor_min_off_sec < 120) {
    *error_code = "hardware_safety";
    *error_message = "min_off_sec must be >= 120 (hardware safety minimum)";
    return false;
  }

  const cJSON *mode = cJSON_GetObjectItem(root, "mode");
  if (!api_cooling_parse_mode(mode, &settings->mode)) {
    *error_code = mode ? "invalid_cooling_mode" : "missing_field";
    *error_message = "mode";
    return false;
  }
  if (settings->mode == HARDWARE_COOLING_MODE_TEST_ON) {
    *error_code = "test_mode_not_persistent";
    *error_message = "Use /api/cooling/mode for runtime Test ON";
    return false;
  }

  const cJSON *polarity = cJSON_GetObjectItem(root, "cooling_relay_polarity");
  if (!api_cooling_parse_polarity(polarity, &settings->relay_polarity)) {
    *error_code = polarity ? "invalid_relay_polarity" : "missing_field";
    *error_message = "cooling_relay_polarity";
    return false;
  }

  /* Sync legacy to 2 default points */
  settings->curve_type = COOLING_CURVE_LINEAR;
  settings->point_count = 2;
  settings->points[0].temp_c_x10 = settings->threshold_c_x10;
  settings->points[0].output_mv = 5000;
  settings->points[1].temp_c_x10 =
      settings->threshold_c_x10 + settings->hysteresis_c_x10;
  settings->points[1].output_mv = COOLING_RULE_MAX_OUTPUT_MV;

  return true;
}

/* {"ok":true, <config fields>, limits, "status":{...}}; a missing runtime status is the
 * old 409 status_unavailable. */
static dapi_status_t cooling_config_reply(cJSON **out,
                                          const nvs_store_cooling_settings_t *settings,
                                          nvs_store_cooling_settings_load_status_t status) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }

  cJSON_AddBoolToObject(root, "ok", true);
  api_cooling_add_config_fields(root, settings, status);
  api_cooling_add_limits(root);
  cJSON *status_obj = cJSON_AddObjectToObject(root, "status");
  if (!status_obj || !api_cooling_add_status_fields(status_obj)) {
    cJSON_Delete(root);
    *out = dapi_error("status_unavailable", "Cooling status is unavailable");
    return DAPI_CONFLICT;
  }
  *out = root;
  return DAPI_OK;
}

/* The saved config could not be applied to the runtime: 500 with the config and status. */
static dapi_status_t cooling_apply_error(cJSON **out, const char *code, const char *message,
                                         const nvs_store_cooling_settings_t *settings) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }

  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", code);
  cJSON_AddStringToObject(root, "message", message);
  api_cooling_add_config_fields(root, settings, NVS_STORE_COOLING_SETTINGS_LOADED);
  cJSON *status_obj = cJSON_AddObjectToObject(root, "status");
  if (status_obj) {
    api_cooling_add_status_fields(status_obj);
  }
  *out = root;
  return DAPI_INTERNAL;
}

/* cooling.status - the json_gen document, wrapped raw so the bytes are unchanged. */
static dapi_status_t dapi_cooling_status(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  char *json_buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!json_buf) {
    json_buf = malloc(2048);
  }
  if (!json_buf) {
    *out = NULL;
    return DAPI_INTERNAL;
  }

  json_gen_t gen;
  json_gen_init(&gen, json_buf, 2048);
  json_gen_start_object(&gen);
  json_gen_add_bool(&gen, "ok", true);

  if (!api_cooling_add_status_fields_gen(&gen)) {
    free(json_buf);
    *out = dapi_error("status_unavailable", "Cooling status is unavailable");
    return DAPI_CONFLICT;
  }
  json_gen_end_object(&gen);

  *out = cJSON_CreateRaw(json_buf);
  free(json_buf);
  return *out ? DAPI_OK : DAPI_INTERNAL;
}

/* cooling.config.get */
static dapi_status_t dapi_cooling_config_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  nvs_store_cooling_settings_t settings;
  nvs_store_cooling_settings_load_status_t status = nvs_store_load_cooling_settings(&settings);
  if (status == NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR ||
      status == NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID) {
    *out = dapi_error("settings_load_failed", "Could not load cooling settings");
    return DAPI_INTERNAL;
  }

  return cooling_config_reply(out, &settings, status);
}

/* cooling.config.set - validate, save, apply to the runtime, reply with the new config. */
static dapi_status_t dapi_cooling_config_set(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  nvs_store_cooling_settings_t settings;
  nvs_store_cooling_settings_defaults(&settings);
  nvs_store_load_cooling_settings(&settings);
  const char *error_code = NULL;
  const char *error_message = NULL;
  bool valid = api_cooling_parse_config_payload(params, &settings, &error_code, &error_message);
  if (!valid) {
    *out = dapi_error(error_code, error_message);
    return DAPI_BAD_REQUEST;
  }

  if (!nvs_store_save_cooling_settings(&settings)) {
    *out = dapi_error("settings_save_failed", "Could not save cooling settings");
    return DAPI_INTERNAL;
  }

  cooling_control_config_t config;
  api_cooling_settings_to_runtime_config(&settings, &config);
  if (!cooling_control_apply_config(&config) && !cooling_control_init(&config)) {
    cooling_control_stop();
    return cooling_apply_error(out, "runtime_apply_failed",
                               "Saved settings but could not apply cooling runtime config",
                               &settings);
  }

  return cooling_config_reply(out, &settings, NVS_STORE_COOLING_SETTINGS_LOADED);
}

/* cooling.mode.set {"mode":"auto"|"force_off"|"test"|"test_on"[,"test_mv"|"test_volts"]} */
static dapi_status_t dapi_cooling_mode_set(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  hardware_cooling_mode_t mode = HARDWARE_COOLING_MODE_FORCE_OFF;
  const cJSON *mode_item = cJSON_GetObjectItem(params, "mode");
  bool mode_present = mode_item != NULL;
  bool valid = api_cooling_parse_mode(mode_item, &mode);
  if (!valid) {
    *out = dapi_error(mode_present ? "invalid_cooling_mode" : "missing_field", "mode");
    return DAPI_BAD_REQUEST;
  }

  bool mode_ok = false;
  if (mode == HARDWARE_COOLING_MODE_TEST_ON) {
    uint16_t test_mv = COOLING_RULE_MAX_OUTPUT_MV;
    const cJSON *tmv = cJSON_GetObjectItem(params, "test_mv");
    const cJSON *tv = cJSON_GetObjectItem(params, "test_volts");
    if (tmv) {
      if (!cJSON_IsNumber(tmv) || tmv->valuedouble < 0.0 ||
          tmv->valuedouble > (double)COOLING_RULE_MAX_OUTPUT_MV) {
        *out = dapi_error("value_out_of_range", "test_mv must be a number between 0 and 10000");
        return DAPI_BAD_REQUEST;
      }
      test_mv = (uint16_t)round(tmv->valuedouble);
    } else if (tv) {
      if (!cJSON_IsNumber(tv) || tv->valuedouble < 0.0 || tv->valuedouble > 10.0) {
        *out = dapi_error("value_out_of_range",
                          "test_volts must be a number between 0.0 and 10.0");
        return DAPI_BAD_REQUEST;
      }
      test_mv = (uint16_t)round(tv->valuedouble * 1000.0);
    }
    mode_ok = cooling_control_start_test(test_mv);
  } else {
    mode_ok = cooling_control_set_mode(api_cooling_mode_from_hardware(mode));
    if (mode_ok) {
      nvs_store_cooling_settings_t settings;
      nvs_store_cooling_settings_defaults(&settings);
      nvs_store_load_cooling_settings(&settings);
      settings.mode = mode;
      if (mode == HARDWARE_COOLING_MODE_AUTO) {
        settings.auto_enable = true;
      } else if (mode == HARDWARE_COOLING_MODE_FORCE_OFF) {
        settings.auto_enable = false;
      }
      if (!nvs_store_save_cooling_settings(&settings)) {
        *out = dapi_error("settings_save_failed", "Cooling mode changed but could not be saved");
        return DAPI_INTERNAL;
      }
    }
  }

  if (!mode_ok) {
    *out = dapi_error("mode_change_failed", "Cooling mode could not be changed");
    return DAPI_CONFLICT;
  }

  nvs_store_cooling_settings_t settings;
  nvs_store_cooling_settings_defaults(&settings);
  nvs_store_cooling_settings_load_status_t status = nvs_store_load_cooling_settings(&settings);
  return cooling_config_reply(out, &settings, status);
}

static const dapi_method_t s_cooling_methods[] = {
    {"cooling.status", DAPI_F_READ, 0, dapi_cooling_status},
    {"cooling.config.get", DAPI_F_READ, 0, dapi_cooling_config_get},
    {"cooling.config.set", DAPI_F_WRITE, COOLING_CONFIG_BODY_BYTES, dapi_cooling_config_set},
    {"cooling.mode.set", DAPI_F_WRITE, COOLING_MODE_BODY_BYTES, dapi_cooling_mode_set},
};

void dapi_cooling_register(void) {
  (void)device_api_register(s_cooling_methods,
                            sizeof(s_cooling_methods) / sizeof(s_cooling_methods[0]));
}
