#include <string.h>

#include "alarm_rules.h"
#include "selftest.h"

void test_alarm_store(void)
{
    /* ring */
    static alarm_ring_t ring;
    alarm_ring_init(&ring);
    alarm_event_t e = {0};
    static alarm_event_t out[40];
    uint32_t oldest;
    CHECK(alarm_ring_since(&ring, 0, out, 40, &oldest) == 0 && oldest == 0);
    for (int i = 0; i < 5; i++) {
        CHECK(alarm_ring_push(&ring, &e) == (uint32_t)(i + 1));
    }
    CHECK(alarm_ring_since(&ring, 2, out, 40, &oldest) == 3 && out[0].seq == 3 && out[2].seq == 5 && oldest == 1);
    CHECK(alarm_ring_since(&ring, 2, out, 2, &oldest) == 2 && out[1].seq == 4); /* max respected, oldest first */
    for (int i = 0; i < 40; i++) {
        alarm_ring_push(&ring, &e); /* overrun: 45 pushed */
    }
    CHECK(alarm_ring_since(&ring, 0, out, 40, &oldest) == 32 && oldest == 14 && out[0].seq == 14 && out[31].seq == 45);
    CHECK(alarm_ring_since(&ring, 45, out, 40, &oldest) == 0 && oldest == 14);

    /* presets */
    static alarm_rule_set_t set;
    const char *f;
    alarm_presets_fill(&set);
    CHECK(set.count == 13 && set.next_id == 14);
    for (int i = 0; i < set.count; i++) {
        CHECK(set.rules[i].id == i + 1 && !set.rules[i].enabled && alarm_rule_validate(&set.rules[i], &f));
    }
    CHECK(set.rules[4].signal_id == 52 && set.rules[4].condition == ALARM_COND_IS_FALSE
          && set.rules[4].gate_mode == ALARM_GATE_WHEN_TRUE && set.rules[4].gate_signal_id == 41
          && set.rules[4].delay_sec == 15);
    CHECK(set.rules[3].latch && set.rules[3].severity == ALARM_SEV_CRITICAL);

    /* blob: Review Focus 2 */
    static uint8_t buf[ALARM_BLOB_MAX_BYTES];
    static alarm_rule_set_t back;
    uint8_t dropped;
    size_t len = alarm_blob_encode(&set, buf, sizeof(buf));
    CHECK(len == 4 + 13 * sizeof(alarm_rule_t) && buf[0] == ALARM_BLOB_VERSION);
    CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 0 && back.count == 13 && back.next_id == 14
          && memcmp(&back.rules[0], &set.rules[0], sizeof(alarm_rule_t)) == 0);
    CHECK(!alarm_blob_decode(buf, len - 1, &back, &dropped)); /* truncated */
    CHECK(!alarm_blob_decode(buf, 3, &back, &dropped));       /* shorter than the header */
    buf[0] = 9;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* unknown version */
    buf[0] = ALARM_BLOB_VERSION;
    buf[1] = 25;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* count > max */
    buf[1] = 13;
    set.rules[2].signal_id = 200; /* a signal this firmware lacks */
    len = alarm_blob_encode(&set, buf, sizeof(buf));
    CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 1 && back.count == 12 && back.next_id == 14);
    CHECK(alarm_blob_encode(&set, buf, 10) == 0);
}
