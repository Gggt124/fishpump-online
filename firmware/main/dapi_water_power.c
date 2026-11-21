#include "dapi_water_power.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "device_api.h"
#include "power_monitor.h"
#include "water_quality.h"

/* Water-quality and power-meter write methods: the code that used to run inside
 * the /api/water/ and /api/power/ handlers after the request body was parsed.
 * Replies keep the exact status and bytes of the old handlers. */

/* Reads a finite JSON number; false if missing or not a number. */
static bool sensors_get_number(const cJSON *root, const char *key, double *out) {
  const cJSON *item = cJSON_GetObjectItem(root, key);
  if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
    return false;
  }
  *out = item->valuedouble;
  return true;
}

/* Maps the result of a sensor-task queue request to the reply. */
static dapi_status_t queue_result(cJSON **out, esp_err_t err, const char *field) {
  switch (err) {
  case ESP_OK:
    *out = cJSON_CreateObject();
    if (*out) {
      cJSON_AddBoolToObject(*out, "ok", true);
      cJSON_AddBoolToObject(*out, "queued", true);
    }
    return DAPI_ACCEPTED;
  case ESP_ERR_INVALID_ARG:
    *out = dapi_error("value_out_of_range", field);
    return DAPI_BAD_REQUEST;
  case ESP_ERR_NO_MEM:
    *out = dapi_error("queue_full", "Sensor is busy, try again");
    return DAPI_UNAVAILABLE;
  default:
    *out = dapi_error("sensor_unavailable", "Sensor task not running");
    return DAPI_UNAVAILABLE;
  }
}

/* {"channel":0|1} */
static dapi_status_t dapi_power_reset_energy(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  double ch = 0;
  bool present = sensors_get_number(params, "channel", &ch);
  if (!present) {
    *out = dapi_error("missing_field", "channel");
    return DAPI_BAD_REQUEST;
  }
  if (ch != 0.0 && ch != 1.0) {
    *out = dapi_error("value_out_of_range", "channel");
    return DAPI_BAD_REQUEST;
  }
  return queue_result(out, power_monitor_request_reset_energy((uint8_t)ch), "channel");
}

/* {"point":1|2,"buffer_ph":0..14} */
static dapi_status_t dapi_water_calibrate(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  double point = 0, buffer_ph = 0;
  bool has_point = sensors_get_number(params, "point", &point);
  bool has_ph = sensors_get_number(params, "buffer_ph", &buffer_ph);
  if (!has_point || !has_ph) {
    *out = dapi_error("missing_field", has_point ? "buffer_ph" : "point");
    return DAPI_BAD_REQUEST;
  }
  if (point != 1.0 && point != 2.0) {
    *out = dapi_error("value_out_of_range", "point");
    return DAPI_BAD_REQUEST;
  }
  if (buffer_ph < 0.0 || buffer_ph > 14.0) {
    *out = dapi_error("value_out_of_range", "buffer_ph");
    return DAPI_BAD_REQUEST;
  }
  return queue_result(out, water_quality_request_calibrate((uint8_t)point, (float)buffer_ph),
                      "buffer_ph");
}

static dapi_status_t dapi_water_calibrate_cancel(const cJSON *params, dapi_source_t src,
                                                 cJSON **out) {
  (void)params;
  (void)src;
  return queue_result(out, water_quality_request_cancel(), "");
}

/* {"ph_offset":+-2} or {"temp_offset":+-10} */
static dapi_status_t dapi_water_offset(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  bool has_ph_key = cJSON_HasObjectItem(params, "ph_offset");
  bool has_temp_key = cJSON_HasObjectItem(params, "temp_offset");
  double value = 0;
  bool is_ph = has_ph_key;
  bool has_value = (has_ph_key != has_temp_key) &&
                   sensors_get_number(params, is_ph ? "ph_offset" : "temp_offset", &value);
  if (!has_value) {
    *out = dapi_error("missing_field", "ph_offset|temp_offset");
    return DAPI_BAD_REQUEST;
  }
  if (is_ph) {
    if (fabs(value) > 2.0) {
      *out = dapi_error("value_out_of_range", "ph_offset");
      return DAPI_BAD_REQUEST;
    }
    return queue_result(out, water_quality_request_ph_offset((float)value), "ph_offset");
  }
  if (fabs(value) > 10.0) {
    *out = dapi_error("value_out_of_range", "temp_offset");
    return DAPI_BAD_REQUEST;
  }
  return queue_result(out, water_quality_request_temp_offset((float)value), "temp_offset");
}

static const dapi_method_t s_water_power_methods[] = {
    {"water.calibrate", DAPI_F_WRITE, SENSORS_BODY_BYTES, dapi_water_calibrate},
    {"water.calibrate.cancel", DAPI_F_WRITE, SENSORS_BODY_BYTES, dapi_water_calibrate_cancel},
    {"water.offset", DAPI_F_WRITE, SENSORS_BODY_BYTES, dapi_water_offset},
    {"power.reset_energy", DAPI_F_WRITE, SENSORS_BODY_BYTES, dapi_power_reset_energy},
};

void dapi_water_power_register(void) {
  (void)device_api_register(s_water_power_methods,
                            sizeof(s_water_power_methods) / sizeof(s_water_power_methods[0]));
}
