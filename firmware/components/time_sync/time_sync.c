#include "time_sync.h"

#include <stdatomic.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "uplink.h"

static const char *TAG = "time_sync";

static atomic_bool s_set = false;
static bool s_inited = false;

bool time_sync_is_set(void) { return atomic_load(&s_set); }

int64_t time_sync_now_unix_ms(void) {
  if (!atomic_load(&s_set)) {
    return 0;
  }
  struct timeval tv;
  if (gettimeofday(&tv, NULL) != 0) {
    return 0;
  }
  return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

/* Runs in the lwIP/SNTP context: keep it short. */
static void sync_cb(struct timeval *tv) {
  int64_t ms = (int64_t)tv->tv_sec * 1000LL + tv->tv_usec / 1000;
  if (ms < TIME_SYNC_MIN_PLAUSIBLE_UNIX_MS) {
    return;
  }
  bool expected = false;
  if (!atomic_compare_exchange_strong(&s_set, &expected, true)) {
    return; /* already set: later syncs are silent */
  }
  uplink_set_wall_clock(time_sync_now_unix_ms);

  time_t t = (time_t)tv->tv_sec;
  struct tm tm_utc;
  char buf[24];
  gmtime_r(&t, &tm_utc);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  ESP_LOGI(TAG, "time set: %s", buf);
}

esp_err_t time_sync_init(void) {
  if (s_inited) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
      2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
  cfg.sync_cb = sync_cb;
  esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err == ESP_OK) {
    s_inited = true;
  }
  return err;
}
