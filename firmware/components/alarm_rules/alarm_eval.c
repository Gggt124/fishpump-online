#include <math.h>
#include <string.h>

#include "alarm_rules.h"

/* Verdict of one evaluation of a rule's condition. */
typedef enum {
    VERDICT_CLEAR,     /* not violating (or not applicable) */
    VERDICT_VIOLATING, /* condition holds */
    VERDICT_FAULT,     /* source is faulted: cannot tell */
    VERDICT_HOLD       /* no information this tick: change nothing */
} verdict_t;

static verdict_t evaluate(const alarm_rule_t *rule, const alarm_runtime_t *rt,
                          const telemetry_snapshot_t *snap, uint32_t now_ms, float *value_out)
{
    *value_out = 0.0f;
    const signal_def_t *sig = signal_registry_by_id(rule->signal_id);
    if (!rule->enabled || sig == NULL) {
        return VERDICT_CLEAR;
    }

    /* 1. Gate: closed gate (or unknown/absent/untrusted gate signal) = not applicable. */
    if (rule->gate_mode != ALARM_GATE_NONE) {
        const signal_def_t *gs = signal_registry_by_id(rule->gate_signal_id);
        signal_value_t g;
        if (gs == NULL) {
            return VERDICT_CLEAR;
        }
        signal_registry_read(gs, snap, now_ms, &g);
        if (!g.present || !g.trusted) {
            return VERDICT_CLEAR;
        }
        bool on = g.value != 0.0;
        if (on != (rule->gate_mode == ALARM_GATE_WHEN_TRUE)) {
            return VERDICT_CLEAR;
        }
    }

    signal_value_t v;
    signal_registry_read(sig, snap, now_ms, &v);

    /* stale: the one condition that needs no valid reading, only the age. */
    if (rule->condition == ALARM_COND_STALE) {
        return v.age_ms > (uint32_t)rule->stale_sec * 1000u ? VERDICT_VIOLATING : VERDICT_CLEAR;
    }

    /* 2. Trust. */
    if (!v.present) {
        return VERDICT_HOLD;
    }
    if (!v.trusted) {
        return v.source_fault ? VERDICT_FAULT : VERDICT_HOLD;
    }
    *value_out = (float)v.value;

    /* 3. Condition with hysteresis. Once raising has begun the release test applies. */
    bool engaged = rt->state == ALARM_STATE_PENDING || rt->state == ALARM_STATE_ACTIVE;
    float x = (float)v.value;
    float lo = rule->threshold;
    float hi = rule->threshold_hi;
    float h = rule->hysteresis;
    bool violating;
    switch (rule->condition) {
    case ALARM_COND_ABOVE:
        violating = engaged ? x > lo - h : x > lo;
        break;
    case ALARM_COND_BELOW:
        violating = engaged ? x < lo + h : x < lo;
        break;
    case ALARM_COND_OUTSIDE:
        violating = engaged ? (x < lo + h || x > hi - h) : (x < lo || x > hi);
        break;
    case ALARM_COND_INSIDE:
        violating = engaged ? (x >= lo - h && x <= hi + h) : (x >= lo && x <= hi);
        break;
    case ALARM_COND_IS_TRUE:
        violating = v.value != 0.0;
        break;
    case ALARM_COND_IS_FALSE:
        violating = v.value == 0.0;
        break;
    default:
        violating = false;
        break;
    }
    return violating ? VERDICT_VIOLATING : VERDICT_CLEAR;
}

static void emit(alarm_transition_t *out, uint8_t type, uint8_t reason, float value)
{
    out->type = type;
    out->reason = reason;
    out->value = value;
}

bool alarm_rule_step(const alarm_rule_t *rule, alarm_runtime_t *rt, const telemetry_snapshot_t *snap,
                     uint32_t now_ms, alarm_transition_t *out)
{
    float value;
    verdict_t verdict = evaluate(rule, rt, snap, now_ms, &value);

    if (verdict == VERDICT_HOLD) {
        if (rt->state == ALARM_STATE_PENDING) {
            if (!rt->holding) {
                rt->holding = true;
                rt->hold_since_ms = now_ms;
            }
            if ((uint32_t)(now_ms - rt->hold_since_ms) >= ALARM_HOLD_RESET_MS) {
                rt->state = ALARM_STATE_NORMAL;
                rt->holding = false;
            }
        }
        return false;
    }
    rt->holding = false;
    if (verdict == VERDICT_FAULT) {
        /* Cannot confirm either way: a pending alarm is dropped, an active one stays. */
        if (rt->state == ALARM_STATE_PENDING) {
            rt->state = ALARM_STATE_NORMAL;
        }
        return false;
    }

    bool violating = verdict == VERDICT_VIOLATING;
    switch (rt->state) {
    case ALARM_STATE_NORMAL:
    case ALARM_STATE_PENDING:
        if (!violating) {
            rt->state = ALARM_STATE_NORMAL;
            return false;
        }
        if (rt->state == ALARM_STATE_NORMAL) {
            rt->state = ALARM_STATE_PENDING;
            rt->pending_since_ms = now_ms;
        }
        rt->last_value = value;
        if ((uint32_t)(now_ms - rt->pending_since_ms) >= (uint32_t)rule->delay_sec * 1000u) {
            rt->state = ALARM_STATE_ACTIVE;
            rt->active_since_ms = now_ms;
            rt->acked = false;
            emit(out, ALARM_EVT_RAISED, ALARM_REASON_NONE, value);
            return true;
        }
        return false;

    case ALARM_STATE_ACTIVE:
        if (violating) {
            rt->last_value = value;
            return false;
        }
        if (rule->latch && !rt->acked) {
            rt->state = ALARM_STATE_LATCHED; /* no event: still open for the consumer */
            return false;
        }
        rt->state = ALARM_STATE_NORMAL;
        rt->acked = false;
        emit(out, ALARM_EVT_CLEARED, ALARM_REASON_CONDITION, value);
        return true;

    case ALARM_STATE_LATCHED:
    default:
        return false; /* only an acknowledge (or a rule edit) leaves this state */
    }
}

bool alarm_rule_ack(alarm_runtime_t *rt, alarm_transition_t *out)
{
    if (rt->state == ALARM_STATE_ACTIVE && !rt->acked) {
        rt->acked = true;
        emit(out, ALARM_EVT_ACKED, ALARM_REASON_NONE, rt->last_value);
        return true;
    }
    if (rt->state == ALARM_STATE_LATCHED) {
        rt->state = ALARM_STATE_NORMAL;
        rt->acked = false;
        emit(out, ALARM_EVT_CLEARED, ALARM_REASON_ACKED, rt->last_value);
        return true;
    }
    return false;
}

bool alarm_rule_reset(alarm_runtime_t *rt, alarm_transition_t *out)
{
    bool was_open = rt->state == ALARM_STATE_ACTIVE || rt->state == ALARM_STATE_LATCHED;
    float last = rt->last_value;
    memset(rt, 0, sizeof(*rt));
    if (was_open) {
        emit(out, ALARM_EVT_CLEARED, ALARM_REASON_RULE_CHANGED, last);
    }
    return was_open;
}

void alarm_counts_add(alarm_counts_t *out, const alarm_runtime_t *rt, uint8_t sev)
{
    uint8_t st = rt->state;
    if (st != ALARM_STATE_ACTIVE && st != ALARM_STATE_LATCHED) {
        return;
    }
    bool unacked = st == ALARM_STATE_LATCHED || !rt->acked;
    out->active++;
    if (sev == ALARM_SEV_CRITICAL) {
        out->critical++;
    }
    if (unacked) {
        out->unacked++;
        if (sev == ALARM_SEV_CRITICAL) {
            out->critical_unacked++;
        } else if (sev == ALARM_SEV_WARNING) {
            out->warning_unacked++;
        } else if (sev == ALARM_SEV_INFO) {
            out->info_unacked++;
        }
    }
    if (sev > out->max_severity) {
        out->max_severity = sev;
    }
}

void alarm_counts(const alarm_rule_set_t *set, const alarm_runtime_t rt[ALARM_MAX_RULES],
                  alarm_counts_t *out)
{
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < set->count && i < ALARM_MAX_RULES; i++) {
        alarm_counts_add(out, &rt[i], set->rules[i].severity);
    }
}

alarm_led_level_t alarm_counts_led_level(const alarm_counts_t *c)
{
    if (c->critical_unacked) {
        return ALARM_LED_CRITICAL;
    }
    if (c->warning_unacked) {
        return ALARM_LED_WARNING;
    }
    if (c->info_unacked) {
        return ALARM_LED_INFO;
    }
    return ALARM_LED_NONE;
}
