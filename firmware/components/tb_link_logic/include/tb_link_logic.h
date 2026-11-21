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

/* Longest wait for the RPC reply to be acked before an action that drops the link (staged Wi-Fi restart). */
#define TB_AFTER_REPLY_MAX_MS 2000u

/* Reconnect delay after a failed MQTT connect. While a Wi-Fi switch is staged the delay stays at the minimum so
 * the confirmation reaches ThingsBoard quickly; otherwise at_cap pins it to the maximum, else it doubles. */
uint32_t tb_backoff_after_fail_ms(uint32_t current_ms, bool at_cap, bool staging);

/* True once the reply was acked or TB_AFTER_REPLY_MAX_MS passed since it was published. */
bool tb_after_reply_ready(bool acked, uint32_t elapsed_ms);

/* Card text for an MQTT transport error (spec 6.6: a TLS failure must say so). tls: the session uses TLS;
 * handshake_failed: esp-tls reported ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED; cert_flags: the certificate verify
 * flags esp-tls reported (0 = none). Without TLS, or without a TLS failure, it is the generic transport error. */
const char *tb_transport_error_text(bool tls, bool handshake_failed, uint32_t cert_flags);

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

/* ---- ThingsBoard firmware OTA (pure helpers) ---- */
#define TB_FW_TITLE                "fishpump"
#define TB_FW_CHUNK_SIZE           16384u
#define TB_FW_CHUNK_SIZE_STR       "16384"
#define TB_FW_CHUNK_TIMEOUT_MS     10000u  /* per attempt */
#define TB_FW_CHUNK_TRIES          3       /* attempts per chunk */
#define TB_FW_VERIFY_TIMEOUT_MS    300000u /* 5 min */
#define TB_FW_ATTR_TOPIC           "v1/devices/me/attributes"
#define TB_FW_ATTR_REQ_PREFIX      "v1/devices/me/attributes/request/"
#define TB_FW_ATTR_RESP_PREFIX     "v1/devices/me/attributes/response/"
#define TB_FW_REQ_PREFIX           "v2/fw/request/"
#define TB_FW_RESP_PREFIX          "v2/fw/response/"
#define TB_FW_ERROR_TOPIC          "v2/fw/error"
#define TB_TELEMETRY_TOPIC         "v1/devices/me/telemetry"
#define TB_FW_ATTR_REQUEST_BODY    "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}"
#define TB_FW_VERSION_MAX          33 /* incl. NUL */

typedef struct {
    char title[33];
    char version[TB_FW_VERSION_MAX];
    uint32_t size;
    char checksum[65]; /* lowercase hex */
} tb_fw_info_t;

typedef enum { TB_FW_ATTR_NONE = 0, TB_FW_ATTR_CLEARED, TB_FW_ATTR_INFO, TB_FW_ATTR_INVALID } tb_fw_attr_t;
/* Attribute push ({"fw_title":...} or {"deleted":[...]}) or request reply ({"shared":{...}}).
 * NONE: no fw_title and not deleted. CLEARED: fw_title deleted, or absent from the shared reply. INFO: all fw_*
 * fields valid, *out filled. INVALID: not an object, or fw_title present with bad other fields (out->title is the
 * title when it fits in 32 bytes, else ""). Fields of *out other than title are valid only on INFO. */
tb_fw_attr_t tb_fw_parse_attrs(const char *json, size_t len, tb_fw_info_t *out);

typedef enum { TB_FW_IGNORE = 0, TB_FW_START, TB_FW_FAIL } tb_fw_decision_t;
/* In order: other title, busy, running version, last failed version -> IGNORE; size > max_size -> FAIL with
 * *reason "too_large" (reason may be NULL; set only on FAIL); else START. last_failed_version may be NULL. */
tb_fw_decision_t tb_fw_decide(const tb_fw_info_t *fw, const char *running_version, const char *last_failed_version,
                              uint32_t max_size, bool busy, const char **reason);

/* Equal after stripping one leading 'v'/'V' from each side. NULL on either side gives false. */
bool tb_fw_version_equal(const char *a, const char *b);
/* True for the fw_error codes that are remembered until reboot (bad_attributes, too_large, bad_chunk,
 * checksum_mismatch, invalid_image). */
bool tb_fw_error_is_final(const char *code);
/* ceil(size / TB_FW_CHUNK_SIZE); expected length of chunk idx, 0 when idx >= count. */
uint32_t tb_fw_chunk_count(uint32_t size);
uint32_t tb_fw_chunk_len(uint32_t size, uint32_t idx);
/* "v2/fw/request/<req>/chunk/<idx>" into out; returns its length, or -1 when it does not fit with the NUL. */
int tb_fw_request_topic(uint32_t req_id, uint32_t idx, char *out, size_t size);
/* "v2/fw/response/<req>/chunk/<idx>" (topic_len bytes, not NUL terminated); same decimal rules as
 * tb_rpc_parse_topic. */
bool tb_fw_parse_chunk_topic(const char *topic, size_t topic_len, uint32_t *req_id, uint32_t *idx);
/* 32 digest bytes against 64 hex characters, case-insensitive. */
bool tb_fw_digest_matches(const uint8_t digest[32], const char *hex64);
/* Keys fw_state, fw_error, current_fw_title, current_fw_version in that order; a NULL argument leaves its key out
 * (all NULL gives "{}"). Returns the length, or -1 when it does not fit with the NUL. */
int tb_fw_state_json(char *buf, size_t size, const char *state, const char *error,
                     const char *cur_title, const char *cur_version);

typedef enum { TB_FW_BOOT_NONE = 0, TB_FW_BOOT_VERIFY, TB_FW_BOOT_UPDATED, TB_FW_BOOT_ROLLED_BACK } tb_fw_boot_t;
/* NULL pending: NONE. Pending equals running: VERIFY while pending_verify, else UPDATED. Else ROLLED_BACK. */
tb_fw_boot_t tb_fw_boot_state(const char *pending_version, const char *running_version, bool pending_verify);

#ifdef __cplusplus
}
#endif
