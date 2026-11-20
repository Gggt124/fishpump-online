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
#include "nvs_store.h"
#include "session.h"
#include "uplink.h"

static const char *TAG = "wifi_remote";

extern atomic_bool g_cancel_rollback_timer;

/* Station's active password (esp_wifi_get_config); false when it cannot be read. */
static bool active_password(char *out, size_t size) {
  wifi_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return false;
  }
  size_t n = 0;
  while (n < sizeof(cfg.sta.password) && n + 1 < size && cfg.sta.password[n] != 0) {
    out[n] = (char)cfg.sta.password[n];
    n++;
  }
  out[n] = '\0';
  memset(&cfg, 0, sizeof(cfg));
  return true;
}

void wifi_remote_on_tb_connected(void) {
  uint8_t stg_type = 0;
  if (!nvs_store_get_staging_type(&stg_type) || stg_type != 1) {
    return;
  }

  char ssid[33] = {0};
  char pass[65] = {0};
  char active[65] = {0};
  if (!nvs_store_get_staged_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    ESP_LOGW(TAG, "staged SSID unreadable; leaving the rollback timers running");
    goto done;
  }

  /* Commit only when actually connected over the staged network with the staged password; otherwise the new
   * network was never tried (a failed staged password falls back to the legacy one for the same SSID). */
  wifi_ap_record_t ap = {0};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || strcmp((const char *)ap.ssid, ssid) != 0) {
    ESP_LOGI(TAG, "connected on %s, not on staged %s; leaving it staged",
             ap.ssid[0] ? (const char *)ap.ssid : "(none)", ssid);
    goto done;
  }
  if (!active_password(active, sizeof(active)) || strcmp(active, pass) != 0) {
    ESP_LOGI(TAG, "connected on %s with other credentials than the staged ones; leaving it staged", ssid);
    goto done;
  }

  /* Heap, not stack: the tb_link task stack is small. */
  wifi_profile_t *profiles = heap_caps_malloc(sizeof(wifi_profile_t) * WIFI_PROFILE_MAX, MALLOC_CAP_DEFAULT);
  if (profiles == NULL) {
    ESP_LOGW(TAG, "no memory to look up %s; leaving it staged", ssid);
    goto done;
  }
  int count = 0, old_auto = -1;
  if (!nvs_store_load_stored_wifi_profiles(profiles, &count, &old_auto)) {
    free(profiles);
    ESP_LOGW(TAG, "saved profiles unreadable; leaving %s staged", ssid);
    goto done;
  }
  free(profiles);

  /* Save the profile and make it auto-connect FIRST; only then clear the staging, so a failure anywhere leaves the
   * staging and the running timers in place and the 180 s rollback returns the board to the old network. */
  if (!nvs_store_save_wifi_profile(ssid, pass)) {
    ESP_LOGW(TAG, "saving staged Wi-Fi %s failed; leaving it staged", ssid);
    goto done;
  }
  profiles = heap_caps_malloc(sizeof(wifi_profile_t) * WIFI_PROFILE_MAX, MALLOC_CAP_DEFAULT);
  int idx = -1, new_auto = -1;
  if (profiles != NULL && nvs_store_load_stored_wifi_profiles(profiles, &count, &new_auto)) {
    for (int i = 0; i < count && i < WIFI_PROFILE_MAX; i++) {
      if (strcmp(profiles[i].ssid, ssid) == 0) {
        idx = i;
        break;
      }
    }
  }
  free(profiles);
  if (idx < 0 || !nvs_store_set_wifi_auto_connect(idx)) {
    ESP_LOGW(TAG, "setting auto-connect for %s failed; leaving it staged", ssid);
    goto done;
  }
  if (nvs_store_commit_staging() != 1) {
    ESP_LOGW(TAG, "clearing the staging for %s failed; leaving it staged", ssid);
    nvs_store_set_wifi_auto_connect(old_auto); /* undo; a profile index that moved by eviction is not restorable */
    goto done;
  }

  atomic_store(&g_cancel_rollback_timer, true);
  uplink_note_settings_changed();
  ESP_LOGI(TAG, "staged Wi-Fi %s committed by ThingsBoard connect", ssid);
  ESP_LOGI(TAG, "auto-connect set to profile %d (%s)", idx, ssid);

done:
  memset(pass, 0, sizeof(pass));
  memset(active, 0, sizeof(active));
}

void wifi_remote_restart_for_stage(void) {
  ESP_LOGI(TAG, "restarting to switch to the staged Wi-Fi");
  nvs_store_set_restart_detail("WIFI_REMOTE_STAGE");
  session_invalidate_all();
  esp_restart();
}
