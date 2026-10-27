#pragma once

#include "app_config.h"
#include "hardware_map.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * NVS store for Wi-Fi credentials and static IP config.
 * Namespace: "wifi_cfg"
 * Keys: "sta_ssid", "sta_pass", "sta_ip", "sta_gw", "sta_netmask", "sta_dns"
 */

#define AP_STOP_TMO_DEFAULT_MS APP_TEMPLATE_AP_STOP_TMO_DEFAULT_MS
#define AP_AUTO_STOP_DEFAULT APP_TEMPLATE_AP_AUTO_STOP_DEFAULT

typedef struct {
  uint32_t timer1_on_sec;
  uint32_t timer1_off_sec;
  uint32_t timer2_on_sec;
  uint32_t timer2_off_sec;
  bool relay_active_low;
  bool relay1_active_low;
  bool relay2_active_low;
  hardware_timer_start_phase_t timer1_start_phase;
  hardware_timer_start_phase_t timer2_start_phase;
  bool auto_start;
  uint32_t min_dwell_sec;
} nvs_store_pump_settings_t;

typedef struct {
  int32_t threshold_c_x10;
  int32_t hysteresis_c_x10;
  bool auto_enable;
  hardware_cooling_mode_t mode;
  uint32_t test_timeout_sec;
  uint32_t compressor_min_off_sec;
  hardware_relay_polarity_t relay_polarity;
} nvs_store_cooling_settings_t;

typedef enum {
  NVS_STORE_PUMP_SETTINGS_LOADED = 0,
  NVS_STORE_PUMP_SETTINGS_DEFAULTS_MISSING,
  NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID,
  NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR,
} nvs_store_pump_settings_load_status_t;

typedef enum {
  NVS_STORE_HARDWARE_MAP_LOADED = 0,
  NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING,
  NVS_STORE_HARDWARE_MAP_DEFAULTS_INVALID,
  NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR,
} nvs_store_hardware_map_load_status_t;

typedef enum {
  NVS_STORE_PENDING_HARDWARE_MAP_LOADED = 0,
  NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND,
  NVS_STORE_PENDING_HARDWARE_MAP_INVALID,
  NVS_STORE_PENDING_HARDWARE_MAP_ERROR,
} nvs_store_pending_hardware_map_load_status_t;

typedef enum {
  NVS_STORE_COOLING_SETTINGS_LOADED = 0,
  NVS_STORE_COOLING_SETTINGS_DEFAULTS_MISSING,
  NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID,
  NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR,
} nvs_store_cooling_settings_load_status_t;

bool nvs_store_init(void);

bool nvs_store_get_jwt_secret(uint8_t secret[32]);
bool nvs_store_set_jwt_secret(const uint8_t secret[32]);

bool nvs_store_get_credentials(char *username_out, size_t user_len,
                               char *password_out, size_t pass_len);
bool nvs_store_set_credentials(const char *username, const char *password);

bool nvs_store_save_wifi(const char *ssid, const char *password);
bool nvs_store_load_wifi(char *ssid_out, size_t ssid_len, char *pass_out,
                         size_t pass_len);
bool nvs_store_clear_wifi(void);

/* ---- Multi-profile Wi-Fi credential management ---- */

/** Maximum number of saved Wi-Fi profiles. */
#define WIFI_PROFILE_MAX 5

/**
 * One saved Wi-Fi credential entry.
 * ssid: null-terminated, max 32 chars.
 * pass: null-terminated, max 64 chars (empty string = open network).
 */
typedef struct {
  char ssid[33];
  char pass[65];
} wifi_profile_t;

/**
 * Load all saved profiles from NVS (namespace "wifi_prof").
 * profiles: caller-allocated array of WIFI_PROFILE_MAX entries.
 * count_out: number of valid profiles stored (0..WIFI_PROFILE_MAX).
 * auto_idx_out: index of the auto-connect profile, or -1 if none.
 * Returns true on success (even when count is 0).
 */
bool nvs_store_load_wifi_profiles(wifi_profile_t *profiles, int *count_out,
                                  int *auto_idx_out);

/**
 * Add or update a profile (upsert by SSID).
 * If SSID already exists, update the password.
 * If array is full, the oldest entry (index 0) is evicted and entries are
 * shifted. Does NOT write NVS when connect fails — caller decides when to call
 * this.
 */
bool nvs_store_save_wifi_profile(const char *ssid, const char *password);

/**
 * Remove the profile matching ssid and compact the array.
 * The auto-connect index is adjusted to remain consistent.
 * Returns true on success (including when the SSID was not found).
 */
bool nvs_store_forget_wifi_profile(const char *ssid);

/**
 * Set which profile index is the auto-connect target.
 * Pass -1 to disable auto-connect.
 */
bool nvs_store_set_wifi_auto_connect(int profile_index);

/**
 * One-time migration: if legacy sta_ssid / sta_pass exist in "wifi_cfg"
 * and there are no profiles yet in "wifi_prof", copy them to profile 0
 * and set auto_idx = 0.  Safe to call on every boot.
 */
bool nvs_store_migrate_legacy_wifi(void);

bool nvs_store_save_ap_config(uint32_t stop_timeout_ms, bool auto_stop);
bool nvs_store_load_ap_config(uint32_t *stop_timeout_ms, bool *auto_stop);

bool nvs_store_save_sta_ip(const char *ip, const char *gateway,
                           const char *netmask, const char *dns);
bool nvs_store_load_sta_ip(char *ip_out, size_t ip_len, char *gw_out,
                           size_t gw_len, char *mask_out, size_t mask_len,
                           char *dns_out, size_t dns_len);
bool nvs_store_clear_sta_ip(void);

void nvs_store_pump_settings_defaults(nvs_store_pump_settings_t *out);
nvs_store_pump_settings_load_status_t
nvs_store_load_pump_settings(nvs_store_pump_settings_t *out);
bool nvs_store_save_pump_settings(const nvs_store_pump_settings_t *settings);
bool nvs_store_clear_pump_settings(void);

void nvs_store_hardware_map_defaults(hardware_map_t *out);
nvs_store_hardware_map_load_status_t
nvs_store_load_hardware_map(hardware_map_t *out);
bool nvs_store_save_hardware_map(const hardware_map_t *map);
bool nvs_store_clear_hardware_map(void);
nvs_store_pending_hardware_map_load_status_t
nvs_store_load_pending_hardware_map(hardware_map_t *out);
bool nvs_store_save_pending_hardware_map(const hardware_map_t *map);
bool nvs_store_clear_pending_hardware_map(void);
bool nvs_store_hardware_reboot_required(bool *reboot_required);

void nvs_store_cooling_settings_defaults(nvs_store_cooling_settings_t *out);
nvs_store_cooling_settings_load_status_t
nvs_store_load_cooling_settings(nvs_store_cooling_settings_t *out);
bool nvs_store_save_cooling_settings(
    const nvs_store_cooling_settings_t *settings);
bool nvs_store_clear_cooling_settings(void);
bool nvs_store_get_staging_type(uint8_t *type);
bool nvs_store_stage_wifi(const char *ssid, const char *pass);
bool nvs_store_stage_creds(const char *user, const char *pass);
bool nvs_store_rollback_staging(void);
uint8_t nvs_store_commit_staging(void);
/**
 * Factory reset: erase the entire NVS partition and re-initialize it.
 * All namespaces (wifi_cfg, pump_cfg, hw_cfg, cool_cfg, disp_cfg, session,
 * wifi_prof) are wiped. The caller MUST call esp_restart() after this returns
 * true, as in-RAM state (timers, Wi-Fi, hardware config) is now stale.
 *
 * Returns true on success (erase + re-init both OK).
 * Returns false if erase or re-init fails; device continues running as-is.
 */
bool nvs_store_factory_reset(void);

bool nvs_store_load_display_settings(uint8_t *dim_percent_out);
bool nvs_store_save_display_settings(uint8_t dim_percent);

/**
 * Save a restart reason detail string to NVS (max 31 chars + null).
 * Call immediately before esp_restart() to record why the restart happened.
 * Returns true on success.
 */
bool nvs_store_set_restart_detail(const char *detail);

/**
 * Load the restart reason detail from NVS into buf, then erase the NVS key.
 * If no detail was stored, sets buf[0] = '\0' and returns true.
 * Returns false only on NVS open/read error.
 */
bool nvs_store_pop_restart_detail(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
