#include "wifi_manager.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_store.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "wifi_mgr";

#define STA_MAX_RETRY 1
#define STA_CONNECT_DELAY_MS 400

static bool s_ap_enabled = false;
static bool s_sta_connected = false;
static bool s_sta_configured = false;
static bool s_sta_connecting = false;
static bool s_sta_manual_connect_pending = false;
static bool s_sta_retry_blocked = false;
static bool s_scan_in_progress = false;
static bool s_scan_cancel_pending = false;
static bool s_ap_configured = false;
static char s_sta_ssid[33] = {0};
static char s_sta_target_ssid[33] = {0};
static char s_sta_target_password[65] = {0};
static bool s_sta_pending_save = false;
static char s_ap_ip[16] = "192.168.4.1";
static char s_sta_ip[16] = {0};
static int64_t s_boot_time_ms = 0;
static int s_sta_retry_count = 0;
static uint32_t s_sta_total_attempts =
    0; /* lifetime count; cleared only by esp_restart() or manual connect */

static uint32_t s_ap_stop_timeout_ms = AP_STOP_TMO_DEFAULT_MS;
static bool s_ap_auto_stop = AP_AUTO_STOP_DEFAULT;

static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

static wifi_scan_cb_t s_scan_callback = NULL;
static void *s_scan_user_ctx = NULL;
static wifi_scan_entry_t s_scan_results[WIFI_SCAN_MAX];
static int s_scan_count = 0;

static esp_timer_handle_t s_ap_stop_timer = NULL;
static esp_timer_handle_t s_sta_connect_timer = NULL;
static esp_timer_handle_t s_ap_timeout_timer = NULL;
static int64_t s_ap_timeout_deadline_ms = 0;

static SemaphoreHandle_t s_wifi_mutex;
static portMUX_TYPE s_wifi_diag_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
  uint32_t sta_connect_requests;
  uint32_t sta_connect_scheduled;
  uint32_t sta_connect_attempts;
  uint32_t sta_connect_start_failures;
  uint32_t sta_disconnect_requests;
  uint32_t sta_forget_requests;
  uint32_t sta_disconnected_events;
  uint32_t sta_got_ip_events;
  uint32_t ap_restore_attempts;
  uint32_t ap_restore_failures;
  uint32_t ap_client_connected_events;
  uint32_t ap_client_disconnected_events;
  uint32_t ap_ip_assigned_events;
  uint32_t scan_started;
  uint32_t scan_completed;
  uint32_t scan_cancel_requests;
  uint32_t scan_failures;
  uint32_t scan_cleanup_events;
  int last_disconnect_reason;
} wifi_manager_diag_t;

static wifi_manager_diag_t s_wifi_diag = {0};

/* --------------- Helpers --------------- */

static uint32_t diag_increment(uint32_t *counter) {
  taskENTER_CRITICAL(&s_wifi_diag_lock);
  uint32_t count = ++(*counter);
  taskEXIT_CRITICAL(&s_wifi_diag_lock);
  return count;
}

const char *wifi_auth_mode_to_string(wifi_auth_mode_t mode) {
  switch (mode) {
  case WIFI_AUTH_OPEN:
    return "Open";
  case WIFI_AUTH_WEP:
    return "WEP";
  case WIFI_AUTH_WPA_PSK:
    return "WPA";
  case WIFI_AUTH_WPA2_PSK:
    return "WPA2";
  case WIFI_AUTH_WPA_WPA2_PSK:
    return "WPA/WPA2";
  case WIFI_AUTH_WPA3_PSK:
    return "WPA3";
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return "WPA2/WPA3";
  default:
    return "Unknown";
  }
}

static void restore_sta_dhcp(void) {
  if (s_sta_netif) {
    esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
    if (err != ESP_OK) {
      ESP_LOGD(TAG, "DHCP client start returned %d", err);
    }
  }
}

static bool apply_static_ip(const wifi_sta_ip_config_t *cfg) {
  if (!cfg || !cfg->ip[0])
    return false;

  if (!cfg->gateway[0]) {
    ESP_LOGW(TAG, "Static IP ignored: gateway is required");
    return false;
  }

  if (!s_sta_netif) {
    ESP_LOGE(TAG, "STA netif not available for static IP");
    return false;
  }

  ip4_addr_t ip;
  ip4_addr_t gw;
  ip4_addr_t mask;
  ip4_addr_t dns_addr;
  const char *netmask = cfg->netmask[0] ? cfg->netmask : "255.255.255.0";
  const char *dns = cfg->dns[0] ? cfg->dns : cfg->gateway;

  if (!ip4addr_aton(cfg->ip, &ip) || !ip4addr_aton(cfg->gateway, &gw) ||
      !ip4addr_aton(netmask, &mask) || !ip4addr_aton(dns, &dns_addr)) {
    ESP_LOGW(TAG, "Static IP ignored: invalid IP/gateway/netmask/DNS value");
    return false;
  }

  esp_netif_ip_info_t ip_info;
  memset(&ip_info, 0, sizeof(ip_info));
  ip_info.ip.addr = ip.addr;
  ip_info.gw.addr = gw.addr;
  ip_info.netmask.addr = mask.addr;

  esp_netif_dhcpc_stop(s_sta_netif);
  esp_err_t err = esp_netif_set_ip_info(s_sta_netif, &ip_info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set static IP info: %d", err);
    restore_sta_dhcp();
    return false;
  }

  esp_netif_dns_info_t dns_info;
  memset(&dns_info, 0, sizeof(dns_info));
  dns_info.ip.u_addr.ip4.addr = dns_addr.addr;
  dns_info.ip.type = ESP_IPADDR_TYPE_V4;
  esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);

  ESP_LOGI(TAG, "Static IP: %s / %s gw=%s dns=%s", cfg->ip, netmask,
           cfg->gateway, dns);
  return true;
}

static void schedule_sta_connect(void);

static const char *wifi_disconnect_reason_to_string(uint8_t reason) {
  switch (reason) {
  case WIFI_REASON_AUTH_EXPIRE:
    return "auth_expire";
  case WIFI_REASON_ASSOC_LEAVE:
    return "assoc_leave";
  case WIFI_REASON_NO_AP_FOUND:
    return "no_ap_found";
  case WIFI_REASON_ASSOC_FAIL:
    return "assoc_fail";
  case WIFI_REASON_CONNECTION_FAIL:
    return "connection_fail";
  default:
    return "other";
  }
}

static void sta_connect_timer_cb(void *arg) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  bool should_connect = s_sta_configured && !s_sta_connected &&
                        !s_sta_connecting && !s_sta_retry_blocked &&
                        !s_scan_in_progress;
  if (should_connect) {
    s_sta_connecting = true;
    s_sta_manual_connect_pending = false;
  }
  xSemaphoreGive(s_wifi_mutex);

  if (!should_connect) {
    return;
  }

  uint32_t attempt = diag_increment(&s_wifi_diag.sta_connect_attempts);

  /* Increment lifetime counter and check reboot gate.
   * This counter is never reset on successful connect — only by esp_restart()
   * (which clears all RAM) or by a manual connect from the dashboard. */
  taskENTER_CRITICAL(&s_wifi_diag_lock);
  s_sta_total_attempts++;
  uint32_t total = s_sta_total_attempts;
  taskEXIT_CRITICAL(&s_wifi_diag_lock);

  if (total >= APP_CONFIG_STA_REBOOT_THRESHOLD) {
    /* Clear connecting flag before restart — defensive cleanup in case
     * esp_restart() is ever delayed or removed by a future refactor.
     * ESP_LOGW uses blocking UART output; no vTaskDelay needed to flush. */
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_sta_connecting = false;
      xSemaphoreGive(s_wifi_mutex);
    }
    ESP_LOGW(TAG,
             "[WIFI_STA] reboot gate: total_attempts=%lu >= threshold=%d. "
             "Rebooting to flush LwIP/driver state.",
             (unsigned long)total, APP_CONFIG_STA_REBOOT_THRESHOLD);
    esp_restart();
  }

  ESP_LOGI(TAG, "[WIFI_EVENT] sta_connect_attempt=%lu total=%lu",
           (unsigned long)attempt, (unsigned long)total);
  esp_err_t err = esp_wifi_connect();
  if (err == ESP_OK) {
    return;
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  s_sta_connecting = false;
  bool should_recover =
      s_sta_configured && !s_sta_retry_blocked && !s_scan_in_progress;
  xSemaphoreGive(s_wifi_mutex);

  uint32_t failures = diag_increment(&s_wifi_diag.sta_connect_start_failures);
  ESP_LOGW(TAG, "Deferred STA connect start failed: err=%s failures=%lu",
           esp_err_to_name(err), (unsigned long)failures);
  if (should_recover && err == ESP_ERR_WIFI_CONN) {
    ESP_LOGI(TAG, "Resetting busy STA state before retry");
    esp_wifi_disconnect();
    schedule_sta_connect();
  }
}

static void schedule_sta_connect(void) {
  if (!s_sta_connect_timer) {
    ESP_LOGW(TAG, "STA connect timer unavailable");
    return;
  }

  esp_timer_stop(s_sta_connect_timer);

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  int retry_count = s_sta_retry_count;
  xSemaphoreGive(s_wifi_mutex);

  uint32_t delay_ms = STA_CONNECT_DELAY_MS;
  if (retry_count > 0) {
    uint32_t backoff_seconds;
    if (retry_count >= APP_CONFIG_STA_COOLDOWN_THRESHOLD) {
      /* Long cooldown: chip rests 30 min, reducing heat accumulation and
       * the probability of LwIP/driver state leaks on repeated failures. */
      backoff_seconds = APP_CONFIG_STA_COOLDOWN_PERIOD_SEC;
      ESP_LOGI(TAG, "[WIFI_STA] cooldown active retry=%d next_attempt_in=%lus",
               retry_count, (unsigned long)backoff_seconds);
    } else {
      backoff_seconds = (1u << (retry_count - 1)); /* 1,2,4,8,16,32,64→cap */
      if (backoff_seconds > 60 || retry_count > 7) {
        backoff_seconds = 60;
      }
    }
    delay_ms = backoff_seconds * 1000;
  }

  esp_err_t err =
      esp_timer_start_once(s_sta_connect_timer, (uint64_t)delay_ms * 1000);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to schedule STA connect: %s", esp_err_to_name(err));
  } else {
    diag_increment(&s_wifi_diag.sta_connect_scheduled);
  }
}

static bool ensure_ap_started(void) { return wifi_manager_start_ap(); }

static void configure_radio_stability(const char *reason) {
  esp_err_t sta_bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
  if (sta_bw_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set STA bandwidth HT20: %s",
             esp_err_to_name(sta_bw_err));
  }

  esp_err_t ap_bw_err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
  if (ap_bw_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set AP bandwidth HT20: %s",
             esp_err_to_name(ap_bw_err));
  }

  esp_err_t power_err =
      esp_wifi_set_max_tx_power(APP_TEMPLATE_WIFI_MAX_TX_POWER_QDBM);
  if (power_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set max TX power: %s", esp_err_to_name(power_err));
    return;
  }

  int8_t tx_power = 0;
  esp_err_t tx_power_err = esp_wifi_get_max_tx_power(&tx_power);
  wifi_country_t country = {0};
  esp_err_t country_err = esp_wifi_get_country(&country);
  int country_last_channel =
      country.nchan > 0 ? country.schan + country.nchan - 1 : 0;
  ESP_LOGI(TAG,
           "[WIFI_RADIO] reason=%s APSTA_HT20=true requested_tx_power_qdbm=%d "
           "tx_power_qdbm=%d tx_power_err=%s country=%c%c country_err=%s "
           "country_max_tx_power_dbm=%d channels=%u-%u policy=%d",
           reason, APP_TEMPLATE_WIFI_MAX_TX_POWER_QDBM, tx_power,
           esp_err_to_name(tx_power_err), country.cc[0], country.cc[1],
           esp_err_to_name(country_err), country.max_tx_power, country.schan,
           country_last_channel, country.policy);
}

/* --------------- AP Password from MAC --------------- */

/**
 * Build a WPA2-compliant AP password derived from the board's base MAC address.
 * Format: "AABBCCDD" (8 uppercase hex chars, no dash).
 * Applies an XOR salt (0x5A) to prevent trivial derivation from broadcasted
 * BSSID.
 */
bool wifi_manager_build_ap_password(char *out_buf, size_t buf_len) {
  if (!out_buf || buf_len < 9) {
    ESP_LOGE(TAG, "Buffer too small for AP password");
    if (out_buf && buf_len > 0)
      out_buf[0] = '\0';
    return false;
  }

  uint8_t mac[6] = {0};
  esp_err_t err = esp_base_mac_addr_get(mac);
  if (err != ESP_OK) {
    ESP_LOGE(
        TAG,
        "CRITICAL: Failed to read base MAC (%s). Cannot generate AP password.",
        esp_err_to_name(err));
    out_buf[0] = '\0';
    return false;
  }

  const uint8_t SALT = 0x5A;
  snprintf(out_buf, buf_len, "%02X%02X%02X%02X", mac[2] ^ SALT, mac[3] ^ SALT,
           mac[4] ^ SALT, mac[5] ^ SALT);

  ESP_LOGI(TAG,
           "[AP_CFG] AP password derived from base MAC %02X:%02X:xx:xx:xx:xx "
           "(salted)",
           mac[0], mac[1]);
  return true;
}

static bool configure_ap(void) {
  char ap_password[16] = {0};
  if (!wifi_manager_build_ap_password(ap_password, sizeof(ap_password))) {
    ESP_LOGE(TAG, "Aborting AP configuration due to missing password");
    return false;
  }

  wifi_config_t ap_cfg = {
      .ap =
          {
              .ssid = APP_TEMPLATE_AP_SSID,
              .ssid_len = strlen(APP_TEMPLATE_AP_SSID),
              .channel = APP_TEMPLATE_AP_CHANNEL,
              .authmode = WIFI_AUTH_WPA2_PSK,
              .max_connection = APP_TEMPLATE_AP_MAX_CONN,
          },
  };
  strlcpy((char *)ap_cfg.ap.password, ap_password, sizeof(ap_cfg.ap.password));

  esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
    return false;
  }

  s_ap_configured = true;
  return true;
}

/* --------------- AP Auto-Stop Timer --------------- */

static void ap_stop_timer_cb(void *arg) {
  uint8_t stg_type = 0;
  if (nvs_store_get_staging_type(&stg_type) && stg_type > 0) {
    ESP_LOGI(TAG,
             "AP auto-stop timer expired, but staging is active (type=%d). "
             "Keeping AP active.",
             stg_type);
    return;
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  if (!s_sta_connected) {
    ESP_LOGI(TAG, "Skipping AP auto-stop because STA is disconnected");
    xSemaphoreGive(s_wifi_mutex);
    return;
  }

  ESP_LOGI(TAG, "AP auto-stop timer expired, stopping AP");
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK) {
    s_ap_enabled = false;
  } else {
    ESP_LOGW(TAG, "Failed to stop AP after timeout: %s", esp_err_to_name(err));
  }
  xSemaphoreGive(s_wifi_mutex);
}

static void ap_timeout_timer_cb(void *arg) {
  uint8_t stg_type = 0;
  if (nvs_store_get_staging_type(&stg_type) && stg_type > 0) {
    ESP_LOGI(TAG,
             "AP timeout expired, but staging is active (type=%d). Keeping AP "
             "active.",
             stg_type);
    wifi_manager_reset_ap_timeout();
    return;
  }

  /* Check if any client is connected to the SoftAP before shutting down.
   * We intentionally do NOT guard on s_sta_connected here — the AP can
   * close when idle even if STA has no uplink. Recovery: hold EXT_BTN
   * (APP_CONFIG_EXT_BTN_GPIO) for 2-5 s to restore AP for 5 minutes. */
  wifi_sta_list_t clients = {0};
  esp_err_t err = esp_wifi_ap_get_sta_list(&clients);
  if (err == ESP_OK && clients.num > 0) {
    ESP_LOGI(TAG,
             "AP timeout expired but %d client(s) still connected, resetting "
             "timeout",
             clients.num);
    wifi_manager_reset_ap_timeout();
    return;
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  bool sta_connected = s_sta_connected;
  ESP_LOGI(TAG,
           "AP idle timeout expired with 0 clients, shutting down SoftAP "
           "(sta_connected=%s)",
           sta_connected ? "true" : "false");
  esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
  if (ret == ESP_OK) {
    s_ap_enabled = false;
  } else {
    ESP_LOGE(TAG, "Failed to stop AP after idle timeout: %s",
             esp_err_to_name(ret));
  }
  xSemaphoreGive(s_wifi_mutex);
}

void wifi_manager_reset_ap_timeout(void) {
  if (!s_wifi_mutex)
    return;
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  bool is_ap_active =
      (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && s_ap_enabled;
  if (is_ap_active && s_ap_timeout_timer) {
    esp_timer_stop(s_ap_timeout_timer);
    esp_timer_start_once(s_ap_timeout_timer,
                         (uint64_t)APP_CONFIG_AP_IDLE_TIMEOUT_MS * 1000);
    s_ap_timeout_deadline_ms =
        (esp_timer_get_time() / 1000) + APP_CONFIG_AP_IDLE_TIMEOUT_MS;
    ESP_LOGD(TAG, "SoftAP idle timeout reset to 10 minutes");
  }
  xSemaphoreGive(s_wifi_mutex);
}

/* --------------- Event Handler --------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
        return;
      }
      bool sta_configured = s_sta_configured;
      xSemaphoreGive(s_wifi_mutex);
      if (sta_configured) {
        schedule_sta_connect();
      }
      break;
    case WIFI_EVENT_STA_DISCONNECTED: {
      wifi_event_sta_disconnected_t *ev =
          (wifi_event_sta_disconnected_t *)event_data;
      taskENTER_CRITICAL(&s_wifi_diag_lock);
      s_wifi_diag.sta_disconnected_events++;
      s_wifi_diag.last_disconnect_reason = ev->reason;
      uint32_t disconnected_events = s_wifi_diag.sta_disconnected_events;
      taskEXIT_CRITICAL(&s_wifi_diag_lock);
      if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
        return;
      }
      s_sta_connected = false;
      s_sta_connecting = false;
      s_sta_ip[0] = '\0';
      s_sta_ssid[0] = '\0';

      if (s_ap_stop_timer) {
        esp_timer_stop(s_ap_stop_timer);
      }

      bool manual_connect_pending = s_sta_manual_connect_pending;
      if (manual_connect_pending) {
        s_sta_manual_connect_pending = false;
        s_sta_retry_count = 0;
      } else if (s_sta_configured && !s_scan_in_progress) {
        s_sta_retry_count++;
      }
      bool retry_limit_reached = s_sta_retry_count > STA_MAX_RETRY;
      bool sta_configured = s_sta_configured;
      bool scan_in_progress = s_scan_in_progress;
      bool retry_blocked = s_sta_retry_blocked;
      int retry_count = s_sta_retry_count;
      xSemaphoreGive(s_wifi_mutex);
      ESP_LOGW(TAG,
               "[WIFI_EVENT] sta_disconnected reason=%d(%s) events=%lu "
               "retry=%d blocked=%s configured=%s scan=%s",
               ev->reason, wifi_disconnect_reason_to_string(ev->reason),
               (unsigned long)disconnected_events, retry_count,
               retry_blocked ? "true" : "false",
               sta_configured ? "true" : "false",
               scan_in_progress ? "true" : "false");

      if (manual_connect_pending && sta_configured && !scan_in_progress) {
        ESP_LOGI(TAG, "Scheduling requested STA connect after disconnect");
        schedule_sta_connect();
      } else if (retry_limit_reached) {
        bool was_pending_save = s_sta_pending_save;
        s_sta_pending_save = false;

        if (was_pending_save) {
          ESP_LOGI(TAG, "New Wi-Fi failed. Reverting to NVS fallback.");
          char saved_ssid[33] = {0};
          char saved_pass[65] = {0};
          if (nvs_store_load_wifi(saved_ssid, sizeof(saved_ssid), saved_pass,
                                  sizeof(saved_pass))) {
            ESP_LOGI(TAG, "Found fallback STA credentials for: %s", saved_ssid);
            wifi_config_t sta_cfg = {0};
            strlcpy((char *)sta_cfg.sta.ssid, saved_ssid, sizeof(sta_cfg.sta.ssid));
            if (saved_pass[0])
              strlcpy((char *)sta_cfg.sta.password, saved_pass, sizeof(sta_cfg.sta.password));
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

            if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
              ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__,
                       __LINE__);
              return;
            }
            s_sta_configured = true;
            s_sta_retry_count = 0;
            s_sta_retry_blocked = false;
            xSemaphoreGive(s_wifi_mutex);

            schedule_sta_connect();
          } else {
            ensure_ap_started();
          }
        } else {
          ESP_LOGI(TAG,
                   "STA retry limit (%d) reached, restoring AP as fallback but "
                   "continuing background retries",
                   STA_MAX_RETRY);
          ensure_ap_started();
          schedule_sta_connect();
        }
      } else if (sta_configured && !scan_in_progress) {
        schedule_sta_connect();
      } else if (!scan_in_progress) {
        ensure_ap_started();
      }
      break;
    }
    case WIFI_EVENT_AP_START: {
      ESP_LOGI(TAG, "[WIFI_EVENT] ap_start");
      wifi_manager_reset_ap_timeout();
      break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
      uint32_t events = diag_increment(&s_wifi_diag.ap_client_connected_events);
      ESP_LOGI(TAG, "[WIFI_EVENT] ap_client_connected events=%lu",
               (unsigned long)events);
      wifi_manager_reset_ap_timeout();
      break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
      uint32_t events =
          diag_increment(&s_wifi_diag.ap_client_disconnected_events);
      ESP_LOGI(TAG, "[WIFI_EVENT] ap_client_disconnected events=%lu",
               (unsigned long)events);
      wifi_manager_reset_ap_timeout();
      break;
    }
    case WIFI_EVENT_SCAN_DONE: {
      diag_increment(&s_wifi_diag.scan_completed);
      uint16_t num = 0;
      esp_err_t count_err = esp_wifi_scan_get_ap_num(&num);
      if (count_err != ESP_OK) {
        diag_increment(&s_wifi_diag.scan_failures);
        ESP_LOGW(TAG, "Failed to get scan result count: %s",
                 esp_err_to_name(count_err));
        num = 0;
      }
      if (num > WIFI_SCAN_MAX)
        num = WIFI_SCAN_MAX;

      wifi_ap_record_t *ap_info =
          num > 0 ? calloc(num, sizeof(wifi_ap_record_t)) : NULL;
      if (num > 0 && !ap_info) {
        diag_increment(&s_wifi_diag.scan_failures);
        ESP_LOGW(TAG, "Failed to allocate scan result buffer");
        esp_wifi_clear_ap_list();
        num = 0;
      } else if (ap_info) {
        esp_err_t records_err = esp_wifi_scan_get_ap_records(&num, ap_info);
        if (records_err != ESP_OK) {
          diag_increment(&s_wifi_diag.scan_failures);
          ESP_LOGW(TAG, "Failed to get scan results: %s",
                   esp_err_to_name(records_err));
          esp_wifi_clear_ap_list();
          num = 0;
        }
      } else {
        esp_wifi_clear_ap_list();
      }

      if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
        free(ap_info);
        return;
      }
      if (s_scan_cancel_pending) {
        s_scan_cancel_pending = false;
        xSemaphoreGive(s_wifi_mutex);
        uint32_t cleanup_events =
            diag_increment(&s_wifi_diag.scan_cleanup_events);
        ESP_LOGI(TAG, "[WIFI_EVENT] scan_cleanup_complete events=%lu",
                 (unsigned long)cleanup_events);
        free(ap_info);
        break;
      }

      if (ap_info) {
        for (int i = 0; i < num; i++) {
          strlcpy(s_scan_results[i].ssid, (char *)ap_info[i].ssid,
                  sizeof(s_scan_results[i].ssid));
          s_scan_results[i].rssi = ap_info[i].rssi;
          strlcpy(s_scan_results[i].auth,
                  wifi_auth_mode_to_string(ap_info[i].authmode),
                  sizeof(s_scan_results[i].auth));
          s_scan_results[i].channel = ap_info[i].primary;
          ESP_LOGI(TAG,
                   "[WIFI_SCAN] index=%d ssid=%s rssi=%d auth=%s channel=%u", i,
                   s_scan_results[i].ssid, s_scan_results[i].rssi,
                   s_scan_results[i].auth, s_scan_results[i].channel);
        }
      }
      s_scan_count = num;
      wifi_scan_cb_t cb = s_scan_callback;
      void *ctx = s_scan_user_ctx;
      s_scan_callback = NULL;
      s_scan_user_ctx = NULL;
      s_scan_in_progress = false;

      /*
       * Serialize callback completion with wifi_manager_cancel_scan().
       * The current callback only copies results and releases a semaphore.
       */
      if (cb) {
        cb(ctx, s_scan_results, s_scan_count);
      }
      bool reconnect_sta =
          s_sta_configured && !s_sta_connected && !s_sta_retry_blocked;
      xSemaphoreGive(s_wifi_mutex);
      free(ap_info);
      ESP_LOGI(TAG, "[WIFI_EVENT] scan_complete results=%u", num);

      if (reconnect_sta) {
        schedule_sta_connect();
      }
      break;
    }
    default:
      break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
      ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
      if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
        return;
      }
      if (!s_sta_configured) {
        xSemaphoreGive(s_wifi_mutex);
        ESP_LOGW(TAG, "Ignoring stale STA got IP after disconnect");
        esp_wifi_disconnect();
        ensure_ap_started();
        break;
      }

      snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
      s_sta_connected = true;
      s_sta_connecting = false;
      s_sta_manual_connect_pending = false;
      s_sta_retry_blocked = false;
      s_sta_retry_count = 0;
      uint32_t got_ip_events = diag_increment(&s_wifi_diag.sta_got_ip_events);
      ESP_LOGI(TAG, "[WIFI_EVENT] sta_got_ip ip=%s events=%lu", s_sta_ip,
               (unsigned long)got_ip_events);

      wifi_ap_record_t ap_info;
      if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strlcpy(s_sta_ssid, (char *)ap_info.ssid, sizeof(s_sta_ssid));
      }

      bool pending_save = s_sta_pending_save;
      char target_ssid[33];
      char target_pass[65];
      strlcpy(target_ssid, s_sta_target_ssid, sizeof(target_ssid));
      strlcpy(target_pass, s_sta_target_password, sizeof(target_pass));
      s_sta_pending_save = false;

      bool ap_enabled = s_ap_enabled;
      bool ap_auto_stop = s_ap_auto_stop;
      uint32_t ap_stop_timeout_ms = s_ap_stop_timeout_ms;
      xSemaphoreGive(s_wifi_mutex);

      if (pending_save) {
        ESP_LOGI(TAG, "Connection successful, saving to NVS");
        nvs_store_save_wifi_profile(target_ssid, target_pass);
        nvs_store_save_wifi(target_ssid, target_pass);
      }

      if (ap_enabled && ap_auto_stop && s_ap_stop_timer) {
        ESP_LOGI(TAG, "Starting AP auto-stop timer (%lu ms)",
                 (unsigned long)ap_stop_timeout_ms);
        esp_timer_start_once(s_ap_stop_timer, ap_stop_timeout_ms * 1000);
      }
      break;
    }
    case IP_EVENT_ASSIGNED_IP_TO_CLIENT: {
      uint32_t events = diag_increment(&s_wifi_diag.ap_ip_assigned_events);
      ESP_LOGI(TAG, "[WIFI_EVENT] ap_ip_assigned events=%lu",
               (unsigned long)events);
      break;
    }
    default:
      break;
    }
  }
}

/* --------------- Init --------------- */

bool wifi_manager_init(void) {
  s_boot_time_ms = esp_timer_get_time() / 1000;

  ESP_ERROR_CHECK(esp_netif_init());
  esp_err_t loop_err = esp_event_loop_create_default();
  if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_err);
  }

  s_ap_netif = esp_netif_create_default_wifi_ap();
  s_sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.wifi_task_core_id = 0;
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  uint8_t init_stg_type = 0;
  if (nvs_store_get_staging_type(&init_stg_type) && init_stg_type > 0) {
    ESP_LOGW(TAG,
             "Staging active during Wi-Fi init (type=%d). Starting SoftAP.",
             init_stg_type);
  } else {
    ESP_LOGI(TAG, "Normal boot. Starting SoftAP fallback.");
  }
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  s_ap_enabled = true;

  s_wifi_mutex = xSemaphoreCreateMutex();
  if (!s_wifi_mutex) {
    ESP_LOGE(TAG, "Failed to create Wi-Fi mutex");
    return false;
  }

  esp_timer_create_args_t sta_connect_timer_args = {
      .callback = &sta_connect_timer_cb,
      .arg = NULL,
      .name = "sta_connect_timer"};
  if (esp_timer_create(&sta_connect_timer_args, &s_sta_connect_timer) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Failed to create STA connect timer");
    return false;
  }

  /* Load AP config from NVS */
  nvs_store_load_ap_config(&s_ap_stop_timeout_ms, &s_ap_auto_stop);
  ESP_LOGI(TAG, "AP config: auto_stop=%s, timeout=%lu ms",
           s_ap_auto_stop ? "ON" : "OFF", (unsigned long)s_ap_stop_timeout_ms);

  /* Load saved STA credentials and attempt connect */
  char saved_ssid[33] = {0};
  char saved_pass[65] = {0};
  bool has_creds = false;
  uint8_t wifi_stg_type = 0;
  if (nvs_store_get_staging_type(&wifi_stg_type) && wifi_stg_type == 1) {
    nvs_handle_t handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
      size_t len = sizeof(saved_ssid);
      if (nvs_get_str(handle, "stg_sta_ssid", saved_ssid, &len) == ESP_OK) {
        len = sizeof(saved_pass);
        nvs_get_str(handle, "stg_sta_pass", saved_pass, &len);
        has_creds = true;
        ESP_LOGI(TAG, "Found staged STA credentials for SSID: %s", saved_ssid);
      }
      nvs_close(handle);
    }
  }
  if (!has_creds) {
    wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
    int count = 0, auto_idx = -1;
    if (nvs_store_load_wifi_profiles(profiles, &count, &auto_idx)) {
      if (auto_idx >= 0 && auto_idx < count && profiles[auto_idx].ssid[0]) {
        strlcpy(saved_ssid, profiles[auto_idx].ssid, sizeof(saved_ssid));
        strlcpy(saved_pass, profiles[auto_idx].pass, sizeof(saved_pass));
        has_creds = true;
        ESP_LOGI(TAG, "Found auto-connect Wi-Fi profile[%d] SSID: %s", auto_idx,
                 saved_ssid);
      } else if (count == 0) {
        /* Fallback to legacy single credential only if no profiles exist yet
         * (first boot/migration path) */
        if (nvs_store_load_wifi(saved_ssid, sizeof(saved_ssid), saved_pass,
                                sizeof(saved_pass))) {
          has_creds = true;
          ESP_LOGI(TAG, "Found legacy saved STA credentials for SSID: %s",
                   saved_ssid);
        }
      } else {
        ESP_LOGI(TAG, "Wi-Fi auto-connect is disabled (auto_idx=%d, count=%d)",
                 auto_idx, count);
      }
    }
  }
  if (has_creds) {

    /* Load optional static IP config and apply before connecting */
    wifi_sta_ip_config_t saved_ip = {0};
    if (nvs_store_load_sta_ip(saved_ip.ip, sizeof(saved_ip.ip),
                              saved_ip.gateway, sizeof(saved_ip.gateway),
                              saved_ip.netmask, sizeof(saved_ip.netmask),
                              saved_ip.dns, sizeof(saved_ip.dns))) {
      if (!apply_static_ip(&saved_ip)) {
        ESP_LOGW(TAG, "Clearing invalid saved static IP config; using DHCP");
        nvs_store_clear_sta_ip();
        restore_sta_dhcp();
      }
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, saved_ssid, sizeof(sta_cfg.sta.ssid));
    if (saved_pass[0]) {
      strlcpy((char *)sta_cfg.sta.password, saved_pass, sizeof(sta_cfg.sta.password));
    }
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to set saved STA config, will retry on connect");
    }
    s_sta_configured = true;
  }

  if (!configure_ap()) {
    return false;
  }

  ESP_ERROR_CHECK(esp_wifi_start());
  configure_radio_stability("init");

  /* Create AP auto-stop timer */
  esp_timer_create_args_t timer_args = {
      .callback = &ap_stop_timer_cb, .arg = NULL, .name = "ap_stop_timer"};
  if (esp_timer_create(&timer_args, &s_ap_stop_timer) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to create AP auto-stop timer");
    s_ap_stop_timer = NULL;
  }

  /* Create AP recovery timeout timer */
  esp_timer_create_args_t timeout_timer_args = {.callback =
                                                    &ap_timeout_timer_cb,
                                                .arg = NULL,
                                                .name = "ap_timeout_timer"};
  if (esp_timer_create(&timeout_timer_args, &s_ap_timeout_timer) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create AP timeout timer");
    return false;
  }

  return true;
}

/* --------------- AP --------------- */

bool wifi_manager_start_ap(void) {
  diag_increment(&s_wifi_diag.ap_restore_attempts);
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t mode_err = esp_wifi_get_mode(&mode);
  bool mode_changed = mode_err != ESP_OK || mode != WIFI_MODE_APSTA;

  if (mode_changed) {
    if (mode_err != ESP_OK) {
      ESP_LOGW(TAG, "Failed to read Wi-Fi mode before AP restore: %s",
               esp_err_to_name(mode_err));
    } else {
      ESP_LOGI(TAG, "Restoring SoftAP fallback from Wi-Fi mode=%d", mode);
    }

    mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (mode_err != ESP_OK) {
      diag_increment(&s_wifi_diag.ap_restore_failures);
      ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(mode_err));
      xSemaphoreGive(s_wifi_mutex);
      return false;
    }
    configure_radio_stability("ap_restore");
  }

  if (!s_ap_configured) {
    if (!configure_ap()) {
      diag_increment(&s_wifi_diag.ap_restore_failures);
      xSemaphoreGive(s_wifi_mutex);
      return false;
    }
  }

  bool log_started = !s_ap_enabled || mode_changed;
  s_ap_enabled = true;
  if (log_started) {
    ESP_LOGI(TAG, "SoftAP started: SSID=%s, IP=%s", AP_SSID, s_ap_ip);
  }
  xSemaphoreGive(s_wifi_mutex);
  return true;
}

bool wifi_manager_stop_ap(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  if (s_ap_stop_timer) {
    esp_timer_stop(s_ap_stop_timer);
  }
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
    xSemaphoreGive(s_wifi_mutex);
    return false;
  }
  s_ap_enabled = false;
  ESP_LOGI(TAG, "SoftAP stopped");
  xSemaphoreGive(s_wifi_mutex);
  return true;
}

void wifi_manager_start_recovery_ap(void) {
  if (!s_wifi_mutex)
    return;
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t err = esp_wifi_get_mode(&mode);
  bool mode_changed = err != ESP_OK || mode != WIFI_MODE_APSTA;

  if (mode_changed) {
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set APSTA mode for recovery: %s",
               esp_err_to_name(err));
      xSemaphoreGive(s_wifi_mutex);
      return;
    }
    configure_radio_stability("recovery_ap");
  }

  if (!s_ap_configured) {
    configure_ap();
  }

  bool log_started = !s_ap_enabled || mode_changed;
  s_ap_enabled = true;
  if (log_started) {
    ESP_LOGI(TAG, "SoftAP (Recovery) started: SSID=%s, IP=%s", AP_SSID,
             s_ap_ip);
  }

  if (s_ap_timeout_timer) {
    esp_timer_stop(s_ap_timeout_timer);
    esp_timer_start_once(s_ap_timeout_timer,
                         (uint64_t)APP_CONFIG_AP_RECOVERY_TIMEOUT_MS * 1000);
    s_ap_timeout_deadline_ms =
        (esp_timer_get_time() / 1000) + APP_CONFIG_AP_RECOVERY_TIMEOUT_MS;
    ESP_LOGI(TAG, "Recovery AP timeout started: %d ms",
             APP_CONFIG_AP_RECOVERY_TIMEOUT_MS);
  }

  xSemaphoreGive(s_wifi_mutex);
}

bool wifi_manager_is_ap_active(void) {
  if (!s_wifi_mutex)
    return false;
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  bool active =
      (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && s_ap_enabled;
  xSemaphoreGive(s_wifi_mutex);
  return active;
}

/* --------------- STA --------------- */

bool wifi_manager_connect_sta(const char *ssid, const char *password,
                              const wifi_sta_ip_config_t *ip_config) {
  if (!ssid || !ssid[0])
    return false;
  uint32_t requests = diag_increment(&s_wifi_diag.sta_connect_requests);
  ESP_LOGI(TAG, "[WIFI_EVENT] sta_connect_requested ssid=%s requests=%lu", ssid,
           (unsigned long)requests);
  if (ip_config && ip_config->ip[0] && !ip_config->gateway[0]) {
    ESP_LOGW(TAG, "Static IP connect rejected: gateway is required");
    return false;
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  bool scan_busy = s_scan_in_progress || s_scan_cancel_pending;
  bool connect_busy = s_sta_connecting || s_sta_manual_connect_pending;
  bool same_target =
      strncmp(s_sta_target_ssid, ssid, sizeof(s_sta_target_ssid)) == 0;
  xSemaphoreGive(s_wifi_mutex);

  if (scan_busy) {
    ESP_LOGW(TAG, "STA connect rejected while Wi-Fi scan is active");
    return false;
  }
  if (connect_busy) {
    ESP_LOGI(TAG,
             "[WIFI_EVENT] sta_connect_deduplicated ssid=%s same_target=%s",
             ssid, same_target ? "true" : "false");
    return same_target;
  }

  /* Save and apply static IP config if provided, otherwise clear it */
  if (ip_config && ip_config->ip[0]) {
    if (!apply_static_ip(ip_config)) {
      return false;
    }
    nvs_store_save_sta_ip(ip_config->ip, ip_config->gateway, ip_config->netmask,
                          ip_config->dns);
  } else {
    nvs_store_clear_sta_ip();
    restore_sta_dhcp();
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  s_sta_retry_count = 0;
  /* Reset lifetime counter — user is deliberately initiating a new connection.
   * Prevents the reboot gate from firing immediately after credential change.
   */
  taskENTER_CRITICAL(&s_wifi_diag_lock);
  s_sta_total_attempts = 0;
  taskEXIT_CRITICAL(&s_wifi_diag_lock);
  s_sta_connected = false;
  s_sta_configured = false;
  s_sta_connecting = false;
  s_sta_manual_connect_pending = true;
  s_sta_retry_blocked = false;
  s_sta_ip[0] = '\0';
  s_sta_ssid[0] = '\0';
  strlcpy(s_sta_target_ssid, ssid, sizeof(s_sta_target_ssid));
  if (password)
    strlcpy(s_sta_target_password, password, sizeof(s_sta_target_password));
  else
    s_sta_target_password[0] = '\0';
  s_sta_pending_save = true;
  xSemaphoreGive(s_wifi_mutex);

  esp_err_t disconnect_err = esp_wifi_disconnect();
  if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
    ESP_LOGW(TAG, "STA disconnect before connect returned: %s",
             esp_err_to_name(disconnect_err));
  }

  wifi_config_t sta_cfg = {0};
  strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
  if (password && password[0]) {
    strlcpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
  }
  sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set STA config");
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
      ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
      return false;
    }
    s_sta_manual_connect_pending = false;
    xSemaphoreGive(s_wifi_mutex);
    return false;
  }

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  s_sta_configured = true;
  xSemaphoreGive(s_wifi_mutex);

  schedule_sta_connect();
  ESP_LOGI(TAG, "Queued STA connect for SSID: %s", ssid);
  return true;
}

bool wifi_manager_disconnect_sta(void) {
  diag_increment(&s_wifi_diag.sta_disconnect_requests);
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  if (s_ap_stop_timer) {
    esp_timer_stop(s_ap_stop_timer);
  }
  if (s_sta_connect_timer) {
    esp_timer_stop(s_sta_connect_timer);
  }
  s_sta_connected = false;
  s_sta_configured = false;
  s_sta_connecting = false;
  s_sta_manual_connect_pending = false;
  s_sta_retry_blocked = false;
  s_sta_ip[0] = '\0';
  s_sta_ssid[0] = '\0';
  s_sta_target_ssid[0] = '\0';
  s_sta_retry_count = 0;
  xSemaphoreGive(s_wifi_mutex);

  esp_err_t ret = esp_wifi_disconnect();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "STA disconnect returned: %s", esp_err_to_name(ret));
  }

  return ensure_ap_started();
}

bool wifi_manager_forget_sta(void) {
  uint32_t requests = diag_increment(&s_wifi_diag.sta_forget_requests);
  ESP_LOGI(TAG, "[WIFI_EVENT] sta_forget_requested requests=%lu",
           (unsigned long)requests);
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  if (s_ap_stop_timer) {
    esp_timer_stop(s_ap_stop_timer);
  }
  if (s_sta_connect_timer) {
    esp_timer_stop(s_sta_connect_timer);
  }
  s_sta_connected = false;
  s_sta_configured = false;
  s_sta_connecting = false;
  s_sta_manual_connect_pending = false;
  s_sta_retry_blocked = false;
  s_sta_ip[0] = '\0';
  s_sta_ssid[0] = '\0';
  s_sta_target_ssid[0] = '\0';
  s_sta_retry_count = 0;
  xSemaphoreGive(s_wifi_mutex);
  /* Reset lifetime attempt counter — forgetting credentials is a definitive
   * user action that voids all previous retry history. */
  taskENTER_CRITICAL(&s_wifi_diag_lock);
  s_sta_total_attempts = 0;
  taskEXIT_CRITICAL(&s_wifi_diag_lock);

  esp_err_t disconnect_err = esp_wifi_disconnect();
  if (disconnect_err != ESP_OK) {
    ESP_LOGW(TAG, "STA forget disconnect returned: %s",
             esp_err_to_name(disconnect_err));
  }
  bool wifi_cleared = nvs_store_clear_wifi();
  bool static_ip_cleared = nvs_store_clear_sta_ip();
  restore_sta_dhcp();
  bool ap_started = ensure_ap_started();
  if (!wifi_cleared || !static_ip_cleared) {
    ESP_LOGW(TAG, "Failed to clear one or more saved STA settings");
  }
  return wifi_cleared && static_ip_cleared && ap_started;
}

/* --------------- Scan --------------- */

bool wifi_manager_scan(void *user_ctx, wifi_scan_cb_t callback) {
  if (!callback)
    return false;

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  if (s_scan_in_progress || s_scan_cancel_pending) {
    xSemaphoreGive(s_wifi_mutex);
    ESP_LOGW(TAG, "Wi-Fi scan already in progress or awaiting cleanup");
    return false;
  }
  if (s_sta_connecting || s_sta_manual_connect_pending) {
    xSemaphoreGive(s_wifi_mutex);
    ESP_LOGW(TAG, "Wi-Fi scan rejected while STA connect is pending");
    return false;
  }
  s_scan_callback = callback;
  s_scan_user_ctx = user_ctx;
  s_scan_count = 0;
  s_scan_in_progress = true;
  xSemaphoreGive(s_wifi_mutex);

  wifi_scan_config_t scan_cfg = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = APP_TEMPLATE_WIFI_SCAN_ACTIVE_MIN_MS,
      .scan_time.active.max = APP_TEMPLATE_WIFI_SCAN_ACTIVE_MAX_MS,
      .home_chan_dwell_time = APP_TEMPLATE_WIFI_SCAN_HOME_DWELL_MS,
  };

  esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
  if (ret != ESP_OK) {
    diag_increment(&s_wifi_diag.scan_failures);
    if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
      ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
      return false;
    }
    s_scan_callback = NULL;
    s_scan_user_ctx = NULL;
    s_scan_in_progress = false;
    bool reconnect_sta =
        s_sta_configured && !s_sta_connected && !s_sta_retry_blocked;
    xSemaphoreGive(s_wifi_mutex);
    ESP_LOGW(TAG, "Failed to start Wi-Fi scan: %s", esp_err_to_name(ret));
    if (reconnect_sta) {
      schedule_sta_connect();
    }
    return false;
  }
  uint32_t started = diag_increment(&s_wifi_diag.scan_started);
  ESP_LOGI(TAG,
           "[WIFI_EVENT] scan_started count=%lu active_min_ms=%d "
           "active_max_ms=%d home_dwell_ms=%d",
           (unsigned long)started, APP_TEMPLATE_WIFI_SCAN_ACTIVE_MIN_MS,
           APP_TEMPLATE_WIFI_SCAN_ACTIVE_MAX_MS,
           APP_TEMPLATE_WIFI_SCAN_HOME_DWELL_MS);
  return true;
}

void wifi_manager_cancel_scan(void) {
  uint32_t cancel_requests = diag_increment(&s_wifi_diag.scan_cancel_requests);
  ESP_LOGW(TAG, "[WIFI_EVENT] scan_cancel_requested count=%lu",
           (unsigned long)cancel_requests);
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  bool scan_in_progress = s_scan_in_progress;
  s_scan_callback = NULL;
  s_scan_user_ctx = NULL;
  s_scan_count = 0;
  s_scan_in_progress = false;
  if (scan_in_progress) {
    s_scan_cancel_pending = true;
  }
  bool reconnect_sta =
      s_sta_configured && !s_sta_connected && !s_sta_retry_blocked;
  xSemaphoreGive(s_wifi_mutex);

  if (scan_in_progress) {
    esp_err_t stop_err = esp_wifi_scan_stop();
    if (stop_err != ESP_OK) {
      diag_increment(&s_wifi_diag.scan_failures);
      ESP_LOGW(TAG, "Failed to stop Wi-Fi scan: %s", esp_err_to_name(stop_err));
      if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
        return;
      }
      s_scan_cancel_pending = false;
      xSemaphoreGive(s_wifi_mutex);
    }
    esp_wifi_clear_ap_list();
  }

  if (reconnect_sta) {
    schedule_sta_connect();
  }
}

/* --------------- State Getters --------------- */

bool wifi_manager_is_ap_enabled(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  bool v = s_ap_enabled;
  xSemaphoreGive(s_wifi_mutex);
  return v;
}

bool wifi_manager_is_sta_connected(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  bool v = s_sta_connected;
  xSemaphoreGive(s_wifi_mutex);
  return v;
}

bool wifi_manager_is_sta_connecting(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  bool v = s_sta_connecting || s_sta_manual_connect_pending;
  xSemaphoreGive(s_wifi_mutex);
  return v;
}

bool wifi_manager_is_sta_retry_blocked(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return false;
  }
  bool v = s_sta_retry_blocked;
  xSemaphoreGive(s_wifi_mutex);
  return v;
}

char *wifi_manager_get_ap_ip(void) { return s_ap_ip; }

char *wifi_manager_get_sta_ip(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return NULL;
  }
  xSemaphoreGive(s_wifi_mutex);
  return s_sta_ip;
}

char *wifi_manager_get_sta_ssid(void) {
  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return NULL;
  }
  xSemaphoreGive(s_wifi_mutex);
  return s_sta_ssid;
}

int64_t wifi_manager_get_uptime_ms(void) {
  return (esp_timer_get_time() / 1000) - s_boot_time_ms;
}

uint32_t wifi_manager_get_free_heap(void) { return esp_get_free_heap_size(); }

void wifi_manager_log_diagnostics(void) {
  if (!s_wifi_mutex) {
    ESP_LOGW(TAG, "[WIFI_DIAG] mutex=unavailable");
    return;
  }

  wifi_manager_diag_t diag;
  uint32_t total_attempts;
  taskENTER_CRITICAL(&s_wifi_diag_lock);
  diag = s_wifi_diag;
  total_attempts = s_sta_total_attempts;
  taskEXIT_CRITICAL(&s_wifi_diag_lock);

  bool ap_enabled;
  bool sta_connected;
  bool sta_configured;
  bool sta_connecting;
  bool manual_connect_pending;
  bool retry_blocked;
  bool scan_in_progress;
  bool scan_cancel_pending;
  int retry_count;
  char sta_ip[sizeof(s_sta_ip)];
  char sta_ssid[sizeof(s_sta_ssid)];
  char sta_target_ssid[sizeof(s_sta_target_ssid)];

  if (xSemaphoreTake(s_wifi_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Wi-Fi mutex timeout at %s:%d", __FUNCTION__, __LINE__);
    return;
  }
  ap_enabled = s_ap_enabled;
  sta_connected = s_sta_connected;
  sta_configured = s_sta_configured;
  sta_connecting = s_sta_connecting;
  manual_connect_pending = s_sta_manual_connect_pending;
  retry_blocked = s_sta_retry_blocked;
  scan_in_progress = s_scan_in_progress;
  scan_cancel_pending = s_scan_cancel_pending;
  retry_count = s_sta_retry_count;
  strlcpy(sta_ip, s_sta_ip, sizeof(sta_ip));
  strlcpy(sta_ssid, s_sta_ssid, sizeof(sta_ssid));
  strlcpy(sta_target_ssid, s_sta_target_ssid, sizeof(sta_target_ssid));
  xSemaphoreGive(s_wifi_mutex);

  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_err_t mode_err = esp_wifi_get_mode(&mode);
  wifi_sta_list_t ap_clients = {0};
  int weakest_ap_rssi = 0;
  if (ap_enabled && esp_wifi_ap_get_sta_list(&ap_clients) == ESP_OK &&
      ap_clients.num > 0) {
    weakest_ap_rssi = ap_clients.sta[0].rssi;
    for (int i = 1; i < ap_clients.num; i++) {
      if (ap_clients.sta[i].rssi < weakest_ap_rssi) {
        weakest_ap_rssi = ap_clients.sta[i].rssi;
      }
    }
  }

  int sta_rssi = 0;
  wifi_ap_record_t sta_ap = {0};
  if (sta_connected && esp_wifi_sta_get_ap_info(&sta_ap) == ESP_OK) {
    sta_rssi = sta_ap.rssi;
  }

  int8_t tx_power = 0;
  esp_err_t tx_power_err = esp_wifi_get_max_tx_power(&tx_power);
  wifi_country_t country = {0};
  esp_err_t country_err = esp_wifi_get_country(&country);
  int country_last_channel =
      country.nchan > 0 ? country.schan + country.nchan - 1 : 0;

  ESP_LOGI(
      TAG,
      "[WIFI_DIAG] mode=%d mode_err=%s ap=%s ap_clients=%u ap_weakest_rssi=%d "
      "sta_connected=%s sta_configured=%s sta_connecting=%s manual_pending=%s "
      "retry_blocked=%s sta_target=%s sta_ssid=%s sta_ip=%s sta_rssi=%d "
      "tx_power_qdbm=%d tx_power_err=%s country=%c%c country_err=%s "
      "country_max_tx_power_dbm=%d country_channels=%u-%u country_policy=%d "
      "retry=%d total_attempts=%lu scan=%s scan_cancel_pending=%s "
      "connect_req=%lu scheduled=%lu attempts=%lu start_fail=%lu "
      "disconnect_req=%lu forget_req=%lu disconnected=%lu got_ip=%lu "
      "last_reason=%d ap_restore=%lu ap_restore_fail=%lu ap_client_join=%lu "
      "ap_client_leave=%lu ap_ip=%lu scan_started=%lu scan_done=%lu "
      "scan_cancel=%lu scan_fail=%lu scan_cleanup=%lu heap=%lu heap_min=%lu "
      "largest=%lu",
      mode, esp_err_to_name(mode_err), ap_enabled ? "true" : "false",
      ap_clients.num, weakest_ap_rssi, sta_connected ? "true" : "false",
      sta_configured ? "true" : "false", sta_connecting ? "true" : "false",
      manual_connect_pending ? "true" : "false",
      retry_blocked ? "true" : "false", sta_target_ssid, sta_ssid, sta_ip,
      sta_rssi, tx_power, esp_err_to_name(tx_power_err), country.cc[0],
      country.cc[1], esp_err_to_name(country_err), country.max_tx_power,
      country.schan, country_last_channel, country.policy, retry_count,
      (unsigned long)total_attempts, scan_in_progress ? "true" : "false",
      scan_cancel_pending ? "true" : "false",
      (unsigned long)diag.sta_connect_requests,
      (unsigned long)diag.sta_connect_scheduled,
      (unsigned long)diag.sta_connect_attempts,
      (unsigned long)diag.sta_connect_start_failures,
      (unsigned long)diag.sta_disconnect_requests,
      (unsigned long)diag.sta_forget_requests,
      (unsigned long)diag.sta_disconnected_events,
      (unsigned long)diag.sta_got_ip_events, diag.last_disconnect_reason,
      (unsigned long)diag.ap_restore_attempts,
      (unsigned long)diag.ap_restore_failures,
      (unsigned long)diag.ap_client_connected_events,
      (unsigned long)diag.ap_client_disconnected_events,
      (unsigned long)diag.ap_ip_assigned_events,
      (unsigned long)diag.scan_started, (unsigned long)diag.scan_completed,
      (unsigned long)diag.scan_cancel_requests,
      (unsigned long)diag.scan_failures,
      (unsigned long)diag.scan_cleanup_events,
      (unsigned long)esp_get_free_heap_size(),
      (unsigned long)esp_get_minimum_free_heap_size(),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
