#pragma once

/* Largest JSON bodies the cooling POSTs accept (HTTP buffer sizes and the
 * max_params_bytes of the write methods). */
#define COOLING_CONFIG_BODY_BYTES 1536
#define COOLING_MODE_BODY_BYTES 256

void dapi_cooling_register(void);
