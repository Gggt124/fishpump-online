#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Failure and cache counters of the HTTP server, reported by status.get. */
typedef struct {
  uint32_t json_send_failures;
  uint32_t static_send_failures;
  uint32_t static_cache_hits;
  uint32_t static_deadline_aborts;
} web_server_http_counters_t;

bool web_server_start(void);
void web_server_get_http_counters(web_server_http_counters_t *out);
void web_server_log_diagnostics(void);
bool web_server_queue_health_check(void);
bool web_server_check_health(uint32_t timeout_ms, uint32_t *out_staleness_ms);

#ifdef __cplusplus
}
#endif
