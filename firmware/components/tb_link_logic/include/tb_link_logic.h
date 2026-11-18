#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB_HOST_MAX 64 /* incl. NUL */
#define TB_NAME_MAX 48
#define TB_TOKEN_MAX 65
#define TB_PROV_FIELD_MAX 33 /* key and secret */
#define TB_DEFAULT_PORT 1883
#define TB_BACKOFF_MIN_MS 5000u
#define TB_BACKOFF_MAX_MS 60000u

typedef struct {
    char host[TB_HOST_MAX]; /* "" = link off */
    uint16_t port;
    bool tls;
    char device_name[TB_NAME_MAX];
    char token[TB_TOKEN_MAX]; /* "" = not provisioned */
    char provision_key[TB_PROV_FIELD_MAX];
    char provision_secret[TB_PROV_FIELD_MAX];
} tb_link_config_t;

/* host "", port 1883, tls false, default device name */
void tb_link_config_defaults(tb_link_config_t *cfg, const uint8_t sta_mac[6]);
/* "fishpump-a1b2c3" from the last three MAC bytes */
void tb_default_device_name(const uint8_t sta_mac[6], char *out, size_t size);
/* *bad_field (may be NULL) = JSON key name of the first invalid field */
bool tb_link_config_valid(const tb_link_config_t *cfg, const char **bad_field);

/* Merge a POST body into *out (starting from *current). Absent key = keep; "" for token/provision_secret = clear.
 * Returns false with *bad_field set on wrong type, too long, or invalid result. */
bool tb_link_config_from_json(const cJSON *body, const tb_link_config_t *current,
                              tb_link_config_t *out, const char **bad_field);
/* Public view: host, port, tls, device_name, provision_key, has_token, has_provision_secret. Never token/secret. */
cJSON *tb_link_config_to_json(const tb_link_config_t *cfg);

/* Provisioning request JSON into buf; returns bytes written (excl. NUL) or -1. */
int tb_provision_build_request(const tb_link_config_t *cfg, char *buf, size_t size);

typedef enum { TB_PROV_OK = 0, TB_PROV_FAILURE, TB_PROV_BAD_REPLY } tb_prov_result_t;
/* OK: token copied. FAILURE: errorMsg copied (may be ""). BAD_REPLY: not JSON, no status, or
 * credentialsType != ACCESS_TOKEN (or token does not fit). token is untouched unless OK. */
tb_prov_result_t tb_provision_parse(const char *json, size_t len, char *token, size_t token_size,
                                    char *err, size_t err_size);

/* 0 -> 5000, else min(2*current, 60000) */
uint32_t tb_backoff_next_ms(uint32_t current_ms);

#ifdef __cplusplus
}
#endif
