#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DRY_RUN_PRIME_SEC_MIN 1
#define DRY_RUN_PRIME_SEC_MAX 600
#define DRY_RUN_TRIP_SEC_MIN 3
#define DRY_RUN_TRIP_SEC_MAX 120
#define DRY_RUN_WAIT_MIN_MIN 1
#define DRY_RUN_WAIT_MIN_MAX 240
#define DRY_RUN_RETRIES_MAX 5
#define DRY_RUN_FLOW_CONFIRM_MS 5000u
#define DRY_RUN_MEASURE_TIMEOUT_MS 120000u

typedef struct {
    bool enabled;            /* default false */
    uint16_t prime_sec;      /* default 15 */
    uint16_t dry_trip_sec;   /* default 10 */
    uint16_t retry_wait_min; /* default 5 */
    bool retry_backoff;      /* default true */
    uint8_t max_retries;     /* default 3 */
} dry_run_config_t;

void dry_run_config_defaults(dry_run_config_t *out);

/* false + *bad_field = JSON field name ("prime_sec", ...) on the first bad value */
bool dry_run_config_validate(const dry_run_config_t *cfg, const char **bad_field);

/* Wait after trip number `trip` (1-based): retry_wait_min, doubled per earlier trip when
 * retry_backoff, capped at DRY_RUN_WAIT_MIN_MAX minutes. Returns ms. */
uint32_t dry_run_wait_ms(const dry_run_config_t *cfg, uint8_t trip);

/* ceil(measured_ms / 1000 * 1.5) + 5, clamped to [PRIME_SEC_MIN, PRIME_SEC_MAX]. */
uint16_t dry_run_suggest_prime_sec(uint32_t measured_ms);
