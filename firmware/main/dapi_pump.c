#include "dapi_pump.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_config.h"
#include "dapi_guards.h"
#include "device_api.h"
#include "dry_run_guard.h"
#include "esp_heap_caps.h"
#include "hardware_map.h"
#include "nvs_store.h"
#include "pump_control.h"
#include "pump_resume.h"
#include "pump_resume_service.h"

/* Pump methods: the code that used to run inside the /api/pump/ handlers after
 * the request body was parsed, plus the pump status and config documents.
 * Replies keep the exact bytes of the old handlers. */

static const char *
api_pump_settings_status_name(nvs_store_pump_settings_load_status_t status) {
  switch (status) {
  case NVS_STORE_PUMP_SETTINGS_LOADED:
    return "loaded";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_MISSING:
    return "defaults_missing";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID:
    return "defaults_invalid";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR:
  default:
    return "defaults_error";
  }
}

static const char *api_pump_relay_polarity_name(bool relay_active_low) {
  return relay_active_low ? "active_low" : "active_high";
}

static bool api_pump_parse_relay_polarity(const cJSON *item,
                                          bool *relay_active_low) {
  if (!item || !cJSON_IsString(item) || !relay_active_low) {
    return false;
  }
  if (strcmp(item->valuestring, "active_low") == 0) {
    *relay_active_low = true;
    return true;
  }
  if (strcmp(item->valuestring, "active_high") == 0) {
    *relay_active_low = false;
    return true;
  }
  return false;
}



static bool api_pump_parse_start_phase(const cJSON *item,
                                       hardware_timer_start_phase_t *phase) {
  if (!item || !cJSON_IsString(item) || !phase) {
    return false;
  }
  if (strcasecmp(item->valuestring, "on") == 0) {
    *phase = HARDWARE_TIMER_START_PHASE_ON;
    return true;
  }
  if (strcasecmp(item->valuestring, "off") == 0) {
    *phase = HARDWARE_TIMER_START_PHASE_OFF;
    return true;
  }
  return false;
}

static bool api_pump_load_active_hardware_map(
    hardware_map_t *map, nvs_store_hardware_map_load_status_t *status) {
  if (!map || !status) {
    return false;
  }

  *status = nvs_store_load_hardware_map(map);
  return *status != NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR;
}

static bool
api_pump_add_config_fields(cJSON *root,
                           const nvs_store_pump_settings_t *settings,
                           nvs_store_pump_settings_load_status_t status) {
  if (!root || !settings) {
    return false;
  }

  hardware_map_t active_map = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t map_status =
      NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING;
  api_pump_load_active_hardware_map(&active_map, &map_status);
  bool hardware_reboot_required = false;
  bool hardware_reboot_status_ok =
      nvs_store_hardware_reboot_required(&hardware_reboot_required);
  nvs_store_cooling_settings_t cooling_settings;
  nvs_store_cooling_settings_defaults(&cooling_settings);
  nvs_store_load_cooling_settings(&cooling_settings);

  cJSON_AddNumberToObject(root, "water_set1_on", settings->timer1_on_sec);
  cJSON_AddNumberToObject(root, "water_set1_off", settings->timer1_off_sec);
  cJSON_AddStringToObject(
      root, "water_set1_start_phase",
      hardware_map_timer_start_phase_name(settings->timer1_start_phase));
  cJSON_AddNumberToObject(root, "water_set2_on", settings->timer2_on_sec);
  cJSON_AddNumberToObject(root, "water_set2_off", settings->timer2_off_sec);
  cJSON_AddStringToObject(
      root, "water_set2_start_phase",
      hardware_map_timer_start_phase_name(settings->timer2_start_phase));
  cJSON_AddNumberToObject(root, "wash_on", settings->wash_on_sec);
  cJSON_AddNumberToObject(root, "wash_off", settings->wash_off_sec);
  cJSON_AddStringToObject(
      root, "wash_start_phase",
      settings->wash_start_phase == HARDWARE_TIMER_START_PHASE_ON ? "ON" : "OFF");
  cJSON_AddBoolToObject(root, "water_auto_start", settings->auto_start);
  cJSON_AddBoolToObject(root, "wash_auto_start", settings->wash_auto_start);

  cJSON_AddNumberToObject(root, "timer1_on_sec", settings->timer1_on_sec);
  cJSON_AddNumberToObject(root, "timer1_off_sec", settings->timer1_off_sec);
  cJSON_AddNumberToObject(root, "timer2_on_sec", settings->timer2_on_sec);
  cJSON_AddNumberToObject(root, "timer2_off_sec", settings->timer2_off_sec);
  cJSON_AddBoolToObject(root, "auto_start", settings->auto_start);
  cJSON_AddStringToObject(
      root, "relay_polarity",
      api_pump_relay_polarity_name(settings->relay1_active_low));
  cJSON_AddNumberToObject(root, "float_gpio", active_map.float_input_gpio);
  cJSON_AddNumberToObject(root, "relay_gpio", active_map.pump_relay1_gpio);
  cJSON_AddNumberToObject(root, "debounce_ms", 2000);

  cJSON_AddNumberToObject(root, "min_dwell_sec", settings->min_dwell_sec);
  cJSON_AddNumberToObject(root, "pump_relay1_gpio",
                          active_map.pump_relay1_gpio);
  cJSON_AddNumberToObject(root, "pump_relay2_gpio",
                          active_map.pump_relay2_gpio);
  cJSON_AddNumberToObject(root, "ds18b20_gpio", active_map.ds18b20_data_gpio);
  cJSON_AddNumberToObject(root, "cooling_relay_gpio",
                          active_map.cooling_relay_gpio);
  cJSON_AddStringToObject(
      root, "relay1_polarity",
      api_pump_relay_polarity_name(settings->relay1_active_low));
  cJSON_AddStringToObject(
      root, "relay2_polarity",
      api_pump_relay_polarity_name(settings->relay2_active_low));
  cJSON_AddStringToObject(
      root, "timer1_start_phase",
      hardware_map_timer_start_phase_name(settings->timer1_start_phase));
  cJSON_AddStringToObject(
      root, "timer2_start_phase",
      hardware_map_timer_start_phase_name(settings->timer2_start_phase));
  cJSON_AddStringToObject(
      root, "cooling_relay_polarity",
      hardware_map_polarity_name(cooling_settings.relay_polarity));
  cJSON_AddBoolToObject(root, "hardware_reboot_required",
                        hardware_reboot_status_ok && hardware_reboot_required);
  cJSON_AddBoolToObject(root, "hardware_fields_read_only", true);
  cJSON_AddStringToObject(root, "hardware_map_status",
                          api_hardware_map_status_name(map_status));
  cJSON_AddStringToObject(root, "settings_status",
                          api_pump_settings_status_name(status));
  return true;
}

static const char *api_pump_float_state_name(pump_control_float_state_t state) {
  switch (state) {
  case PUMP_CONTROL_FLOAT_OFF:
    return "off";
  case PUMP_CONTROL_FLOAT_ON:
    return "on";
  case PUMP_CONTROL_FLOAT_UNKNOWN:
  default:
    return "unknown";
  }
}

static const char *
api_pump_active_timer_name(pump_control_active_set_t set) {
  switch (set) {
  case PUMP_CONTROL_SET_1:
    return "timer1";
  case PUMP_CONTROL_SET_2:
    return "timer2";
  default:
    return "none";
  }
}

static const char *api_pump_timer_phase_name(pump_control_timer_phase_t phase) {
  switch (phase) {
  case PUMP_CONTROL_PHASE_ON:
    return "on";
  case PUMP_CONTROL_PHASE_OFF:
    return "off";
  case PUMP_CONTROL_PHASE_IDLE:
  default:
    return "idle";
  }
}

static const char *api_pump_timer_phase_name_upper(pump_control_timer_phase_t phase) {
  switch (phase) {
  case PUMP_CONTROL_PHASE_ON:
    return "ON";
  case PUMP_CONTROL_PHASE_OFF:
    return "OFF";
  case PUMP_CONTROL_PHASE_IDLE:
  default:
    return "IDLE";
  }
}

bool api_pump_add_status_fields_gen(json_gen_t *gen) {
  if (!gen) {
    return false;
  }

  pump_control_status_t status = {0};
  if (!pump_control_get_status(&status)) {
    return false;
  }

  nvs_store_pump_settings_t settings;
  nvs_store_pump_settings_load_status_t settings_status =
      nvs_store_load_pump_settings(&settings);
  hardware_map_t active_map = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t map_status =
      NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING;
  api_pump_load_active_hardware_map(&active_map, &map_status);
  bool hardware_reboot_required = false;
  bool hardware_reboot_status_ok =
      nvs_store_hardware_reboot_required(&hardware_reboot_required);
  nvs_store_cooling_settings_t cooling_settings;
  nvs_store_cooling_settings_defaults(&cooling_settings);
  nvs_store_load_cooling_settings(&cooling_settings);

  /* system_water JSON object */
  json_gen_add_key(gen, "system_water");
  json_gen_append(gen, "{");
  gen->first_item = true;
  json_gen_add_bool(gen, "running", status.water.running);
  json_gen_add_bool(gen, "relay_energized", status.water.relay_energized);
  json_gen_add_string(gen, "float_state",
                      status.water.float_state == PUMP_CONTROL_FLOAT_ON ? "ON" : "OFF");
  json_gen_add_number(gen, "active_set",
                      (double)(status.water.active_set == PUMP_CONTROL_SET_2 ? 2 : 1));
  json_gen_add_string(gen, "phase",
                      api_pump_timer_phase_name_upper(status.water.phase));
  json_gen_add_number(gen, "countdown_sec", (double)(status.water.countdown_sec));
  json_gen_add_number(gen, "total_duration_sec",
                      (double)(status.water.total_duration_sec));
  json_gen_add_number(gen, "cooldown_remaining_sec",
                      (double)(status.water.cooldown_remaining_sec));
  json_gen_add_bool(gen, "auto_start", status.water.auto_start);
  json_gen_add_number(gen, "resume_in_sec",
                      (double)pump_resume_service_remaining_sec());
  json_gen_end_object(gen);

  /* system_wash JSON object */
  json_gen_add_key(gen, "system_wash");
  json_gen_append(gen, "{");
  gen->first_item = true;
  json_gen_add_bool(gen, "running", status.wash.running);
  json_gen_add_bool(gen, "relay_energized", status.wash.relay_energized);
  json_gen_add_string(gen, "phase",
                      api_pump_timer_phase_name_upper(status.wash.phase));
  json_gen_add_number(gen, "countdown_sec", (double)(status.wash.countdown_sec));
  json_gen_add_number(gen, "total_duration_sec",
                      (double)(status.wash.total_duration_sec));
  json_gen_add_bool(gen, "auto_start", status.wash.auto_start);
  json_gen_end_object(gen);

  /* Root-level backward-compatible fields */
  json_gen_add_bool(gen, "running", status.water.running);
  json_gen_add_bool(gen, "initialized", status.initialized);
  json_gen_add_bool(gen, "config_valid", true);
  json_gen_add_bool(gen, "initial_stabilizing", false);
  json_gen_add_bool(gen, "fault", status.fault);
  json_gen_add_string(gen, "float_state",
                      api_pump_float_state_name(status.water.float_state));
  json_gen_add_string(gen, "active_timer",
                      api_pump_active_timer_name(status.water.active_set));
  json_gen_add_string(gen, "active_relay", status.water.active_set == PUMP_CONTROL_SET_1 ? "relay1" : "relay2");
  json_gen_add_string(gen, "phase", api_pump_timer_phase_name(status.water.phase));
  json_gen_add_number(gen, "countdown_sec", (double)(status.water.countdown_sec));
  json_gen_add_number(gen, "min_dwell_sec", (double)settings.min_dwell_sec);
  json_gen_add_number(gen, "cooldown_remaining_sec",
                      (double)status.water.cooldown_remaining_sec);
  json_gen_add_bool(gen, "relay_energized", status.water.relay_energized);
  json_gen_add_bool(gen, "relay1_energized", status.water.relay_energized);
  json_gen_add_bool(gen, "relay2_energized", status.wash.relay_energized);

  json_gen_add_number(gen, "float_gpio", (double)(active_map.float_input_gpio));
  json_gen_add_number(gen, "relay_gpio", (double)(active_map.pump_relay1_gpio));
  json_gen_add_number(gen, "active_relay_gpio", (double)(active_map.pump_relay1_gpio));
  json_gen_add_number(gen, "pump_relay1_gpio",
                      (double)(active_map.pump_relay1_gpio));
  json_gen_add_number(gen, "pump_relay2_gpio",
                      (double)(active_map.pump_relay2_gpio));
  json_gen_add_number(gen, "ds18b20_gpio",
                      (double)(active_map.ds18b20_data_gpio));
  json_gen_add_number(gen, "cooling_relay_gpio",
                      (double)(active_map.cooling_relay_gpio));
  json_gen_add_string(gen, "relay1_polarity",
                      api_pump_relay_polarity_name(settings.relay1_active_low));
  json_gen_add_string(gen, "relay2_polarity",
                      api_pump_relay_polarity_name(settings.relay2_active_low));
  json_gen_add_string(
      gen, "cooling_relay_polarity",
      hardware_map_polarity_name(cooling_settings.relay_polarity));
  json_gen_add_string(
      gen, "timer1_start_phase",
      hardware_map_timer_start_phase_name(settings.timer1_start_phase));
  json_gen_add_string(
      gen, "timer2_start_phase",
      hardware_map_timer_start_phase_name(settings.timer2_start_phase));
  json_gen_add_bool(gen, "hardware_reboot_required",
                    hardware_reboot_status_ok && hardware_reboot_required);
  json_gen_add_string(gen, "hardware_map_status",
                      api_hardware_map_status_name(map_status));
  json_gen_add_bool(gen, "auto_start", settings.auto_start);

  json_gen_add_string(gen, "settings_status",
                      api_pump_settings_status_name(settings_status));
  return true;
}

static bool api_pump_add_status_fields(cJSON *root) {
  if (!root) {
    return false;
  }

  pump_control_status_t status = {0};
  if (!pump_control_get_status(&status)) {
    return false;
  }

  nvs_store_pump_settings_t settings;
  nvs_store_pump_settings_load_status_t settings_status =
      nvs_store_load_pump_settings(&settings);
  hardware_map_t active_map = hardware_map_defaults();
  nvs_store_hardware_map_load_status_t map_status =
      NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING;
  api_pump_load_active_hardware_map(&active_map, &map_status);
  bool hardware_reboot_required = false;
  bool hardware_reboot_status_ok =
      nvs_store_hardware_reboot_required(&hardware_reboot_required);
  nvs_store_cooling_settings_t cooling_settings;
  nvs_store_cooling_settings_defaults(&cooling_settings);
  nvs_store_load_cooling_settings(&cooling_settings);

  /* system_water JSON object */
  cJSON *water_obj = cJSON_AddObjectToObject(root, "system_water");
  if (water_obj) {
    cJSON_AddBoolToObject(water_obj, "running", status.water.running);
    cJSON_AddBoolToObject(water_obj, "relay_energized", status.water.relay_energized);
    cJSON_AddStringToObject(
        water_obj, "float_state",
        status.water.float_state == PUMP_CONTROL_FLOAT_ON ? "ON" : "OFF");
    cJSON_AddNumberToObject(
        water_obj, "active_set",
        status.water.active_set == PUMP_CONTROL_SET_2 ? 2 : 1);
    cJSON_AddStringToObject(
        water_obj, "phase",
        api_pump_timer_phase_name_upper(status.water.phase));
    cJSON_AddNumberToObject(water_obj, "countdown_sec", status.water.countdown_sec);
    cJSON_AddNumberToObject(water_obj, "total_duration_sec",
                            status.water.total_duration_sec);
    cJSON_AddNumberToObject(water_obj, "cooldown_remaining_sec",
                            status.water.cooldown_remaining_sec);
    cJSON_AddBoolToObject(water_obj, "auto_start", status.water.auto_start);
    cJSON_AddNumberToObject(water_obj, "resume_in_sec",
                            pump_resume_service_remaining_sec());
  }

  /* system_wash JSON object */
  cJSON *wash_obj = cJSON_AddObjectToObject(root, "system_wash");
  if (wash_obj) {
    cJSON_AddBoolToObject(wash_obj, "running", status.wash.running);
    cJSON_AddBoolToObject(wash_obj, "relay_energized", status.wash.relay_energized);
    cJSON_AddStringToObject(
        wash_obj, "phase",
        api_pump_timer_phase_name_upper(status.wash.phase));
    cJSON_AddNumberToObject(wash_obj, "countdown_sec", status.wash.countdown_sec);
    cJSON_AddNumberToObject(wash_obj, "total_duration_sec",
                            status.wash.total_duration_sec);
    cJSON_AddBoolToObject(wash_obj, "auto_start", status.wash.auto_start);
  }

  /* Root-level backward-compatible fields */
  cJSON_AddBoolToObject(root, "running", status.water.running);
  cJSON_AddBoolToObject(root, "initialized", status.initialized);
  cJSON_AddBoolToObject(root, "config_valid", true);
  cJSON_AddBoolToObject(root, "initial_stabilizing", false);
  cJSON_AddBoolToObject(root, "fault", status.fault);
  cJSON_AddStringToObject(root, "float_state",
                          api_pump_float_state_name(status.water.float_state));
  cJSON_AddStringToObject(root, "active_timer",
                          api_pump_active_timer_name(status.water.active_set));
  cJSON_AddStringToObject(root, "active_relay", status.water.active_set == PUMP_CONTROL_SET_1 ? "relay1" : "relay2");
  cJSON_AddStringToObject(root, "phase",
                          api_pump_timer_phase_name(status.water.phase));
  cJSON_AddNumberToObject(root, "countdown_sec", status.water.countdown_sec);
  cJSON_AddNumberToObject(root, "min_dwell_sec", settings.min_dwell_sec);
  cJSON_AddNumberToObject(root, "cooldown_remaining_sec",
                          status.water.cooldown_remaining_sec);
  cJSON_AddBoolToObject(root, "relay_energized", status.water.relay_energized);
  cJSON_AddBoolToObject(root, "relay1_energized", status.water.relay_energized);
  cJSON_AddBoolToObject(root, "relay2_energized", status.wash.relay_energized);

  cJSON_AddNumberToObject(root, "float_gpio", active_map.float_input_gpio);
  cJSON_AddNumberToObject(root, "relay_gpio", active_map.pump_relay1_gpio);
  cJSON_AddNumberToObject(root, "active_relay_gpio", active_map.pump_relay1_gpio);
  cJSON_AddNumberToObject(root, "pump_relay1_gpio",
                          active_map.pump_relay1_gpio);
  cJSON_AddNumberToObject(root, "pump_relay2_gpio",
                          active_map.pump_relay2_gpio);
  cJSON_AddNumberToObject(root, "ds18b20_gpio", active_map.ds18b20_data_gpio);
  cJSON_AddNumberToObject(root, "cooling_relay_gpio",
                          active_map.cooling_relay_gpio);
  cJSON_AddStringToObject(
      root, "relay1_polarity",
      api_pump_relay_polarity_name(settings.relay1_active_low));
  cJSON_AddStringToObject(
      root, "relay2_polarity",
      api_pump_relay_polarity_name(settings.relay2_active_low));
  cJSON_AddStringToObject(
      root, "cooling_relay_polarity",
      hardware_map_polarity_name(cooling_settings.relay_polarity));
  cJSON_AddStringToObject(
      root, "timer1_start_phase",
      hardware_map_timer_start_phase_name(settings.timer1_start_phase));
  cJSON_AddStringToObject(
      root, "timer2_start_phase",
      hardware_map_timer_start_phase_name(settings.timer2_start_phase));
  cJSON_AddBoolToObject(root, "hardware_reboot_required",
                        hardware_reboot_status_ok && hardware_reboot_required);
  cJSON_AddStringToObject(root, "hardware_map_status",
                          api_hardware_map_status_name(map_status));
  cJSON_AddBoolToObject(root, "auto_start", settings.auto_start);

  cJSON_AddStringToObject(root, "settings_status",
                          api_pump_settings_status_name(settings_status));
  return true;
}

/* Appends "dry_run":{enabled + runtime fields} to a pump status document. */
static void dry_run_add_status_gen(json_gen_t *gen) {
  dry_run_guard_status_t gs;
  if (!dry_run_guard_get_status(&gs)) {
    return;
  }
  json_gen_add_key(gen, "dry_run");
  json_gen_append(gen, "{");
  gen->first_item = true;
  json_gen_add_bool(gen, "enabled", gs.cfg.enabled);
  json_gen_add_string(gen, "state", dry_run_state_name(gs.state));
  json_gen_add_number(gen, "trips", gs.trips);
  json_gen_add_number(gen, "max_retries", gs.cfg.max_retries);
  json_gen_add_number(gen, "wait_remaining_sec", (double)gs.wait_remaining_sec);
  json_gen_add_number(gen, "dry_accum_sec", (double)gs.dry_accum_sec);
  json_gen_end_object(gen);
}

static void dry_run_add_status_object(cJSON *root) {
  dry_run_guard_status_t gs;
  if (!dry_run_guard_get_status(&gs)) {
    return;
  }
  cJSON *obj = cJSON_AddObjectToObject(root, "dry_run");
  if (!obj) {
    return;
  }
  cJSON_AddBoolToObject(obj, "enabled", gs.cfg.enabled);
  dry_run_add_runtime_fields(obj, &gs);
}

static bool api_pump_add_status_object(cJSON *root) {
  cJSON *status_obj = cJSON_AddObjectToObject(root, "status");
  if (!status_obj) {
    return false;
  }
  if (!api_pump_add_status_fields(status_obj)) {
    return false;
  }
  dry_run_add_status_object(root);
  return true;
}

static bool api_pump_json_has_field(const cJSON *root, const char *name) {
  return cJSON_GetObjectItem(root, name) != NULL;
}

static const cJSON *api_pump_get_item(const cJSON *root, const char *key1, const char *key2) {
  return api_json_get_item(root, key1, key2);
}

static bool api_pump_parse_settings_payload(const cJSON *root,
                                            nvs_store_pump_settings_t *settings,
                                            const char **error_code,
                                            const char **error_message) {
  if (!root || !settings) {
    *error_code = "invalid_json";
    *error_message = "Expected a JSON object";
    return false;
  }

  const char *readonly_fields[] = {"float_gpio",
                                   "relay_gpio",
                                   "debounce_ms",
                                   "pump_relay1_gpio",
                                   "pump_relay2_gpio",
                                   "ds18b20_gpio",
                                   "cooling_relay_gpio",
                                   "cooling_relay_polarity",
                                   "hardware_reboot_required"};
  for (size_t i = 0; i < sizeof(readonly_fields) / sizeof(readonly_fields[0]);
       i++) {
    if (api_pump_json_has_field(root, readonly_fields[i])) {
      *error_code = "read_only_field";
      *error_message = readonly_fields[i];
      return false;
    }
  }

  const cJSON *t1_on = api_pump_get_item(root, "water_set1_on", "timer1_on_sec");
  const cJSON *t1_off = api_pump_get_item(root, "water_set1_off", "timer1_off_sec");
  const cJSON *t2_on = api_pump_get_item(root, "water_set2_on", "timer2_on_sec");
  const cJSON *t2_off = api_pump_get_item(root, "water_set2_off", "timer2_off_sec");
  const cJSON *auto_st = api_pump_get_item(root, "water_auto_start", "auto_start");

  if (!t1_on || !cJSON_IsNumber(t1_on) || t1_on->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC || t1_on->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC ||
      !t1_off || !cJSON_IsNumber(t1_off) || t1_off->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC || t1_off->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC ||
      !t2_on || !cJSON_IsNumber(t2_on) || t2_on->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC || t2_on->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC ||
      !t2_off || !cJSON_IsNumber(t2_off) || t2_off->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC || t2_off->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC ||
      !auto_st || !cJSON_IsBool(auto_st)) {
    *error_code = "missing_or_invalid_field";
    *error_message = "Required timer or auto_start fields missing or out of range";
    return false;
  }

  settings->timer1_on_sec = (uint32_t)t1_on->valuedouble;
  settings->timer1_off_sec = (uint32_t)t1_off->valuedouble;
  settings->timer2_on_sec = (uint32_t)t2_on->valuedouble;
  settings->timer2_off_sec = (uint32_t)t2_off->valuedouble;
  settings->auto_start = cJSON_IsTrue(auto_st);

  const cJSON *min_dwell = cJSON_GetObjectItem(root, "min_dwell_sec");
  if (min_dwell) {
    if (!cJSON_IsNumber(min_dwell) || min_dwell->valuedouble < 0.0) {
      *error_code = "invalid_type";
      *error_message = "min_dwell_sec";
      return false;
    }
    uint32_t md = (uint32_t)min_dwell->valuedouble;
    if ((double)md != min_dwell->valuedouble ||
        md > APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC) {
      *error_code = "min_dwell_out_of_range";
      *error_message = "min_dwell_sec";
      return false;
    }
    settings->min_dwell_sec = md;
  }

  const cJSON *relay1_polarity = cJSON_GetObjectItem(root, "relay1_polarity");
  const cJSON *legacy_polarity = cJSON_GetObjectItem(root, "relay_polarity");
  const cJSON *relay1_source =
      relay1_polarity ? relay1_polarity : legacy_polarity;
  if (!relay1_source) {
    *error_code = "missing_field";
    *error_message = "relay1_polarity";
    return false;
  }
  if (!api_pump_parse_relay_polarity(relay1_source,
                                     &settings->relay1_active_low)) {
    *error_code = "invalid_relay_polarity";
    *error_message = relay1_polarity ? "relay1_polarity" : "relay_polarity";
    return false;
  }
  settings->relay_active_low = settings->relay1_active_low;

  const cJSON *relay2_polarity = cJSON_GetObjectItem(root, "relay2_polarity");
  if (relay2_polarity && !api_pump_parse_relay_polarity(
                             relay2_polarity, &settings->relay2_active_low)) {
    *error_code = "invalid_relay_polarity";
    *error_message = "relay2_polarity";
    return false;
  }

  const cJSON *timer1_start = api_pump_get_item(root, "water_set1_start_phase", "timer1_start_phase");
  if (!timer1_start) {
    *error_code = "missing_field";
    *error_message = "timer1_start_phase";
    return false;
  }
  if (!api_pump_parse_start_phase(timer1_start,
                                  &settings->timer1_start_phase) ||
      !hardware_map_timer_start_phase_valid(settings->timer1_start_phase)) {
    *error_code = "invalid_start_phase";
    *error_message = "timer1_start_phase";
    return false;
  }

  const cJSON *timer2_start = api_pump_get_item(root, "water_set2_start_phase", "timer2_start_phase");
  if (!timer2_start) {
    *error_code = "missing_field";
    *error_message = "timer2_start_phase";
    return false;
  }
  if (!api_pump_parse_start_phase(timer2_start,
                                  &settings->timer2_start_phase) ||
      !hardware_map_timer_start_phase_valid(settings->timer2_start_phase)) {
    *error_code = "invalid_start_phase";
    *error_message = "timer2_start_phase";
    return false;
  }

  const cJSON *w_on = api_pump_get_item(root, "wash_on", "wash_on_sec");
  if (w_on) {
    if (!cJSON_IsNumber(w_on)) {
      *error_code = "invalid_type";
      *error_message = "wash_on";
      return false;
    }
    if (w_on->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC ||
        w_on->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC) {
      *error_code = "duration_out_of_range";
      *error_message = "wash_on";
      return false;
    }
    settings->wash_on_sec = (uint32_t)w_on->valuedouble;
  }

  const cJSON *w_off = api_pump_get_item(root, "wash_off", "wash_off_sec");
  if (w_off) {
    if (!cJSON_IsNumber(w_off)) {
      *error_code = "invalid_type";
      *error_message = "wash_off";
      return false;
    }
    if (w_off->valuedouble < APP_TEMPLATE_PUMP_TIMER_MIN_SEC ||
        w_off->valuedouble > APP_TEMPLATE_PUMP_TIMER_MAX_SEC) {
      *error_code = "duration_out_of_range";
      *error_message = "wash_off";
      return false;
    }
    settings->wash_off_sec = (uint32_t)w_off->valuedouble;
  }

  const cJSON *w_start = cJSON_GetObjectItem(root, "wash_start_phase");
  if (w_start) {
    hardware_timer_start_phase_t sp;
    if (!api_pump_parse_start_phase(w_start, &sp) || !hardware_map_timer_start_phase_valid(sp)) {
      *error_code = "invalid_start_phase";
      *error_message = "wash_start_phase";
      return false;
    }
    settings->wash_start_phase = sp;
  }

  const cJSON *w_auto = cJSON_GetObjectItem(root, "wash_auto_start");
  if (w_auto) {
    if (!cJSON_IsBool(w_auto)) {
      *error_code = "invalid_type";
      *error_message = "wash_auto_start";
      return false;
    }
    settings->wash_auto_start = cJSON_IsTrue(w_auto);
  }

  return true;
}

/* Water start is refused while the dry-run guard waits or is locked, and while the measure test
 * holds relay 1. Builds the 409 body and returns true when blocked. Never touches the pump (stopping
 * it would erase the user's running intent). A guard that is not initialised does not block. */
static bool pump_start_blocked(cJSON **out, dapi_status_t *st) {
  dry_run_guard_status_t gs;
  if (dry_run_guard_get_status(&gs)) {
    if (gs.state == DRY_RUN_WAIT) {
      cJSON *root = cJSON_CreateObject();
      if (!root) {
        *out = dapi_error("memory", "JSON allocation failed");
        *st = DAPI_INTERNAL;
        return true;
      }
      cJSON_AddBoolToObject(root, "ok", false);
      cJSON_AddStringToObject(root, "error", "dry_run_waiting");
      cJSON_AddStringToObject(root, "message", "Water pump is waiting to retry after a dry run");
      cJSON_AddNumberToObject(root, "wait_remaining_sec", gs.wait_remaining_sec);
      *out = root;
      *st = DAPI_CONFLICT;
      return true;
    }
    if (gs.state == DRY_RUN_LOCKED) {
      *out = dapi_error("dry_run_locked",
                        "Water pump is locked after repeated dry runs; acknowledge the alarm");
      *st = DAPI_CONFLICT;
      return true;
    }
  }
  dry_run_measure_status_t ms;
  if (dry_run_guard_get_measure(&ms) && ms.state == DRY_RUN_MEASURE_RUNNING) {
    *out = dapi_error("measuring", "Flow-time measurement is running");
    *st = DAPI_CONFLICT;
    return true;
  }
  return false;
}

/* {"ok":false,"error","message","status":{pump status},"dry_run":{...}} - the errors that
 * carry the pump status. */
static dapi_status_t pump_error_with_status(cJSON **out, dapi_status_t st, const char *code,
                                            const char *message) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = dapi_error("memory", "JSON allocation failed");
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", code);
  cJSON_AddStringToObject(root, "message", message);
  api_pump_add_status_object(root);
  *out = root;
  return st;
}

static dapi_status_t pump_status_unavailable(cJSON **out) {
  *out = dapi_error("status_unavailable", "Pump status is unavailable");
  return DAPI_CONFLICT;
}

/* {"ok":true, <flag if any>, "status":{...}, "dry_run":{...}} */
static dapi_status_t pump_ok_with_status(cJSON **out, const char *flag, bool flag_value) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  if (flag) {
    cJSON_AddBoolToObject(root, flag, flag_value);
  }
  if (!api_pump_add_status_object(root)) {
    cJSON_Delete(root);
    return pump_status_unavailable(out);
  }
  *out = root;
  return DAPI_OK;
}

/* pump.status - the json_gen document, wrapped raw so the bytes are unchanged. */
static dapi_status_t dapi_pump_status(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  char *json_buf = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!json_buf) {
    json_buf = malloc(3072);
  }
  if (!json_buf) {
    *out = NULL;
    return DAPI_INTERNAL;
  }

  json_gen_t gen;
  json_gen_init(&gen, json_buf, 3072);
  json_gen_start_object(&gen);
  json_gen_add_bool(&gen, "ok", true);

  if (!api_pump_add_status_fields_gen(&gen)) {
    free(json_buf);
    return pump_status_unavailable(out);
  }
  dry_run_add_status_gen(&gen);
  json_gen_end_object(&gen);

  *out = cJSON_CreateRaw(json_buf);
  free(json_buf);
  return *out ? DAPI_OK : DAPI_INTERNAL;
}

/* {"ok":true, <config fields>[, "status":{...}]} */
static dapi_status_t pump_config_reply(cJSON **out, const nvs_store_pump_settings_t *settings,
                                       nvs_store_pump_settings_load_status_t status,
                                       bool include_status) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    *out = NULL;
    return DAPI_INTERNAL;
  }

  cJSON_AddBoolToObject(root, "ok", true);
  api_pump_add_config_fields(root, settings, status);
  if (include_status) {
    api_pump_add_status_object(root);
  }
  *out = root;
  return DAPI_OK;
}

/* pump.config.get */
static dapi_status_t dapi_pump_config_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  nvs_store_pump_settings_t settings;
  nvs_store_pump_settings_load_status_t status = nvs_store_load_pump_settings(&settings);
  if (status == NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR) {
    *out = dapi_error("settings_load_failed", "Could not load pump settings");
    return DAPI_INTERNAL;
  }
  return pump_config_reply(out, &settings, status, false);
}

/* pump.config.set - full replacement of the editable pump settings */
static dapi_status_t dapi_pump_config_set(const cJSON *params, dapi_source_t src, cJSON **out) {
  nvs_store_pump_settings_t settings;
  nvs_store_pump_settings_defaults(&settings);
  nvs_store_load_pump_settings(&settings);

  /* ThingsBoard cannot flip a relay polarity blind: it may only re-send the stored value. */
  if (src == DAPI_SRC_TB) {
    const char *r1_stored = api_pump_relay_polarity_name(settings.relay1_active_low);
    const char *r2_stored = api_pump_relay_polarity_name(settings.relay2_active_low);
    const struct {
      const char *key;
      const char *stored;
    } polarity_keys[] = {{"relay1_polarity", r1_stored}, {"relay_polarity", r1_stored}, {"relay2_polarity", r2_stored}};
    for (size_t i = 0; i < sizeof(polarity_keys) / sizeof(polarity_keys[0]); i++) {
      const cJSON *item = cJSON_GetObjectItem(params, polarity_keys[i].key);
      const char *requested = (item && cJSON_IsString(item)) ? item->valuestring : NULL;
      if (dapi_polarity_write_blocked(src, requested, polarity_keys[i].stored)) {
        *out = dapi_error("read_only_field", polarity_keys[i].key);
        return DAPI_BAD_REQUEST;
      }
    }
  }
  const char *error_code = NULL;
  const char *error_message = NULL;
  bool valid = api_pump_parse_settings_payload(params, &settings, &error_code, &error_message);
  if (!valid) {
    *out = dapi_error(error_code, error_message);
    return DAPI_BAD_REQUEST;
  }

  pump_control_config_update_t update = {
      .water_set1 =
          {
              .on_sec = settings.timer1_on_sec,
              .off_sec = settings.timer1_off_sec,
              .start_phase = (settings.timer1_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_set2 =
          {
              .on_sec = settings.timer2_on_sec,
              .off_sec = settings.timer2_off_sec,
              .start_phase = (settings.timer2_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .wash_timer =
          {
              .on_sec = settings.wash_on_sec,
              .off_sec = settings.wash_off_sec,
              .start_phase = (settings.wash_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_auto_start = settings.auto_start,
      .wash_auto_start = settings.wash_auto_start,
      .min_dwell_sec = settings.min_dwell_sec,
  };

  if (!nvs_store_save_pump_settings(&settings)) {
    *out = dapi_error("settings_save_failed", "Could not save pump settings");
    return DAPI_INTERNAL;
  }

  if (!pump_control_update_config(&update)) {
    *out = dapi_error("settings_apply_failed", "Could not apply pump settings to runtime");
    return DAPI_INTERNAL;
  }

  return pump_config_reply(out, &settings, NVS_STORE_PUMP_SETTINGS_LOADED, true);
}

/* pump.control {"system":"water"|"wash"|"all","action":"start"|"stop"}; both optional
 * (defaults water/start), as in the old handler. */
static dapi_status_t dapi_pump_control(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  char system_buf[32] = "water";
  char action_buf[32] = "start";

  const cJSON *sys_item = cJSON_GetObjectItem(params, "system");
  if (sys_item && cJSON_IsString(sys_item) && sys_item->valuestring) {
    strlcpy(system_buf, sys_item->valuestring, sizeof(system_buf));
  }
  const cJSON *act_item = cJSON_GetObjectItem(params, "action");
  if (act_item && cJSON_IsString(act_item) && act_item->valuestring) {
    strlcpy(action_buf, act_item->valuestring, sizeof(action_buf));
  }

  bool is_start = (strcmp(action_buf, "start") == 0);
  bool is_stop = (strcmp(action_buf, "stop") == 0);
  if (!is_start && !is_stop) {
    *out = dapi_error("invalid_action", "Action must be 'start' or 'stop'");
    return DAPI_BAD_REQUEST;
  }

  bool do_water = (strcmp(system_buf, "water") == 0 || strcmp(system_buf, "all") == 0);
  bool do_wash = (strcmp(system_buf, "wash") == 0 || strcmp(system_buf, "all") == 0);

  if (!do_water && !do_wash) {
    *out = dapi_error("invalid_system", "System must be 'water', 'wash', or 'all'");
    return DAPI_BAD_REQUEST;
  }

  /* resume_after_min is only valid with a water stop. Reject it before the dry-run
   * guard and before any pump command: a request with the default action=start must
   * not touch the pump. */
  const cJSON *resume_item = cJSON_GetObjectItem(params, "resume_after_min");
  bool resume_has_field = resume_item != NULL;
  bool resume_is_number = cJSON_IsNumber(resume_item);
  double resume_value = resume_is_number ? resume_item->valuedouble : 0.0;
  uint16_t resume_minutes = 0;
  pump_resume_param_t resume_param = pump_resume_check_param(
      resume_has_field, resume_is_number, resume_value, is_stop, do_water,
      &resume_minutes);
  if (resume_param == PUMP_RESUME_PARAM_BAD_COMBO) {
    *out = dapi_error(
        "invalid_resume",
        "resume_after_min needs action stop and system water or all");
    return DAPI_BAD_REQUEST;
  }
  if (resume_param == PUMP_RESUME_PARAM_OUT_OF_RANGE) {
    *out = dapi_error("value_out_of_range", "resume_after_min");
    return DAPI_BAD_REQUEST;
  }

  if (is_start && do_water) {
    dapi_status_t blocked_st = DAPI_CONFLICT;
    if (pump_start_blocked(out, &blocked_st)) {
      return blocked_st;
    }
  }

  bool success = true;
  bool water_ok = false;
  if (do_water) {
    pump_resume_service_cancel();
    if (is_start) {
      water_ok = pump_control_start_water();
    } else {
      water_ok = pump_control_stop_water();
    }
    if (!water_ok) success = false;
  }

  if (do_wash) {
    if (is_start) {
      if (!pump_control_start_wash()) success = false;
    } else {
      if (!pump_control_stop_wash()) success = false;
    }
  }

  if (!success) {
    return pump_error_with_status(out, DAPI_INTERNAL, "control_failed", "Pump control action failed");
  }

  if (resume_param == PUMP_RESUME_PARAM_OK && is_stop && water_ok) {
    pump_resume_service_arm(resume_minutes);
  }

  return pump_ok_with_status(out, NULL, false);
}

/* pump.start - idempotent water pump start */
static dapi_status_t dapi_pump_start(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  pump_resume_service_cancel();
  pump_control_status_t before_status = {0};
  if (!pump_control_get_status(&before_status)) {
    return pump_status_unavailable(out);
  }

  dapi_status_t blocked_st = DAPI_CONFLICT;
  if (pump_start_blocked(out, &blocked_st)) {
    return blocked_st;
  }

  bool already_running = before_status.water.running;
  if (!already_running && !pump_control_start_water()) {
    pump_control_stop_water();
    return pump_error_with_status(out, DAPI_INTERNAL, "start_failed",
                                  "Pump control could not be started");
  }

  return pump_ok_with_status(out, "already_running", already_running);
}

/* pump.stop - idempotent water pump stop */
static dapi_status_t dapi_pump_stop(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  pump_resume_service_cancel();
  pump_control_status_t before_status = {0};
  if (!pump_control_get_status(&before_status)) {
    return pump_status_unavailable(out);
  }

  bool already_stopped = !before_status.water.running;
  if (!already_stopped || before_status.water.relay_energized) {
    if (!pump_control_stop_water()) {
      return pump_error_with_status(out, DAPI_INTERNAL, "stop_failed",
                                    "Pump control could not be stopped");
    }
  }

  pump_control_status_t after_status = {0};
  if (!pump_control_get_status(&after_status) || after_status.water.running ||
      after_status.water.relay_energized) {
    return pump_error_with_status(out, DAPI_CONFLICT, "stop_failed",
                                  "Pump relay inactive state could not be confirmed");
  }

  return pump_ok_with_status(out, "already_stopped", already_stopped);
}

static const dapi_method_t s_pump_methods[] = {
    {"pump.status", DAPI_F_READ, 0, dapi_pump_status},
    {"pump.config.get", DAPI_F_READ, 0, dapi_pump_config_get},
    {"pump.config.set", DAPI_F_WRITE, PUMP_CONFIG_BODY_BYTES, dapi_pump_config_set},
    {"pump.control", DAPI_F_WRITE, PUMP_CONTROL_BODY_BYTES, dapi_pump_control},
    {"pump.start", DAPI_F_WRITE, 0, dapi_pump_start},
    {"pump.stop", DAPI_F_WRITE, 0, dapi_pump_stop},
};

void dapi_pump_register(void) {
  (void)device_api_register(s_pump_methods,
                            sizeof(s_pump_methods) / sizeof(s_pump_methods[0]));
}
