#include <string.h>

#include "alarm_rules.h"
#include "selftest.h"

#define NO_EVT (-1)

static telemetry_snapshot_t s_snap;

static void snap_ph(float ph, uint32_t t)
{
    s_snap.has_water = true;
    s_snap.water = (telemetry_water_payload_t){.valid = true, .ph = ph};
    s_snap.water_updated_ms = t;
}

static void snap_ph_state(bool valid, bool fault, uint32_t t)
{
    s_snap.has_water = true;
    s_snap.water.valid = valid;
    s_snap.water.fault = fault;
    s_snap.water_updated_ms = t;
}

static void snap_relay1(bool on)
{
    s_snap.has_pump = true;
    s_snap.pump.relay_1_active = on;
    s_snap.pump_updated_ms = 1;
}

static void snap_flow(bool on)
{
    s_snap.has_flow = true;
    s_snap.flow.flow_on = on;
    s_snap.flow_updated_ms = 1;
}

static alarm_rule_t mk(const char *key, alarm_cond_t cond, float lo, float hi, float hyst, uint16_t delay, bool latch)
{
    alarm_rule_t r;
    memset(&r, 0, sizeof(r));
    r.id = 1;
    r.enabled = true;
    r.signal_id = signal_registry_by_key(key)->id;
    r.condition = cond;
    r.severity = ALARM_SEV_WARNING;
    r.latch = latch;
    r.threshold = lo;
    r.threshold_hi = hi;
    r.hysteresis = hyst;
    r.delay_sec = delay;
    strcpy(r.name, "t");
    return r;
}

static alarm_rule_t mk_stale(const char *key, uint16_t stale_sec)
{
    alarm_rule_t r = mk(key, ALARM_COND_STALE, 0, 0, 0, 0, false);
    r.stale_sec = stale_sec;
    return r;
}

static alarm_transition_t s_tr;

static int step(const alarm_rule_t *r, alarm_runtime_t *rt, uint32_t now)
{
    return alarm_rule_step(r, rt, &s_snap, now, &s_tr) ? (int)s_tr.type : NO_EVT;
}

void test_alarm_eval(void)
{
    alarm_runtime_t rt;
    alarm_rule_t r;

    /* delay */
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 60, false);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_PENDING);
    CHECK(step(&r, &rt, 59999) == NO_EVT && rt.state == ALARM_STATE_PENDING);
    CHECK(step(&r, &rt, 60000) == ALARM_EVT_RAISED && rt.state == ALARM_STATE_ACTIVE && s_tr.value == 9.0f);

    /* pending drops */
    memset(&rt, 0, sizeof(rt));
    snap_ph(9.0f, 1);
    step(&r, &rt, 0);
    snap_ph(8.0f, 1);
    CHECK(step(&r, &rt, 30000) == NO_EVT && rt.state == ALARM_STATE_NORMAL);

    /* hysteresis above */
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0.2f, 0, false);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED && rt.state == ALARM_STATE_ACTIVE);
    snap_ph(8.4f, 1);
    CHECK(step(&r, &rt, 1000) == NO_EVT && rt.state == ALARM_STATE_ACTIVE); /* needs <= 8.3 */
    snap_ph(8.3f, 1);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_CLEARED && s_tr.reason == ALARM_REASON_CONDITION && rt.state == ALARM_STATE_NORMAL);

    /* flicker raises once */
    memset(&rt, 0, sizeof(rt));
    int raised = 0, cleared = 0;
    float flick[] = {8.6f, 8.45f, 8.6f, 8.45f};
    for (int i = 0; i < 4; i++) {
        snap_ph(flick[i], 1);
        int e = step(&r, &rt, (uint32_t)(i * 1000));
        raised += e == ALARM_EVT_RAISED;
        cleared += e == ALARM_EVT_CLEARED;
    }
    CHECK(raised == 1 && cleared == 0);

    /* below */
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_BELOW, 6.5f, 0, 0.5f, 0, false);
    snap_ph(6.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph(6.8f, 1);
    CHECK(step(&r, &rt, 1000) == NO_EVT); /* needs >= 7.0 */
    snap_ph(7.5f, 1);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_CLEARED);

    /* outside: clears only within lo+h .. hi-h */
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_OUTSIDE, 6.5f, 8.5f, 0.25f, 0, false);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph(8.4f, 1); /* inside the range but above hi - h = 8.25 */
    CHECK(step(&r, &rt, 1000) == NO_EVT && rt.state == ALARM_STATE_ACTIVE);
    snap_ph(7.0f, 1);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_CLEARED);
    snap_ph(6.0f, 1);
    CHECK(step(&r, &rt, 3000) == ALARM_EVT_RAISED);
    snap_ph(6.6f, 1); /* below lo + h = 6.75 */
    CHECK(step(&r, &rt, 4000) == NO_EVT);
    snap_ph(6.9f, 1);
    CHECK(step(&r, &rt, 5000) == ALARM_EVT_CLEARED);

    /* inside: clears only below lo-h or above hi+h */
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_INSIDE, 6.5f, 8.5f, 0.25f, 0, false);
    snap_ph(7.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph(6.4f, 1); /* just outside, but not below lo - h = 6.25 */
    CHECK(step(&r, &rt, 1000) == NO_EVT && rt.state == ALARM_STATE_ACTIVE);
    snap_ph(6.0f, 1);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_CLEARED);
    snap_ph(8.7f, 1); /* above hi but inside the raise test? no: not inside -> stays normal */
    CHECK(step(&r, &rt, 3000) == NO_EVT && rt.state == ALARM_STATE_NORMAL);
    snap_ph(8.0f, 1);
    CHECK(step(&r, &rt, 4000) == ALARM_EVT_RAISED);
    snap_ph(8.7f, 1); /* above hi but not above hi + h = 8.75 */
    CHECK(step(&r, &rt, 5000) == NO_EVT);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 6000) == ALARM_EVT_CLEARED);

    /* bool */
    memset(&rt, 0, sizeof(rt));
    r = mk("flow_on", ALARM_COND_IS_FALSE, 0, 0, 0, 0, false);
    snap_flow(false);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_flow(true);
    CHECK(step(&r, &rt, 1000) == ALARM_EVT_CLEARED);
    r = mk("flow_on", ALARM_COND_IS_TRUE, 0, 0, 0, 0, false);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_RAISED);

    /* gate */
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    r = mk("flow_on", ALARM_COND_IS_FALSE, 0, 0, 0, 15, false);
    r.gate_mode = ALARM_GATE_WHEN_TRUE;
    r.gate_signal_id = signal_registry_by_key("relay1_on")->id;
    snap_flow(false);
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_NORMAL); /* gate signal not present: closed */
    snap_relay1(false);
    CHECK(step(&r, &rt, 100000) == NO_EVT && rt.state == ALARM_STATE_NORMAL);
    snap_relay1(true);
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_PENDING);
    snap_relay1(false);
    CHECK(step(&r, &rt, 10000) == NO_EVT && rt.state == ALARM_STATE_NORMAL);
    snap_relay1(true);
    CHECK(step(&r, &rt, 20000) == NO_EVT && rt.state == ALARM_STATE_PENDING);
    CHECK(step(&r, &rt, 34999) == NO_EVT);
    CHECK(step(&r, &rt, 35000) == ALARM_EVT_RAISED); /* delay restarted when the gate opened */
    snap_relay1(false);
    CHECK(step(&r, &rt, 36000) == ALARM_EVT_CLEARED && s_tr.reason == ALARM_REASON_CONDITION);

    /* transient miss */
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 0, false);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph_state(false, false, 2000);
    CHECK(step(&r, &rt, 2000) == NO_EVT && rt.state == ALARM_STATE_ACTIVE);
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 60, false);
    snap_ph(9.0f, 1);
    step(&r, &rt, 0);
    snap_ph_state(false, false, 20000);
    CHECK(step(&r, &rt, 20000) == NO_EVT && rt.state == ALARM_STATE_PENDING); /* miss: timer keeps running */
    snap_ph_state(true, false, 60000);
    CHECK(step(&r, &rt, 60000) == ALARM_EVT_RAISED);

    /* fault */
    memset(&rt, 0, sizeof(rt));
    snap_ph(9.0f, 1);
    step(&r, &rt, 0);
    snap_ph_state(false, true, 5000);
    CHECK(step(&r, &rt, 5000) == NO_EVT && rt.state == ALARM_STATE_NORMAL); /* pending drops */
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 0, false);
    memset(&rt, 0, sizeof(rt));
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph_state(false, true, 5000);
    CHECK(step(&r, &rt, 5000) == NO_EVT && rt.state == ALARM_STATE_ACTIVE); /* active holds */

    /* signal never present */
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_NORMAL);

    /* stale */
    memset(&rt, 0, sizeof(rt));
    r = mk_stale("water_ph", 30);
    snap_ph(7.0f, 1000);
    CHECK(step(&r, &rt, 31000) == NO_EVT); /* age 30000 is not > 30000 */
    CHECK(step(&r, &rt, 31001) == ALARM_EVT_RAISED && s_tr.value == 0.0f);
    snap_ph(7.0f, 40000);
    CHECK(step(&r, &rt, 40000) == ALARM_EVT_CLEARED);
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    CHECK(step(&r, &rt, 30000) == NO_EVT);
    CHECK(step(&r, &rt, 30001) == ALARM_EVT_RAISED); /* never published */

    /* latch */
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 0, true);
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == ALARM_EVT_RAISED);
    snap_ph(7.0f, 1);
    CHECK(step(&r, &rt, 1000) == NO_EVT && rt.state == ALARM_STATE_LATCHED);
    CHECK(alarm_rule_ack(&rt, &s_tr) && s_tr.type == ALARM_EVT_CLEARED && s_tr.reason == ALARM_REASON_ACKED
          && rt.state == ALARM_STATE_NORMAL);

    /* ack while active */
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 2000) == ALARM_EVT_RAISED);
    CHECK(alarm_rule_ack(&rt, &s_tr) && s_tr.type == ALARM_EVT_ACKED && rt.state == ALARM_STATE_ACTIVE && rt.acked);
    CHECK(!alarm_rule_ack(&rt, &s_tr)); /* second ack */
    snap_ph(7.0f, 1);
    CHECK(step(&r, &rt, 3000) == ALARM_EVT_CLEARED && s_tr.reason == ALARM_REASON_CONDITION
          && rt.state == ALARM_STATE_NORMAL); /* acked: no latch even with latch=true */
    CHECK(!alarm_rule_ack(&rt, &s_tr)); /* NORMAL */

    /* reset */
    memset(&rt, 0, sizeof(rt));
    snap_ph(9.0f, 1);
    step(&r, &rt, 0);
    CHECK(alarm_rule_reset(&rt, &s_tr) && s_tr.type == ALARM_EVT_CLEARED && s_tr.reason == ALARM_REASON_RULE_CHANGED
          && rt.state == ALARM_STATE_NORMAL);
    CHECK(!alarm_rule_reset(&rt, &s_tr));

    /* disabled / unknown signal */
    memset(&rt, 0, sizeof(rt));
    r.enabled = false;
    snap_ph(9.0f, 1);
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_NORMAL);
    r.enabled = true;
    r.signal_id = 200;
    CHECK(step(&r, &rt, 0) == NO_EVT && rt.state == ALARM_STATE_NORMAL);

    /* one raised, one cleared */
    memset(&rt, 0, sizeof(rt));
    r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 0, true);
    int n_raised = 0, n_cleared = 0;
#define COUNT(e) do { int e_ = (e); n_raised += e_ == ALARM_EVT_RAISED; n_cleared += e_ == ALARM_EVT_CLEARED; } while (0)
    snap_ph(9.0f, 1);
    COUNT(step(&r, &rt, 0));
    snap_ph(7.0f, 1);
    COUNT(step(&r, &rt, 1000)); /* -> latched */
    COUNT(alarm_rule_ack(&rt, &s_tr) ? (int)s_tr.type : NO_EVT);
    snap_ph(9.0f, 1);
    COUNT(step(&r, &rt, 2000));
    snap_ph(7.0f, 1);
    COUNT(step(&r, &rt, 3000)); /* -> latched */
    COUNT(alarm_rule_ack(&rt, &s_tr) ? (int)s_tr.type : NO_EVT);
    CHECK(n_raised == 2 && n_cleared == 2);

    /* counts */
    alarm_rule_set_t set;
    alarm_runtime_t rts[ALARM_MAX_RULES];
    alarm_counts_t c;
    memset(&set, 0, sizeof(set));
    memset(rts, 0, sizeof(rts));
    set.count = 4;
    for (int i = 0; i < 4; i++) {
        set.rules[i] = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 0, false);
        set.rules[i].severity = ALARM_SEV_WARNING;
    }
    alarm_counts(&set, rts, &c);
    CHECK(c.active == 0 && c.critical == 0 && c.unacked == 0 && c.critical_unacked == 0 && c.max_severity == 0);
    set.rules[0].severity = ALARM_SEV_CRITICAL;
    rts[0].state = ALARM_STATE_ACTIVE; /* critical, unacked */
    rts[1].state = ALARM_STATE_ACTIVE; /* warning, acked */
    rts[1].acked = true;
    set.rules[2].severity = ALARM_SEV_CRITICAL;
    rts[2].state = ALARM_STATE_LATCHED; /* critical, always unacked */
    rts[3].state = ALARM_STATE_NORMAL;
    alarm_counts(&set, rts, &c);
    CHECK(c.active == 3 && c.critical == 2 && c.unacked == 2 && c.critical_unacked == 2 && c.max_severity == 3);
    rts[0].state = ALARM_STATE_NORMAL;
    rts[2].state = ALARM_STATE_NORMAL;
    alarm_counts(&set, rts, &c);
    CHECK(c.active == 1 && c.critical == 0 && c.unacked == 0 && c.critical_unacked == 0 && c.max_severity == 2);
}

void test_alarm_eval_wrap(void)
{
    alarm_runtime_t rt;
    alarm_rule_t r = mk("water_ph", ALARM_COND_ABOVE, 8.5f, 0, 0, 60, false);
    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    uint32_t t0 = 0xFFFFFF00u;
    snap_ph(9.0f, t0);
    CHECK(step(&r, &rt, t0) == NO_EVT && rt.state == ALARM_STATE_PENDING);
    snap_ph(9.0f, t0 + 59999u);
    CHECK(step(&r, &rt, t0 + 59999u) == NO_EVT && rt.state == ALARM_STATE_PENDING); /* wrapped */
    CHECK(step(&r, &rt, t0 + 60000u) == ALARM_EVT_RAISED);

    memset(&s_snap, 0, sizeof(s_snap));
    memset(&rt, 0, sizeof(rt));
    r = mk_stale("water_ph", 30);
    snap_ph(7.0f, 0xFFFFFFF0u);
    CHECK(step(&r, &rt, 0x00000100u) == NO_EVT && rt.state == ALARM_STATE_NORMAL); /* age 272 ms */
}
