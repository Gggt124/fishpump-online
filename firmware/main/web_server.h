#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool web_server_start(void);
void web_server_log_diagnostics(void);
bool web_server_queue_health_check(void);
bool web_server_check_health(uint32_t timeout_ms, uint32_t *out_staleness_ms);

#ifdef __cplusplus
}
#endif
