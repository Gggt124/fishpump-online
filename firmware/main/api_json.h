#pragma once

#include <stdint.h>

/* Helpers shared by web_server.c and the dapi_<group>.c method files. */

/* Milliseconds since boot (wraps at 32 bits). */
uint32_t sensors_now_ms(void);
/* Dry-run guard state as the API names it ("monitor", "wait", "locked", "disabled"). */
const char *dry_run_state_name(uint8_t state);
/* Alarm history event type as the API names it ("raised", "cleared", "acked"). */
const char *alarms_type_name(uint8_t type);
