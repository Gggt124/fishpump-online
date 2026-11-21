#include <math.h>
#include <string.h>

#include "alarm_rules.h"

#define ALARM_DELAY_MAX_SEC 3600
#define ALARM_STALE_MAX_SEC 3600
#define ALARM_STALE_MIN_SEC 5

/* Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. */
static bool utf8_valid(const char *s, size_t len)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= len || (p[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false;
            if (c == 0xE0 && p[i + 1] < 0xA0) return false;  /* overlong */
            if (c == 0xED && p[i + 1] >= 0xA0) return false; /* surrogate */
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= len || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80 ||
                (p[i + 3] & 0xC0) != 0x80) return false;
            if (c == 0xF0 && p[i + 1] < 0x90) return false; /* overlong */
            if (c == 0xF4 && p[i + 1] >= 0x90) return false; /* > U+10FFFF */
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

static bool name_valid(const char *name)
{
    size_t len = strnlen(name, ALARM_NAME_BYTES);
    if (len == 0 || len >= ALARM_NAME_BYTES) {
        return false; /* empty, or not NUL-terminated within 96 bytes */
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c < 0x20 || c == 0x7F || c == '"' || c == '\\') {
            return false;
        }
    }
    return utf8_valid(name, len);
}

#define FAIL(field)          \
    do {                     \
        *bad_field = (field); \
        return false;        \
    } while (0)

bool alarm_rule_validate(const alarm_rule_t *rule, const char **bad_field)
{
    const char *dummy;
    if (bad_field == NULL) {
        bad_field = &dummy;
    }
    *bad_field = "";

    if (!name_valid(rule->name)) FAIL("name");

    const signal_def_t *sig = signal_registry_by_id(rule->signal_id);
    if (sig == NULL) FAIL("signal");

    if (rule->condition >= ALARM_COND_COUNT) FAIL("condition");
    bool is_range = false;
    switch (rule->condition) {
    case ALARM_COND_ABOVE:
    case ALARM_COND_BELOW:
        if (sig->kind != SIGNAL_KIND_NUMBER) FAIL("condition");
        if (!isfinite(rule->threshold)) FAIL("threshold");
        break;
    case ALARM_COND_OUTSIDE:
    case ALARM_COND_INSIDE:
        if (sig->kind != SIGNAL_KIND_NUMBER) FAIL("condition");
        if (!isfinite(rule->threshold)) FAIL("lo");
        if (!isfinite(rule->threshold_hi)) FAIL("hi");
        if (!(rule->threshold < rule->threshold_hi)) FAIL("hi");
        is_range = true;
        break;
    case ALARM_COND_IS_TRUE:
    case ALARM_COND_IS_FALSE:
        if (sig->kind != SIGNAL_KIND_BOOL) FAIL("condition");
        break;
    case ALARM_COND_STALE:
        if (sig->period_sec == 0) FAIL("condition");
        break;
    default:
        FAIL("condition");
    }

    if (!isfinite(rule->hysteresis) || rule->hysteresis < 0.0f) FAIL("hysteresis");
    if (is_range &&
        !((double)rule->hysteresis < ((double)rule->threshold_hi - (double)rule->threshold) / 2.0)) {
        FAIL("hysteresis");
    }

    if (rule->delay_sec > ALARM_DELAY_MAX_SEC) FAIL("delay_sec");

    if (rule->condition == ALARM_COND_STALE) {
        unsigned min_sec = 3u * sig->period_sec;
        if (min_sec < ALARM_STALE_MIN_SEC) {
            min_sec = ALARM_STALE_MIN_SEC;
        }
        if (rule->stale_sec < min_sec || rule->stale_sec > ALARM_STALE_MAX_SEC) FAIL("stale_sec");
    }

    if (rule->severity < ALARM_SEV_INFO || rule->severity > ALARM_SEV_CRITICAL) FAIL("severity");

    if (rule->gate_mode > ALARM_GATE_WHEN_FALSE) FAIL("gate");
    if (rule->gate_mode != ALARM_GATE_NONE) {
        const signal_def_t *g = signal_registry_by_id(rule->gate_signal_id);
        if (g == NULL || g->kind != SIGNAL_KIND_BOOL || g->id == rule->signal_id) FAIL("gate");
    }
    return true;
}
