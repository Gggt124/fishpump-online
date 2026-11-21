#pragma once

/* Largest JSON bodies the protection POSTs accept (HTTP buffer sizes and the
 * max_params_bytes of the write methods). */
#define PROTECTION_POST_BODY_BYTES 384
#define PROTECTION_MEASURE_BODY_BYTES 128

void dapi_protection_register(void);
