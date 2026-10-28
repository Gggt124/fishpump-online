#include "nvs_store.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "nvs_store";

#define NVS_NAMESPACE "wifi_cfg"
#define NVS_KEY_SSID "sta_ssid"
#define NVS_KEY_PASS "sta_pass"
#define NVS_KEY_AP_TMO "ap_stop_tmo"
#define NVS_KEY_AP_AUTO "ap_auto_stop"
#define NVS_KEY_IP "sta_ip"
#define NVS_KEY_GW "sta_gw"
#define NVS_KEY_MASK "sta_netmask"
#define NVS_KEY_DNS "sta_dns"
#define NVS_KEY_STG_TYPE "stg_type"
#define NVS_KEY_STG_SSID "stg_sta_ssid"
#define NVS_KEY_STG_PASS "stg_sta_pass"
#define NVS_KEY_STG_USER "stg_admin_user"
#define NVS_KEY_STG_CRED_PASS "stg_admin_pass"
#define NVS_KEY_RST_DETAIL "rst_detail"

#define NVS_SESSION_NAMESPACE "session"
#define NVS_KEY_JWT_SECRET "jwt_secret"

#define NVS_PUMP_NAMESPACE "pump_cfg"
#define NVS_PUMP_KEY_T1_ON "t1_on"
#define NVS_PUMP_KEY_T1_OFF "t1_off"
#define NVS_PUMP_KEY_T2_ON "t2_on"
#define NVS_PUMP_KEY_T2_OFF "t2_off"
#define NVS_PUMP_KEY_RELAY_LOW "relay_low"
#define NVS_PUMP_KEY_AUTO_START "auto_start"
#define NVS_PUMP_KEY_RELAY1_LOW "r1_low"
#define NVS_PUMP_KEY_RELAY2_LOW "r2_low"
#define NVS_PUMP_KEY_T1_START "t1_start"
#define NVS_PUMP_KEY_T2_START "t2_start"

/* A6 audit: atomic blob key — replaces 10 individual pump keys */
#define NVS_PUMP_KEY_SETTINGS_BLOB "pump_blob"
#define NVS_PUMP_BLOB_VERSION 3 /* v3: added wash timer fields (append-only) */

#define NVS_PUMP_KEY_WASH_ON "wash_on"
#define NVS_PUMP_KEY_WASH_OFF "wash_off"
#define NVS_PUMP_KEY_WASH_START "wash_start"
#define NVS_PUMP_KEY_WASH_AUTOSTART "wash_autostart"

#define NVS_HW_NAMESPACE "hw_cfg"
#define NVS_HW_KEY_ACT_FLOAT "act_float"
#define NVS_HW_KEY_ACT_R1 "act_r1"
#define NVS_HW_KEY_ACT_R2 "act_r2"
#define NVS_HW_KEY_ACT_DS "act_ds"
#define NVS_HW_KEY_ACT_COOL "act_cool"
#define NVS_HW_KEY_PEND_VALID "pend_valid"
#define NVS_HW_KEY_PEND_FLOAT "pend_float"
#define NVS_HW_KEY_PEND_R1 "pend_r1"
#define NVS_HW_KEY_PEND_R2 "pend_r2"
#define NVS_HW_KEY_PEND_DS "pend_ds"
#define NVS_HW_KEY_PEND_COOL "pend_cool"

#define NVS_COOL_NAMESPACE "cool_cfg"
#define NVS_COOL_KEY_THRESHOLD "threshold_x10"
#define NVS_COOL_KEY_HYST "hyst_x10"
#define NVS_COOL_KEY_AUTO_EN "auto_en"
#define NVS_COOL_KEY_MODE "mode"
#define NVS_COOL_KEY_TEST_TMO "test_tmo_s"
#define NVS_COOL_KEY_MIN_OFF "min_off_s"
#define NVS_COOL_KEY_RELAY_LOW "relay_low"

#define NVS_DISP_NAMESPACE "disp_cfg"
#define NVS_DISP_KEY_DIM "dim_pct"

static bool pump_duration_valid(uint32_t seconds) {
  return seconds >= APP_TEMPLATE_PUMP_TIMER_MIN_SEC &&
         seconds <= APP_TEMPLATE_PUMP_TIMER_MAX_SEC;
}

static bool pump_bool_raw_valid(uint8_t value) {
  return value == 0 || value == 1;
}

_Static_assert(sizeof(bool) == 1,
               "pump settings blob expects one-byte bool fields");

static bool pump_blob_bool_field_valid(const uint8_t *payload,
                                       size_t payload_len, size_t offset) {
  if (!payload || payload_len <= offset) {
    return true;
  }
  return payload_len >= offset + sizeof(bool) &&
         pump_bool_raw_valid(payload[offset]);
}

static bool pump_blob_boolean_fields_valid(const uint8_t *payload,
                                           size_t payload_len) {
  return pump_blob_bool_field_valid(
             payload, payload_len,
             offsetof(nvs_store_pump_settings_t, relay_active_low)) &&
         pump_blob_bool_field_valid(
             payload, payload_len,
             offsetof(nvs_store_pump_settings_t, relay1_active_low)) &&
         pump_blob_bool_field_valid(
             payload, payload_len,
             offsetof(nvs_store_pump_settings_t, relay2_active_low)) &&
         pump_blob_bool_field_valid(
             payload, payload_len,
             offsetof(nvs_store_pump_settings_t, auto_start)) &&
         pump_blob_bool_field_valid(
             payload, payload_len,
             offsetof(nvs_store_pump_settings_t, wash_auto_start));
}

static bool pump_settings_valid(const nvs_store_pump_settings_t *settings) {
  return settings && pump_bool_raw_valid((uint8_t)settings->relay_active_low) &&
         pump_bool_raw_valid((uint8_t)settings->relay1_active_low) &&
         pump_bool_raw_valid((uint8_t)settings->relay2_active_low) &&
         pump_bool_raw_valid((uint8_t)settings->auto_start) &&
         pump_bool_raw_valid((uint8_t)settings->wash_auto_start) &&
         pump_duration_valid(settings->timer1_on_sec) &&
         pump_duration_valid(settings->timer1_off_sec) &&
         pump_duration_valid(settings->timer2_on_sec) &&
         pump_duration_valid(settings->timer2_off_sec) &&
         pump_duration_valid(settings->wash_on_sec) &&
         pump_duration_valid(settings->wash_off_sec) &&
         hardware_map_timer_start_phase_valid(settings->timer1_start_phase) &&
         hardware_map_timer_start_phase_valid(settings->timer2_start_phase) &&
         hardware_map_timer_start_phase_valid(settings->wash_start_phase) &&
         settings->min_dwell_sec <= APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC;
}

static bool
cooling_settings_valid(const nvs_store_cooling_settings_t *settings) {
  return settings && settings->threshold_c_x10 >= -550 &&
         settings->threshold_c_x10 <= 1250 && settings->hysteresis_c_x10 > 0 &&
         settings->hysteresis_c_x10 <= 500 && settings->test_timeout_sec > 0 &&
         settings->test_timeout_sec <= 3600 &&
         settings->compressor_min_off_sec >= 60 &&
         settings->compressor_min_off_sec <= 86400 &&
         hardware_map_cooling_mode_valid(settings->mode) &&
         hardware_map_polarity_valid(settings->relay_polarity);
}

static esp_err_t load_u32_key(nvs_handle_t handle, const char *key,
                              uint32_t *out) {
  return nvs_get_u32(handle, key, out);
}

static esp_err_t load_bool_key(nvs_handle_t handle, const char *key,
                               bool *out) {
  uint8_t raw = 0;
  esp_err_t ret = nvs_get_u8(handle, key, &raw);
  if (ret != ESP_OK) {
    return ret;
  }
  if (!pump_bool_raw_valid(raw)) {
    return ESP_ERR_INVALID_STATE;
  }
  *out = raw != 0;
  return ESP_OK;
}

static esp_err_t load_gpio_key(nvs_handle_t handle, const char *key,
                               gpio_num_t *out) {
  uint32_t raw = 0;
  esp_err_t ret = nvs_get_u32(handle, key, &raw);
  if (ret != ESP_OK) {
    return ret;
  }
  if (raw > GPIO_NUM_MAX) {
    return ESP_ERR_INVALID_STATE;
  }
  *out = (gpio_num_t)raw;
  return ESP_OK;
}

static esp_err_t load_u8_key(nvs_handle_t handle, const char *key,
                             uint8_t *out) {
  return nvs_get_u8(handle, key, out);
}

static bool set_gpio_key(nvs_handle_t handle, const char *key,
                         gpio_num_t gpio) {
  return nvs_set_u32(handle, key, (uint32_t)gpio) == ESP_OK;
}

static bool erase_optional_key(nvs_handle_t handle, const char *key) {
  esp_err_t ret = nvs_erase_key(handle, key);
  return ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND;
}

bool nvs_store_init(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    ret = nvs_flash_init();
  }
  if (ret == ESP_OK) {
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    ESP_LOGI(TAG, "NVS init OK (encryption enabled)");
#else
    ESP_LOGI(TAG, "NVS init OK (no encryption)");
#endif
  }
  return ret == ESP_OK;
}

bool nvs_store_set_jwt_secret(const uint8_t secret[32]) {
  if (!secret)
    return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_SESSION_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (nvs_set_blob(handle, NVS_KEY_JWT_SECRET, secret, 32) != ESP_OK) {
    ok = false;
  }
  if (ok && nvs_commit(handle) != ESP_OK) {
    ok = false;
  }
  nvs_close(handle);
  return ok;
}

bool nvs_store_get_jwt_secret(uint8_t secret[32]) {
  if (!secret)
    return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_SESSION_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return false;

  size_t required_size = 32;
  esp_err_t ret =
      nvs_get_blob(handle, NVS_KEY_JWT_SECRET, secret, &required_size);
  nvs_close(handle);

  return (ret == ESP_OK && required_size == 32);
}

bool nvs_store_get_credentials(char *username_out, size_t user_len,
                               char *password_out, size_t pass_len) {
  if (!username_out || !password_out || user_len == 0 || pass_len == 0)
    return false;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  bool loaded = false;

  if (err == ESP_OK) {
    uint8_t stg_type = 0;
    esp_err_t stg_err = nvs_get_u8(handle, NVS_KEY_STG_TYPE, &stg_type);
    if (stg_err == ESP_OK && stg_type == 2) {
      size_t u_len = user_len;
      size_t p_len = pass_len;
      if (nvs_get_str(handle, NVS_KEY_STG_USER, username_out, &u_len) ==
              ESP_OK &&
          nvs_get_str(handle, NVS_KEY_STG_CRED_PASS, password_out, &p_len) ==
              ESP_OK) {
        loaded = true;
      }
    }
    if (!loaded) {
      size_t u_len = user_len;
      size_t p_len = pass_len;
      if (nvs_get_str(handle, "admin_user", username_out, &u_len) == ESP_OK &&
          nvs_get_str(handle, "admin_pass", password_out, &p_len) == ESP_OK) {
        loaded = true;
      }
    }
    nvs_close(handle);
  }

  if (!loaded) {
    strlcpy(username_out, APP_TEMPLATE_DEFAULT_USERNAME, user_len);
    strlcpy(password_out, APP_TEMPLATE_DEFAULT_PASSWORD, pass_len);
  }

  return true;
}

bool nvs_store_set_credentials(const char *username, const char *password) {
  if (!username || !password || username[0] == '\0' || password[0] == '\0')
    return false;

  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (nvs_set_str(handle, "admin_user", username) != ESP_OK)
    ok = false;
  if (nvs_set_str(handle, "admin_pass", password) != ESP_OK)
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK) {
    ok = false;
  }
  nvs_close(handle);
  return ok;
}

bool nvs_store_save_wifi(const char *ssid, const char *password) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (nvs_set_str(handle, NVS_KEY_SSID, ssid) != ESP_OK)
    ok = false;
  if (password && password[0]) {
    if (nvs_set_str(handle, NVS_KEY_PASS, password) != ESP_OK)
      ok = false;
  } else {
    nvs_erase_key(handle, NVS_KEY_PASS);
  }

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_load_wifi(char *ssid_out, size_t ssid_len, char *pass_out,
                         size_t pass_len) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    if (ssid_out)
      ssid_out[0] = '\0';
    if (pass_out)
      pass_out[0] = '\0';
    return false;
  }

  bool has_ssid = false;
  size_t len = ssid_len;
  if (nvs_get_str(handle, NVS_KEY_SSID, ssid_out, &len) == ESP_OK && ssid_out &&
      ssid_out[0]) {
    has_ssid = true;
  } else if (ssid_out) {
    ssid_out[0] = '\0';
  }

  len = pass_len;
  if (nvs_get_str(handle, NVS_KEY_PASS, pass_out, &len) != ESP_OK) {
    if (pass_out) {
      pass_out[0] = '\0';
    }
  }

  nvs_close(handle);
  return has_ssid;
}

bool nvs_store_clear_wifi(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = true;
  if (!erase_optional_key(handle, NVS_KEY_SSID))
    ok = false;
  if (!erase_optional_key(handle, NVS_KEY_PASS))
    ok = false;
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

/* ============================================================
 * Multi-profile Wi-Fi credential management
 * Namespace: "wifi_prof"
 * Keys:
 *   p_count  (u8)  — number of valid profiles (0..WIFI_PROFILE_MAX)
 *   p_auto   (i8)  — auto-connect index, stored as u8 with 0xFF = none
 *   pN_ssid  (str) — SSID for profile N
 *   pN_pass  (str) — password for profile N
 * ============================================================ */

#define NVS_PROF_NAMESPACE "wifi_prof"
#define NVS_PROF_KEY_COUNT "p_count"
#define NVS_PROF_KEY_AUTO "p_auto"
#define NVS_PROF_AUTO_NONE 0xFF

/* Build key strings like "p0_ssid", "p3_pass" into caller-supplied buf (len >=
 * 8). */
static void prof_ssid_key(char *buf, int idx) {
  buf[0] = 'p';
  buf[1] = '0' + idx;
  buf[2] = '_';
  buf[3] = 's';
  buf[4] = 's';
  buf[5] = 'i';
  buf[6] = 'd';
  buf[7] = '\0';
}
static void prof_pass_key(char *buf, int idx) {
  buf[0] = 'p';
  buf[1] = '0' + idx;
  buf[2] = '_';
  buf[3] = 'p';
  buf[4] = 'a';
  buf[5] = 's';
  buf[6] = 's';
  buf[7] = '\0';
}

bool nvs_store_load_wifi_profiles(wifi_profile_t *profiles, int *count_out,
                                  int *auto_idx_out) {
  if (!profiles || !count_out || !auto_idx_out)
    return false;
  memset(profiles, 0, sizeof(wifi_profile_t) * WIFI_PROFILE_MAX);
  *count_out = 0;
  *auto_idx_out = -1;

  uint8_t stg_type = 0;
  if (nvs_store_get_staging_type(&stg_type) && stg_type == 1) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
      char ssid[33] = {0};
      char pass[65] = {0};
      size_t ssid_len = sizeof(ssid);
      size_t pass_len = sizeof(pass);
      esp_err_t err_ssid =
          nvs_get_str(handle, NVS_KEY_STG_SSID, ssid, &ssid_len);
      esp_err_t err_pass =
          nvs_get_str(handle, NVS_KEY_STG_PASS, pass, &pass_len);
      nvs_close(handle);
      if (err_ssid == ESP_OK) {
        strlcpy(profiles[0].ssid, ssid, sizeof(profiles[0].ssid));
        if (err_pass == ESP_OK) {
          strlcpy(profiles[0].pass, pass, sizeof(profiles[0].pass));
        }
        *count_out = 1;
        *auto_idx_out = 0;
        return true;
      }
    }
  }

  nvs_handle_t h;
  esp_err_t ret = nvs_open(NVS_PROF_NAMESPACE, NVS_READONLY, &h);
  if (ret == ESP_ERR_NVS_NOT_FOUND)
    return true; /* no profiles yet */
  if (ret != ESP_OK)
    return false;

  uint8_t count = 0;
  nvs_get_u8(h, NVS_PROF_KEY_COUNT, &count);
  if (count > WIFI_PROFILE_MAX)
    count = WIFI_PROFILE_MAX;

  uint8_t auto_raw = NVS_PROF_AUTO_NONE;
  nvs_get_u8(h, NVS_PROF_KEY_AUTO, &auto_raw);
  *auto_idx_out = (auto_raw == NVS_PROF_AUTO_NONE || auto_raw >= count)
                      ? -1
                      : (int)auto_raw;

  char key[8];
  for (int i = 0; i < (int)count; i++) {
    prof_ssid_key(key, i);
    size_t ssid_len = sizeof(profiles[i].ssid);
    if (nvs_get_str(h, key, profiles[i].ssid, &ssid_len) != ESP_OK) {
      profiles[i].ssid[0] = '\0';
    }
    prof_pass_key(key, i);
    size_t pass_len = sizeof(profiles[i].pass);
    if (nvs_get_str(h, key, profiles[i].pass, &pass_len) != ESP_OK) {
      profiles[i].pass[0] = '\0';
    }
  }
  *count_out = (int)count;
  nvs_close(h);
  return true;
}

/* Write the in-memory profile array and auto_idx back to NVS atomically. */
static bool prof_write_all(nvs_handle_t h, const wifi_profile_t *profiles,
                           int count, int auto_idx) {
  bool ok = true;
  char key[8];

  if (nvs_set_u8(h, NVS_PROF_KEY_COUNT, (uint8_t)count) != ESP_OK)
    ok = false;
  uint8_t auto_raw = (auto_idx >= 0 && auto_idx < count) ? (uint8_t)auto_idx
                                                         : NVS_PROF_AUTO_NONE;
  if (nvs_set_u8(h, NVS_PROF_KEY_AUTO, auto_raw) != ESP_OK)
    ok = false;

  for (int i = 0; i < count; i++) {
    prof_ssid_key(key, i);
    if (nvs_set_str(h, key, profiles[i].ssid) != ESP_OK)
      ok = false;
    prof_pass_key(key, i);
    if (nvs_set_str(h, key, profiles[i].pass) != ESP_OK)
      ok = false;
  }
  /* Erase slots that are no longer used */
  for (int i = count; i < WIFI_PROFILE_MAX; i++) {
    prof_ssid_key(key, i);
    erase_optional_key(h, key);
    prof_pass_key(key, i);
    erase_optional_key(h, key);
  }
  if (ok && nvs_commit(h) != ESP_OK)
    ok = false;
  return ok;
}

bool nvs_store_save_wifi_profile(const char *ssid, const char *password) {
  if (!ssid || ssid[0] == '\0')
    return false;

  wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
  int count = 0, auto_idx = -1;
  nvs_store_load_wifi_profiles(profiles, &count, &auto_idx);

  /* Upsert: if SSID already in list, update password in-place */
  for (int i = 0; i < count; i++) {
    if (strcmp(profiles[i].ssid, ssid) == 0) {
      const char *new_pass = password ? password : "";
      if (strcmp(profiles[i].pass, new_pass) == 0) {
        return true; /* Unchanged, skip NVS write */
      }
      strlcpy(profiles[i].pass, new_pass, sizeof(profiles[i].pass));
      nvs_handle_t h;
      if (nvs_open(NVS_PROF_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
      bool ok = prof_write_all(h, profiles, count, auto_idx);
      nvs_close(h);
      return ok;
    }
  }

  /* New SSID: if full, evict oldest (index 0) by shifting left */
  if (count >= WIFI_PROFILE_MAX) {
    /* If auto_idx was 0, it is gone; shift down others */
    if (auto_idx == 0)
      auto_idx = -1;
    else if (auto_idx > 0)
      auto_idx--;
    for (int i = 0; i < WIFI_PROFILE_MAX - 1; i++) {
      profiles[i] = profiles[i + 1];
    }
    count = WIFI_PROFILE_MAX - 1;
  }

  /* Append new profile */
  strlcpy(profiles[count].ssid, ssid, sizeof(profiles[count].ssid));
  strlcpy(profiles[count].pass, password ? password : "",
          sizeof(profiles[count].pass));
  count++;

  nvs_handle_t h;
  if (nvs_open(NVS_PROF_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok = prof_write_all(h, profiles, count, auto_idx);
  nvs_close(h);
  return ok;
}

bool nvs_store_forget_wifi_profile(const char *ssid) {
  if (!ssid || ssid[0] == '\0')
    return true;

  wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
  int count = 0, auto_idx = -1;
  if (!nvs_store_load_wifi_profiles(profiles, &count, &auto_idx))
    return false;

  int found = -1;
  for (int i = 0; i < count; i++) {
    if (strcmp(profiles[i].ssid, ssid) == 0) {
      found = i;
      break;
    }
  }
  if (found < 0)
    return true; /* not found — nothing to do */

  /* Compact array */
  for (int i = found; i < count - 1; i++) {
    profiles[i] = profiles[i + 1];
  }
  count--;
  memset(&profiles[count], 0, sizeof(wifi_profile_t));

  /* Adjust auto_idx */
  if (auto_idx == found)
    auto_idx = -1;
  else if (auto_idx > found)
    auto_idx--;

  nvs_handle_t h;
  if (nvs_open(NVS_PROF_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok = prof_write_all(h, profiles, count, auto_idx);
  nvs_close(h);

  /* Clear legacy Wi-Fi credential if it matches the forgotten SSID */
  if (ok) {
    char legacy_ssid[33] = {0};
    char legacy_pass[65] = {0};
    if (nvs_store_load_wifi(legacy_ssid, sizeof(legacy_ssid), legacy_pass,
                            sizeof(legacy_pass))) {
      if (strcmp(legacy_ssid, ssid) == 0) {
        nvs_store_clear_wifi();
      }
    }
  }

  return ok;
}

bool nvs_store_set_wifi_auto_connect(int profile_index) {
  wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
  int count = 0, auto_idx = -1;
  if (!nvs_store_load_wifi_profiles(profiles, &count, &auto_idx))
    return false;

  if (profile_index != -1 && (profile_index < 0 || profile_index >= count))
    return false;

  nvs_handle_t h;
  if (nvs_open(NVS_PROF_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    return false;
  uint8_t raw =
      (profile_index >= 0) ? (uint8_t)profile_index : NVS_PROF_AUTO_NONE;
  bool ok = (nvs_set_u8(h, NVS_PROF_KEY_AUTO, raw) == ESP_OK);
  if (ok)
    ok = (nvs_commit(h) == ESP_OK);
  nvs_close(h);
  return ok;
}

bool nvs_store_migrate_legacy_wifi(void) {
  /* Check if profiles already exist — if so, nothing to migrate */
  wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
  int count = 0, auto_idx = -1;
  nvs_store_load_wifi_profiles(profiles, &count, &auto_idx);
  if (count > 0)
    return true; /* already migrated */

  /* Try to load legacy credential */
  char ssid[33] = {0}, pass[65] = {0};
  bool has = nvs_store_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
  if (!has || ssid[0] == '\0')
    return true; /* nothing to migrate */

  ESP_LOGI(TAG, "Migrating legacy Wi-Fi credential (SSID: %s) to profile store",
           ssid);
  if (!nvs_store_save_wifi_profile(ssid, pass))
    return false;
  /* Set it as auto-connect */
  return nvs_store_set_wifi_auto_connect(0);
}

bool nvs_store_save_ap_config(uint32_t stop_timeout_ms, bool auto_stop) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (nvs_set_u32(handle, NVS_KEY_AP_TMO, stop_timeout_ms) != ESP_OK)
    ok = false;
  uint8_t val = auto_stop ? 1 : 0;
  if (nvs_set_u8(handle, NVS_KEY_AP_AUTO, val) != ESP_OK)
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_load_ap_config(uint32_t *stop_timeout_ms, bool *auto_stop) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    if (stop_timeout_ms)
      *stop_timeout_ms = AP_STOP_TMO_DEFAULT_MS;
    if (auto_stop)
      *auto_stop = AP_AUTO_STOP_DEFAULT;
    return false;
  }

  uint32_t tmo = AP_STOP_TMO_DEFAULT_MS;
  if (nvs_get_u32(handle, NVS_KEY_AP_TMO, &tmo) != ESP_OK) {
    tmo = AP_STOP_TMO_DEFAULT_MS;
  }
  if (stop_timeout_ms)
    *stop_timeout_ms = tmo;

  uint8_t val = AP_AUTO_STOP_DEFAULT ? 1 : 0;
  if (nvs_get_u8(handle, NVS_KEY_AP_AUTO, &val) != ESP_OK) {
    val = AP_AUTO_STOP_DEFAULT ? 1 : 0;
  }
  if (auto_stop)
    *auto_stop = (val != 0);

  nvs_close(handle);
  return true;
}

bool nvs_store_save_sta_ip(const char *ip, const char *gateway,
                           const char *netmask, const char *dns) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (ip && ip[0]) {
    if (nvs_set_str(handle, NVS_KEY_IP, ip) != ESP_OK)
      ok = false;
  } else {
    nvs_erase_key(handle, NVS_KEY_IP);
  }
  if (gateway && gateway[0]) {
    if (nvs_set_str(handle, NVS_KEY_GW, gateway) != ESP_OK)
      ok = false;
  } else {
    nvs_erase_key(handle, NVS_KEY_GW);
  }
  if (netmask && netmask[0]) {
    if (nvs_set_str(handle, NVS_KEY_MASK, netmask) != ESP_OK)
      ok = false;
  } else {
    nvs_erase_key(handle, NVS_KEY_MASK);
  }
  if (dns && dns[0]) {
    if (nvs_set_str(handle, NVS_KEY_DNS, dns) != ESP_OK)
      ok = false;
  } else {
    nvs_erase_key(handle, NVS_KEY_DNS);
  }

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_load_sta_ip(char *ip_out, size_t ip_len, char *gw_out,
                           size_t gw_len, char *mask_out, size_t mask_len,
                           char *dns_out, size_t dns_len) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    if (ip_out)
      ip_out[0] = '\0';
    if (gw_out)
      gw_out[0] = '\0';
    if (mask_out)
      mask_out[0] = '\0';
    if (dns_out)
      dns_out[0] = '\0';
    return false;
  }

  bool has_ip = false;
  size_t len = ip_len;
  if (nvs_get_str(handle, NVS_KEY_IP, ip_out, &len) == ESP_OK && ip_out &&
      ip_out[0]) {
    has_ip = true;
  } else if (ip_out) {
    ip_out[0] = '\0';
  }

  len = gw_len;
  if (nvs_get_str(handle, NVS_KEY_GW, gw_out, &len) != ESP_OK && gw_out) {
    gw_out[0] = '\0';
  }

  len = mask_len;
  if (nvs_get_str(handle, NVS_KEY_MASK, mask_out, &len) != ESP_OK && mask_out) {
    mask_out[0] = '\0';
  }

  len = dns_len;
  if (nvs_get_str(handle, NVS_KEY_DNS, dns_out, &len) != ESP_OK && dns_out) {
    dns_out[0] = '\0';
  }

  nvs_close(handle);
  return has_ip;
}

bool nvs_store_clear_sta_ip(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = true;
  if (!erase_optional_key(handle, NVS_KEY_IP))
    ok = false;
  if (!erase_optional_key(handle, NVS_KEY_GW))
    ok = false;
  if (!erase_optional_key(handle, NVS_KEY_MASK))
    ok = false;
  if (!erase_optional_key(handle, NVS_KEY_DNS))
    ok = false;
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

void nvs_store_pump_settings_defaults(nvs_store_pump_settings_t *out) {
  if (!out) {
    return;
  }
  memset(out, 0, sizeof(*out));

  out->timer1_on_sec = APP_TEMPLATE_PUMP_TIMER1_ON_SEC;
  out->timer1_off_sec = APP_TEMPLATE_PUMP_TIMER1_OFF_SEC;
  out->timer2_on_sec = APP_TEMPLATE_PUMP_TIMER2_ON_SEC;
  out->timer2_off_sec = APP_TEMPLATE_PUMP_TIMER2_OFF_SEC;
  out->relay_active_low = APP_TEMPLATE_PUMP_RELAY_ACTIVE_LOW;
  out->relay1_active_low = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW;
  out->relay2_active_low = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW;
  out->timer1_start_phase = APP_TEMPLATE_PUMP_TIMER_START_ON
                                ? HARDWARE_TIMER_START_PHASE_ON
                                : HARDWARE_TIMER_START_PHASE_OFF;
  out->timer2_start_phase = APP_TEMPLATE_PUMP_TIMER_START_ON
                                ? HARDWARE_TIMER_START_PHASE_ON
                                : HARDWARE_TIMER_START_PHASE_OFF;
  out->auto_start = APP_TEMPLATE_PUMP_AUTO_START_DEFAULT;
  out->min_dwell_sec = APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_SEC;

  /* Wash system defaults (Blob v3) */
  out->wash_on_sec = 10;
  out->wash_off_sec = 60;
  out->wash_start_phase = HARDWARE_TIMER_START_PHASE_ON;
  out->wash_auto_start = false;
}

static void load_legacy_wash_keys(nvs_handle_t handle,
                                  nvs_store_pump_settings_t *loaded) {
  uint32_t wash_on = loaded->wash_on_sec;
  uint32_t wash_off = loaded->wash_off_sec;
  uint8_t wash_start = (uint8_t)loaded->wash_start_phase;
  bool wash_autostart = loaded->wash_auto_start;

  if (load_u32_key(handle, NVS_PUMP_KEY_WASH_ON, &wash_on) == ESP_OK &&
      pump_duration_valid(wash_on)) {
    loaded->wash_on_sec = wash_on;
  }
  if (load_u32_key(handle, NVS_PUMP_KEY_WASH_OFF, &wash_off) == ESP_OK &&
      pump_duration_valid(wash_off)) {
    loaded->wash_off_sec = wash_off;
  }
  if (load_u8_key(handle, NVS_PUMP_KEY_WASH_START, &wash_start) == ESP_OK &&
      hardware_map_timer_start_phase_valid(
          (hardware_timer_start_phase_t)wash_start)) {
    loaded->wash_start_phase = (hardware_timer_start_phase_t)wash_start;
  }
  if (load_bool_key(handle, NVS_PUMP_KEY_WASH_AUTOSTART, &wash_autostart) ==
      ESP_OK) {
    loaded->wash_auto_start = wash_autostart;
  }
}

nvs_store_pump_settings_load_status_t
nvs_store_load_pump_settings(nvs_store_pump_settings_t *out) {
  if (!out) {
    return NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR;
  }

  nvs_store_pump_settings_defaults(out);

  nvs_handle_t handle;
  esp_err_t open_ret = nvs_open(NVS_PUMP_NAMESPACE, NVS_READONLY, &handle);
  if (open_ret == ESP_ERR_NVS_NOT_FOUND) {
    return NVS_STORE_PUMP_SETTINGS_DEFAULTS_MISSING;
  }
  if (open_ret != ESP_OK) {
    ESP_LOGE(TAG, "Open %s failed: %s", NVS_PUMP_NAMESPACE,
             esp_err_to_name(open_ret));
    return NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR;
  }

  nvs_store_pump_settings_t loaded = {0};
  nvs_store_pump_settings_defaults(&loaded);

  /* A6 audit: try blob first (new format), fallback to legacy keys for
   * migration */
  /* Use a fixed buffer size for forward compatibility if struct grows */
  uint8_t blob[sizeof(nvs_store_pump_settings_t) + 32];
  size_t blob_len = sizeof(blob);
  esp_err_t blob_err =
      nvs_get_blob(handle, NVS_PUMP_KEY_SETTINGS_BLOB, blob, &blob_len);

  if (blob_err == ESP_OK) {
    if (blob_len > 0 && (blob[0] == NVS_PUMP_BLOB_VERSION || blob[0] == 2 ||
                         blob[0] == 1)) {
      if (blob_len <= 1) {
        ESP_LOGW(TAG,
                 "Pump settings blob has no payload; suppressing pump start");
        nvs_close(handle);
        return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
      }
      size_t copy_len = blob_len - 1;
      if (copy_len > sizeof(nvs_store_pump_settings_t)) {
        copy_len = sizeof(nvs_store_pump_settings_t);
      } else if (copy_len < sizeof(nvs_store_pump_settings_t)) {
        ESP_LOGW(TAG,
                 "Blob size mismatch: got %u bytes, struct expects %u. "
                 "New fields will use defaults (firmware downgrade or struct "
                 "growth).",
                 (unsigned)copy_len,
                 (unsigned)sizeof(nvs_store_pump_settings_t));
      }
      if (!pump_blob_boolean_fields_valid(blob + 1, copy_len)) {
        ESP_LOGW(TAG,
                 "Pump settings blob contains invalid boolean fields; "
                 "suppressing pump start");
        nvs_close(handle);
        return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
      }
      memcpy(&loaded, blob + 1, copy_len);
      if (blob[0] < NVS_PUMP_BLOB_VERSION) {
        load_legacy_wash_keys(handle, &loaded);
      }
      ESP_LOGD(TAG, "Pump settings loaded from blob (v%d)", blob[0]);
    } else {
      ESP_LOGW(TAG,
               "Incompatible blob version or format; suppressing pump start");
      nvs_close(handle);
      return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
    }
    nvs_close(handle);
  } else if (blob_err == ESP_ERR_NVS_NOT_FOUND) {
    /* Fallback: read legacy individual keys (for devices upgrading from old
     * firmware) */
    ESP_LOGW(TAG, "Pump blob not found, trying legacy keys");

    bool relay_active_low = false;
    bool relay1_active_low = loaded.relay1_active_low;
    bool relay2_active_low = loaded.relay2_active_low;
    bool auto_start = false;
    uint8_t timer1_start = (uint8_t)loaded.timer1_start_phase;
    uint8_t timer2_start = (uint8_t)loaded.timer2_start_phase;

    esp_err_t results[] = {
        load_u32_key(handle, NVS_PUMP_KEY_T1_ON, &loaded.timer1_on_sec),
        load_u32_key(handle, NVS_PUMP_KEY_T1_OFF, &loaded.timer1_off_sec),
        load_u32_key(handle, NVS_PUMP_KEY_T2_ON, &loaded.timer2_on_sec),
        load_u32_key(handle, NVS_PUMP_KEY_T2_OFF, &loaded.timer2_off_sec),
        load_bool_key(handle, NVS_PUMP_KEY_RELAY_LOW, &relay_active_low),
        load_bool_key(handle, NVS_PUMP_KEY_AUTO_START, &auto_start),
    };

    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
      if (results[i] == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
      }
      if (results[i] != ESP_OK) {
        ESP_LOGW(TAG, "Invalid pump settings in NVS (%s)",
                 esp_err_to_name(results[i]));
        nvs_close(handle);
        return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
      }
    }

    esp_err_t r1_ret =
        load_bool_key(handle, NVS_PUMP_KEY_RELAY1_LOW, &relay1_active_low);
    if (r1_ret == ESP_ERR_NVS_NOT_FOUND) {
      relay1_active_low = relay_active_low;
    } else if (r1_ret != ESP_OK) {
      nvs_close(handle);
      return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
    }

    esp_err_t r2_ret =
        load_bool_key(handle, NVS_PUMP_KEY_RELAY2_LOW, &relay2_active_low);
    if (r2_ret == ESP_ERR_NVS_NOT_FOUND) {
      relay2_active_low = relay_active_low;
    } else if (r2_ret != ESP_OK) {
      nvs_close(handle);
      return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
    }

    esp_err_t t1_ret =
        load_u8_key(handle, NVS_PUMP_KEY_T1_START, &timer1_start);
    esp_err_t t2_ret =
        load_u8_key(handle, NVS_PUMP_KEY_T2_START, &timer2_start);

    load_legacy_wash_keys(handle, &loaded);
    nvs_close(handle);
    if ((t1_ret != ESP_OK && t1_ret != ESP_ERR_NVS_NOT_FOUND) ||
        (t2_ret != ESP_OK && t2_ret != ESP_ERR_NVS_NOT_FOUND)) {
      return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
    }

    loaded.relay1_active_low = relay1_active_low;
    loaded.relay2_active_low = relay2_active_low;
    loaded.relay_active_low = relay1_active_low;
    loaded.timer1_start_phase = (hardware_timer_start_phase_t)timer1_start;
    loaded.timer2_start_phase = (hardware_timer_start_phase_t)timer2_start;
    loaded.auto_start = auto_start;

    /* After successful legacy read, migrate to blob immediately */
    if (pump_settings_valid(&loaded)) {
      nvs_store_save_pump_settings(&loaded); /* migrate on next boot */
      ESP_LOGI(TAG, "Migrated pump settings from legacy keys to blob");
    }
  } else {
    ESP_LOGW(TAG, "Pump blob read failed (%s); suppressing pump start",
             esp_err_to_name(blob_err));
    nvs_close(handle);
    return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
  }
  if (!pump_settings_valid(&loaded)) {
    return NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID;
  }

  /* Automatically upgrade older blob versions (v1, v2) to current blob version (v3) */
  if (blob_err == ESP_OK && blob[0] < NVS_PUMP_BLOB_VERSION) {
    nvs_store_save_pump_settings(&loaded);
    ESP_LOGI(TAG, "Migrated pump settings blob from v%d to v%d", blob[0],
             NVS_PUMP_BLOB_VERSION);
  }

  *out = loaded;
  return NVS_STORE_PUMP_SETTINGS_LOADED;
}

bool nvs_store_save_pump_settings(const nvs_store_pump_settings_t *settings) {
  if (!pump_settings_valid(settings)) {
    return false;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_PUMP_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  /* A6 audit: write all 10 fields in one atomic blob to prevent partial-write
   * corruption on power loss. Version byte at index 0 for future migration. */
  uint8_t blob[sizeof(nvs_store_pump_settings_t) + 1];
  memset(blob, 0, sizeof(blob));
  blob[0] = NVS_PUMP_BLOB_VERSION;
  memcpy(blob + 1, settings, sizeof(nvs_store_pump_settings_t));

  bool ok = (nvs_set_blob(handle, NVS_PUMP_KEY_SETTINGS_BLOB, blob,
                          sizeof(blob)) == ESP_OK);
  if (ok) {
    ok = (nvs_commit(handle) == ESP_OK);
  }
  if (!ok) {
    ESP_LOGE(TAG, "Failed to save pump settings blob to NVS");
  }

  nvs_close(handle);
  return ok;
}

bool nvs_store_clear_pump_settings(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_PUMP_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_SETTINGS_BLOB))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T1_ON))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T1_OFF))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T2_ON))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T2_OFF))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_RELAY_LOW))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_RELAY1_LOW))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_RELAY2_LOW))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T1_START))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_T2_START))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_AUTO_START))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_WASH_ON))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_WASH_OFF))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_WASH_START))
    ok = false;
  if (!erase_optional_key(handle, NVS_PUMP_KEY_WASH_AUTOSTART))
    ok = false;
  if (!erase_optional_key(handle, "w_set1_on"))
    ok = false;
  if (!erase_optional_key(handle, "w_set1_off"))
    ok = false;
  if (!erase_optional_key(handle, "w_set1_start"))
    ok = false;
  if (!erase_optional_key(handle, "w_set2_on"))
    ok = false;
  if (!erase_optional_key(handle, "w_set2_off"))
    ok = false;
  if (!erase_optional_key(handle, "w_set2_start"))
    ok = false;
  if (!erase_optional_key(handle, "w_autostart"))
    ok = false;
  if (!erase_optional_key(handle, "rel1_pol"))
    ok = false;
  if (!erase_optional_key(handle, "rel2_pol"))
    ok = false;
  if (!erase_optional_key(handle, "flt_gpio"))
    ok = false;
  if (!erase_optional_key(handle, "rel1_gpio"))
    ok = false;
  if (!erase_optional_key(handle, "rel2_gpio"))
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

void nvs_store_hardware_map_defaults(hardware_map_t *out) {
  if (out) {
    *out = hardware_map_defaults();
  }
}

static bool set_hardware_map_keys(nvs_handle_t handle,
                                  const hardware_map_t *map,
                                  const char *float_key, const char *r1_key,
                                  const char *r2_key, const char *ds_key,
                                  const char *cool_key) {
  return set_gpio_key(handle, float_key, map->float_input_gpio) &&
         set_gpio_key(handle, r1_key, map->pump_relay1_gpio) &&
         set_gpio_key(handle, r2_key, map->pump_relay2_gpio) &&
         set_gpio_key(handle, ds_key, map->ds18b20_data_gpio) &&
         set_gpio_key(handle, cool_key, map->cooling_relay_gpio);
}

static esp_err_t load_hardware_map_keys(nvs_handle_t handle,
                                        hardware_map_t *map,
                                        const char *float_key,
                                        const char *r1_key, const char *r2_key,
                                        const char *ds_key,
                                        const char *cool_key) {
  esp_err_t results[] = {
      load_gpio_key(handle, float_key, &map->float_input_gpio),
      load_gpio_key(handle, r1_key, &map->pump_relay1_gpio),
      load_gpio_key(handle, r2_key, &map->pump_relay2_gpio),
      load_gpio_key(handle, ds_key, &map->ds18b20_data_gpio),
      load_gpio_key(handle, cool_key, &map->cooling_relay_gpio),
  };

  for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
    if (results[i] != ESP_OK) {
      return results[i];
    }
  }
  return ESP_OK;
}

nvs_store_hardware_map_load_status_t
nvs_store_load_hardware_map(hardware_map_t *out) {
  if (!out) {
    return NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR;
  }

  nvs_store_hardware_map_defaults(out);

  nvs_handle_t handle;
  esp_err_t open_ret = nvs_open(NVS_HW_NAMESPACE, NVS_READONLY, &handle);
  if (open_ret == ESP_ERR_NVS_NOT_FOUND) {
    return NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING;
  }
  if (open_ret != ESP_OK) {
    ESP_LOGE(TAG, "Open %s failed: %s", NVS_HW_NAMESPACE,
             esp_err_to_name(open_ret));
    return NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR;
  }

  hardware_map_t loaded = {0};
  esp_err_t ret = load_hardware_map_keys(
      handle, &loaded, NVS_HW_KEY_ACT_FLOAT, NVS_HW_KEY_ACT_R1,
      NVS_HW_KEY_ACT_R2, NVS_HW_KEY_ACT_DS, NVS_HW_KEY_ACT_COOL);
  nvs_close(handle);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    return NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING;
  }
  if (ret != ESP_OK || !hardware_map_validate(&loaded)) {
    ESP_LOGW(TAG, "Invalid active hardware map in NVS");
    return NVS_STORE_HARDWARE_MAP_DEFAULTS_INVALID;
  }

  *out = loaded;
  return NVS_STORE_HARDWARE_MAP_LOADED;
}

bool nvs_store_save_hardware_map(const hardware_map_t *map) {
  if (!hardware_map_validate(map)) {
    return false;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_HW_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = set_hardware_map_keys(handle, map, NVS_HW_KEY_ACT_FLOAT,
                                  NVS_HW_KEY_ACT_R1, NVS_HW_KEY_ACT_R2,
                                  NVS_HW_KEY_ACT_DS, NVS_HW_KEY_ACT_COOL);
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_clear_hardware_map(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_HW_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (!erase_optional_key(handle, NVS_HW_KEY_ACT_FLOAT))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_ACT_R1))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_ACT_R2))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_ACT_DS))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_ACT_COOL))
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

nvs_store_pending_hardware_map_load_status_t
nvs_store_load_pending_hardware_map(hardware_map_t *out) {
  if (!out) {
    return NVS_STORE_PENDING_HARDWARE_MAP_ERROR;
  }
  *out = hardware_map_defaults();

  nvs_handle_t handle;
  esp_err_t open_ret = nvs_open(NVS_HW_NAMESPACE, NVS_READONLY, &handle);
  if (open_ret == ESP_ERR_NVS_NOT_FOUND) {
    return NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND;
  }
  if (open_ret != ESP_OK) {
    ESP_LOGE(TAG, "Open %s failed: %s", NVS_HW_NAMESPACE,
             esp_err_to_name(open_ret));
    return NVS_STORE_PENDING_HARDWARE_MAP_ERROR;
  }

  bool pending_valid = false;
  esp_err_t valid_ret =
      load_bool_key(handle, NVS_HW_KEY_PEND_VALID, &pending_valid);
  if (valid_ret == ESP_ERR_NVS_NOT_FOUND || !pending_valid) {
    nvs_close(handle);
    return NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND;
  }
  if (valid_ret != ESP_OK) {
    nvs_close(handle);
    return NVS_STORE_PENDING_HARDWARE_MAP_INVALID;
  }

  hardware_map_t loaded = {0};
  esp_err_t ret = load_hardware_map_keys(
      handle, &loaded, NVS_HW_KEY_PEND_FLOAT, NVS_HW_KEY_PEND_R1,
      NVS_HW_KEY_PEND_R2, NVS_HW_KEY_PEND_DS, NVS_HW_KEY_PEND_COOL);
  nvs_close(handle);
  if (ret != ESP_OK || !hardware_map_validate(&loaded)) {
    return NVS_STORE_PENDING_HARDWARE_MAP_INVALID;
  }

  *out = loaded;
  return NVS_STORE_PENDING_HARDWARE_MAP_LOADED;
}

bool nvs_store_save_pending_hardware_map(const hardware_map_t *map) {
  if (!hardware_map_validate(map)) {
    return false;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_HW_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = set_hardware_map_keys(handle, map, NVS_HW_KEY_PEND_FLOAT,
                                  NVS_HW_KEY_PEND_R1, NVS_HW_KEY_PEND_R2,
                                  NVS_HW_KEY_PEND_DS, NVS_HW_KEY_PEND_COOL);
  if (nvs_set_u8(handle, NVS_HW_KEY_PEND_VALID, 1) != ESP_OK)
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_clear_pending_hardware_map(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_HW_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_VALID))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_FLOAT))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_R1))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_R2))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_DS))
    ok = false;
  if (!erase_optional_key(handle, NVS_HW_KEY_PEND_COOL))
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_hardware_reboot_required(bool *reboot_required) {
  if (!reboot_required) {
    return false;
  }

  hardware_map_t active = {0};
  hardware_map_t pending = {0};
  nvs_store_hardware_map_load_status_t active_status =
      nvs_store_load_hardware_map(&active);
  nvs_store_pending_hardware_map_load_status_t pending_status =
      nvs_store_load_pending_hardware_map(&pending);

  if (active_status == NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR ||
      pending_status == NVS_STORE_PENDING_HARDWARE_MAP_ERROR ||
      pending_status == NVS_STORE_PENDING_HARDWARE_MAP_INVALID) {
    return false;
  }

  *reboot_required = pending_status == NVS_STORE_PENDING_HARDWARE_MAP_LOADED &&
                     hardware_map_reboot_required(&active, &pending);
  return true;
}

void nvs_store_cooling_settings_defaults(nvs_store_cooling_settings_t *out) {
  if (!out) {
    return;
  }

  out->threshold_c_x10 = APP_TEMPLATE_COOLING_THRESHOLD_C_X10;
  out->hysteresis_c_x10 = APP_TEMPLATE_COOLING_HYSTERESIS_C_X10;
  out->auto_enable = APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT;
  out->mode = HARDWARE_COOLING_MODE_FORCE_OFF;
  out->test_timeout_sec = APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC;
  out->compressor_min_off_sec = APP_TEMPLATE_COOLING_MIN_OFF_SEC;
  out->relay_polarity = APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW
                            ? HARDWARE_RELAY_ACTIVE_LOW
                            : HARDWARE_RELAY_ACTIVE_HIGH;
}

nvs_store_cooling_settings_load_status_t
nvs_store_load_cooling_settings(nvs_store_cooling_settings_t *out) {
  if (!out) {
    return NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR;
  }

  nvs_store_cooling_settings_defaults(out);

  nvs_handle_t handle;
  esp_err_t open_ret = nvs_open(NVS_COOL_NAMESPACE, NVS_READONLY, &handle);
  if (open_ret == ESP_ERR_NVS_NOT_FOUND) {
    return NVS_STORE_COOLING_SETTINGS_DEFAULTS_MISSING;
  }
  if (open_ret != ESP_OK) {
    ESP_LOGE(TAG, "Open %s failed: %s", NVS_COOL_NAMESPACE,
             esp_err_to_name(open_ret));
    return NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR;
  }

  nvs_store_cooling_settings_t loaded = {0};
  nvs_store_cooling_settings_defaults(&loaded);
  uint8_t auto_enable = 0;
  uint8_t mode = 0;
  bool relay_active_low = APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW;
  esp_err_t results[] = {
      nvs_get_i32(handle, NVS_COOL_KEY_THRESHOLD, &loaded.threshold_c_x10),
      nvs_get_i32(handle, NVS_COOL_KEY_HYST, &loaded.hysteresis_c_x10),
      nvs_get_u8(handle, NVS_COOL_KEY_AUTO_EN, &auto_enable),
      nvs_get_u8(handle, NVS_COOL_KEY_MODE, &mode),
      nvs_get_u32(handle, NVS_COOL_KEY_TEST_TMO, &loaded.test_timeout_sec),
      nvs_get_u32(handle, NVS_COOL_KEY_MIN_OFF, &loaded.compressor_min_off_sec),
      load_bool_key(handle, NVS_COOL_KEY_RELAY_LOW, &relay_active_low),
  };
  nvs_close(handle);

  for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
    if (results[i] == ESP_ERR_NVS_NOT_FOUND) {
      return NVS_STORE_COOLING_SETTINGS_DEFAULTS_MISSING;
    }
    if (results[i] != ESP_OK) {
      return NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID;
    }
  }

  if (!pump_bool_raw_valid(auto_enable)) {
    return NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID;
  }
  loaded.auto_enable = auto_enable != 0;
  loaded.mode = (hardware_cooling_mode_t)mode;
  loaded.relay_polarity =
      relay_active_low ? HARDWARE_RELAY_ACTIVE_LOW : HARDWARE_RELAY_ACTIVE_HIGH;

  if (loaded.compressor_min_off_sec < 60) {
    loaded.compressor_min_off_sec = APP_TEMPLATE_COOLING_MIN_OFF_SEC;
  }

  if (!cooling_settings_valid(&loaded)) {
    return NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID;
  }

  *out = loaded;
  return NVS_STORE_COOLING_SETTINGS_LOADED;
}

bool nvs_store_save_cooling_settings(
    const nvs_store_cooling_settings_t *settings) {
  if (!cooling_settings_valid(settings)) {
    return false;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_COOL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (nvs_set_i32(handle, NVS_COOL_KEY_THRESHOLD, settings->threshold_c_x10) !=
      ESP_OK)
    ok = false;
  if (nvs_set_i32(handle, NVS_COOL_KEY_HYST, settings->hysteresis_c_x10) !=
      ESP_OK)
    ok = false;
  if (nvs_set_u8(handle, NVS_COOL_KEY_AUTO_EN, settings->auto_enable ? 1 : 0) !=
      ESP_OK)
    ok = false;
  if (nvs_set_u8(handle, NVS_COOL_KEY_MODE, (uint8_t)settings->mode) != ESP_OK)
    ok = false;
  if (nvs_set_u32(handle, NVS_COOL_KEY_TEST_TMO, settings->test_timeout_sec) !=
      ESP_OK)
    ok = false;
  if (nvs_set_u32(handle, NVS_COOL_KEY_MIN_OFF,
                  settings->compressor_min_off_sec) != ESP_OK)
    ok = false;
  if (nvs_set_u8(handle, NVS_COOL_KEY_RELAY_LOW,
                 settings->relay_polarity == HARDWARE_RELAY_ACTIVE_LOW
                     ? 1
                     : 0) != ESP_OK)
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_clear_cooling_settings(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_COOL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;

  bool ok = true;
  if (!erase_optional_key(handle, NVS_COOL_KEY_THRESHOLD))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_HYST))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_AUTO_EN))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_MODE))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_TEST_TMO))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_MIN_OFF))
    ok = false;
  if (!erase_optional_key(handle, NVS_COOL_KEY_RELAY_LOW))
    ok = false;

  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_get_staging_type(uint8_t *type) {
  if (!type)
    return false;
  *type = 0;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  uint8_t val = 0;
  esp_err_t err = nvs_get_u8(handle, NVS_KEY_STG_TYPE, &val);
  nvs_close(handle);
  if (err == ESP_OK) {
    *type = val;
    return true;
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    *type = 0;
    return true;
  }
  return false;
}

bool nvs_store_stage_wifi(const char *ssid, const char *pass) {
  if (!ssid || ssid[0] == '\0')
    return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = true;
  if (nvs_set_u8(handle, NVS_KEY_STG_TYPE, 1) != ESP_OK)
    ok = false;
  if (nvs_set_str(handle, NVS_KEY_STG_SSID, ssid) != ESP_OK)
    ok = false;
  if (pass) {
    if (nvs_set_str(handle, NVS_KEY_STG_PASS, pass) != ESP_OK)
      ok = false;
  } else {
    if (nvs_erase_key(handle, NVS_KEY_STG_PASS) != ESP_OK)
      ok = false;
  }
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_stage_creds(const char *user, const char *pass) {
  if (!user || user[0] == '\0' || !pass || pass[0] == '\0')
    return false;
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = true;
  if (nvs_set_u8(handle, NVS_KEY_STG_TYPE, 2) != ESP_OK)
    ok = false;
  if (nvs_set_str(handle, NVS_KEY_STG_USER, user) != ESP_OK)
    ok = false;
  if (nvs_set_str(handle, NVS_KEY_STG_CRED_PASS, pass) != ESP_OK)
    ok = false;
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

bool nvs_store_rollback_staging(void) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = true;
  if (nvs_set_u8(handle, NVS_KEY_STG_TYPE, 0) != ESP_OK)
    ok = false;
  if (ok && nvs_commit(handle) != ESP_OK)
    ok = false;
  nvs_close(handle);
  return ok;
}

uint8_t nvs_store_commit_staging(void) {
  uint8_t type = 0;
  if (!nvs_store_get_staging_type(&type) || type == 0) {
    return 0;
  }

  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return 0;
  }

  if (type == 1) {
    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    esp_err_t err_ssid = nvs_get_str(handle, NVS_KEY_STG_SSID, ssid, &ssid_len);
    esp_err_t err_pass = nvs_get_str(handle, NVS_KEY_STG_PASS, pass, &pass_len);
    nvs_close(handle);

    if (err_ssid == ESP_OK) {
      nvs_store_save_wifi_profile(ssid, (err_pass == ESP_OK) ? pass : "");
    }
  } else if (type == 2) {
    char user[33] = {0};
    char pass[65] = {0};
    size_t user_len = sizeof(user);
    size_t pass_len = sizeof(pass);
    esp_err_t err_user = nvs_get_str(handle, NVS_KEY_STG_USER, user, &user_len);
    esp_err_t err_pass =
        nvs_get_str(handle, NVS_KEY_STG_CRED_PASS, pass, &pass_len);

    if (err_user == ESP_OK && err_pass == ESP_OK) {
      bool cred_ok = true;
      if (nvs_set_str(handle, "admin_user", user) != ESP_OK)
        cred_ok = false;
      if (nvs_set_str(handle, "admin_pass", pass) != ESP_OK)
        cred_ok = false;
      if (cred_ok && nvs_commit(handle) != ESP_OK)
        cred_ok = false;

      if (!cred_ok) {
        ESP_LOGE(TAG, "CRITICAL: Failed to commit staging credentials — "
                      "preserving staging state for rollback");
        nvs_close(handle);
        return 0;
      }
    }
    nvs_close(handle);
  } else {
    nvs_close(handle);
  }

  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_u8(handle, NVS_KEY_STG_TYPE, 0);
    nvs_commit(handle);
    nvs_close(handle);
  }

  return type;
}

bool nvs_store_factory_reset(void) {
  /* Wipes ALL namespaces: wifi_cfg, wifi_prof, session, pump_cfg,
   * hw_cfg, cool_cfg, disp_cfg. OTA/app partitions are NOT affected.
   * Caller must call esp_restart() after this returns true. */
  ESP_LOGW(TAG, "Factory reset: erasing entire NVS partition...");
  esp_err_t ret = nvs_flash_erase();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Factory reset NVS erase failed: %s", esp_err_to_name(ret));
    return false;
  }
  /* Re-init so NVS subsystem is in a defined state before caller restarts.
   * This allows nvs_store_set_restart_detail() to write normally after return.
   */
  if (!nvs_store_init()) {
    ESP_LOGE(TAG, "Factory reset NVS re-init failed");
    return false;
  }
  ESP_LOGI(TAG, "Factory reset NVS erase complete.");
  return true;
}

bool nvs_store_load_display_settings(uint8_t *dim_percent_out) {
  nvs_handle_t h;
  if (nvs_open(NVS_DISP_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
    return false;
  }
  uint8_t val = *dim_percent_out;
  if (nvs_get_u8(h, NVS_DISP_KEY_DIM, &val) == ESP_OK) {
    *dim_percent_out = (val <= 100) ? val : *dim_percent_out;
  }
  nvs_close(h);
  return true;
}

bool nvs_store_save_display_settings(uint8_t dim_percent) {
  nvs_handle_t h;
  if (nvs_open(NVS_DISP_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
    return false;
  esp_err_t err = nvs_set_u8(h, NVS_DISP_KEY_DIM, dim_percent);
  if (err == ESP_OK)
    nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

bool nvs_store_set_restart_detail(const char *detail) {
  if (!detail)
    return false;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "rst_detail: NVS open failed");
    return false;
  }
  esp_err_t err = nvs_set_str(h, NVS_KEY_RST_DETAIL, detail);
  if (err == ESP_OK) {
    nvs_commit(h);
  } else {
    ESP_LOGE(TAG, "rst_detail: nvs_set_str failed: %s", esp_err_to_name(err));
  }
  nvs_close(h);
  return err == ESP_OK;
}

bool nvs_store_pop_restart_detail(char *buf, size_t buf_size) {
  if (!buf || buf_size == 0)
    return false;
  buf[0] = '\0';

  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
    return false;
  }

  size_t required = 0;
  esp_err_t err = nvs_get_str(h, NVS_KEY_RST_DETAIL, NULL, &required);
  if (err == ESP_OK && required <= buf_size) {
    nvs_get_str(h, NVS_KEY_RST_DETAIL, buf, &required);
    /* Erase after reading — one-shot "last words" */
    nvs_erase_key(h, NVS_KEY_RST_DETAIL);
    nvs_commit(h);
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    /* No detail stored — normal case (power-on, external reset, etc.) */
    buf[0] = '\0';
  } else {
    ESP_LOGW(TAG,
             "rst_detail: read error or buf too small (%s, need=%u, have=%u)",
             esp_err_to_name(err), (unsigned)required, (unsigned)buf_size);
    nvs_close(h);
    return false;
  }

  nvs_close(h);
  return true;
}
