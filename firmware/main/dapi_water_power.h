#pragma once

/* Largest JSON body the water and power POSTs accept (HTTP buffer size and the
 * max_params_bytes of the write methods). */
#define SENSORS_BODY_BYTES 256

void dapi_water_power_register(void);
