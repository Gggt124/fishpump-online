#include "dapi_alarms.h"

#include <math.h>
#include <stdlib.h>

#include "alarm_engine.h"
#include "alarm_rules.h"
#include "api_json.h"
#include "device_api.h"
#include "dry_run_guard.h"
#include "nvs_store.h"
#include "signal_registry.h"
#include "telemetry_hub.h"

/* Alarm methods: the code that used to run inside the /api/alarms/ handlers
 * after the request body was parsed. Replies keep the exact bytes of the old
 * handlers. Errors are returned as {"ok":false,"error":code,"message":...}. */

/* A finite number never leaves as NaN/Infinity (not valid JSON). */
static void alarms_add_value(cJSON *obj, const char *key, float v) {
  if (isfinite(v)) {
    cJSON_AddNumberToObject(obj, key, alarm_json_number(v));
  } else {
    cJSON_AddNullToObject(obj, key);
  }
}

static const char *alarms_storage_name(nvs_store_alarm_rules_load_status_t st) {
  switch (st) {
  case NVS_STORE_ALARM_RULES_LOADED:
    return "saved";
  case NVS_STORE_ALARM_RULES_DEFAULTS_MISSING:
    return "defaults";
  default:
    return "invalid";
  }
}

static bool alarms_engine_stalled(const alarm_engine_summary_t *sm, bool ok) {
  return !ok || !sm->running ||
         (uint32_t)(sensors_now_ms() - sm->last_tick_ms) > ALARM_ENGINE_STALL_MS;
}

static void alarms_add_counts(cJSON *parent, const alarm_counts_t *c) {
  cJSON *o = cJSON_AddObjectToObject(parent, "summary");
  if (!o) {
    return;
  }
  cJSON_AddNumberToObject(o, "active", c->active);
  cJSON_AddNumberToObject(o, "critical", c->critical);
  cJSON_AddNumberToObject(o, "unacked", c->unacked);
  cJSON_AddNumberToObject(o, "critical_unacked", c->critical_unacked);
  cJSON_AddNumberToObject(o, "warning_unacked", c->warning_unacked);
  cJSON_AddNumberToObject(o, "info_unacked", c->info_unacked);
  cJSON_AddNumberToObject(o, "max_severity", c->max_severity);
}

/* alarms.signals - the signal registry */
static dapi_status_t dapi_alarms_signals(const cJSON *params, dapi_source_t src,
                                         cJSON **out) {
  (void)params;
  (void)src;
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = root ? cJSON_AddArrayToObject(root, "signals") : NULL;
  if (!arr) {
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  size_t n = 0;
  const signal_def_t *all = signal_registry_all(&n);
  for (size_t i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    if (!o) {
      break;
    }
    cJSON_AddStringToObject(o, "key", all[i].key);
    cJSON_AddStringToObject(o, "label", all[i].label_th);
    cJSON_AddStringToObject(o, "unit", all[i].unit);
    cJSON_AddStringToObject(
        o, "kind", all[i].kind == SIGNAL_KIND_BOOL ? "bool" : "number");
    cJSON_AddStringToObject(o, "group", all[i].group);
    cJSON_AddNumberToObject(o, "period_sec", all[i].period_sec);
    cJSON_AddNumberToObject(o, "decimals", all[i].decimals);
    cJSON_AddItemToArray(arr, o);
  }
  *out = root;
  return DAPI_OK;
}

/* alarms.values - current reading of every signal */
static dapi_status_t dapi_alarms_values(const cJSON *params, dapi_source_t src,
                                        cJSON **out) {
  (void)params;
  (void)src;
  telemetry_snapshot_t *snap = calloc(1, sizeof(*snap));
  cJSON *root = cJSON_CreateObject();
  cJSON *vals = root ? cJSON_AddObjectToObject(root, "values") : NULL;
  if (!snap || !vals || telemetry_hub_get_latest(snap) != ESP_OK) {
    free(snap);
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  uint32_t now = sensors_now_ms();
  size_t n = 0;
  const signal_def_t *all = signal_registry_all(&n);
  for (size_t i = 0; i < n; i++) {
    signal_value_t sv;
    signal_registry_read(&all[i], snap, now, &sv);
    cJSON *o = cJSON_AddObjectToObject(vals, all[i].key);
    if (!o) {
      break;
    }
    if (!sv.present || (all[i].kind == SIGNAL_KIND_NUMBER && !isfinite(sv.value))) {
      cJSON_AddNullToObject(o, "v");
    } else if (all[i].kind == SIGNAL_KIND_BOOL) {
      cJSON_AddBoolToObject(o, "v", sv.value != 0.0);
    } else {
      cJSON_AddNumberToObject(o, "v", sv.value);
    }
    cJSON_AddBoolToObject(o, "trusted", sv.trusted);
  }
  free(snap);
  *out = root;
  return DAPI_OK;
}

/* alarms.rules.get */
static dapi_status_t dapi_alarms_rules_get(const cJSON *params, dapi_source_t src,
                                           cJSON **out) {
  (void)params;
  (void)src;
  alarm_rule_set_t *set = calloc(1, sizeof(*set));
  nvs_store_alarm_rules_load_status_t storage = NVS_STORE_ALARM_RULES_DEFAULTS_ERROR;
  if (!set || !alarm_engine_get_rules(set, &storage)) {
    free(set);
    *out = dapi_error("unavailable", "Alarm engine not running");
    return DAPI_UNAVAILABLE;
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = root ? cJSON_AddArrayToObject(root, "rules") : NULL;
  if (!arr) {
    free(set);
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  for (uint8_t i = 0; i < set->count; i++) {
    cJSON *r = alarm_rule_to_json(&set->rules[i]);
    if (r) {
      cJSON_AddItemToArray(arr, r);
    }
  }
  cJSON_AddNumberToObject(root, "max", ALARM_MAX_RULES);
  cJSON_AddStringToObject(root, "storage", alarms_storage_name(storage));
  free(set);
  *out = root;
  return DAPI_OK;
}

/* Maps a rule-JSON parse error to the 400 contract: the message is the field. */
static cJSON *alarms_json_error(alarm_json_err_t err, const char *field) {
  const char *code = err == ALARM_JSON_MISSING_FIELD  ? "missing_field"
                     : err == ALARM_JSON_INVALID_TYPE ? "invalid_type"
                                                      : "value_out_of_range";
  return dapi_error(code, field);
}

/* alarms.rules.set - no "id" = create, with "id" = replace */
static dapi_status_t dapi_alarms_rules_set(const cJSON *params, dapi_source_t src,
                                           cJSON **out) {
  (void)src;
  alarm_rule_t rule;
  bool has_id = false;
  const char *field = "";
  alarm_json_err_t jerr = alarm_rule_from_json(params, &rule, &has_id, &field);
  if (jerr != ALARM_JSON_OK) {
    *out = alarms_json_error(jerr, field);
    return DAPI_BAD_REQUEST;
  }

  alarm_rule_t stored;
  esp_err_t err = alarm_engine_upsert_rule(&rule, has_id, &stored, &field);
  switch (err) {
  case ESP_OK:
    break;
  case ESP_ERR_INVALID_ARG:
    *out = dapi_error("value_out_of_range", field);
    return DAPI_BAD_REQUEST;
  case ESP_ERR_NOT_FOUND:
    *out = dapi_error("not_found", "id");
    return DAPI_NOT_FOUND;
  case ESP_ERR_INVALID_SIZE:
    *out = dapi_error("rules_full", "rules");
    return DAPI_CONFLICT;
  case ESP_ERR_NO_MEM:
    *out = dapi_error("out_of_memory", "Not enough memory, try again");
    return DAPI_UNAVAILABLE;
  case ESP_ERR_INVALID_STATE:
    *out = dapi_error("unavailable", "Alarm engine not running");
    return DAPI_UNAVAILABLE;
  default:
    *out = dapi_error("storage_failed", "Could not save rules");
    return DAPI_INTERNAL;
  }

  cJSON *o = cJSON_CreateObject();
  cJSON *rj = alarm_rule_to_json(&stored);
  if (!o || !rj) {
    cJSON_Delete(o);
    cJSON_Delete(rj);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(o, "ok", true);
  cJSON_AddItemToObject(o, "rule", rj);
  *out = o;
  return DAPI_OK;
}

/* Reads {"id":n} style fields; on a bad value sets *err to the 400 object and
 * returns false. */
static bool alarms_read_id(const cJSON *root, uint16_t *id, cJSON **err) {
  const cJSON *it = cJSON_GetObjectItem(root, "id");
  if (!it) {
    *err = dapi_error("missing_field", "id");
    return false;
  }
  if (!cJSON_IsNumber(it)) {
    *err = dapi_error("invalid_type", "id");
    return false;
  }
  double d = it->valuedouble;
  if (!isfinite(d) || d < 1.0 || d > 65535.0 || d != floor(d)) {
    *err = dapi_error("value_out_of_range", "id");
    return false;
  }
  *id = (uint16_t)d;
  return true;
}

/* alarms.rules.delete {"id":n} */
static dapi_status_t dapi_alarms_rules_delete(const cJSON *params, dapi_source_t src,
                                              cJSON **out) {
  (void)src;
  uint16_t id = 0;
  if (!alarms_read_id(params, &id, out)) {
    return DAPI_BAD_REQUEST;
  }
  esp_err_t err = alarm_engine_delete_rule(id);
  if (err == ESP_ERR_NOT_FOUND) {
    *out = dapi_error("not_found", "id");
    return DAPI_NOT_FOUND;
  }
  if (err != ESP_OK) {
    *out = dapi_error("storage_failed", "Could not save rules");
    return DAPI_INTERNAL;
  }
  cJSON *o = cJSON_CreateObject();
  if (!o) {
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(o, "ok", true); /* prints {"ok":true} */
  *out = o;
  return DAPI_OK;
}

/* Maps an alarm_engine_ack / ack_all error to its reply. */
static dapi_status_t alarms_ack_error(esp_err_t err, cJSON **out) {
  switch (err) {
  case ESP_ERR_NOT_FOUND:
    *out = dapi_error("not_found", "id");
    return DAPI_NOT_FOUND;
  case ESP_ERR_NO_MEM:
    *out = dapi_error("out_of_memory", "Not enough memory, try again");
    return DAPI_UNAVAILABLE;
  case ESP_ERR_INVALID_STATE:
    *out = dapi_error("unavailable", "Alarm engine not running");
    return DAPI_UNAVAILABLE;
  default:
    *out = dapi_error("internal_error", "Ack failed");
    return DAPI_INTERNAL;
  }
}

/* alarms.ack {"id":n} or {"all":true} */
static dapi_status_t dapi_alarms_ack(const cJSON *params, dapi_source_t src,
                                     cJSON **out) {
  (void)src;
  size_t acked = 0;
  const cJSON *all = cJSON_GetObjectItem(params, "all");
  if (cJSON_IsTrue(all)) {
    esp_err_t err = alarm_engine_ack_all(&acked);
    if (err != ESP_OK) {
      return alarms_ack_error(err, out);
    }
  } else {
    uint16_t id = 0;
    if (!alarms_read_id(params, &id, out)) {
      return DAPI_BAD_REQUEST;
    }
    esp_err_t err = alarm_engine_ack(id);
    if (err != ESP_OK) {
      return alarms_ack_error(err, out);
    }
    acked = 1;
  }
  cJSON *o = cJSON_CreateObject();
  if (!o) {
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(o, "ok", true);
  cJSON_AddNumberToObject(o, "acked", (double)acked); /* prints {"ok":true,"acked":N} */
  *out = o;
  return DAPI_OK;
}

/* alarms.summary - small enough to poll every 10 s */
static dapi_status_t dapi_alarms_summary(const cJSON *params, dapi_source_t src,
                                         cJSON **out) {
  (void)params;
  (void)src;
  alarm_engine_summary_t sm = {0};
  bool ok = alarm_engine_get_summary(&sm);
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "boot_id", sm.boot_id);
  cJSON_AddNumberToObject(root, "last_seq", sm.last_seq);
  cJSON_AddNumberToObject(root, "last_critical_seq", sm.last_critical_seq);
  cJSON_AddBoolToObject(root, "engine_stalled", alarms_engine_stalled(&sm, ok));
  alarms_add_counts(root, &sm.counts);
  *out = root;
  return DAPI_OK;
}

/* alarms.status - summary, active alarms and recent history */
static dapi_status_t dapi_alarms_status(const cJSON *params, dapi_source_t src,
                                        cJSON **out) {
  (void)params;
  (void)src;
  alarm_engine_summary_t sm = {0};
  bool ok = alarm_engine_get_summary(&sm);
  alarm_active_t *active =
      calloc(ALARM_MAX_RULES + ALARM_ENGINE_MAX_SYSTEM, sizeof(*active));
  alarm_event_t *events = calloc(ALARM_HISTORY_LEN, sizeof(*events));
  cJSON *root = cJSON_CreateObject();
  if (!active || !events || !root) {
    free(active);
    free(events);
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  size_t n_active = ok ? alarm_engine_get_active(active, ALARM_MAX_RULES + ALARM_ENGINE_MAX_SYSTEM) : 0;
  uint32_t oldest = 0;
  size_t n_events =
      ok ? alarm_engine_get_events_since(0, events, ALARM_HISTORY_LEN, &oldest) : 0;

  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "boot_id", sm.boot_id);
  cJSON_AddNumberToObject(root, "last_seq", sm.last_seq);
  cJSON_AddNumberToObject(root, "uptime_ms", sensors_now_ms());
  cJSON_AddBoolToObject(root, "engine_stalled", alarms_engine_stalled(&sm, ok));
  alarms_add_counts(root, &sm.counts);

  cJSON *arr = cJSON_AddArrayToObject(root, "active");
  for (size_t i = 0; arr && i < n_active; i++) {
    const signal_def_t *sig = signal_registry_by_id(active[i].signal_id);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
      break;
    }
    cJSON_AddNumberToObject(o, "rule_id", active[i].rule_id);
    cJSON_AddStringToObject(o, "name", active[i].name);
    cJSON_AddStringToObject(o, "signal", sig ? sig->key : "");
    cJSON_AddStringToObject(o, "severity", alarm_severity_name(active[i].severity));
    cJSON_AddStringToObject(
        o, "state", active[i].state == ALARM_STATE_LATCHED ? "latched" : "active");
    cJSON_AddBoolToObject(o, "acked", active[i].acked);
    alarms_add_value(o, "value", active[i].value);
    cJSON_AddNumberToObject(o, "since_ms", active[i].since_ms);
    if (active[i].rule_id == ALARM_SYSTEM_ID_DRY_RUN) {
      dry_run_guard_status_t gs;
      cJSON *dr = dry_run_guard_get_status(&gs) ? cJSON_AddObjectToObject(o, "dry_run") : NULL;
      if (dr) {
        cJSON_AddStringToObject(dr, "state", dry_run_state_name(gs.state));
        cJSON_AddNumberToObject(dr, "trips", gs.trips);
        cJSON_AddNumberToObject(dr, "max_retries", gs.cfg.max_retries);
        cJSON_AddNumberToObject(dr, "wait_remaining_sec", gs.wait_remaining_sec);
      }
    }
    cJSON_AddItemToArray(arr, o);
  }

  /* Newest first, for display. */
  arr = cJSON_AddArrayToObject(root, "history");
  for (size_t i = n_events; arr && i > 0; i--) {
    const alarm_event_t *e = &events[i - 1];
    const signal_def_t *sig = signal_registry_by_id(e->signal_id);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
      break;
    }
    cJSON_AddNumberToObject(o, "seq", e->seq);
    cJSON_AddStringToObject(o, "type", alarms_type_name(e->type));
    cJSON_AddNumberToObject(o, "rule_id", e->rule_id);
    cJSON_AddStringToObject(o, "name", e->name);
    cJSON_AddStringToObject(o, "signal", sig ? sig->key : "");
    cJSON_AddStringToObject(o, "severity", alarm_severity_name(e->severity));
    alarms_add_value(o, "value", e->value);
    cJSON_AddNumberToObject(o, "uptime_ms", e->uptime_ms);
    if (e->reason == ALARM_REASON_NONE) {
      cJSON_AddNullToObject(o, "reason");
    } else {
      cJSON_AddStringToObject(o, "reason",
                              e->reason == ALARM_REASON_CONDITION  ? "condition"
                              : e->reason == ALARM_REASON_ACKED    ? "acked"
                                                                   : "rule_changed");
    }
    cJSON_AddItemToArray(arr, o);
  }
  free(active);
  free(events);
  *out = root;
  return DAPI_OK;
}

static const dapi_method_t s_alarm_methods[] = {
    {"alarms.signals", DAPI_F_READ, 0, dapi_alarms_signals},
    {"alarms.values", DAPI_F_READ, 0, dapi_alarms_values},
    {"alarms.status", DAPI_F_READ, 0, dapi_alarms_status},
    {"alarms.summary", DAPI_F_READ, 0, dapi_alarms_summary},
    {"alarms.rules.get", DAPI_F_READ, 0, dapi_alarms_rules_get},
    {"alarms.rules.set", DAPI_F_WRITE, ALARM_POST_BODY_BYTES, dapi_alarms_rules_set},
    {"alarms.rules.delete", DAPI_F_WRITE, ALARM_POST_BODY_BYTES, dapi_alarms_rules_delete},
    {"alarms.ack", DAPI_F_WRITE, ALARM_POST_BODY_BYTES, dapi_alarms_ack},
};

void dapi_alarms_register(void) {
  (void)device_api_register(s_alarm_methods,
                            sizeof(s_alarm_methods) / sizeof(s_alarm_methods[0]));
}
