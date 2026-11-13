#include "selftest.h"
#include "telemetry_hub.h"

static int s_events_seen;

static void count_cb(const telemetry_event_t *event, void *user_ctx)
{
    (void)event;
    (void)user_ctx;
    s_events_seen++;
}

void test_telemetry(void)
{
    telemetry_snapshot_t snap;
    telemetry_hub_init();
    telemetry_hub_subscribe(count_cb, NULL);
    s_events_seen = 0;

    telemetry_event_t p = {.type = TELEMETRY_EVT_POWER};
    p.data.power.channel = 1;
    p.data.power.valid = true;
    p.data.power.current_a = 1.5f;
    telemetry_hub_publish(&p);
    telemetry_hub_get_latest(&snap);
    CHECK(snap.has_power[1] && !snap.has_power[0] && snap.power[1].current_a == 1.5f);

    telemetry_event_t bad = {.type = TELEMETRY_EVT_POWER};
    bad.data.power.channel = TELEMETRY_POWER_CHANNELS;
    bad.data.power.current_a = 9.0f;
    esp_err_t err = telemetry_hub_publish(&bad);
    telemetry_hub_get_latest(&snap);
    CHECK(err == ESP_ERR_INVALID_ARG && !snap.has_power[0] && snap.power[1].current_a == 1.5f);

    telemetry_event_t w = {.type = TELEMETRY_EVT_WATER};
    w.data.water.valid = true;
    w.data.water.ph = 7.1f;
    telemetry_hub_publish(&w);
    telemetry_hub_get_latest(&snap);
    CHECK(snap.has_water && snap.water.ph == 7.1f && snap.water.valid);

    CHECK(s_events_seen == 2); /* the rejected publish is not delivered */
    telemetry_hub_unsubscribe(count_cb);
}
