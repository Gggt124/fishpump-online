#pragma once

/* Largest JSON body an alarms POST accepts (HTTP buffer size and the
 * max_params_bytes of the write methods). */
#define ALARM_POST_BODY_BYTES 768

void dapi_alarms_register(void);
