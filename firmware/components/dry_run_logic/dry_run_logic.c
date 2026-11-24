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

void dry_run_limits(dry_run_limits_t *out)
{
    out->prime_min = DRY_RUN_PRIME_SEC_MIN;
    out->prime_max = DRY_RUN_PRIME_SEC_MAX;
    out->trip_min = DRY_RUN_TRIP_SEC_MIN;
    out->trip_max = DRY_RUN_TRIP_SEC_MAX;
    out->wait_min_min = DRY_RUN_WAIT_MIN_MIN;
    out->wait_min_max = DRY_RUN_WAIT_MIN_MAX;
    out->retries_max = DRY_RUN_RETRIES_MAX;
}

bool dry_run_config_clamp(dry_run_config_t *cfg)
{
    bool changed = false;
    if (cfg->prime_sec < DRY_RUN_PRIME_SEC_MIN) {
        cfg->prime_sec = DRY_RUN_PRIME_SEC_MIN;
        changed = true;
    } else if (cfg->prime_sec > DRY_RUN_PRIME_SEC_MAX) {
        cfg->prime_sec = DRY_RUN_PRIME_SEC_MAX;
        changed = true;
    }
    if (cfg->dry_trip_sec < DRY_RUN_TRIP_SEC_MIN) {
        cfg->dry_trip_sec = DRY_RUN_TRIP_SEC_MIN;
        changed = true;
    } else if (cfg->dry_trip_sec > DRY_RUN_TRIP_SEC_MAX) {
        cfg->dry_trip_sec = DRY_RUN_TRIP_SEC_MAX;
        changed = true;
    }
    if (cfg->retry_wait_min < DRY_RUN_WAIT_MIN_MIN) {
        cfg->retry_wait_min = DRY_RUN_WAIT_MIN_MIN;
        changed = true;
    } else if (cfg->retry_wait_min > DRY_RUN_WAIT_MIN_MAX) {
        cfg->retry_wait_min = DRY_RUN_WAIT_MIN_MAX;
        changed = true;
    }
    if (cfg->max_retries > DRY_RUN_RETRIES_MAX) {
        cfg->max_retries = DRY_RUN_RETRIES_MAX;
        changed = true;
    }
    return changed;
}

const char *dry_run_cfg_source_name(uint8_t source)
{
    switch (source) {
    case DRY_RUN_CFG_STORED: return "stored";
    case DRY_RUN_CFG_CLAMPED: return "clamped";
    case DRY_RUN_CFG_DEFAULTS: return "defaults";
    case DRY_RUN_CFG_DEFAULTS_AFTER_ERROR: return "defaults_after_error";
    default: return "?";
    }
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

/* clear_reason values mirror ALARM_REASON_*; literals keep this component free of alarm_rules. */
#define REASON_CONDITION 1
#define REASON_ACKED 2
#define REASON_RULE_CHANGED 3

static void out_zero(dry_run_out_t *out)
{
    out->actions = 0;
    out->clear_reason = 0;
    out->value = 0.0f;
}

static void rt_reset_counters(dry_run_rt_t *rt, uint32_t now_ms)
{
    rt->trips = 0;
    rt->dry_accum_ms = 0;
    rt->flow_since_ms = 0;
    rt->wait_until_ms = 0;
    rt->relay_was_on = false;
    rt->on_since_ms = 0;
    rt->last_tick_ms = now_ms;
}

void dry_run_begin(dry_run_rt_t *rt, const dry_run_config_t *cfg, bool locked_at_boot, uint32_t now_ms)
{
    rt->alarm_active = false;
    rt_reset_counters(rt, now_ms);
    if (!cfg->enabled) {
        rt->state = DRY_RUN_DISABLED;
    } else if (locked_at_boot) {
        rt->state = DRY_RUN_LOCKED;
        rt->alarm_active = true;
        rt->trips = (uint8_t)(cfg->max_retries + 1);
    } else {
        rt->state = DRY_RUN_MONITOR;
    }
}

void dry_run_step(const dry_run_config_t *cfg, dry_run_rt_t *rt, const dry_run_input_t *in,
                  uint32_t now_ms, dry_run_out_t *out)
{
    out_zero(out);
    uint32_t dt = now_ms - rt->last_tick_ms;
    rt->last_tick_ms = now_ms;
    if (rt->state == DRY_RUN_DISABLED) {
        return;
    }

    /* the measure button's relay-on is invisible (spec 5.7) */
    bool relay = in->relay_on && !in->test_active;
    if (relay && !rt->relay_was_on) {
        rt->on_since_ms = now_ms;
    }
    rt->relay_was_on = relay;

    if (rt->state == DRY_RUN_WAIT) {
        if ((int32_t)(now_ms - rt->wait_until_ms) >= 0) {
            rt->state = DRY_RUN_MONITOR;
            rt->dry_accum_ms = 0;
            rt->flow_since_ms = 0;
            rt->relay_was_on = false; /* the restart must be seen as a rising edge */
            out->actions |= (in->water_running ? DRY_RUN_ACT_RELEASE_RESUME : DRY_RUN_ACT_RELEASE) |
                            DRY_RUN_ACT_STATE_CHANGED;
        }
        return;
    }
    if (rt->state != DRY_RUN_MONITOR) {
        return; /* LOCKED: only an acknowledge leaves it */
    }

    if (relay && in->flow_on) {
        rt->dry_accum_ms = 0;
        if (rt->flow_since_ms == 0) {
            rt->flow_since_ms = now_ms != 0 ? now_ms : 1; /* 0 means "no streak" */
        }
        if (rt->alarm_active && (now_ms - rt->flow_since_ms) >= DRY_RUN_FLOW_CONFIRM_MS) {
            rt->trips = 0;
            rt->alarm_active = false;
            out->actions |= DRY_RUN_ACT_CLEAR;
            out->clear_reason = REASON_CONDITION;
        }
        return;
    }
    rt->flow_since_ms = 0;

    if (relay && (now_ms - rt->on_since_ms) >= (uint32_t)cfg->prime_sec * 1000u) {
        rt->dry_accum_ms += dt;
    }

    if (rt->dry_accum_ms >= (uint32_t)cfg->dry_trip_sec * 1000u) {
        rt->trips++;
        rt->dry_accum_ms = 0;
        rt->alarm_active = true;
        if (rt->trips > cfg->max_retries) {
            rt->state = DRY_RUN_LOCKED;
        } else {
            rt->state = DRY_RUN_WAIT;
            rt->wait_until_ms = now_ms + dry_run_wait_ms(cfg, rt->trips);
        }
        out->actions |= DRY_RUN_ACT_INHIBIT | DRY_RUN_ACT_RAISE | DRY_RUN_ACT_STATE_CHANGED;
        out->value = (float)rt->trips;
    }
}

void dry_run_ack(const dry_run_config_t *cfg, dry_run_rt_t *rt, bool water_running,
                 uint32_t now_ms, dry_run_out_t *out)
{
    (void)cfg;
    out_zero(out);
    if (rt->state == DRY_RUN_DISABLED || !rt->alarm_active) {
        return;
    }
    bool was_held = rt->state == DRY_RUN_WAIT || rt->state == DRY_RUN_LOCKED;
    out->value = (float)(rt->dry_accum_ms / 1000u);
    rt->trips = 0;
    rt->dry_accum_ms = 0;
    rt->flow_since_ms = 0;
    rt->alarm_active = false;
    rt->state = DRY_RUN_MONITOR;
    rt->last_tick_ms = now_ms;
    if (was_held) {
        rt->relay_was_on = false; /* restart is seen as a rising edge */
        out->actions |= (water_running ? DRY_RUN_ACT_RELEASE_RESUME : DRY_RUN_ACT_RELEASE) |
                        DRY_RUN_ACT_STATE_CHANGED;
    }
    out->actions |= DRY_RUN_ACT_CLEAR;
    out->clear_reason = REASON_ACKED;
}

void dry_run_config_changed(const dry_run_config_t *old_cfg, const dry_run_config_t *new_cfg,
                            dry_run_rt_t *rt, uint32_t now_ms, dry_run_out_t *out)
{
    out_zero(out);
    if (old_cfg->enabled && !new_cfg->enabled) {
        if (rt->alarm_active) {
            out->actions |= DRY_RUN_ACT_CLEAR;
            out->clear_reason = REASON_RULE_CHANGED;
        }
        out->actions |= DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_STATE_CHANGED;
        rt->state = DRY_RUN_DISABLED;
        rt->alarm_active = false;
        rt_reset_counters(rt, now_ms);
    } else if (!old_cfg->enabled && new_cfg->enabled) {
        rt->state = DRY_RUN_MONITOR;
        rt->alarm_active = false;
        rt_reset_counters(rt, now_ms);
        out->actions |= DRY_RUN_ACT_STATE_CHANGED;
    }
}

uint32_t dry_run_wait_remaining_sec(const dry_run_rt_t *rt, uint32_t now_ms)
{
    if (rt->state != DRY_RUN_WAIT) {
        return 0;
    }
    int32_t left = (int32_t)(rt->wait_until_ms - now_ms);
    return left > 0 ? ((uint32_t)left + 999u) / 1000u : 0;
}
