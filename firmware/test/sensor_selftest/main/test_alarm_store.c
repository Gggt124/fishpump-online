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
    CHECK(set.count == 12 && set.next_id == 13);
    for (int i = 0; i < set.count; i++) {
        CHECK(set.rules[i].id == i + 1 && !set.rules[i].enabled && alarm_rule_validate(&set.rules[i], &f));
    }
    /* the dry-run preset is retired: rule 5 is now "flow while pump off" (is_true, gate relay1 false) */
    CHECK(set.rules[4].signal_id == 52 && set.rules[4].condition == ALARM_COND_IS_TRUE
          && set.rules[4].gate_mode == ALARM_GATE_WHEN_FALSE && set.rules[4].gate_signal_id == 41
          && set.rules[4].delay_sec == 30);
    CHECK(set.rules[3].latch && set.rules[3].severity == ALARM_SEV_CRITICAL);

    /* blob v2: [2][count][next_id u16][rec_size u16][count x rec_size] */
    static uint8_t buf[ALARM_BLOB_READ_MAX_BYTES];
    static alarm_rule_set_t back;
    uint8_t dropped;
    const size_t rs = sizeof(alarm_rule_t);
    size_t len = alarm_blob_encode(&set, buf, sizeof(buf));
    CHECK(len == ALARM_BLOB_HEADER_BYTES + 12 * rs && buf[0] == 2 && buf[0] == ALARM_BLOB_VERSION);
    CHECK((size_t)(buf[4] | (buf[5] << 8)) == rs);
    CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 0 && back.count == 12 && back.next_id == 13
          && memcmp(&back.rules[0], &set.rules[0], rs) == 0
          && memcmp(&back.rules[11], &set.rules[11], rs) == 0);
    CHECK(!alarm_blob_decode(buf, len - 1, &back, &dropped)); /* truncated */
    CHECK(!alarm_blob_decode(buf, 5, &back, &dropped));       /* shorter than the header */
    buf[0] = 9;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* unknown version */
    buf[0] = 3;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* version 3 */
    buf[0] = ALARM_BLOB_VERSION;
    buf[1] = 25;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* count > max */
    buf[1] = 12;
    buf[4] = 0;
    buf[5] = 0;
    CHECK(!alarm_blob_decode(buf, len, &back, &dropped)); /* rec_size == 0 */
    buf[4] = (uint8_t)(rs & 0xFF);
    buf[5] = (uint8_t)(rs >> 8);
    CHECK(!alarm_blob_decode(buf, len + 1, &back, &dropped)); /* length mismatch */
    CHECK(alarm_blob_decode(buf, len, &back, &dropped) && back.count == 12);

    /* v1 blob written by the previous firmware: [1][count][next_id u16][count x alarm_rule_t] */
    {
        static uint8_t v1[ALARM_BLOB_HEADER_BYTES + 3 * sizeof(alarm_rule_t)];
        v1[0] = ALARM_BLOB_V1;
        v1[1] = 3;
        v1[2] = 40;
        v1[3] = 0;
        for (int i = 0; i < 3; i++) {
            alarm_rule_t r = set.rules[i];
            r.id = (uint16_t)(10 + i);
            r.enabled = true;
            r.threshold = 1.5f + (float)i;
            r.delay_sec = (uint16_t)(5 + i);
            memcpy(v1 + 4 + i * rs, &r, rs);
        }
        size_t v1len = 4 + 3 * rs;
        CHECK(alarm_blob_decode(v1, v1len, &back, &dropped) && dropped == 0 && back.count == 3 && back.next_id == 40);
        for (int i = 0; i < 3; i++) {
            CHECK(memcmp(&back.rules[i], v1 + 4 + i * rs, rs) == 0 && back.rules[i].enabled);
        }
        CHECK(!alarm_blob_decode(v1, v1len - 1, &back, &dropped)); /* v1 keeps the exact-length rule */
        CHECK(!alarm_blob_decode(v1, v1len + 1, &back, &dropped));
    }

    /* v2 with a larger record: extra tail bytes are ignored */
    {
        static uint8_t big[ALARM_BLOB_READ_MAX_BYTES];
        size_t brs = rs + 8;
        big[0] = 2;
        big[1] = 12;
        big[2] = 13;
        big[3] = 0;
        big[4] = (uint8_t)(brs & 0xFF);
        big[5] = (uint8_t)(brs >> 8);
        for (int i = 0; i < 12; i++) {
            memcpy(big + ALARM_BLOB_HEADER_BYTES + i * brs, &set.rules[i], rs);
            memset(big + ALARM_BLOB_HEADER_BYTES + i * brs + rs, 0xAA, 8);
        }
        size_t blen = ALARM_BLOB_HEADER_BYTES + 12 * brs;
        CHECK(blen <= sizeof(big));
        CHECK(alarm_blob_decode(big, blen, &back, &dropped) && dropped == 0 && back.count == 12);
        for (int i = 0; i < 12; i++) {
            CHECK(memcmp(&back.rules[i], &set.rules[i], rs) == 0);
        }
    }

    /* v2 with a smaller record: must not read past len; result is false or every rule valid */
    {
        static uint8_t small[ALARM_BLOB_MAX_BYTES];
        size_t srs = rs - 4;
        small[0] = 2;
        small[1] = 12;
        small[2] = 13;
        small[3] = 0;
        small[4] = (uint8_t)(srs & 0xFF);
        small[5] = (uint8_t)(srs >> 8);
        for (int i = 0; i < 12; i++) {
            memcpy(small + ALARM_BLOB_HEADER_BYTES + i * srs, &set.rules[i], srs);
        }
        size_t slen = ALARM_BLOB_HEADER_BYTES + 12 * srs;
        bool sok = alarm_blob_decode(small, slen, &back, &dropped);
        bool all_valid = true;
        if (sok) {
            for (int i = 0; i < back.count; i++) {
                all_valid = all_valid && alarm_rule_validate(&back.rules[i], &f);
            }
        }
        CHECK(!sok || all_valid);
    }

    set.rules[2].signal_id = 200; /* a signal this firmware lacks */
    len = alarm_blob_encode(&set, buf, sizeof(buf));
    CHECK(alarm_blob_decode(buf, len, &back, &dropped) && dropped == 1 && back.count == 11 && back.next_id == 13);
    CHECK(alarm_blob_encode(&set, buf, 10) == 0);
}
