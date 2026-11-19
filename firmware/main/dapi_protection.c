#include "dapi_protection.h"

#include <math.h>
#include <string.h>

#include "api_json.h"
#include "device_api.h"
#include "dry_run_guard.h"

/* Dry-run protection methods: the code that used to run inside the
 * /api/protection handlers after the request body was parsed. Replies keep the
 * exact bytes of the old handlers. */

static dapi_status_t protection_unavailable(cJSON **out) {
  *out = dapi_error("unavailable", "Dry-run protection not running");
  return DAPI_UNAVAILABLE;
}

/* The status document: {"ok":true,"pump":{settings},"pump_status":{runtime}}.
 * 503 when the guard is not running. */
static dapi_status_t protection_status(cJSON **out) {
  dry_run_guard_status_t gs;
  if (!dry_run_guard_get_status(&gs)) {
    return protection_unavailable(out);
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *pump = root ? cJSON_AddObjectToObject(root, "pump") : NULL;
  if (!pump) {
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddBoolToObject(pump, "enabled", gs.cfg.enabled);
  cJSON_AddNumberToObject(pump, "prime_sec", gs.cfg.prime_sec);
  cJSON_AddNumberToObject(pump, "dry_trip_sec", gs.cfg.dry_trip_sec);
  cJSON_AddNumberToObject(pump, "retry_wait_min", gs.cfg.retry_wait_min);
  cJSON_AddBoolToObject(pump, "retry_backoff", gs.cfg.retry_backoff);
  cJSON_AddNumberToObject(pump, "max_retries", gs.cfg.max_retries);
  cJSON *st = cJSON_AddObjectToObject(root, "pump_status");
  if (!st) {
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  dry_run_add_runtime_fields(st, &gs);
  *out = root;
  return DAPI_OK;
}

/* protection.get */
static dapi_status_t dapi_protection_get(const cJSON *params, dapi_source_t src,
                                         cJSON **out) {
  (void)params;
  (void)src;
  return protection_status(out);
}

/* Reads a required JSON boolean; sets the 400 body and returns false on a bad value. */
static bool protection_read_bool(const cJSON *obj, const char *name, bool *value,
                                 cJSON **err) {
  const cJSON *it = cJSON_GetObjectItem(obj, name);
  if (!it) {
    *err = dapi_error("missing_field", name);
    return false;
  }
  if (!cJSON_IsBool(it)) {
    *err = dapi_error("invalid_type", name);
    return false;
  }
  *value = cJSON_IsTrue(it);
  return true;
}

/* Reads a required whole JSON number in 0..max (same rules as alarms_read_id). */
static bool protection_read_uint(const cJSON *obj, const char *name, double max,
                                 uint32_t *value, cJSON **err) {
  const cJSON *it = cJSON_GetObjectItem(obj, name);
  if (!it) {
    *err = dapi_error("missing_field", name);
    return false;
  }
  if (!cJSON_IsNumber(it)) {
    *err = dapi_error("invalid_type", name);
    return false;
  }
  double d = it->valuedouble;
  if (!isfinite(d) || d < 0.0 || d > max || d != floor(d)) {
    *err = dapi_error("value_out_of_range", name);
    return false;
  }
  *value = (uint32_t)d;
  return true;
}

/* protection.set {"pump":{enabled,prime_sec,dry_trip_sec,retry_wait_min,retry_backoff,max_retries}} */
static dapi_status_t dapi_protection_set(const cJSON *params, dapi_source_t src,
                                         cJSON **out) {
  (void)src;
  const cJSON *pump = cJSON_GetObjectItem(params, "pump");
  if (!pump) {
    *out = dapi_error("missing_field", "pump");
    return DAPI_BAD_REQUEST;
  }
  if (!cJSON_IsObject(pump)) {
    *out = dapi_error("invalid_type", "pump");
    return DAPI_BAD_REQUEST;
  }
  dry_run_config_t cfg = {0};
  uint32_t prime = 0, trip = 0, wait_min = 0, retries = 0;
  bool ok = protection_read_bool(pump, "enabled", &cfg.enabled, out) &&
            protection_read_uint(pump, "prime_sec", 65535.0, &prime, out) &&
            protection_read_uint(pump, "dry_trip_sec", 65535.0, &trip, out) &&
            protection_read_uint(pump, "retry_wait_min", 65535.0, &wait_min, out) &&
            protection_read_bool(pump, "retry_backoff", &cfg.retry_backoff, out) &&
            protection_read_uint(pump, "max_retries", 255.0, &retries, out);
  if (!ok) {
    return DAPI_BAD_REQUEST;
  }
  cfg.prime_sec = (uint16_t)prime;
  cfg.dry_trip_sec = (uint16_t)trip;
  cfg.retry_wait_min = (uint16_t)wait_min;
  cfg.max_retries = (uint8_t)retries;

  const char *field = "";
  esp_err_t err = dry_run_guard_set_config(&cfg, &field);
  switch (err) {
  case ESP_OK:
    return protection_status(out);
  case ESP_ERR_INVALID_ARG:
    *out = dapi_error("value_out_of_range", field);
    return DAPI_BAD_REQUEST;
  case ESP_ERR_INVALID_STATE:
    return protection_unavailable(out);
  default:
    *out = dapi_error("storage_failed", "Could not save settings");
    return DAPI_INTERNAL;
  }
}

static const char *protection_measure_state_name(uint8_t state) {
  switch (state) {
  case DRY_RUN_MEASURE_RUNNING:
    return "running";
  case DRY_RUN_MEASURE_DONE:
    return "done";
  case DRY_RUN_MEASURE_NO_FLOW:
    return "no_flow";
  case DRY_RUN_MEASURE_STOPPED:
    return "stopped";
  default:
    return "idle";
  }
}

/* {"ok":true,"state","elapsed_ms","suggested_prime_sec"}; 503 when the guard is not running. */
static dapi_status_t protection_measure(cJSON **out) {
  dry_run_measure_status_t ms;
  if (!dry_run_guard_get_measure(&ms)) {
    return protection_unavailable(out);
  }
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "state", protection_measure_state_name(ms.state));
  cJSON_AddNumberToObject(root, "elapsed_ms", ms.elapsed_ms);
  cJSON_AddNumberToObject(root, "suggested_prime_sec", ms.suggested_prime_sec);
  *out = root;
  return DAPI_OK;
}

/* protection.measure.get */
static dapi_status_t dapi_protection_measure_get(const cJSON *params,
                                                 dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  return protection_measure(out);
}

/* protection.measure.set {"action":"start"|"stop"} */
static dapi_status_t dapi_protection_measure_set(const cJSON *params,
                                                 dapi_source_t src, cJSON **out) {
  (void)src;
  const cJSON *act = cJSON_GetObjectItem(params, "action");
  bool is_start = cJSON_IsString(act) && act->valuestring && strcmp(act->valuestring, "start") == 0;
  bool is_stop = cJSON_IsString(act) && act->valuestring && strcmp(act->valuestring, "stop") == 0;
  if (!is_start && !is_stop) {
    *out = dapi_error("invalid_action", "Action must be 'start' or 'stop'");
    return DAPI_BAD_REQUEST;
  }
  esp_err_t err = is_start ? dry_run_guard_measure_start() : dry_run_guard_measure_stop();
  if (is_start && err == ESP_ERR_INVALID_RESPONSE) {
    *out = dapi_error("flow_already_on", "Flow switch reads flow before the pump started");
    return DAPI_CONFLICT;
  }
  if (is_start && err == ESP_ERR_NOT_FOUND) {
    *out = dapi_error("flow_unavailable", "Flow switch could not be read");
    return DAPI_UNAVAILABLE;
  }
  if (is_start && err == ESP_ERR_INVALID_STATE) {
    *out = dapi_error("measure_unavailable", "Stop the water pump first");
    return DAPI_CONFLICT;
  }
  if (err != ESP_OK) {
    return protection_unavailable(out);
  }
  return protection_measure(out);
}

static const dapi_method_t s_protection_methods[] = {
    {"protection.get", DAPI_F_READ, 0, dapi_protection_get},
    {"protection.set", DAPI_F_WRITE, PROTECTION_POST_BODY_BYTES, dapi_protection_set},
    {"protection.measure.get", DAPI_F_READ, 0, dapi_protection_measure_get},
    {"protection.measure.set", DAPI_F_WRITE, PROTECTION_MEASURE_BODY_BYTES,
     dapi_protection_measure_set},
};

void dapi_protection_register(void) {
  (void)device_api_register(s_protection_methods,
                            sizeof(s_protection_methods) / sizeof(s_protection_methods[0]));
}
