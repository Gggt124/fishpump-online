#include "dapi_wifi.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "api_json.h"
#include "dapi_guards.h"
#include "device_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_store.h"
#include "wifi_manager.h"

/* Wi-Fi methods: the code that used to run inside the /api/wifi/ handlers after
 * the request body was parsed. Replies keep the exact status and bytes of the
 * old handlers, including the ones that answer 200 with ok:false.
 * wifi.connect and wifi.disconnect are local-only: they can cut the link the
 * remote caller is using. */

static const char *TAG = "dapi_wifi";

#define WIFI_DISCONNECT_TASK_STACK_BYTES 2048
#define WIFI_DISCONNECT_DELAY_MS 500

/* Scan results are cached so a burst of polls does not hammer the radio. */
#define SCAN_CACHE_TTL_MS 10000u  /* 10 s - reuse previous results   */
#define SCAN_CACHE_JSON_MAX 4096u /* max bytes of cached JSON string  */

static bool s_wifi_disconnect_task_pending = false;
static portMUX_TYPE s_wifi_disconnect_task_lock = portMUX_INITIALIZER_UNLOCKED;

/* wifi.scan runs outside the device_api lock (it waits on the radio for seconds):
 * the in-progress flag is a test-and-set under a spinlock, and the cache has its own mutex. */
static bool s_scan_in_progress = false;
static portMUX_TYPE s_scan_flag_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_scan_cache_mutex = NULL;
static uint32_t s_scan_cache_ts_ms = 0; /* 0 = cache empty       */
static char s_scan_cache_json[SCAN_CACHE_JSON_MAX] = {0};

/* Scan context for the non-blocking callback */
typedef struct {
  SemaphoreHandle_t sem;
  wifi_scan_entry_t entries[WIFI_SCAN_MAX];
  int count;
} wifi_scan_ctx_t;

static void scan_complete_cb(void *user_ctx, wifi_scan_entry_t *entries, int count) {
  wifi_scan_ctx_t *ctx = (wifi_scan_ctx_t *)user_ctx;
  ctx->count = (count > WIFI_SCAN_MAX) ? WIFI_SCAN_MAX : count;
  if (ctx->count > 0) {
    memcpy(ctx->entries, entries, ctx->count * sizeof(wifi_scan_entry_t));
  }
  xSemaphoreGive(ctx->sem);
}

/* The saved-profile table is over 256 B: it lives on the heap, not the task stack. */
static wifi_profile_t *profiles_alloc(void) {
  return heap_caps_calloc(WIFI_PROFILE_MAX, sizeof(wifi_profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* A fixed reply text, printed as is. */
static cJSON *raw(const char *text) { return cJSON_CreateRaw(text); }

/* True when this caller now owns the scan; false when one is already running. */
static bool scan_try_begin(void) {
  taskENTER_CRITICAL(&s_scan_flag_lock);
  bool busy = s_scan_in_progress;
  if (!busy) {
    s_scan_in_progress = true;
  }
  taskEXIT_CRITICAL(&s_scan_flag_lock);
  return !busy;
}

static void scan_end(void) {
  taskENTER_CRITICAL(&s_scan_flag_lock);
  s_scan_in_progress = false;
  taskEXIT_CRITICAL(&s_scan_flag_lock);
}

/* Creates the cache mutex once; false if it cannot be created. */
static bool scan_cache_ready(void) {
  if (!s_scan_cache_mutex) {
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    taskENTER_CRITICAL(&s_scan_flag_lock);
    if (!s_scan_cache_mutex) {
      s_scan_cache_mutex = m;
      m = NULL;
    }
    taskEXIT_CRITICAL(&s_scan_flag_lock);
    if (m) {
      vSemaphoreDelete(m);
    }
  }
  return s_scan_cache_mutex != NULL;
}

/* Cached reply as a raw item, or NULL when the cache is empty or stale. */
static cJSON *scan_cache_get(void) {
  cJSON *hit = NULL;
  xSemaphoreTake(s_scan_cache_mutex, portMAX_DELAY);
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  if (s_scan_cache_ts_ms != 0 && (now_ms - s_scan_cache_ts_ms) < SCAN_CACHE_TTL_MS && s_scan_cache_json[0] != '\0') {
    ESP_LOGI(TAG, "[SCAN] Serving cached results (age=%lums)", (unsigned long)(now_ms - s_scan_cache_ts_ms));
    hit = cJSON_CreateRaw(s_scan_cache_json);
  }
  xSemaphoreGive(s_scan_cache_mutex);
  return hit;
}

static void scan_cache_put(const char *json_str) {
  xSemaphoreTake(s_scan_cache_mutex, portMAX_DELAY);
  if (strlen(json_str) < SCAN_CACHE_JSON_MAX) {
    strlcpy(s_scan_cache_json, json_str, SCAN_CACHE_JSON_MAX);
    s_scan_cache_ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "[SCAN] Results cached (%zu bytes, TTL=%dms)", strlen(json_str), SCAN_CACHE_TTL_MS);
  } else {
    ESP_LOGW(TAG, "[SCAN] Result too large to cache (%zu bytes)", strlen(json_str));
  }
  xSemaphoreGive(s_scan_cache_mutex);
}

/* wifi.scan: returns the visible networks (cached for 10 s). */
static dapi_status_t dapi_wifi_scan(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  if (!scan_cache_ready()) {
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }

  /* Cache check: serve previous results if still fresh */
  cJSON *cached = scan_cache_get();
  if (cached) {
    *out = cached;
    return DAPI_OK;
  }

  /* In-progress guard: reject a concurrent scan immediately */
  if (!scan_try_begin()) {
    ESP_LOGW(TAG, "[SCAN] Scan already in progress - returning busy");
    *out = raw("{\"ok\":false,\"error\":\"scan_busy\"}");
    return DAPI_UNAVAILABLE;
  }

  wifi_scan_ctx_t *ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ctx) {
    scan_end();
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }
  ctx->sem = xSemaphoreCreateBinary();
  ctx->count = 0;
  if (!ctx->sem) {
    free(ctx);
    scan_end();
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }

  if (!wifi_manager_scan(ctx, scan_complete_cb)) {
    vSemaphoreDelete(ctx->sem);
    free(ctx);
    scan_end();
    *out = raw("{\"ok\":false,\"error\":\"scan_failed\"}");
    return DAPI_OK;
  }

  if (xSemaphoreTake(ctx->sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
    wifi_manager_cancel_scan();
    vSemaphoreDelete(ctx->sem);
    free(ctx);
    scan_end();
    *out = raw("{\"ok\":false,\"error\":\"scan_timeout\"}");
    return DAPI_OK;
  }
  vSemaphoreDelete(ctx->sem);

  bool sta_connected = wifi_manager_is_sta_connected();
  char connected_ssid[33] = {0};
  if (sta_connected) {
    strlcpy(connected_ssid, wifi_manager_get_sta_ssid(), sizeof(connected_ssid));
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON *arr = cJSON_AddArrayToObject(root, "networks");
  for (int i = 0; i < ctx->count; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", ctx->entries[i].ssid);
    cJSON_AddNumberToObject(item, "rssi", ctx->entries[i].rssi);
    cJSON_AddStringToObject(item, "auth", ctx->entries[i].auth);
    cJSON_AddNumberToObject(item, "channel", ctx->entries[i].channel);
    cJSON_AddBoolToObject(item, "connected", sta_connected && strcmp(ctx->entries[i].ssid, connected_ssid) == 0);
    cJSON_AddItemToArray(arr, item);
  }
  free(ctx);

  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str) {
    cJSON_Delete(root);
    scan_end();
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }

  scan_cache_put(json_str);
  scan_end();
  free(json_str);

  *out = root;
  return DAPI_OK;
}

/* wifi.profiles.get: the saved credential profiles (no passwords). */
static dapi_status_t dapi_wifi_profiles_get(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  wifi_profile_t *profiles = profiles_alloc();
  if (!profiles) {
    return DAPI_INTERNAL; /* *out stays NULL: device_api answers the memory error */
  }
  int count = 0, auto_idx = -1;
  nvs_store_load_wifi_profiles(profiles, &count, &auto_idx);

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    free(profiles);
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "auto_index", auto_idx);
  cJSON *arr = cJSON_AddArrayToObject(root, "profiles");
  for (int i = 0; i < count; i++) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", profiles[i].ssid);
    cJSON_AddBoolToObject(item, "auto", i == auto_idx);
    cJSON_AddBoolToObject(item, "connected",
                          wifi_manager_is_sta_connected() &&
                              strcmp(wifi_manager_get_sta_ssid(), profiles[i].ssid) == 0);
    cJSON_AddItemToArray(arr, item);
  }
  free(profiles);
  *out = root;
  return DAPI_OK;
}

/* {"ssid":"..","password":".."} - persist a credential after a confirmed connect */
static dapi_status_t dapi_wifi_profiles_save(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  char ssid[33] = {0}, password[65] = {0};
  const cJSON *s = cJSON_GetObjectItem(params, "ssid");
  const cJSON *p = cJSON_GetObjectItem(params, "password");
  if (s && cJSON_IsString(s)) {
    if (strlen(s->valuestring) >= sizeof(ssid)) {
      *out = dapi_error("invalid_ssid", "SSID is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(ssid, s->valuestring, sizeof(ssid));
  }
  if (p && cJSON_IsString(p)) {
    if (strlen(p->valuestring) >= sizeof(password)) {
      *out = dapi_error("invalid_password", "Password is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(password, p->valuestring, sizeof(password));
  }
  if (ssid[0] == '\0') {
    *out = raw("{\"ok\":false,\"error\":\"missing_ssid\"}");
    return DAPI_BAD_REQUEST;
  }
  /* Only persist if the device is actually connected to this SSID */
  if (!wifi_manager_is_sta_connected() || strcmp(wifi_manager_get_sta_ssid(), ssid) != 0) {
    *out = raw("{\"ok\":false,\"error\":\"not_connected\"}");
    return DAPI_OK;
  }

  wifi_profile_t *profiles = profiles_alloc();
  if (!profiles) {
    return DAPI_INTERNAL;
  }
  int count = 0, auto_idx = -1;
  bool exists_unchanged = false;
  if (nvs_store_load_wifi_profiles(profiles, &count, &auto_idx)) {
    for (int i = 0; i < count; i++) {
      if (strcmp(profiles[i].ssid, ssid) == 0) {
        if (password[0] == '\0' || strcmp(profiles[i].pass, password) == 0) {
          exists_unchanged = true;
          break;
        }
      }
    }
  }
  free(profiles);

  if (exists_unchanged) {
    ESP_LOGI(TAG, "Wi-Fi profile already exists and is unchanged for SSID=%s", ssid);
    *out = raw("{\"ok\":true,\"reboot_pending\":false}");
    return DAPI_OK;
  }

  bool ok = nvs_store_save_wifi_profile(ssid, password);
  ESP_LOGI(TAG, "Saved Wi-Fi profile: SSID=%s ok=%d", ssid, ok);
  if (!ok) {
    *out = raw("{\"ok\":false,\"error\":\"save_failed\"}");
    return DAPI_OK;
  }
  *out = raw("{\"ok\":true,\"reboot_pending\":false}");
  return DAPI_OK;
}

/* {"index":-1..n} or {"ssid":".."} - set or clear the auto-connect profile */
static dapi_status_t dapi_wifi_profiles_setauto(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  int target_idx = -1;
  /* Accept {"ssid":"..."} to find index, or {"index":-1} to clear */
  const cJSON *idx_item = cJSON_GetObjectItem(params, "index");
  if (idx_item && cJSON_IsNumber(idx_item)) {
    target_idx = (int)idx_item->valuedouble;
  } else {
    const cJSON *s = cJSON_GetObjectItem(params, "ssid");
    if (s && cJSON_IsString(s)) {
      wifi_profile_t *profiles = profiles_alloc();
      if (!profiles) {
        return DAPI_INTERNAL;
      }
      int count = 0, auto_idx = -1;
      nvs_store_load_wifi_profiles(profiles, &count, &auto_idx);
      for (int i = 0; i < count; i++) {
        if (strcmp(profiles[i].ssid, s->valuestring) == 0) {
          target_idx = i;
          break;
        }
      }
      free(profiles);
    }
  }

  bool ok = nvs_store_set_wifi_auto_connect(target_idx);
  ESP_LOGI(TAG, "Set Wi-Fi auto-connect index=%d ok=%d", target_idx, ok);
  *out = raw(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"setauto_failed\"}");
  return DAPI_OK;
}

/* {"ssid":".."} - remove a saved credential; ThingsBoard may not remove the network in use */
static dapi_status_t dapi_wifi_profiles_forget(const cJSON *params, dapi_source_t src, cJSON **out) {
  char ssid[33] = {0};
  const cJSON *s = cJSON_GetObjectItem(params, "ssid");
  if (s && cJSON_IsString(s)) {
    if (strlen(s->valuestring) >= sizeof(ssid)) {
      *out = dapi_error("invalid_ssid", "SSID is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(ssid, s->valuestring, sizeof(ssid));
  }
  if (ssid[0] == '\0') {
    *out = raw("{\"ok\":false,\"error\":\"missing_ssid\"}");
    return DAPI_BAD_REQUEST;
  }

  char connected_ssid[33] = {0};
  if (wifi_manager_is_sta_connected()) {
    strlcpy(connected_ssid, wifi_manager_get_sta_ssid(), sizeof(connected_ssid));
  }
  if (dapi_wifi_forget_blocked(src, ssid, connected_ssid)) {
    *out = dapi_error("in_use", "Cannot forget the network in use");
    return DAPI_CONFLICT;
  }

  bool ok = nvs_store_forget_wifi_profile(ssid);
  ESP_LOGI(TAG, "Forget Wi-Fi profile: SSID=%s ok=%d", ssid, ok);
  *out = raw(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"forget_failed\"}");
  return DAPI_OK;
}

/* {"ssid":"..","password":"..", "ip":"..","gateway":"..","netmask":"..","dns":".."} - local only */
static dapi_status_t dapi_wifi_connect(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)src;
  char ssid[64] = {0};
  char password[65] = {0};
  wifi_sta_ip_config_t ip_cfg = {0};
  bool has_static_ip = false;

  const cJSON *s = cJSON_GetObjectItem(params, "ssid");
  const cJSON *p = cJSON_GetObjectItem(params, "password");
  if (s && cJSON_IsString(s)) {
    if (strlen(s->valuestring) >= sizeof(ssid)) {
      *out = dapi_error("invalid_ssid", "SSID is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(ssid, s->valuestring, sizeof(ssid));
  }
  if (p && cJSON_IsString(p)) {
    if (strlen(p->valuestring) >= sizeof(password)) {
      *out = dapi_error("invalid_password", "Password is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(password, p->valuestring, sizeof(password));
  }
  const cJSON *ip_item = cJSON_GetObjectItem(params, "ip");
  if (ip_item && cJSON_IsString(ip_item) && ip_item->valuestring[0]) {
    if (strlen(ip_item->valuestring) >= sizeof(ip_cfg.ip)) {
      *out = dapi_error("invalid_ip", "IP address is too long");
      return DAPI_BAD_REQUEST;
    }
    strlcpy(ip_cfg.ip, ip_item->valuestring, sizeof(ip_cfg.ip));
    has_static_ip = true;
    const cJSON *gw = cJSON_GetObjectItem(params, "gateway");
    if (gw && cJSON_IsString(gw)) {
      if (strlen(gw->valuestring) >= sizeof(ip_cfg.gateway)) {
        *out = dapi_error("invalid_gateway", "Gateway is too long");
        return DAPI_BAD_REQUEST;
      }
      strlcpy(ip_cfg.gateway, gw->valuestring, sizeof(ip_cfg.gateway));
    }
    const cJSON *nm = cJSON_GetObjectItem(params, "netmask");
    if (nm && cJSON_IsString(nm)) {
      if (strlen(nm->valuestring) >= sizeof(ip_cfg.netmask)) {
        *out = dapi_error("invalid_netmask", "Netmask is too long");
        return DAPI_BAD_REQUEST;
      }
      strlcpy(ip_cfg.netmask, nm->valuestring, sizeof(ip_cfg.netmask));
    }
    const cJSON *dns = cJSON_GetObjectItem(params, "dns");
    if (dns && cJSON_IsString(dns)) {
      if (strlen(dns->valuestring) >= sizeof(ip_cfg.dns)) {
        *out = dapi_error("invalid_dns", "DNS is too long");
        return DAPI_BAD_REQUEST;
      }
      strlcpy(ip_cfg.dns, dns->valuestring, sizeof(ip_cfg.dns));
    }
  }

  if (strlen(ssid) == 0) {
    *out = raw("{\"ok\":false,\"error\":\"missing_ssid\"}");
    return DAPI_OK;
  }

  /* If password is empty, try to lookup from saved profiles */
  if (password[0] == '\0') {
    wifi_profile_t *profiles = profiles_alloc();
    if (!profiles) {
      return DAPI_INTERNAL;
    }
    int count = 0, auto_idx = -1;
    nvs_store_load_wifi_profiles(profiles, &count, &auto_idx);
    for (int i = 0; i < count; i++) {
      if (strcmp(profiles[i].ssid, ssid) == 0) {
        strlcpy(password, profiles[i].pass, sizeof(password));
        ESP_LOGI(TAG, "Using saved password for SSID: %s", ssid);
        break;
      }
    }
    free(profiles);
  }

  if (strlen(ssid) > 32) {
    *out = raw("{\"ok\":false,\"error\":\"ssid_too_long\"}");
    return DAPI_BAD_REQUEST;
  }

  if (!is_valid_utf8_printable(ssid, sizeof(ssid))) {
    *out = raw("{\"ok\":false,\"error\":\"invalid_ssid\"}");
    return DAPI_BAD_REQUEST;
  }

  if (strlen(password) > 64) {
    *out = raw("{\"ok\":false,\"error\":\"password_too_long\"}");
    return DAPI_BAD_REQUEST;
  }

  if (password[0] && !is_valid_utf8_printable(password, sizeof(password))) {
    *out = raw("{\"ok\":false,\"error\":\"invalid_password\"}");
    return DAPI_BAD_REQUEST;
  }

  if (has_static_ip && !ip_cfg.gateway[0]) {
    *out = raw("{\"ok\":false,\"error\":\"missing_gateway\"}");
    return DAPI_BAD_REQUEST;
  }

  if (wifi_manager_is_sta_connected() && strcmp(wifi_manager_get_sta_ssid(), ssid) == 0) {
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
      return DAPI_INTERNAL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "already_connected", true);
    cJSON_AddStringToObject(resp, "ssid", wifi_manager_get_sta_ssid());
    cJSON_AddStringToObject(resp, "ip", wifi_manager_get_sta_ip());
    *out = resp;
    return DAPI_OK;
  }

  if (!wifi_manager_connect_sta(ssid, password, has_static_ip ? &ip_cfg : NULL)) {
    *out = raw("{\"ok\":false,\"error\":\"connect_error\"}");
    return DAPI_OK;
  }

  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    *out = raw("{\"ok\":false,\"error\":\"memory\"}");
    return DAPI_INTERNAL;
  }
  cJSON_AddBoolToObject(resp, "ok", true);
  cJSON_AddBoolToObject(resp, "connecting", true);
  cJSON_AddStringToObject(resp, "ssid", ssid);
  *out = resp;
  return DAPI_OK;
}

static void wifi_disconnect_after_response_task(void *arg) {
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(WIFI_DISCONNECT_DELAY_MS));
  if (!wifi_manager_forget_sta()) {
    ESP_LOGW(TAG, "Deferred STA disconnect/forget reported failure");
  }

  taskENTER_CRITICAL(&s_wifi_disconnect_task_lock);
  s_wifi_disconnect_task_pending = false;
  taskEXIT_CRITICAL(&s_wifi_disconnect_task_lock);
  vTaskDelete(NULL);
}

/* wifi.disconnect: drops the station link after the reply is sent - local only */
static dapi_status_t dapi_wifi_disconnect(const cJSON *params, dapi_source_t src, cJSON **out) {
  (void)params;
  (void)src;
  taskENTER_CRITICAL(&s_wifi_disconnect_task_lock);
  bool already_pending = s_wifi_disconnect_task_pending;
  if (!already_pending) {
    s_wifi_disconnect_task_pending = true;
  }
  taskEXIT_CRITICAL(&s_wifi_disconnect_task_lock);

  if (already_pending) {
    *out = raw("{\"ok\":true,\"disconnect_pending\":true}");
    return DAPI_OK;
  }

  if (xTaskCreate(wifi_disconnect_after_response_task, "wifi_disconnect", WIFI_DISCONNECT_TASK_STACK_BYTES, NULL,
                  tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
    taskENTER_CRITICAL(&s_wifi_disconnect_task_lock);
    s_wifi_disconnect_task_pending = false;
    taskEXIT_CRITICAL(&s_wifi_disconnect_task_lock);
    *out = raw("{\"ok\":false,\"error\":\"disconnect_task_failed\"}");
    return DAPI_INTERNAL;
  }

  *out = raw("{\"ok\":true,\"disconnect_pending\":true}");
  return DAPI_OK;
}

/* wifi.connect stays local-only until milestone 4 adds the staged commit. */
static const dapi_method_t s_wifi_methods[] = {
    {"wifi.scan", DAPI_F_READ | DAPI_F_NO_LOCK, 0, dapi_wifi_scan},
    {"wifi.profiles.get", DAPI_F_READ, 0, dapi_wifi_profiles_get},
    {"wifi.profiles.save", DAPI_F_WRITE, WIFI_SAVE_BODY_BYTES, dapi_wifi_profiles_save},
    {"wifi.profiles.setauto", DAPI_F_WRITE, WIFI_PROFILE_BODY_BYTES, dapi_wifi_profiles_setauto},
    {"wifi.profiles.forget", DAPI_F_WRITE, WIFI_PROFILE_BODY_BYTES, dapi_wifi_profiles_forget},
    {"wifi.connect", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY, WIFI_CONNECT_BODY_BYTES, dapi_wifi_connect},
    {"wifi.disconnect", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY, 0, dapi_wifi_disconnect},
};

void dapi_wifi_register(void) {
  (void)device_api_register(s_wifi_methods, sizeof(s_wifi_methods) / sizeof(s_wifi_methods[0]));
}
