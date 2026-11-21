#pragma once

#include "app_config.h"
#include "esp_wifi_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AP_SSID APP_TEMPLATE_AP_SSID
#define AP_MAX_CONN APP_TEMPLATE_AP_MAX_CONN
#define WIFI_SCAN_MAX APP_TEMPLATE_WIFI_SCAN_MAX

typedef struct {
  char ssid[33];
  int8_t rssi;
  char auth[16];
  uint8_t channel;
} wifi_scan_entry_t;

typedef void (*wifi_scan_cb_t)(void *user_ctx, wifi_scan_entry_t *entries,
                               int count);

typedef struct {
  char ip[16];
  char gateway[16];
  char netmask[16];
  char dns[16];
} wifi_sta_ip_config_t;

bool wifi_manager_init(void);
bool wifi_manager_start_ap(void);
bool wifi_manager_stop_ap(void);
bool wifi_manager_connect_sta(const char *ssid, const char *password,
                              const wifi_sta_ip_config_t *ip_config);
bool wifi_manager_disconnect_sta(void);
bool wifi_manager_forget_sta(void);
bool wifi_manager_scan(void *user_ctx, wifi_scan_cb_t callback);
void wifi_manager_cancel_scan(void);

bool wifi_manager_is_ap_enabled(void);
bool wifi_manager_is_sta_connected(void);
bool wifi_manager_is_sta_connecting(void);
bool wifi_manager_is_sta_retry_blocked(void);
char *wifi_manager_get_ap_ip(void);
char *wifi_manager_get_sta_ip(void);
char *wifi_manager_get_sta_ssid(void);
int64_t wifi_manager_get_uptime_ms(void);
uint32_t wifi_manager_get_free_heap(void);
void wifi_manager_log_diagnostics(void);

const char *wifi_auth_mode_to_string(wifi_auth_mode_t mode);

void wifi_manager_start_recovery_ap(void);
void wifi_manager_reset_ap_timeout(void);
bool wifi_manager_is_ap_active(void);
bool wifi_manager_build_ap_password(char *out_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
