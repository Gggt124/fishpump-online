#pragma once

#include "app_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Stateful session system using 4 in-memory slots.
 * Tokens are 64 char hex strings generated via esp_random.
 */

#define SESSION_TOKEN_LEN 65
#define SESSION_MAX_AGE_SEC APP_TEMPLATE_SESSION_MAX_AGE_SEC

bool session_init(void);
bool session_create(const char *username, const char *client_ip,
                    char token_out[SESSION_TOKEN_LEN]);
bool session_validate(const char *token, const char *client_ip);
void session_destroy(const char *token);
void session_invalidate_all(void);
void session_invalidate_others(const char *current_token);

#ifdef __cplusplus
}
#endif
