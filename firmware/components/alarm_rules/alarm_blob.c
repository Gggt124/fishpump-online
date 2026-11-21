#include <string.h>

#include "alarm_rules.h"

#define BLOB_V1_HEADER_BYTES 4
#define BLOB_MAX_REC_BYTES 1024

size_t alarm_blob_encode(const alarm_rule_set_t *set, uint8_t *buf, size_t size)
{
    if (set->count > ALARM_MAX_RULES) {
        return 0;
    }
    size_t need = ALARM_BLOB_HEADER_BYTES + (size_t)set->count * sizeof(alarm_rule_t);
    if (size < need) {
        return 0;
    }
    buf[0] = ALARM_BLOB_VERSION;
    buf[1] = set->count;
    buf[2] = (uint8_t)(set->next_id & 0xFF);
    buf[3] = (uint8_t)(set->next_id >> 8);
    buf[4] = (uint8_t)(sizeof(alarm_rule_t) & 0xFF);
    buf[5] = (uint8_t)(sizeof(alarm_rule_t) >> 8);
    memcpy(buf + ALARM_BLOB_HEADER_BYTES, set->rules, (size_t)set->count * sizeof(alarm_rule_t));
    return need;
}

bool alarm_blob_decode(const uint8_t *buf, size_t len, alarm_rule_set_t *out, uint8_t *dropped)
{
    uint8_t dummy;
    if (dropped == NULL) {
        dropped = &dummy;
    }
    *dropped = 0;
    if (len < BLOB_V1_HEADER_BYTES || (buf[0] != ALARM_BLOB_V1 && buf[0] != ALARM_BLOB_VERSION)) {
        return false;
    }
    uint8_t count = buf[1];
    size_t header = BLOB_V1_HEADER_BYTES;
    size_t rec_size = ALARM_BLOB_V1_REC_SIZE; /* v1 stored the struct as it was then, not as it is now */
    if (buf[0] == ALARM_BLOB_VERSION) {
        if (len < ALARM_BLOB_HEADER_BYTES) {
            return false;
        }
        header = ALARM_BLOB_HEADER_BYTES;
        rec_size = (size_t)(buf[4] | (buf[5] << 8));
        if (rec_size < 1 || rec_size > BLOB_MAX_REC_BYTES) {
            return false;
        }
    }
    if (count > ALARM_MAX_RULES || len != header + (size_t)count * rec_size) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    uint16_t next_id = (uint16_t)(buf[2] | (buf[3] << 8));
    size_t copy = rec_size < sizeof(alarm_rule_t) ? rec_size : sizeof(alarm_rule_t);
    for (uint8_t i = 0; i < count; i++) {
        alarm_rule_t r;
        const char *bad;
        memset(&r, 0, sizeof(r)); /* a shorter record leaves the new fields at 0 = default */
        memcpy(&r, buf + header + (size_t)i * rec_size, copy);
        /* A rule naming a signal this firmware lacks (or otherwise invalid) is
         * skipped; the user's other rules are kept. */
        if (r.id == 0 || !alarm_rule_validate(&r, &bad)) {
            (*dropped)++;
            continue;
        }
        out->rules[out->count++] = r;
        if (r.id >= next_id) {
            next_id = (uint16_t)(r.id + 1); /* ids are never reused */
        }
    }
    out->next_id = next_id == 0 ? 1 : next_id;
    return true;
}
