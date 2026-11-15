#include "contact_debounce.h"

void contact_debounce_init(contact_debounce_t *d, bool initial_state, uint32_t now_ms)
{
    d->state = initial_state;
    d->candidate = initial_state;
    d->candidate_since_ms = now_ms;
}

bool contact_debounce_update(contact_debounce_t *d, bool raw, uint32_t now_ms, uint32_t stable_ms)
{
    if (raw == d->state) {
        /* Back at the accepted level: forget any pending change. */
        d->candidate = raw;
        d->candidate_since_ms = now_ms;
        return false;
    }
    if (raw != d->candidate) {
        d->candidate = raw;
        d->candidate_since_ms = now_ms;
        return false;
    }
    if ((uint32_t)(now_ms - d->candidate_since_ms) >= stable_ms) {
        d->state = raw;
        return true;
    }
    return false;
}
