#include "ota_lock.h"

#include <stdatomic.h>

static atomic_bool s_held = false;

bool ota_lock_claim(void) {
  bool expected = false;
  return atomic_compare_exchange_strong(&s_held, &expected, true);
}

void ota_lock_release(void) { atomic_store(&s_held, false); }

bool ota_lock_held(void) { return atomic_load(&s_held); }
