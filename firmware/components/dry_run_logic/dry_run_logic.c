#include "dry_run_logic.h"

void dry_run_config_defaults(dry_run_config_t *out)
{
    out->enabled = false;
    out->prime_sec = 15;
    out->dry_trip_sec = 10;
    out->retry_wait_min = 5;
    out->retry_backoff = true;
    out->max_retries = 3;
}

bool dry_run_config_validate(const dry_run_config_t *cfg, const char **bad_field)
{
    const char *bad = NULL;
    if (cfg->prime_sec < DRY_RUN_PRIME_SEC_MIN || cfg->prime_sec > DRY_RUN_PRIME_SEC_MAX) {
        bad = "prime_sec";
    } else if (cfg->dry_trip_sec < DRY_RUN_TRIP_SEC_MIN || cfg->dry_trip_sec > DRY_RUN_TRIP_SEC_MAX) {
        bad = "dry_trip_sec";
    } else if (cfg->retry_wait_min < DRY_RUN_WAIT_MIN_MIN || cfg->retry_wait_min > DRY_RUN_WAIT_MIN_MAX) {
        bad = "retry_wait_min";
    } else if (cfg->max_retries > DRY_RUN_RETRIES_MAX) {
        bad = "max_retries";
    }
    if (bad != NULL) {
        if (bad_field != NULL) {
            *bad_field = bad;
        }
        return false;
    }
    return true;
}

uint32_t dry_run_wait_ms(const dry_run_config_t *cfg, uint8_t trip)
{
    uint32_t min = cfg->retry_wait_min;
    if (min > DRY_RUN_WAIT_MIN_MAX) {
        min = DRY_RUN_WAIT_MIN_MAX;
    }
    if (cfg->retry_backoff) {
        for (uint8_t i = 1; i < trip && min < DRY_RUN_WAIT_MIN_MAX; i++) {
            min *= 2;
        }
        if (min > DRY_RUN_WAIT_MIN_MAX) {
            min = DRY_RUN_WAIT_MIN_MAX;
        }
    }
    return min * 60000u;
}

uint16_t dry_run_suggest_prime_sec(uint32_t measured_ms)
{
    /* ceil(ms * 1.5 / 1000) = ceil(ms * 3 / 2000), in 64-bit to avoid overflow */
    uint64_t sec = ((uint64_t)measured_ms * 3u + 1999u) / 2000u + 5u;
    if (sec < DRY_RUN_PRIME_SEC_MIN) {
        return DRY_RUN_PRIME_SEC_MIN;
    }
    if (sec > DRY_RUN_PRIME_SEC_MAX) {
        return DRY_RUN_PRIME_SEC_MAX;
    }
    return (uint16_t)sec;
}
