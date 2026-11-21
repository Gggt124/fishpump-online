#pragma once

#include <stdbool.h>

/* One writer of the OTA slot at a time (web upload or ThingsBoard). */

/* Atomic compare-exchange false -> true. Returns true when this caller now
 * holds the lock. */
bool ota_lock_claim(void);
void ota_lock_release(void);
bool ota_lock_held(void);
