#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure "stable for N ms" debounce of a contact. Wrap-safe (unsigned math). */
typedef struct {
    bool state;               /* accepted (debounced) state */
    bool candidate;           /* raw level currently being timed */
    uint32_t candidate_since_ms;
} contact_debounce_t;

void contact_debounce_init(contact_debounce_t *d, bool initial_state, uint32_t now_ms);

/* Feed one raw sample. Returns true exactly when d->state changes. */
bool contact_debounce_update(contact_debounce_t *d, bool raw, uint32_t now_ms, uint32_t stable_ms);

#ifdef __cplusplus
}
#endif
