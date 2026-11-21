#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the ThingsBoard state source with the uplink (client attribute and status telemetry builders)
 * and the device_api write hook that asks for the attributes to be rebuilt. Call after device_api_init(). */
esp_err_t tb_state_init(void);

#ifdef __cplusplus
}
#endif
