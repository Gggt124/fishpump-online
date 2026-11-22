#include "dapi_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_json.h"
#include "app_config.h"
#include "ota_key.h"
#include "app_main.h"
#include "dapi_pump.h"
#include "device_api.h"
#include "dns_server.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "flow_switch.h"
#include "nvs_store.h"
#include "power_monitor.h"
#include "sdkconfig.h"
#include "telemetry_hub.h"
#include "uplink.h"
#include "water_quality.h"
#include "web_server.h"
#include "wifi_manager.h"

/* Status, sensors and uplink-preview methods: the code that used to run inside
 * the /api/status, /api/sensors/status and /api/uplink/preview handlers. Replies
 * keep the exact bytes of the old handlers. */

static void json_gen_add_string_safe(json_gen_t *gen, const char *key,
                                     const char *val) {
  if (!val || val[0] == '\0') {
    json_gen_add_string(gen, key, val);
  } else if (is_valid_utf8_printable(val, 128)) {
    json_gen_add_string(gen, key, val);
  } else {
    json_gen_add_string(gen, key, NULL);
  }
}

/* Convert chip model enum to string */
static const char *chip_model_to_string(esp_chip_model_t model) {
  switch (model) {
  case CHIP_ESP32:
    return "ESP32";
  case CHIP_ESP32S2:
    return "ESP32-S2";
  case CHIP_ESP32S3:
    return "ESP32-S3";
  case CHIP_ESP32C2:
    return "ESP32-C2";
  case CHIP_ESP32C3:
    return "ESP32-C3";
  case CHIP_ESP32C5:
    return "ESP32-C5";
  case CHIP_ESP32C6:
    return "ESP32-C6";
  case CHIP_ESP32H2:
    return "ESP32-H2";
  case CHIP_ESP32P4:
    return "ESP32-P4";
  default:
    return "Unknown";
  }
}

static const char *reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power_on";
  case ESP_RST_EXT:
    return "external_reset";
  case ESP_RST_SW:
    return "software_reset";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt_watchdog";
  case ESP_RST_TASK_WDT:
    return "task_watchdog";
  case ESP_RST_WDT:
    return "other_watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep_sleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  case ESP_RST_UNKNOWN:
  default:
    return "unknown";
  }
}

/* Format MAC address bytes to XX:XX:XX:XX:XX:XX string */
static void format_mac(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

static void sensors_add_age(cJSON *obj, uint32_t last_ok_ms) {
  if (last_ok_ms == 0) {
    cJSON_AddNullToObject(obj, "age_ms");
  } else {
    cJSON_AddNumberToObject(obj, "age_ms", (double)(uint32_t)(sensors_now_ms() - last_ok_ms));
  }
}

static void sensors_add_notice(cJSON *parent, const char *key, const char *text, bool ok) {
  cJSON *n = cJSON_AddObjectToObject(parent, key);
  if (n) {
    cJSON_AddStringToObject(n, "text", text ? text : "");
    cJSON_AddBoolToObject(n, "ok", ok);
  }
}


/* sensors.get - full sensor state for the Sensors page. The big structs live on the heap. */
static dapi_status_t dapi_sensors_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  power_monitor_status_t *pw = calloc(1, sizeof(*pw));
  water_quality_status_t *wq = calloc(1, sizeof(*wq));
  telemetry_snapshot_t *snap = calloc(1, sizeof(*snap));
  cJSON *root = cJSON_CreateObject();
  if (!pw || !wq || !snap || !root) {
    free(pw);
    free(wq);
    free(snap);
    cJSON_Delete(root);
    *out = NULL;
    return DAPI_INTERNAL;
  }
  bool pw_ok = power_monitor_get_status(pw);
  bool wq_ok = water_quality_get_status(wq);

  cJSON_AddBoolToObject(root, "ok", true);

  cJSON *power = cJSON_AddArrayToObject(root, "power");
  for (int ch = 0; power && ch < POWER_MONITOR_CHANNELS; ch++) {
    const power_monitor_channel_t *c = &pw->ch[ch];
    cJSON *o = cJSON_CreateObject();
    if (!o) {
      break;
    }
    cJSON_AddNumberToObject(o, "channel", ch);
    cJSON_AddStringToObject(o, "name", ch == 0 ? "pump" : "chiller");
    cJSON_AddBoolToObject(o, "valid", pw_ok && c->valid);
    cJSON_AddBoolToObject(o, "fault", !pw_ok || c->fault);
    cJSON_AddNumberToObject(o, "voltage", c->voltage_v);
    cJSON_AddNumberToObject(o, "current", c->current_a);
    cJSON_AddNumberToObject(o, "power", c->power_w);
    cJSON_AddNumberToObject(o, "energy_wh", c->energy_wh);
    cJSON_AddNumberToObject(o, "frequency", c->frequency_hz);
    cJSON_AddNumberToObject(o, "pf", c->pf);
    sensors_add_age(o, c->last_ok_ms);
    cJSON_AddItemToArray(power, o);
  }
  sensors_add_notice(root, "power_notice", pw->notice, pw->notice_ok);

  cJSON *water = cJSON_AddObjectToObject(root, "water");
  if (water) {
    cJSON_AddBoolToObject(water, "valid", wq_ok && wq->valid);
    cJSON_AddBoolToObject(water, "fault", !wq_ok || wq->fault);
    cJSON_AddBoolToObject(water, "stable", wq_ok && wq->stable);
    cJSON_AddNumberToObject(water, "ph", wq->ph);
    cJSON_AddNumberToObject(water, "temperature_c", wq->temperature_c);
    cJSON *off = cJSON_AddObjectToObject(water, "offsets");
    if (off) {
      cJSON_AddBoolToObject(off, "known", wq->offsets_known);
      cJSON_AddNumberToObject(off, "ph", wq->ph_offset);
      cJSON_AddNumberToObject(off, "temp", wq->temp_offset);
    }
    cJSON *cal = cJSON_AddObjectToObject(water, "calibration");
    if (cal) {
      cJSON_AddStringToObject(cal, "state", wq->cal_state ? wq->cal_state : "idle");
      if (wq->cal_error) {
        cJSON_AddStringToObject(cal, "error", wq->cal_error);
      } else {
        cJSON_AddNullToObject(cal, "error");
      }
    }
    sensors_add_notice(water, "notice", wq->notice, wq->notice_ok);
    sensors_add_age(water, wq->last_ok_ms);
  }
  free(pw);
  free(wq);

  /* Flow switch: no fault state (a bare contact cannot report one). */
  cJSON *flow = cJSON_AddObjectToObject(root, "flow");
  if (flow) {
    flow_switch_status_t fs;
    bool fs_ok = flow_switch_get_status(&fs);
    bool snap_ok = telemetry_hub_get_latest(snap) == ESP_OK && snap->has_flow;
    cJSON_AddBoolToObject(flow, "on", fs_ok ? fs.flow_on : (snap_ok && snap->flow.flow_on));
    if (snap_ok && snap->flow_updated_ms != 0) {
      cJSON_AddNumberToObject(flow, "age_ms", (double)(uint32_t)(sensors_now_ms() - snap->flow_updated_ms));
    } else {
      cJSON_AddNullToObject(flow, "age_ms");
    }
  }
  free(snap);

  *out = root;
  return DAPI_OK;
}

/* uplink.preview - what the uplink would send to ThingsBoard, wrapped raw (it is already JSON text). */
static dapi_status_t dapi_uplink_preview(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  char *json = uplink_preview_json();
  if (!json) {
    *out = dapi_error("memory", "Uplink preview unavailable");
    return DAPI_UNAVAILABLE;
  }
  *out = cJSON_CreateRaw(json);
  free(json);
  return *out ? DAPI_OK : DAPI_INTERNAL;
}

/* status.get - the json_gen document, built on the heap and wrapped raw so the bytes are unchanged. */
static dapi_status_t dapi_status_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  bool sta_conn = wifi_manager_is_sta_connected();
  bool ap_up = wifi_manager_is_ap_enabled();

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

  /* ---------- System ---------- */
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  json_gen_add_string_safe(&gen, "chip_model", chip_model_to_string(chip.model));
  json_gen_add_string(&gen, "board_name", APP_TEMPLATE_BOARD_NAME);
  json_gen_add_string(&gen, "board_target", APP_TEMPLATE_BOARD_TARGET);
  json_gen_add_number(&gen, "chip_revision", (double)(chip.revision));
  json_gen_add_number(&gen, "chip_cores", (double)(chip.cores));
  json_gen_add_number(&gen, "cpu_freq_mhz", (double)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
  json_gen_add_string(&gen, "idf_version", esp_get_idf_version());
  json_gen_add_string(&gen, "project_name", APP_TEMPLATE_NAME);
  json_gen_add_string(&gen, "project_version", APP_TEMPLATE_FIRMWARE_VERSION);
  char key_fp[OTA_KEY_FP_LEN + 1] = "";
  ota_key_fingerprint(APP_CONFIG_OTA_ENCRYPTION_KEY, key_fp);
  json_gen_add_string(&gen, "ota_key_fp", key_fp);
  json_gen_add_bool(&gen, "ota_key_dev", ota_key_is_dev(APP_CONFIG_OTA_ENCRYPTION_KEY));
  json_gen_add_string(&gen, "reset_reason", reset_reason_to_string(esp_reset_reason()));

  const char *rst_detail = app_get_last_restart_detail();
  if (rst_detail && rst_detail[0]) {
    json_gen_add_string_safe(&gen, "reset_detail", rst_detail);
  }

  /* ---------- MAC addresses ---------- */
  uint8_t mac[6];
  char mac_str[18];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  format_mac(mac, mac_str);
  json_gen_add_string(&gen, "mac_sta", mac_str);
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  format_mac(mac, mac_str);
  json_gen_add_string(&gen, "mac_ap", mac_str);

  /* ---------- Memory (internal DRAM, same capability for all metrics) ---------- */
  uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  uint32_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  json_gen_add_number(&gen, "free_heap", (double)(free_heap));
  json_gen_add_number(&gen, "min_free_heap", (double)(min_free_heap));
  json_gen_add_number(&gen, "total_heap", (double)(total_heap));
  json_gen_add_number(&gen, "largest_free_block", (double)(largest_free_block));

  uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  uint32_t psram_free = psram_total > 0 ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
  json_gen_add_number(&gen, "psram_total", (double)(psram_total));
  json_gen_add_number(&gen, "psram_free", (double)(psram_free));

  /* ---------- Uptime ---------- */
  json_gen_add_number(&gen, "uptime_ms", (double)(wifi_manager_get_uptime_ms()));

  /* ---------- Wi-Fi ---------- */
  wifi_mode_t wmode;
  esp_wifi_get_mode(&wmode);
  const char *wmode_str = "NULL";
  if (wmode == WIFI_MODE_STA)
    wmode_str = "STA";
  else if (wmode == WIFI_MODE_AP)
    wmode_str = "AP";
  else if (wmode == WIFI_MODE_APSTA)
    wmode_str = "APSTA";
  json_gen_add_string(&gen, "wifi_mode", wmode_str);

  /* AP info */
  json_gen_add_bool(&gen, "ap_enabled", ap_up);
  json_gen_add_string(&gen, "ap_ssid", ap_up ? AP_SSID : "");
  json_gen_add_string(&gen, "ap_ip", ap_up ? wifi_manager_get_ap_ip() : "");

  wifi_sta_list_t sta_list;
  if (ap_up && esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
    json_gen_add_number(&gen, "ap_clients", (double)(sta_list.num));
    if (sta_list.num > 0) {
      int8_t weakest_rssi = sta_list.sta[0].rssi;
      for (int i = 1; i < sta_list.num; i++) {
        if (sta_list.sta[i].rssi < weakest_rssi) {
          weakest_rssi = sta_list.sta[i].rssi;
        }
      }
      json_gen_add_number(&gen, "ap_client_weakest_rssi", (double)(weakest_rssi));
    }
  } else {
    json_gen_add_number(&gen, "ap_clients", (double)(0));
  }

  /* STA info */
  json_gen_add_bool(&gen, "sta_connected", sta_conn);
  json_gen_add_bool(&gen, "sta_connecting", wifi_manager_is_sta_connecting());
  json_gen_add_bool(&gen, "sta_retry_blocked", wifi_manager_is_sta_retry_blocked());
  json_gen_add_string(&gen, "sta_ip", sta_conn ? wifi_manager_get_sta_ip() : "");
  json_gen_add_string_safe(&gen, "sta_ssid", sta_conn ? wifi_manager_get_sta_ssid() : "");

  if (sta_conn) {
    wifi_ap_record_t ap_rec;
    if (esp_wifi_sta_get_ap_info(&ap_rec) == ESP_OK) {
      json_gen_add_number(&gen, "sta_rssi", (double)(ap_rec.rssi));
      json_gen_add_number(&gen, "sta_channel", (double)(ap_rec.primary));
      json_gen_add_string(&gen, "sta_auth", wifi_auth_mode_to_string(ap_rec.authmode));
    }
  }

  /* ---------- Services ---------- */
  web_server_http_counters_t http;
  web_server_get_http_counters(&http);
  json_gen_add_bool(&gen, "dns_server", dns_server_is_running());
  json_gen_add_number(&gen, "http_json_send_failures", (double)(http.json_send_failures));
  json_gen_add_number(&gen, "http_static_send_failures", (double)(http.static_send_failures));
  json_gen_add_number(&gen, "http_static_cache_hits", (double)(http.static_cache_hits));
  json_gen_add_number(&gen, "http_static_deadline_aborts", (double)(http.static_deadline_aborts));

  uint8_t stg_type = 0;
  nvs_store_get_staging_type(&stg_type);
  json_gen_add_number(&gen, "stg_type", (double)(stg_type));

  /* ---------- Pump ---------- */
  api_pump_add_status_fields_gen(&gen);

  json_gen_end_object(&gen);

  *out = cJSON_CreateRaw(json_buf);
  free(json_buf);
  return *out ? DAPI_OK : DAPI_INTERNAL;
}

static const dapi_method_t s_status_methods[] = {
    {"status.get", DAPI_F_READ, 0, dapi_status_get},
    {"sensors.get", DAPI_F_READ, 0, dapi_sensors_get},
    {"uplink.preview", DAPI_F_READ, 0, dapi_uplink_preview},
};

void dapi_status_register(void) {
  (void)device_api_register(s_status_methods, sizeof(s_status_methods) / sizeof(s_status_methods[0]));
}
