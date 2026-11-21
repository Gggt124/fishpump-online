#pragma once

#include <stdbool.h>

#include "api_json.h"

/* Largest JSON bodies the pump POSTs accept (HTTP buffer sizes and the
 * max_params_bytes of the write methods). */
#define PUMP_CONFIG_BODY_BYTES 512
#define PUMP_CONTROL_BODY_BYTES 256

void dapi_pump_register(void);

/* Appends the pump runtime fields to a json_gen document (also used by /api/status). */
bool api_pump_add_status_fields_gen(json_gen_t *gen);
