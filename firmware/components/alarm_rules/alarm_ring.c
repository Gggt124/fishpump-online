#include <string.h>

#include "alarm_rules.h"

void alarm_ring_init(alarm_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));
}

uint32_t alarm_ring_push(alarm_ring_t *ring, const alarm_event_t *evt)
{
    alarm_event_t *slot = &ring->items[ring->head];
    *slot = *evt;
    slot->seq = ++ring->last_seq;
    ring->head = (uint8_t)((ring->head + 1) % ALARM_HISTORY_LEN);
    if (ring->count < ALARM_HISTORY_LEN) {
        ring->count++;
    }
    return slot->seq;
}

size_t alarm_ring_since(const alarm_ring_t *ring, uint32_t after_seq, alarm_event_t *out, size_t max,
                        uint32_t *oldest_seq)
{
    if (oldest_seq != NULL) {
        *oldest_seq = ring->count == 0 ? 0 : ring->last_seq - ring->count + 1u;
    }
    size_t n = 0;
    size_t start = (ring->head + ALARM_HISTORY_LEN - ring->count) % ALARM_HISTORY_LEN;
    for (size_t i = 0; i < ring->count && n < max; i++) {
        const alarm_event_t *e = &ring->items[(start + i) % ALARM_HISTORY_LEN];
        if (e->seq > after_seq) {
            out[n++] = *e;
        }
    }
    return n;
}
