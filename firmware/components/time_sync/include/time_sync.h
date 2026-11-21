#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2026-01-01T00:00:00Z: earlier "synced" times are treated as bogus. */
#define TIME_SYNC_MIN_PLAUSIBLE_UNIX_MS 1767225600000LL

/* Start SNTP (pool.ntp.org, time.google.com). Non-blocking. A second call
 * returns ESP_ERR_INVALID_STATE. Requires the netif/event loop to exist. */
esp_err_t time_sync_init(void);

/* 0 until the first plausible sync; then wall-clock unix time in ms. */
int64_t time_sync_now_unix_ms(void);

bool time_sync_is_set(void);

#ifdef __cplusplus
}
#endif
