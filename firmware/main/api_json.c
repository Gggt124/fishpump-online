#include "api_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "alarm_engine.h"
#include "dry_run_guard.h"
#include "esp_timer.h"

uint32_t sensors_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *dry_run_state_name(uint8_t state) {
  switch (state) {
  case DRY_RUN_MONITOR:
    return "monitor";
  case DRY_RUN_WAIT:
    return "wait";
  case DRY_RUN_LOCKED:
    return "locked";
  default:
    return "disabled";
  }
}

const char *alarms_type_name(uint8_t type) {
  switch (type) {
  case ALARM_EVT_RAISED:
    return "raised";
  case ALARM_EVT_CLEARED:
    return "cleared";
  default:
    return "acked";
  }
}

void dry_run_add_runtime_fields(cJSON *obj, const dry_run_guard_status_t *gs) {
  cJSON_AddStringToObject(obj, "state", dry_run_state_name(gs->state));
  cJSON_AddNumberToObject(obj, "trips", gs->trips);
  cJSON_AddNumberToObject(obj, "max_retries", gs->cfg.max_retries);
  cJSON_AddNumberToObject(obj, "wait_remaining_sec", gs->wait_remaining_sec);
  cJSON_AddNumberToObject(obj, "dry_accum_sec", gs->dry_accum_sec);
}

const char *api_hardware_map_status_name(nvs_store_hardware_map_load_status_t status) {
  switch (status) {
  case NVS_STORE_HARDWARE_MAP_LOADED:
    return "loaded";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING:
    return "defaults_missing";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_INVALID:
    return "defaults_invalid";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR:
  default:
    return "defaults_error";
  }
}

const cJSON *api_json_get_item(const cJSON *root, const char *key1, const char *key2) {
  const cJSON *item = cJSON_GetObjectItem(root, key1);
  if (!item && key2) {
    item = cJSON_GetObjectItem(root, key2);
  }
  return item;
}

void json_gen_init(json_gen_t *gen, char *buf, size_t capacity) {
  if (gen) {
    gen->buf = buf;
    gen->capacity = capacity;
    gen->len = 0;
    gen->first_item = true;
    if (capacity > 0 && buf)
      buf[0] = '\0';
  }
}

void json_gen_append(json_gen_t *gen, const char *fmt, ...) {
  if (!gen || !gen->buf || gen->len >= gen->capacity)
    return;
  va_list args;
  va_start(args, fmt);
  int space = gen->capacity - gen->len;
  int written = vsnprintf(gen->buf + gen->len, space, fmt, args);
  va_end(args);
  if (written > 0) {
    if (written >= space) {
      gen->len = gen->capacity;
    } else {
      gen->len += written;
    }
  }
}

void json_gen_start_object(json_gen_t *gen) {
  if (!gen->first_item)
    json_gen_append(gen, ",");
  json_gen_append(gen, "{");
  gen->first_item = true;
}

void json_gen_end_object(json_gen_t *gen) {
  json_gen_append(gen, "}");
  gen->first_item = false;
}

void json_gen_add_key(json_gen_t *gen, const char *key) {
  if (!gen->first_item)
    json_gen_append(gen, ",");
  json_gen_append(gen, "\"%s\":", key);
  gen->first_item = false;
}

void json_gen_add_bool(json_gen_t *gen, const char *key, bool val) {
  json_gen_add_key(gen, key);
  json_gen_append(gen, val ? "true" : "false");
}

void json_gen_add_string(json_gen_t *gen, const char *key, const char *val) {
  json_gen_add_key(gen, key);
  if (val)
    json_gen_append(gen, "\"%s\"", val);
  else
    json_gen_append(gen, "null");
}

void json_gen_add_number(json_gen_t *gen, const char *key, double val) {
  json_gen_add_key(gen, key);
  json_gen_append(gen, "%g", val);
}

bool is_valid_utf8_printable(const char *s, size_t max_len) {
  if (!s || !s[0])
    return false;
  size_t len = strnlen(s, max_len);
  if (len >= max_len)
    return false;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 && c != '	')
      return false;
  }
  return true;
}
