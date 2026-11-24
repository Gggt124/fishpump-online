#include <string.h>
#include "selftest.h"
#include "dry_run_logic.h"

void test_dry_run_config(void)
{
    dry_run_config_t c;
    dry_run_config_defaults(&c);
    CHECK(!c.enabled && c.prime_sec == 15 && c.dry_trip_sec == 10 && c.retry_wait_min == 5 &&
          c.retry_backoff && c.max_retries == 3);
    const char *bad = "";
    CHECK(dry_run_config_validate(&c, &bad));
    c.prime_sec = 0;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
    c.prime_sec = 4;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
    c.prime_sec = 5;   CHECK(dry_run_config_validate(&c, &bad));
    c.prime_sec = 120; CHECK(dry_run_config_validate(&c, &bad));
    c.prime_sec = 121; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "prime_sec") == 0);
    dry_run_config_defaults(&c); c.dry_trip_sec = 2;   CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "dry_trip_sec") == 0);
    dry_run_config_defaults(&c); c.dry_trip_sec = 30;  CHECK(dry_run_config_validate(&c, &bad));
    dry_run_config_defaults(&c); c.dry_trip_sec = 31;  CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "dry_trip_sec") == 0);
    dry_run_config_defaults(&c); c.retry_wait_min = 0; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "retry_wait_min") == 0);
    dry_run_config_defaults(&c); c.retry_wait_min = 60; CHECK(dry_run_config_validate(&c, &bad));
    dry_run_config_defaults(&c); c.retry_wait_min = 61; CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "retry_wait_min") == 0);
    dry_run_config_defaults(&c); c.max_retries = 6;    CHECK(!dry_run_config_validate(&c, &bad) && strcmp(bad, "max_retries") == 0);
    dry_run_config_defaults(&c);
    CHECK(dry_run_wait_ms(&c, 1) == 5u * 60000u);
    CHECK(dry_run_wait_ms(&c, 2) == 10u * 60000u);
    CHECK(dry_run_wait_ms(&c, 3) == 20u * 60000u);
    c.retry_backoff = false; CHECK(dry_run_wait_ms(&c, 3) == 5u * 60000u);
    /* backoff cap is now 60 min */
    c.retry_backoff = true; c.retry_wait_min = 40; CHECK(dry_run_wait_ms(&c, 2) == 60u * 60000u);
    c.retry_wait_min = 60; CHECK(dry_run_wait_ms(&c, 5) == 60u * 60000u);
    CHECK(dry_run_suggest_prime_sec(8000) == 17);      /* ceil(12) + 5 */
    CHECK(dry_run_suggest_prime_sec(8100) == 18);      /* ceil(12.15) + 5 */
    CHECK(dry_run_suggest_prime_sec(0) == 5);
    CHECK(dry_run_suggest_prime_sec(1000000) == 120);  /* clamp */
}

/* ---- state machine ---- */

static uint8_t g_clear_reason;
static float g_value;

static void mk(dry_run_config_t *c, dry_run_rt_t *rt, uint32_t t0)
{
    dry_run_config_defaults(c);
    c->enabled = true;
    dry_run_begin(rt, c, false, t0);
}

static uint8_t run_w(const dry_run_config_t *c, dry_run_rt_t *rt, bool relay, bool flow, uint32_t t, bool water)
{
    dry_run_input_t in = { .relay_on = relay, .flow_on = flow, .test_active = false, .water_running = water };
    dry_run_out_t out;
    dry_run_step(c, rt, &in, t, &out);
    g_clear_reason = out.clear_reason;
    g_value = out.value;
    return out.actions;
}

static uint8_t run(const dry_run_config_t *c, dry_run_rt_t *rt, bool relay, bool flow, uint32_t t)
{
    return run_w(c, rt, relay, flow, t, true);
}

/* relay on, no flow, 1 s ticks from t0 until a RAISE; returns the trip time, or 0 if none within limit_s */
static uint32_t until_trip(const dry_run_config_t *c, dry_run_rt_t *rt, uint32_t t0, uint32_t limit_s)
{
    for (uint32_t s = 0; s <= limit_s; s++) {
        if (run(c, rt, true, false, t0 + s * 1000u) & DRY_RUN_ACT_RAISE) {
            return t0 + s * 1000u;
        }
    }
    return 0;
}

static uint8_t ack(const dry_run_config_t *c, dry_run_rt_t *rt, bool water, uint32_t t)
{
    dry_run_out_t out;
    dry_run_ack(c, rt, water, t, &out);
    g_clear_reason = out.clear_reason;
    return out.actions;
}

void test_dry_run_machine(void)
{
    dry_run_config_t c;
    dry_run_rt_t rt;
    uint8_t a = 0;
    uint32_t t = 0;

    /* trips_after_prime_and_trip_time */
    mk(&c, &rt, 0);
    for (uint32_t s = 0; s < 24; s++) {
        CHECK(!(run(&c, &rt, true, false, s * 1000u) & DRY_RUN_ACT_RAISE));
    }
    a = run(&c, &rt, true, false, 24000);
    CHECK((a & (DRY_RUN_ACT_RAISE | DRY_RUN_ACT_INHIBIT)) == (DRY_RUN_ACT_RAISE | DRY_RUN_ACT_INHIBIT));
    CHECK(a & DRY_RUN_ACT_STATE_CHANGED);
    CHECK(rt.state == DRY_RUN_WAIT && rt.trips == 1 && g_value == 1.0f && rt.alarm_active);

    /* wait_then_resume */
    CHECK(dry_run_wait_remaining_sec(&rt, 24000) == 300);
    CHECK(run(&c, &rt, false, false, 24000 + 299000u) == 0);
    a = run(&c, &rt, false, false, 24000 + 300000u);
    CHECK((a & DRY_RUN_ACT_RELEASE_RESUME) && rt.state == DRY_RUN_MONITOR && rt.alarm_active);
    CHECK(dry_run_wait_remaining_sec(&rt, 24000 + 300000u) == 0);

    /* user_stop_during_wait */
    mk(&c, &rt, 0);
    until_trip(&c, &rt, 0, 30);
    a = run_w(&c, &rt, false, false, 24000 + 300000u, false);
    CHECK((a & DRY_RUN_ACT_RELEASE) && !(a & DRY_RUN_ACT_RELEASE_RESUME) && rt.alarm_active);

    /* short_on_periods_accumulate */
    mk(&c, &rt, 0);
    uint32_t trip_t = 0;
    for (uint32_t s = 0; s < 120 && !trip_t; s++) {
        bool on = s < 20 || (s >= 80 && s < 100);
        if (run(&c, &rt, on, false, s * 1000u) & DRY_RUN_ACT_RAISE) {
            trip_t = s;
        }
    }
    CHECK(trip_t == 99);

    /* flow_blip_resets_accum */
    mk(&c, &rt, 0);
    for (uint32_t s = 0; s <= 22; s++) {            /* ticks 15..22: 8 s of dry after prime */
        CHECK(!(run(&c, &rt, true, false, s * 1000u) & DRY_RUN_ACT_RAISE));
    }
    CHECK(rt.dry_accum_ms == 8000u);
    run(&c, &rt, true, true, 23000);                /* blip */
    CHECK(rt.dry_accum_ms == 0);
    trip_t = 0;
    for (uint32_t s = 24; s < 60 && !trip_t; s++) {
        if (run(&c, &rt, true, false, s * 1000u) & DRY_RUN_ACT_RAISE) {
            trip_t = s;
        }
    }
    CHECK(trip_t == 33);                            /* 10 s after the blip */

    /* prime_honoured_each_on */
    mk(&c, &rt, 0);
    for (uint32_t s = 0; s < 10; s++) run(&c, &rt, true, false, s * 1000u);
    for (uint32_t s = 10; s < 20; s++) run(&c, &rt, false, false, s * 1000u);
    CHECK(rt.dry_accum_ms == 0);
    for (uint32_t s = 20; s < 35; s++) {            /* new ON at 20: ticks 20..34 add nothing */
        run(&c, &rt, true, false, s * 1000u);
        CHECK(rt.dry_accum_ms == 0);
    }
    run(&c, &rt, true, false, 35000);
    CHECK(rt.dry_accum_ms == 1000u);

    /* backoff_and_lock */
    mk(&c, &rt, 0);
    t = until_trip(&c, &rt, 0, 30);
    CHECK(t == 24000 && rt.state == DRY_RUN_WAIT && rt.trips == 1);
    uint32_t waits[3] = { 5, 10, 20 };
    for (int i = 0; i < 3; i++) {
        CHECK(dry_run_wait_remaining_sec(&rt, t) == waits[i] * 60u);
        t += waits[i] * 60000u;
        a = run(&c, &rt, false, false, t);
        CHECK(a & DRY_RUN_ACT_RELEASE_RESUME);
        t = until_trip(&c, &rt, t + 1000u, 30);
        CHECK(t != 0);
        if (i < 2) {
            CHECK(rt.state == DRY_RUN_WAIT && rt.trips == i + 2);
        }
    }
    CHECK(rt.state == DRY_RUN_LOCKED && rt.trips == 4 && g_value == 4.0f);
    a = run(&c, &rt, false, false, t + 36000000u);
    CHECK(rt.state == DRY_RUN_LOCKED && !(a & (DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_RELEASE_RESUME)));

    /* max_retries_zero_locks_first_trip */
    mk(&c, &rt, 0);
    c.max_retries = 0;
    t = until_trip(&c, &rt, 0, 30);
    CHECK(t == 24000 && rt.state == DRY_RUN_LOCKED);

    /* recovery_clears */
    mk(&c, &rt, 0);
    t = until_trip(&c, &rt, 0, 30);
    t += 300000u;
    run(&c, &rt, false, false, t);                  /* WAIT ends */
    for (uint32_t s = 1; s <= 5; s++) {   /* streak starts at s=1, held 5 s at s=6 */
        CHECK(!(run(&c, &rt, true, true, t + s * 1000u) & DRY_RUN_ACT_CLEAR));
    }
    a = run(&c, &rt, true, true, t + 6000u);
    CHECK((a & DRY_RUN_ACT_CLEAR) && g_clear_reason == 1 && rt.trips == 0 && !rt.alarm_active);

    /* ack_in_wait */
    mk(&c, &rt, 0);
    t = until_trip(&c, &rt, 0, 30);
    a = ack(&c, &rt, true, t + 1000u);
    CHECK((a & (DRY_RUN_ACT_RELEASE_RESUME | DRY_RUN_ACT_CLEAR)) == (DRY_RUN_ACT_RELEASE_RESUME | DRY_RUN_ACT_CLEAR));
    CHECK(g_clear_reason == 2 && rt.state == DRY_RUN_MONITOR && rt.trips == 0 && !rt.alarm_active);
    CHECK(ack(&c, &rt, true, t + 2000u) == 0);      /* nothing active */

    /* ack_in_wait, water stopped */
    mk(&c, &rt, 0);
    t = until_trip(&c, &rt, 0, 30);
    a = ack(&c, &rt, false, t + 1000u);
    CHECK((a & DRY_RUN_ACT_RELEASE) && !(a & DRY_RUN_ACT_RELEASE_RESUME) && (a & DRY_RUN_ACT_CLEAR));

    /* ack_in_locked */
    mk(&c, &rt, 0);
    c.max_retries = 0;
    t = until_trip(&c, &rt, 0, 30);
    a = ack(&c, &rt, true, t + 5000u);
    CHECK((a & DRY_RUN_ACT_RELEASE_RESUME) && (a & DRY_RUN_ACT_CLEAR) && g_clear_reason == 2);
    CHECK(rt.state == DRY_RUN_MONITOR && rt.trips == 0);

    /* ack_in_monitor_after_retry */
    mk(&c, &rt, 0);
    t = until_trip(&c, &rt, 0, 30) + 300000u;
    run(&c, &rt, false, false, t);
    a = ack(&c, &rt, true, t + 1000u);
    CHECK((a & DRY_RUN_ACT_CLEAR) && !(a & (DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_RELEASE_RESUME)));
    CHECK(rt.state == DRY_RUN_MONITOR && rt.trips == 0 && rt.dry_accum_ms == 0);

    /* test_active_invisible */
    mk(&c, &rt, 0);
    {
        bool raised = false;
        for (uint32_t s = 0; s < 200; s++) {
            dry_run_input_t in = { .relay_on = true, .flow_on = false, .test_active = true, .water_running = false };
            dry_run_out_t o;
            dry_run_step(&c, &rt, &in, s * 1000u, &o);
            raised = raised || (o.actions & DRY_RUN_ACT_RAISE);
        }
        CHECK(!raised && rt.dry_accum_ms == 0 && rt.state == DRY_RUN_MONITOR);
    }

    /* disable_while_locked */
    {
        dry_run_config_t oc, nc;
        mk(&oc, &rt, 0);
        oc.max_retries = 0;
        until_trip(&oc, &rt, 0, 30);
        CHECK(rt.state == DRY_RUN_LOCKED);
        nc = oc;
        nc.enabled = false;
        dry_run_out_t o;
        dry_run_config_changed(&oc, &nc, &rt, 40000u, &o);
        CHECK((o.actions & (DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_CLEAR)) == (DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_CLEAR));
        CHECK(o.clear_reason == 3 && rt.state == DRY_RUN_DISABLED);
        run(&nc, &rt, true, false, 41000u);
        CHECK(rt.state == DRY_RUN_DISABLED);
        dry_run_config_changed(&nc, &oc, &rt, 50000u, &o);
        CHECK(rt.state == DRY_RUN_MONITOR && rt.trips == 0 && !rt.alarm_active);
    }

    /* locked_at_boot */
    dry_run_config_defaults(&c);
    c.enabled = true;
    dry_run_begin(&rt, &c, true, 0);
    CHECK(rt.state == DRY_RUN_LOCKED && rt.alarm_active && rt.trips == c.max_retries + 1);
    c.enabled = false;
    dry_run_begin(&rt, &c, true, 0);
    CHECK(rt.state == DRY_RUN_DISABLED && !rt.alarm_active);

    /* wrap */
    mk(&c, &rt, 0xFFFFF000u);
    t = until_trip(&c, &rt, 0xFFFFF000u, 30);
    CHECK(t == 0xFFFFF000u + 24000u && rt.state == DRY_RUN_WAIT);
    a = run(&c, &rt, false, false, t + 300000u);
    CHECK((a & DRY_RUN_ACT_RELEASE_RESUME) && rt.state == DRY_RUN_MONITOR);
}
