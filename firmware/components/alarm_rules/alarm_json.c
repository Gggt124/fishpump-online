#include <float.h>
#include <math.h>
#include <string.h>

#include "alarm_rules.h"

static const char *const s_cond_names[ALARM_COND_COUNT] = {
    "above", "below", "outside", "inside", "is_true", "is_false", "stale"};

const char *alarm_cond_name(uint8_t cond)
{
    return cond < ALARM_COND_COUNT ? s_cond_names[cond] : "unknown";
}

const char *alarm_severity_name(uint8_t sev)
{
    switch (sev) {
    case ALARM_SEV_INFO: return "info";
    case ALARM_SEV_WARNING: return "warning";
    case ALARM_SEV_CRITICAL: return "critical";
    default: return "unknown";
    }
}

typedef struct {
    const cJSON *obj;
    const char **bad;
} ctx_t;

static alarm_json_err_t fail(ctx_t *c, const char *field, alarm_json_err_t e)
{
    *c->bad = field;
    return e;
}

/* Required string field. */
static alarm_json_err_t get_string(ctx_t *c, const char *key, const char **out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(c->obj, key);
    if (it == NULL) return fail(c, key, ALARM_JSON_MISSING_FIELD);
    if (!cJSON_IsString(it) || it->valuestring == NULL) return fail(c, key, ALARM_JSON_INVALID_TYPE);
    *out = it->valuestring;
    return ALARM_JSON_OK;
}

/* Float field. absent -> *present false (or MISSING_FIELD when required). */
static alarm_json_err_t get_float(ctx_t *c, const char *key, bool required, float *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(c->obj, key);
    if (it == NULL) {
        return required ? fail(c, key, ALARM_JSON_MISSING_FIELD) : ALARM_JSON_OK;
    }
    if (!cJSON_IsNumber(it)) return fail(c, key, ALARM_JSON_INVALID_TYPE);
    double d = it->valuedouble;
    if (!isfinite(d) || fabs(d) > (double)FLT_MAX) return fail(c, key, ALARM_JSON_INVALID_VALUE);
    *out = (float)d;
    return ALARM_JSON_OK;
}

static alarm_json_err_t get_uint(ctx_t *c, const char *key, bool required, unsigned max, unsigned *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(c->obj, key);
    if (it == NULL) {
        return required ? fail(c, key, ALARM_JSON_MISSING_FIELD) : ALARM_JSON_OK;
    }
    if (!cJSON_IsNumber(it)) return fail(c, key, ALARM_JSON_INVALID_TYPE);
    double d = it->valuedouble;
    if (!isfinite(d) || d < 0.0 || d > (double)max || d != floor(d)) {
        return fail(c, key, ALARM_JSON_INVALID_VALUE);
    }
    *out = (unsigned)d;
    return ALARM_JSON_OK;
}

static alarm_json_err_t get_bool(ctx_t *c, const char *key, bool *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(c->obj, key);
    if (it == NULL) return ALARM_JSON_OK;
    if (!cJSON_IsBool(it)) return fail(c, key, ALARM_JSON_INVALID_TYPE);
    *out = cJSON_IsTrue(it);
    return ALARM_JSON_OK;
}

#define TRY(expr)                              \
    do {                                       \
        alarm_json_err_t e_ = (expr);          \
        if (e_ != ALARM_JSON_OK) return e_;    \
    } while (0)

alarm_json_err_t alarm_rule_from_json(const cJSON *obj, alarm_rule_t *out, bool *has_id,
                                      const char **bad_field)
{
    const char *dummy;
    bool dummy_id;
    if (bad_field == NULL) bad_field = &dummy;
    if (has_id == NULL) has_id = &dummy_id;
    *bad_field = "";
    *has_id = false;
    memset(out, 0, sizeof(*out));
    ctx_t c = {obj, bad_field};

    if (!cJSON_IsObject(obj)) return fail(&c, "body", ALARM_JSON_INVALID_TYPE);

    unsigned u;
    if (cJSON_GetObjectItemCaseSensitive(obj, "id") != NULL) {
        TRY(get_uint(&c, "id", false, 65535, &u));
        if (u == 0 || u >= ALARM_SYSTEM_ID_BASE) return fail(&c, "id", ALARM_JSON_INVALID_VALUE);
        out->id = (uint16_t)u;
        *has_id = true;
    }

    const char *s;
    TRY(get_string(&c, "name", &s));
    if (strlen(s) >= ALARM_NAME_BYTES) return fail(&c, "name", ALARM_JSON_INVALID_VALUE);
    strcpy(out->name, s);

    TRY(get_string(&c, "signal", &s));
    const signal_def_t *sig = signal_registry_by_key(s);
    if (sig == NULL) return fail(&c, "signal", ALARM_JSON_INVALID_VALUE);
    out->signal_id = sig->id;

    TRY(get_string(&c, "condition", &s));
    out->condition = ALARM_COND_COUNT;
    for (uint8_t i = 0; i < ALARM_COND_COUNT; i++) {
        if (strcmp(s, s_cond_names[i]) == 0) {
            out->condition = i;
            break;
        }
    }
    if (out->condition == ALARM_COND_COUNT) return fail(&c, "condition", ALARM_JSON_INVALID_VALUE);

    TRY(get_string(&c, "severity", &s));
    out->severity = 0;
    for (uint8_t v = ALARM_SEV_INFO; v <= ALARM_SEV_CRITICAL; v++) {
        if (strcmp(s, alarm_severity_name(v)) == 0) {
            out->severity = v;
            break;
        }
    }
    if (out->severity == 0) return fail(&c, "severity", ALARM_JSON_INVALID_VALUE);

    out->enabled = true;
    TRY(get_bool(&c, "enabled", &out->enabled));
    TRY(get_bool(&c, "latch", &out->latch));

    switch (out->condition) {
    case ALARM_COND_ABOVE:
    case ALARM_COND_BELOW:
        TRY(get_float(&c, "threshold", true, &out->threshold));
        break;
    case ALARM_COND_OUTSIDE:
    case ALARM_COND_INSIDE:
        TRY(get_float(&c, "lo", true, &out->threshold));
        TRY(get_float(&c, "hi", true, &out->threshold_hi));
        break;
    case ALARM_COND_STALE:
        TRY(get_uint(&c, "stale_sec", true, 65535, &u));
        out->stale_sec = (uint16_t)u;
        break;
    default:
        break;
    }

    TRY(get_float(&c, "hysteresis", false, &out->hysteresis));
    u = 0;
    TRY(get_uint(&c, "delay_sec", false, 65535, &u));
    out->delay_sec = (uint16_t)u;

    const cJSON *gate = cJSON_GetObjectItemCaseSensitive(obj, "gate");
    if (gate != NULL && !cJSON_IsNull(gate)) {
        if (!cJSON_IsObject(gate)) return fail(&c, "gate", ALARM_JSON_INVALID_TYPE);
        const cJSON *gs = cJSON_GetObjectItemCaseSensitive(gate, "signal");
        const cJSON *gw = cJSON_GetObjectItemCaseSensitive(gate, "when");
        if (gs == NULL || gw == NULL) return fail(&c, "gate", ALARM_JSON_MISSING_FIELD);
        if (!cJSON_IsString(gs) || gs->valuestring == NULL || !cJSON_IsBool(gw)) {
            return fail(&c, "gate", ALARM_JSON_INVALID_TYPE);
        }
        const signal_def_t *g = signal_registry_by_key(gs->valuestring);
        if (g == NULL) return fail(&c, "gate", ALARM_JSON_INVALID_VALUE);
        out->gate_signal_id = g->id;
        out->gate_mode = cJSON_IsTrue(gw) ? ALARM_GATE_WHEN_TRUE : ALARM_GATE_WHEN_FALSE;
    }

    const char *bad = "";
    if (!alarm_rule_validate(out, &bad)) return fail(&c, bad, ALARM_JSON_INVALID_VALUE);
    return ALARM_JSON_OK;
}

cJSON *alarm_rule_to_json(const alarm_rule_t *r)
{
    const signal_def_t *sig = signal_registry_by_id(r->signal_id);
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || sig == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    /* id 0 = not stored yet; omit it so the JSON round-trips as a "create". */
    bool ok = (r->id == 0 || cJSON_AddNumberToObject(o, "id", r->id) != NULL) &&
              cJSON_AddStringToObject(o, "name", r->name) != NULL &&
              cJSON_AddBoolToObject(o, "enabled", r->enabled) != NULL &&
              cJSON_AddStringToObject(o, "signal", sig->key) != NULL &&
              cJSON_AddStringToObject(o, "condition", alarm_cond_name(r->condition)) != NULL;
    if (ok) {
        switch (r->condition) {
        case ALARM_COND_ABOVE:
        case ALARM_COND_BELOW:
            ok = cJSON_AddNumberToObject(o, "threshold", r->threshold) != NULL;
            break;
        case ALARM_COND_OUTSIDE:
        case ALARM_COND_INSIDE:
            ok = cJSON_AddNumberToObject(o, "lo", r->threshold) != NULL &&
                 cJSON_AddNumberToObject(o, "hi", r->threshold_hi) != NULL;
            break;
        case ALARM_COND_STALE:
            ok = cJSON_AddNumberToObject(o, "stale_sec", r->stale_sec) != NULL;
            break;
        default:
            break;
        }
    }
    ok = ok && cJSON_AddNumberToObject(o, "hysteresis", r->hysteresis) != NULL &&
         cJSON_AddNumberToObject(o, "delay_sec", r->delay_sec) != NULL &&
         cJSON_AddStringToObject(o, "severity", alarm_severity_name(r->severity)) != NULL &&
         cJSON_AddBoolToObject(o, "latch", r->latch) != NULL;
    if (ok && r->gate_mode != ALARM_GATE_NONE) {
        const signal_def_t *g = signal_registry_by_id(r->gate_signal_id);
        cJSON *gate = cJSON_AddObjectToObject(o, "gate");
        ok = gate != NULL && g != NULL && cJSON_AddStringToObject(gate, "signal", g->key) != NULL &&
             cJSON_AddBoolToObject(gate, "when", r->gate_mode == ALARM_GATE_WHEN_TRUE) != NULL;
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}
