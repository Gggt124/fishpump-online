#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "dry_run_guard.h"
#include "nvs_store.h"

/* Helpers shared by web_server.c and the dapi_<group>.c method files. */

/* Milliseconds since boot (wraps at 32 bits). */
uint32_t sensors_now_ms(void);
/* Dry-run guard state as the API names it ("monitor", "wait", "locked", "disabled"). */
const char *dry_run_state_name(uint8_t state);
/* Alarm history event type as the API names it ("raised", "cleared", "acked"). */
const char *alarms_type_name(uint8_t type);
/* Adds {state,trips,max_retries,wait_remaining_sec,dry_accum_sec} - the pump_status shape. */
void dry_run_add_runtime_fields(cJSON *obj, const dry_run_guard_status_t *gs);
/* Hardware map load status as the API names it ("loaded", "defaults_missing", ...). */
const char *api_hardware_map_status_name(nvs_store_hardware_map_load_status_t status);
/* First of two accepted key spellings that is present, or NULL. */
const cJSON *api_json_get_item(const cJSON *root, const char *key1, const char *key2);

/* True for a non-empty string shorter than max_len with no control characters (tab allowed). */
bool is_valid_utf8_printable(const char *s, size_t max_len);

/* --------------- JSON Generator --------------- */
/* Writes a JSON object into a caller buffer with "%g" numbers. */
typedef struct {
  char *buf;
  size_t capacity;
  size_t len;
  bool first_item;
} json_gen_t;

void json_gen_init(json_gen_t *gen, char *buf, size_t capacity);
void json_gen_append(json_gen_t *gen, const char *fmt, ...);
void json_gen_start_object(json_gen_t *gen);
void json_gen_end_object(json_gen_t *gen);
void json_gen_add_key(json_gen_t *gen, const char *key);
void json_gen_add_bool(json_gen_t *gen, const char *key, bool val);
void json_gen_add_string(json_gen_t *gen, const char *key, const char *val);
void json_gen_add_number(json_gen_t *gen, const char *key, double val);
