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

/* ---- ThingsBoard server-side RPC ---- */
#define TB_RPC_REQ_PREFIX  "v1/devices/me/rpc/request/"
#define TB_RPC_RESP_PREFIX "v1/devices/me/rpc/response/"
#define TB_RPC_MAX_REQUEST 1792 /* > largest HTTP body (1536) + envelope; < mqtt buffer 2048 */
#define TB_RPC_METHOD_MAX  48

/* Topic "v1/devices/me/rpc/request/<decimal id>" (topic_len bytes, not NUL terminated): true and *req_id set.
 * False for another prefix, an empty or non-decimal id, or an id over 4294967295. */
bool tb_rpc_parse_topic(const char *topic, size_t topic_len, uint32_t *req_id);

typedef enum { TB_RPC_PARSE_OK = 0, TB_RPC_PARSE_BAD_JSON, TB_RPC_PARSE_NO_METHOD } tb_rpc_parse_t;
/* Envelope {"method":"...","params":...}. BAD_JSON: not a JSON object. NO_METHOD: method missing, not a string,
 * empty or not fitting method_size with its NUL. OK: method copied; *params = the params item detached from the
 * envelope (any JSON type) or NULL when absent; the caller deletes it. *params is NULL unless OK. */
tb_rpc_parse_t tb_rpc_parse_request(const char *payload, size_t len, char *method, size_t method_size,
                                    cJSON **params);

/* "v1/devices/me/rpc/response/<id>" into out; returns its length, or -1 when it does not fit with the NUL. */
int tb_rpc_response_topic(uint32_t req_id, char *out, size_t out_size);

/* ---- RPC duplicate suppression (ring of the last handled requests) ----
 * ThingsBoard re-sends a pending request after a re-subscribe, and the id counter can restart, so the key is the
 * request id + payload length + a hash of the payload bytes. A hit inside the window means: re-publish the cached
 * reply, do not run the method again. The ring holds keys only; the caller keeps the reply bytes per slot. */
#define TB_RPC_DEDUP_SLOTS     4
#define TB_RPC_DEDUP_WINDOW_MS 30000u /* longer than the widget's 10 s timeout */

typedef struct {
    bool used;
    uint32_t req_id;
    uint32_t len;
    uint32_t hash;
    uint32_t at_ms;
    uint32_t seq; /* insert order, for eviction */
} tb_rpc_dedup_entry_t;

typedef struct {
    tb_rpc_dedup_entry_t e[TB_RPC_DEDUP_SLOTS];
    uint32_t seq;
} tb_rpc_dedup_t;

/* FNV-1a 32 bit over the bytes */
uint32_t tb_rpc_hash(const char *data, size_t len);
void tb_rpc_dedup_init(tb_rpc_dedup_t *d);
/* Slot of an entry with this key that is at most TB_RPC_DEDUP_WINDOW_MS old (uint32 ms subtraction), else -1. */
int tb_rpc_dedup_find(const tb_rpc_dedup_t *d, uint32_t req_id, const char *payload, size_t len, uint32_t now_ms);
/* Record a handled request and return its slot: the slot of the same key if present, else a free or expired
 * slot, else the oldest entry (evicted). The caller replaces that slot's cached reply. */
int tb_rpc_dedup_insert(tb_rpc_dedup_t *d, uint32_t req_id, const char *payload, size_t len, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
