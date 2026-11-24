#include <math.h>
#include <string.h>

#include "alarm_rules.h"
#include "cJSON.h"
#include "selftest.h"
#include "uplink_format.h"

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static bool num_eq(const cJSON *o, const char *k, double v)
{
    const cJSON *it = get(o, k);
    return cJSON_IsNumber(it) && it->valuedouble == v;
}

static bool str_eq(const cJSON *o, const char *k, const char *v)
{
    const cJSON *it = get(o, k);
    return cJSON_IsString(it) && strcmp(it->valuestring, v) == 0;
}

static telemetry_snapshot_t s_snap;
static char s_buf[UPLINK_IDENTITY_BUF];
static alarm_rule_set_t s_set;

void test_uplink_format(void)
{
    /* ---- telemetry ---- */
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.has_power[0] = true;
    s_snap.power_updated_ms[0] = 1000;
    s_snap.power[0] = (telemetry_power_payload_t){.channel = 0, .valid = true, .current_a = 1.2344f, .voltage_v = 229.84f};
    s_snap.has_water = true;
    s_snap.water = (telemetry_water_payload_t){.valid = false, .fault = true, .ph = 7.1f};
    s_snap.has_flow = true;
    s_snap.flow.flow_on = true;
    s_snap.has_health = true;
    s_snap.health.wifi_rssi = 0;
    s_snap.health.uptime_sec = 734;
    alarm_counts_t counts = {.active = 2, .critical = 1, .unacked = 2, .critical_unacked = 1, .max_severity = 3};

    int n = uplink_format_telemetry(&s_snap, 5000, &counts, s_buf, UPLINK_TELEMETRY_BUF);
    CHECK(n > 0 && (size_t)n == strlen(s_buf));
    cJSON *t = cJSON_Parse(s_buf);
    CHECK(t != NULL);
    if (t != NULL) {
        CHECK(num_eq(t, "pump_current_a", 1.234));
        CHECK(num_eq(t, "pump_voltage_v", 229.8));
        CHECK(cJSON_IsTrue(get(t, "flow_on")));
        CHECK(cJSON_IsTrue(get(t, "water_sensor_fault")));
        CHECK(get(t, "water_ph") == NULL);
        CHECK(get(t, "wifi_rssi_dbm") == NULL);
        CHECK(get(t, "chiller_current_a") == NULL);
        CHECK(num_eq(t, "uptime_sec", 734));
        CHECK(num_eq(t, "alarm_active", 2) && num_eq(t, "alarm_critical", 1) && num_eq(t, "alarm_unacked", 2)
              && num_eq(t, "alarm_max_severity", 3));
        cJSON_Delete(t);
    }

    /* non-finite values never reach the JSON (Review Focus 4) */
    s_snap.power[0].current_a = NAN;
    s_snap.power[0].power_w = INFINITY;
    n = uplink_format_telemetry(&s_snap, 5000, &counts, s_buf, UPLINK_TELEMETRY_BUF);
    CHECK(n > 0);
    t = cJSON_Parse(s_buf);
    CHECK(t != NULL && get(t, "pump_current_a") == NULL && get(t, "pump_power_w") == NULL);
    CHECK(strstr(s_buf, "nan") == NULL && strstr(s_buf, "NaN") == NULL && strstr(s_buf, "inf") == NULL
          && strstr(s_buf, "Inf") == NULL);
    cJSON_Delete(t);

    /* too small */
    CHECK(uplink_format_telemetry(&s_snap, 5000, &counts, s_buf, 16) == -1);

    /* ---- event ---- */
    alarm_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = 12;
    ev.type = ALARM_EVT_RAISED;
    ev.reason = ALARM_REASON_NONE;
    ev.rule_id = 3;
    ev.signal_id = 21;
    ev.severity = ALARM_SEV_WARNING;
    ev.value = 8.92f;
    strcpy(ev.name, "pH ผิดช่วง");
    n = uplink_format_event(&ev, 2891734u, 0, s_buf, UPLINK_EVENT_BUF);
    CHECK(n > 0);
    /* a float value prints as its short decimal, not the double expansion 8.9200000762939453 */
    CHECK(strstr(s_buf, "\"value\":8.92,") != NULL);
    t = cJSON_Parse(s_buf);
    const cJSON *ae = t ? get(t, "alarm_event") : NULL;
    CHECK(ae != NULL);
    if (ae != NULL) {
        CHECK(num_eq(ae, "seq", 12) && num_eq(ae, "boot_id", 2891734) && str_eq(ae, "type", "raised")
              && num_eq(ae, "rule_id", 3) && str_eq(ae, "name", "pH ผิดช่วง") && str_eq(ae, "signal", "water_ph")
              && str_eq(ae, "severity", "warning") && cJSON_IsNull(get(ae, "reason")));
        CHECK(fabs(get(ae, "value")->valuedouble - 8.92) < 0.001);
    }
    CHECK(t != NULL && get(t, "ts") == NULL);
    cJSON_Delete(t);

    n = uplink_format_event(&ev, 2891734u, 1745000000000LL, s_buf, UPLINK_EVENT_BUF);
    CHECK(n > 0);
    t = cJSON_Parse(s_buf);
    CHECK(t != NULL && num_eq(t, "ts", 1745000000000.0));
    const cJSON *vals = t ? get(t, "values") : NULL;
    CHECK(vals != NULL && get(vals, "alarm_event") != NULL && num_eq(get(vals, "alarm_event"), "seq", 12));
    cJSON_Delete(t);

    ev.type = ALARM_EVT_CLEARED;
    ev.reason = ALARM_REASON_ACKED;
    n = uplink_format_event(&ev, 1, 0, s_buf, UPLINK_EVENT_BUF);
    t = cJSON_Parse(s_buf);
    ae = t ? get(t, "alarm_event") : NULL;
    CHECK(ae != NULL && str_eq(ae, "type", "cleared") && str_eq(ae, "reason", "acked"));
    cJSON_Delete(t);

    ev.value = NAN; /* a non-finite event value becomes null, never "nan" */
    n = uplink_format_event(&ev, 1, 0, s_buf, UPLINK_EVENT_BUF);
    t = cJSON_Parse(s_buf);
    ae = t ? get(t, "alarm_event") : NULL;
    CHECK(n > 0 && ae != NULL && cJSON_IsNull(get(ae, "value")));
    cJSON_Delete(t);

    /* ---- identity ---- */
    n = uplink_format_identity("0.2.0-dev", s_buf, UPLINK_IDENTITY_BUF);
    CHECK(n > 0 && n < UPLINK_IDENTITY_BUF);
    t = cJSON_Parse(s_buf);
    CHECK(t != NULL && str_eq(t, "fw_type", "fishpump") && num_eq(t, "contract_version", 2)
          && str_eq(t, "fw_version", "0.2.0-dev"));
    const cJSON *sigs = t ? get(t, "signals") : NULL;
    CHECK(cJSON_IsArray(sigs) && cJSON_GetArraySize(sigs) == 37);
    bool found = false;
    const cJSON *s;
    cJSON_ArrayForEach(s, sigs) {
        if (str_eq(s, "key", "pump_current_a")) {
            found = str_eq(s, "unit", "A") && str_eq(s, "kind", "number") && num_eq(s, "decimals", 3)
                    && num_eq(s, "period_sec", 2);
        }
    }
    CHECK(found);
    cJSON_Delete(t);

    /* ---- rules ---- */
    alarm_presets_fill(&s_set);
    s_set.count = ALARM_MAX_RULES;
    for (int i = 0; i < ALARM_MAX_RULES; i++) {
        alarm_rule_t *r = &s_set.rules[i];
        r->id = (uint16_t)(i + 1);
        r->signal_id = 52;
        r->condition = ALARM_COND_IS_FALSE;
        r->severity = ALARM_SEV_CRITICAL;
        r->gate_mode = ALARM_GATE_WHEN_TRUE;
        r->gate_signal_id = 41;
        r->enabled = true;
        memset(r->name, 0, sizeof(r->name));
        for (int k = 0; k < 32; k++) {
            memcpy(r->name + 3 * k, "ก", 3);
        }
    }
    static char rules_buf[UPLINK_RULES_BUF];
    n = uplink_format_rules(&s_set, rules_buf, sizeof(rules_buf));
    CHECK(n > 0 && n < UPLINK_RULES_BUF);
    t = cJSON_Parse(rules_buf);
    const cJSON *ar = t ? get(t, "alarm_rules") : NULL;
    CHECK(cJSON_IsArray(ar) && cJSON_GetArraySize(ar) == 24);
    cJSON_Delete(t);
    CHECK(uplink_format_rules(&s_set, rules_buf, 64) == -1);

    /* ---- ts math ---- */
    CHECK(uplink_event_ts(1745000060000LL, 100000, 40000) == 1745000000000LL);
    CHECK(uplink_event_ts(1745000000000LL, 0x00000100u, 0xFFFFFF00u) == 1745000000000LL - 512);
    CHECK(uplink_event_ts(0, 5, 1) == 0);

    /* ---- forward accounting ---- */
    uplink_forward_t f = {.last_forwarded_seq = 3, .dropped = 0};
    CHECK(uplink_forward_account_gap(&f, 2) == 0 && f.last_forwarded_seq == 3 && f.dropped == 0);
    CHECK(uplink_forward_account_gap(&f, 14) == 10 && f.last_forwarded_seq == 13 && f.dropped == 10);
    CHECK(uplink_forward_account_gap(&f, 0) == 0 && f.last_forwarded_seq == 13); /* empty ring */
    CHECK(uplink_forward_account_gap(&f, 14) == 0 && f.dropped == 10);            /* nothing lost since */

    /* ---- hold events for the clock ---- */
    CHECK(!uplink_forward_hold_for_clock(true, 0, 32, 0));
    CHECK(uplink_forward_hold_for_clock(false, 0, 32, 0));
    CHECK(uplink_forward_hold_for_clock(false, 23, 32, 0));
    CHECK(!uplink_forward_hold_for_clock(false, 24, 32, 0));
    CHECK(!uplink_forward_hold_for_clock(false, 40, 32, 0));
    CHECK(!uplink_forward_hold_for_clock(false, 0, 8, 0)); /* the margin swallows a small ring */
    CHECK(uplink_forward_hold_for_clock(false, 0, 32, 119999u));
    CHECK(!uplink_forward_hold_for_clock(false, 0, 32, 120000u)); /* time cap */
    CHECK(UPLINK_CLOCK_HOLD_MAX_MS == 120000u);

    /* ---- hold/latch step ---- */
    {
        uplink_clock_hold_t h = {0};
        uplink_clock_hold_event_t ev;
        /* hold starts, stays, releases at the cap, and the release persists across later sends */
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 1000, &ev) == UPLINK_CLOCK_HOLD && ev == UPLINK_CLOCK_EV_HOLD_STARTED);
        CHECK(uplink_clock_hold_step(&h, false, true, 2, 32, 60000, &ev) == UPLINK_CLOCK_HOLD && ev == UPLINK_CLOCK_EV_NONE);
        CHECK(uplink_clock_hold_step(&h, false, true, 2, 32, 1000 + 119999u, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h, false, true, 2, 32, 1000 + 120000u, &ev) == UPLINK_CLOCK_SEND_NO_TS &&
              ev == UPLINK_CLOCK_EV_RELEASED_CAP);
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 1000 + 130000u, &ev) == UPLINK_CLOCK_SEND_NO_TS &&
              ev == UPLINK_CLOCK_EV_NONE); /* latched: no new hold */
        /* the sink dropping and coming back does not restart the hold */
        CHECK(uplink_clock_hold_step(&h, false, false, 1, 32, 1000 + 131000u, &ev) == UPLINK_CLOCK_IDLE);
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 1000 + 132000u, &ev) == UPLINK_CLOCK_SEND_NO_TS &&
              ev == UPLINK_CLOCK_EV_NONE);
        /* the clock arriving resets everything and sends with ts */
        CHECK(uplink_clock_hold_step(&h, true, true, 1, 32, 1000 + 133000u, &ev) == UPLINK_CLOCK_SEND_TS);
        CHECK(!h.active && !h.released);
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 1000 + 134000u, &ev) == UPLINK_CLOCK_HOLD &&
              ev == UPLINK_CLOCK_EV_HOLD_STARTED); /* a later clock loss holds afresh */
    }
    {
        uplink_clock_hold_t h = {0};
        uplink_clock_hold_event_t ev;
        /* a drop while holding keeps the start time: the cap still fires after the reconnect */
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 5000, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h, false, false, 1, 32, 50000, &ev) == UPLINK_CLOCK_IDLE && h.active);
        CHECK(uplink_clock_hold_step(&h, false, true, 1, 32, 5000 + 120000u, &ev) == UPLINK_CLOCK_SEND_NO_TS &&
              ev == UPLINK_CLOCK_EV_RELEASED_CAP);
        /* clock set while holding logs the end */
        uplink_clock_hold_t h2 = {0};
        CHECK(uplink_clock_hold_step(&h2, false, true, 1, 32, 10, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h2, true, true, 1, 32, 20, &ev) == UPLINK_CLOCK_SEND_TS &&
              ev == UPLINK_CLOCK_EV_ENDED_CLOCK);
        /* nothing pending ends the hold; the next event starts a fresh timer */
        uplink_clock_hold_t h3 = {0};
        CHECK(uplink_clock_hold_step(&h3, false, true, 1, 32, 100, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h3, false, true, 0, 32, 200, &ev) == UPLINK_CLOCK_IDLE &&
              ev == UPLINK_CLOCK_EV_ENDED_EMPTY && !h3.active);
        CHECK(uplink_clock_hold_step(&h3, false, true, 1, 32, 119000, &ev) == UPLINK_CLOCK_HOLD &&
              ev == UPLINK_CLOCK_EV_HOLD_STARTED);
        CHECK(uplink_clock_hold_step(&h3, false, true, 1, 32, 119000 + 119999u, &ev) == UPLINK_CLOCK_HOLD);
        /* the 24-pending ring margin releases at once, and latches */
        uplink_clock_hold_t h4 = {0};
        CHECK(uplink_clock_hold_step(&h4, false, true, 23, 32, 0, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h4, false, true, 24, 32, 1000, &ev) == UPLINK_CLOCK_SEND_NO_TS &&
              ev == UPLINK_CLOCK_EV_RELEASED_MARGIN);
        CHECK(uplink_clock_hold_step(&h4, false, true, 1, 32, 2000, &ev) == UPLINK_CLOCK_SEND_NO_TS);
        /* wrap-safe: the hold starts just before the uptime wraps */
        uplink_clock_hold_t h5 = {0};
        CHECK(uplink_clock_hold_step(&h5, false, true, 1, 32, 0xFFFFFFF0u, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h5, false, true, 1, 32, 0xFFFFFFF0u + 119999u, &ev) == UPLINK_CLOCK_HOLD);
        CHECK(uplink_clock_hold_step(&h5, false, true, 1, 32, 0xFFFFFFF0u + 120000u, &ev) == UPLINK_CLOCK_SEND_NO_TS);
    }
}
