#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DRY_RUN_PRIME_SEC_MIN 5
#define DRY_RUN_PRIME_SEC_MAX 120
#define DRY_RUN_TRIP_SEC_MIN 3
#define DRY_RUN_TRIP_SEC_MAX 30
#define DRY_RUN_WAIT_MIN_MIN 1
#define DRY_RUN_WAIT_MIN_MAX 60
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

typedef enum { DRY_RUN_DISABLED = 0, DRY_RUN_MONITOR, DRY_RUN_WAIT, DRY_RUN_LOCKED } dry_run_state_t;

typedef struct {
    uint8_t state;          /* dry_run_state_t */
    uint8_t trips;
    bool alarm_active;
    bool relay_was_on;
    uint32_t on_since_ms;
    uint32_t dry_accum_ms;
    uint32_t flow_since_ms; /* 0 = no flow streak running */
    uint32_t wait_until_ms;
    uint32_t last_tick_ms;
} dry_run_rt_t;

typedef struct {
    bool relay_on;      /* pump_control water.relay_energized */
    bool flow_on;       /* flow_switch debounced */
    bool test_active;   /* measure button holds relay 1: invisible to the machine (5.7) */
    bool water_running; /* user intent: water system started */
} dry_run_input_t;

/* Bit flags: what the glue must do after a call. */
#define DRY_RUN_ACT_INHIBIT        0x01 /* set pump guard WAIT/LOCKED (per new state) */
#define DRY_RUN_ACT_RELEASE        0x02 /* set pump guard MONITOR/OFF, no restart */
#define DRY_RUN_ACT_RELEASE_RESUME 0x04 /* set pump guard MONITOR, restart water schedule from ON */
#define DRY_RUN_ACT_RAISE          0x08 /* raise/re-raise the system alarm (every trip re-alerts) */
#define DRY_RUN_ACT_CLEAR          0x10 /* clear the system alarm with out->clear_reason */
#define DRY_RUN_ACT_STATE_CHANGED  0x20 /* log one line; persist locked flag if entering/leaving LOCKED */

typedef struct {
    uint8_t actions;
    uint8_t clear_reason; /* ALARM_REASON_* value: 1 condition, 2 acked, 3 rule_changed */
    float value;          /* alarm value: trip number on RAISE, dry seconds otherwise */
} dry_run_out_t;

void dry_run_begin(dry_run_rt_t *rt, const dry_run_config_t *cfg, bool locked_at_boot, uint32_t now_ms);
void dry_run_step(const dry_run_config_t *cfg, dry_run_rt_t *rt, const dry_run_input_t *in,
                  uint32_t now_ms, dry_run_out_t *out);
void dry_run_ack(const dry_run_config_t *cfg, dry_run_rt_t *rt, bool water_running,
                 uint32_t now_ms, dry_run_out_t *out);
/* Call after saving new settings. enabled->disabled: DISABLED, RELEASE, CLEAR(rule_changed).
 * disabled->enabled: MONITOR with counters at 0. Otherwise keeps state. */
void dry_run_config_changed(const dry_run_config_t *old_cfg, const dry_run_config_t *new_cfg,
                            dry_run_rt_t *rt, uint32_t now_ms, dry_run_out_t *out);
uint32_t dry_run_wait_remaining_sec(const dry_run_rt_t *rt, uint32_t now_ms); /* 0 unless WAIT */
