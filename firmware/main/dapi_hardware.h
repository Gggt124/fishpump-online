#pragma once

/* Largest JSON body the hardware map POST accepts (HTTP buffer size and the
 * max_params_bytes of hardware.map.set). */
#define HARDWARE_MAP_BODY_BYTES 512

void dapi_hardware_register(void);
