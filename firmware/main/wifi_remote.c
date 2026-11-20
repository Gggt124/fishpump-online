#include "wifi_remote.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_store.h"
#include "session.h"
#include "uplink.h"

static const char *TAG = "wifi_remote";

extern atomic_bool g_cancel_rollback_timer;

void wifi_remote_on_tb_connected(void) {
  uint8_t stg_type = 0;
  if (!nvs_store_get_staging_type(&stg_type) || stg_type != 1) {
    return;
  }

  /* Read the staged SSID before the commit clears the staging. Never the password. */
  char ssid[33] = {0};
  bool have_ssid = false;
  nvs_handle_t handle;
  if (nvs_open("wifi_cfg", NVS_READONLY, &handle) == ESP_OK) {
    size_t len = sizeof(ssid);
    have_ssid = (nvs_get_str(handle, "stg_sta_ssid", ssid, &len) == ESP_OK);
    nvs_close(handle);
  }
  if (!have_ssid) {
    ESP_LOGW(TAG, "staged SSID unreadable; leaving the rollback timers running");
    return;
  }

  /* Commit only when actually connected over the staged network; otherwise the new network was never tried. */
  wifi_ap_record_t ap = {0};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || strcmp((const char *)ap.ssid, ssid) != 0) {
    ESP_LOGI(TAG, "connected on %s, staged Wi-Fi %s not tried yet; leaving it staged",
             ap.ssid[0] ? (const char *)ap.ssid : "(none)", ssid);
    return;
  }

  if (nvs_store_commit_staging() != 1) {
    ESP_LOGW(TAG, "commit of staged Wi-Fi %s failed; leaving the rollback timers running", ssid);
    return;
  }

  /* Heap, not stack: the tb_link task stack is small and set_wifi_auto_connect uses a profile array too. */
  wifi_profile_t *profiles = heap_caps_malloc(sizeof(wifi_profile_t) * WIFI_PROFILE_MAX, MALLOC_CAP_DEFAULT);
  if (profiles == NULL) {
    ESP_LOGW(TAG, "no memory to look up %s; auto-connect not set", ssid);
  } else {
    int count = 0, auto_idx = -1, idx = -1;
    if (nvs_store_load_wifi_profiles(profiles, &count, &auto_idx)) {
      for (int i = 0; i < count && i < WIFI_PROFILE_MAX; i++) {
        if (strcmp(profiles[i].ssid, ssid) == 0) {
          idx = i;
          break;
        }
      }
    }
    free(profiles);
    if (idx < 0) {
      ESP_LOGW(TAG, "%s not found in saved profiles; auto-connect not set", ssid);
    } else if (!nvs_store_set_wifi_auto_connect(idx)) {
      ESP_LOGW(TAG, "setting auto-connect to profile %d failed", idx);
    } else {
      ESP_LOGI(TAG, "auto-connect set to profile %d (%s)", idx, ssid);
    }
  }

  atomic_store(&g_cancel_rollback_timer, true);
  uplink_note_settings_changed();
  ESP_LOGI(TAG, "staged Wi-Fi %s committed by ThingsBoard connect", ssid);
}

void wifi_remote_restart_for_stage(void) {
  ESP_LOGI(TAG, "restarting to switch to the staged Wi-Fi");
  nvs_store_set_restart_detail("WIFI_REMOTE_STAGE");
  session_invalidate_all();
  esp_restart();
}
