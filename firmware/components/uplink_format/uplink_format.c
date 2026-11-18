#include "uplink_format.h"

#include <math.h>
#include <string.h>

/* Prints a tree into the caller's buffer and consumes the tree. */
static int print_into(cJSON *root, char *buf, size_t size)
{
    if (root == NULL || buf == NULL || size == 0) {
        cJSON_Delete(root);
        return -1;
    }
    /* cJSON_PrintPreallocated needs a little slack beyond the text; ask for it. */
    bool ok = size >= 8 && cJSON_PrintPreallocated(root, buf, (int)size, 0);
    cJSON_Delete(root);
    if (!ok) {
        if (size > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    return (int)strlen(buf);
}

static double round_to(double v, uint8_t decimals)
{
    double scale = 1.0;
    for (uint8_t i = 0; i < decimals; i++) {
        scale *= 10.0;
    }
    return round(v * scale) / scale;
}

int uplink_format_telemetry(const telemetry_snapshot_t *snap, uint32_t now_ms, const alarm_counts_t *counts,
                            char *buf, size_t size)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    size_t n = 0;
    const signal_def_t *all = signal_registry_all(&n);
    for (size_t i = 0; i < n; i++) {
        signal_value_t v;
        signal_registry_read(&all[i], snap, now_ms, &v);
        if (!v.present || !v.trusted) {
            continue; /* ThingsBoard keeps its last value; the *_fault key reports the problem */
        }
        if (all[i].kind == SIGNAL_KIND_BOOL) {
            if (cJSON_AddBoolToObject(root, all[i].key, v.value != 0.0) == NULL) {
                cJSON_Delete(root);
                return -1;
            }
        } else if (isfinite(v.value)) {
            if (cJSON_AddNumberToObject(root, all[i].key, round_to(v.value, all[i].decimals)) == NULL) {
                cJSON_Delete(root);
                return -1;
            }
        }
    }
    if (counts != NULL) {
        if (cJSON_AddNumberToObject(root, "alarm_active", counts->active) == NULL ||
            cJSON_AddNumberToObject(root, "alarm_critical", counts->critical) == NULL ||
            cJSON_AddNumberToObject(root, "alarm_unacked", counts->unacked) == NULL ||
            cJSON_AddNumberToObject(root, "alarm_max_severity", counts->max_severity) == NULL) {
            cJSON_Delete(root);
            return -1;
        }
    }
    return print_into(root, buf, size);
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case ALARM_EVT_RAISED: return "raised";
    case ALARM_EVT_CLEARED: return "cleared";
    default: return "acked";
    }
}

static cJSON *event_object(const alarm_event_t *e, uint32_t boot_id)
{
    const signal_def_t *sig = signal_registry_by_id(e->signal_id);
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    bool ok = cJSON_AddNumberToObject(o, "seq", e->seq) != NULL &&
              cJSON_AddNumberToObject(o, "boot_id", boot_id) != NULL &&
              cJSON_AddStringToObject(o, "type", type_name(e->type)) != NULL &&
              cJSON_AddNumberToObject(o, "rule_id", e->rule_id) != NULL &&
              cJSON_AddStringToObject(o, "name", e->name) != NULL &&
              cJSON_AddStringToObject(o, "signal", sig != NULL ? sig->key : "") != NULL &&
              cJSON_AddStringToObject(o, "severity", alarm_severity_name(e->severity)) != NULL;
    if (ok) {
        ok = isfinite(e->value) ? cJSON_AddNumberToObject(o, "value", alarm_json_number(e->value)) != NULL
                                : cJSON_AddNullToObject(o, "value") != NULL;
    }
    if (ok) {
        const char *reason = e->reason == ALARM_REASON_CONDITION      ? "condition"
                             : e->reason == ALARM_REASON_ACKED        ? "acked"
                             : e->reason == ALARM_REASON_RULE_CHANGED ? "rule_changed"
                                                                      : NULL;
        ok = reason != NULL ? cJSON_AddStringToObject(o, "reason", reason) != NULL
                            : cJSON_AddNullToObject(o, "reason") != NULL;
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

int uplink_format_event(const alarm_event_t *evt, uint32_t boot_id, int64_t ts_unix_ms, char *buf, size_t size)
{
    cJSON *ev = event_object(evt, boot_id);
    cJSON *root = cJSON_CreateObject();
    if (ev == NULL || root == NULL) {
        cJSON_Delete(ev);
        cJSON_Delete(root);
        return -1;
    }
    if (ts_unix_ms == 0) {
        cJSON_AddItemToObject(root, "alarm_event", ev);
    } else {
        cJSON *values = cJSON_CreateObject();
        if (values == NULL || cJSON_AddNumberToObject(root, "ts", (double)ts_unix_ms) == NULL) {
            cJSON_Delete(values);
            cJSON_Delete(ev);
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToObject(values, "alarm_event", ev);
        cJSON_AddItemToObject(root, "values", values);
    }
    return print_into(root, buf, size);
}

int uplink_format_identity(const char *fw_version, char *buf, size_t size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "signals") : NULL;
    if (arr == NULL ||
        cJSON_AddStringToObject(root, "fw_type", UPLINK_FW_TYPE) == NULL ||
        cJSON_AddStringToObject(root, "fw_version", fw_version != NULL ? fw_version : "") == NULL ||
        cJSON_AddNumberToObject(root, "contract_version", UPLINK_CONTRACT_VERSION) == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    size_t n = 0;
    const signal_def_t *all = signal_registry_all(&n);
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(arr, o);
        if (cJSON_AddStringToObject(o, "key", all[i].key) == NULL ||
            cJSON_AddStringToObject(o, "label", all[i].label_th) == NULL ||
            cJSON_AddStringToObject(o, "unit", all[i].unit) == NULL ||
            cJSON_AddStringToObject(o, "kind", all[i].kind == SIGNAL_KIND_BOOL ? "bool" : "number") == NULL ||
            cJSON_AddStringToObject(o, "group", all[i].group) == NULL ||
            cJSON_AddNumberToObject(o, "decimals", all[i].decimals) == NULL ||
            cJSON_AddNumberToObject(o, "period_sec", all[i].period_sec) == NULL) {
            cJSON_Delete(root);
            return -1;
        }
    }
    return print_into(root, buf, size);
}

int uplink_format_rules(const alarm_rule_set_t *set, char *buf, size_t size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_AddArrayToObject(root, "alarm_rules") : NULL;
    if (arr == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    for (uint8_t i = 0; i < set->count && i < ALARM_MAX_RULES; i++) {
        cJSON *r = alarm_rule_to_json(&set->rules[i]);
        if (r == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddItemToArray(arr, r);
    }
    return print_into(root, buf, size);
}

int64_t uplink_event_ts(int64_t now_unix_ms, uint32_t now_uptime_ms, uint32_t event_uptime_ms)
{
    if (now_unix_ms == 0) {
        return 0;
    }
    return now_unix_ms - (int64_t)(uint32_t)(now_uptime_ms - event_uptime_ms);
}

uint32_t uplink_forward_account_gap(uplink_forward_t *f, uint32_t oldest_seq)
{
    if (oldest_seq == 0 || oldest_seq <= f->last_forwarded_seq + 1u) {
        return 0;
    }
    uint32_t gap = oldest_seq - 1u - f->last_forwarded_seq;
    f->dropped += gap;
    f->last_forwarded_seq = oldest_seq - 1u;
    return gap;
}

bool uplink_forward_hold_for_clock(bool clock_set, uint32_t pending, uint32_t ring_len, uint32_t held_ms)
{
    return !clock_set && pending + UPLINK_CLOCK_HOLD_MARGIN < ring_len && held_ms < UPLINK_CLOCK_HOLD_MAX_MS;
}

uplink_clock_decision_t uplink_clock_hold_step(uplink_clock_hold_t *st, bool clock_set, bool sink_ready,
                                               uint32_t pending, uint32_t ring_len, uint32_t now_ms,
                                               uplink_clock_hold_event_t *event)
{
    uplink_clock_hold_event_t ev = UPLINK_CLOCK_EV_NONE;
    uplink_clock_decision_t d;
    if (clock_set) {
        if (st->active) {
            ev = UPLINK_CLOCK_EV_ENDED_CLOCK;
        }
        st->active = false;
        st->released = false;
        d = sink_ready && pending > 0 ? UPLINK_CLOCK_SEND_TS : UPLINK_CLOCK_IDLE;
    } else if (!sink_ready) {
        d = UPLINK_CLOCK_IDLE; /* state is kept across drops */
    } else if (pending == 0) {
        if (st->active) {
            ev = UPLINK_CLOCK_EV_ENDED_EMPTY;
        }
        st->active = false;
        d = UPLINK_CLOCK_IDLE;
    } else if (st->released) {
        d = UPLINK_CLOCK_SEND_NO_TS;
    } else {
        uint32_t held_ms = st->active ? (uint32_t)(now_ms - st->start_ms) : 0;
        if (uplink_forward_hold_for_clock(false, pending, ring_len, held_ms)) {
            if (!st->active) {
                st->active = true;
                st->start_ms = now_ms;
                ev = UPLINK_CLOCK_EV_HOLD_STARTED;
            }
            d = UPLINK_CLOCK_HOLD;
        } else {
            ev = held_ms >= UPLINK_CLOCK_HOLD_MAX_MS ? UPLINK_CLOCK_EV_RELEASED_CAP : UPLINK_CLOCK_EV_RELEASED_MARGIN;
            st->active = false;
            st->released = true;
            d = UPLINK_CLOCK_SEND_NO_TS;
        }
    }
    if (event != NULL) {
        *event = ev;
    }
    return d;
}
