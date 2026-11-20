#include "tb_link_logic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static bool in_set(const char *s, bool allow_dot)
{
    for (; *s; s++) {
        char c = *s;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                  (allow_dot ? c == '.' : c == '_');
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* empty or 1..maxlen chars from [A-Za-z0-9_-] */
static bool id_ok(const char *s, size_t maxlen)
{
    size_t n = strnlen(s, maxlen + 1);
    return n <= maxlen && in_set(s, false);
}

static bool name_ok(const char *s)
{
    size_t n = strnlen(s, TB_NAME_MAX);
    if (n < 1 || n > TB_NAME_MAX - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') {
            return false;
        }
    }
    return true;
}

void tb_default_device_name(const uint8_t sta_mac[6], char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    snprintf(out, size, "fishpump-%02x%02x%02x", sta_mac[3], sta_mac[4], sta_mac[5]);
}

void tb_link_config_defaults(tb_link_config_t *cfg, const uint8_t sta_mac[6])
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = TB_DEFAULT_PORT;
    tb_default_device_name(sta_mac, cfg->device_name, sizeof(cfg->device_name));
}

bool tb_link_config_valid(const tb_link_config_t *cfg, const char **bad_field)
{
    const char *dummy;
    if (!bad_field) {
        bad_field = &dummy;
    }
    size_t hl = strnlen(cfg->host, TB_HOST_MAX);
    if (hl > TB_HOST_MAX - 1 || !in_set(cfg->host, true)) {
        *bad_field = "host";
        return false;
    }
    if (cfg->port < 1) {
        *bad_field = "port";
        return false;
    }
    if (!name_ok(cfg->device_name)) {
        *bad_field = "device_name";
        return false;
    }
    if (!id_ok(cfg->token, TB_TOKEN_MAX - 1)) {
        *bad_field = "token";
        return false;
    }
    if (!id_ok(cfg->provision_key, TB_PROV_FIELD_MAX - 1)) {
        *bad_field = "provision_key";
        return false;
    }
    if (!id_ok(cfg->provision_secret, TB_PROV_FIELD_MAX - 1)) {
        *bad_field = "provision_secret";
        return false;
    }
    if (cfg->host[0] && !cfg->token[0] && !(cfg->provision_key[0] && cfg->provision_secret[0])) {
        *bad_field = "token";
        return false;
    }
    return true;
}

/* Copy a JSON string field into dst. Absent = keep. Wrong type or too long = false. */
static bool take_str(const cJSON *body, const char *key, char *dst, size_t dst_size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || !it->valuestring) {
        return false;
    }
    if (strlen(it->valuestring) >= dst_size) {
        return false;
    }
    strcpy(dst, it->valuestring);
    return true;
}

bool tb_link_config_from_json(const cJSON *body, const tb_link_config_t *current,
                              tb_link_config_t *out, const char **bad_field)
{
    const char *dummy;
    if (!bad_field) {
        bad_field = &dummy;
    }
    if (!cJSON_IsObject(body)) {
        *bad_field = "body";
        return false;
    }
    tb_link_config_t m = *current;

    if (!take_str(body, "host", m.host, sizeof(m.host))) {
        *bad_field = "host";
        return false;
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(body, "port");
    if (port) {
        if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535 ||
            port->valuedouble != floor(port->valuedouble)) {
            *bad_field = "port";
            return false;
        }
        m.port = (uint16_t)port->valuedouble;
    }
    const cJSON *tls = cJSON_GetObjectItemCaseSensitive(body, "tls");
    if (tls) {
        if (!cJSON_IsBool(tls)) {
            *bad_field = "tls";
            return false;
        }
        m.tls = cJSON_IsTrue(tls);
    }
    if (!take_str(body, "device_name", m.device_name, sizeof(m.device_name))) {
        *bad_field = "device_name";
        return false;
    }
    if (!take_str(body, "token", m.token, sizeof(m.token))) {
        *bad_field = "token";
        return false;
    }
    if (!take_str(body, "provision_key", m.provision_key, sizeof(m.provision_key))) {
        *bad_field = "provision_key";
        return false;
    }
    if (!take_str(body, "provision_secret", m.provision_secret, sizeof(m.provision_secret))) {
        *bad_field = "provision_secret";
        return false;
    }
    if (!tb_link_config_valid(&m, bad_field)) {
        return false;
    }
    *out = m;
    return true;
}

cJSON *tb_link_config_to_json(const tb_link_config_t *cfg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "host", cfg->host);
    cJSON_AddNumberToObject(o, "port", cfg->port);
    cJSON_AddBoolToObject(o, "tls", cfg->tls);
    cJSON_AddStringToObject(o, "device_name", cfg->device_name);
    cJSON_AddStringToObject(o, "provision_key", cfg->provision_key);
    cJSON_AddBoolToObject(o, "has_token", cfg->token[0] != 0);
    cJSON_AddBoolToObject(o, "has_provision_secret", cfg->provision_secret[0] != 0);
    return o;
}

int tb_provision_build_request(const tb_link_config_t *cfg, char *buf, size_t size)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return -1;
    }
    cJSON_AddStringToObject(o, "deviceName", cfg->device_name);
    cJSON_AddStringToObject(o, "provisionDeviceKey", cfg->provision_key);
    cJSON_AddStringToObject(o, "provisionDeviceSecret", cfg->provision_secret);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) {
        return -1;
    }
    size_t n = strlen(txt);
    int ret = -1;
    if (buf && n < size) {
        memcpy(buf, txt, n + 1);
        ret = (int)n;
    }
    cJSON_free(txt);
    return ret;
}

tb_prov_result_t tb_provision_parse(const char *json, size_t len, char *token, size_t token_size,
                                    char *err, size_t err_size)
{
    if (err && err_size) {
        err[0] = 0;
    }
    cJSON *r = cJSON_ParseWithLength(json, len);
    if (!r) {
        return TB_PROV_BAD_REPLY;
    }
    tb_prov_result_t res = TB_PROV_BAD_REPLY;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
    if (cJSON_IsString(status) && status->valuestring) {
        if (strcmp(status->valuestring, "SUCCESS") == 0) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(r, "credentialsType");
            const cJSON *val = cJSON_GetObjectItemCaseSensitive(r, "credentialsValue");
            if (cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "ACCESS_TOKEN") == 0 &&
                cJSON_IsString(val) && val->valuestring && val->valuestring[0] && token && token_size &&
                strlen(val->valuestring) < token_size) {
                strcpy(token, val->valuestring);
                res = TB_PROV_OK;
            }
        } else {
            /* FAILURE, NOT_FOUND (wrong key/secret) or anything else: show ThingsBoard's text,
             * or the status itself when there is no errorMsg. */
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(r, "errorMsg");
            if (err && err_size) {
                const char *text = (cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : NULL;
                if (!text && strcmp(status->valuestring, "FAILURE") != 0) {
                    text = status->valuestring;
                }
                if (text) {
                    snprintf(err, err_size, "%s", text);
                }
            }
            res = TB_PROV_FAILURE;
        }
    }
    cJSON_Delete(r);
    return res;
}

uint32_t tb_backoff_next_ms(uint32_t current_ms)
{
    if (current_ms == 0) {
        return TB_BACKOFF_MIN_MS;
    }
    if (current_ms >= TB_BACKOFF_MAX_MS / 2) {
        return TB_BACKOFF_MAX_MS;
    }
    return current_ms * 2;
}

uint32_t tb_backoff_after_fail_ms(uint32_t current_ms, bool at_cap, bool staging)
{
    if (staging) {
        return TB_BACKOFF_MIN_MS;
    }
    if (at_cap) {
        return TB_BACKOFF_MAX_MS;
    }
    return tb_backoff_next_ms(current_ms);
}

bool tb_after_reply_ready(bool acked, uint32_t elapsed_ms)
{
    return acked || elapsed_ms >= TB_AFTER_REPLY_MAX_MS;
}

/* ---- ThingsBoard server-side RPC ---- */

bool tb_rpc_parse_topic(const char *topic, size_t topic_len, uint32_t *req_id)
{
    const size_t pl = sizeof(TB_RPC_REQ_PREFIX) - 1;
    if (topic == NULL || req_id == NULL || topic_len <= pl || topic_len > pl + 10 ||
        memcmp(topic, TB_RPC_REQ_PREFIX, pl) != 0) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = pl; i < topic_len; i++) {
        if (topic[i] < '0' || topic[i] > '9') {
            return false;
        }
        v = v * 10 + (uint64_t)(topic[i] - '0');
    }
    if (v > 0xffffffffull) {
        return false;
    }
    *req_id = (uint32_t)v;
    return true;
}

tb_rpc_parse_t tb_rpc_parse_request(const char *payload, size_t len, char *method, size_t method_size,
                                    cJSON **params)
{
    if (params != NULL) {
        *params = NULL;
    }
    if (payload == NULL || len == 0 || method == NULL || method_size == 0 || params == NULL) {
        return TB_RPC_PARSE_BAD_JSON;
    }
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return TB_RPC_PARSE_BAD_JSON;
    }
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(m) || m->valuestring == NULL || m->valuestring[0] == '\0' ||
        strlen(m->valuestring) >= method_size) {
        cJSON_Delete(root);
        return TB_RPC_PARSE_NO_METHOD;
    }
    memcpy(method, m->valuestring, strlen(m->valuestring) + 1);
    *params = cJSON_DetachItemFromObjectCaseSensitive(root, "params");
    cJSON_Delete(root);
    return TB_RPC_PARSE_OK;
}

int tb_rpc_response_topic(uint32_t req_id, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    int n = snprintf(out, out_size, TB_RPC_RESP_PREFIX "%lu", (unsigned long)req_id);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

uint32_t tb_rpc_hash(const char *data, size_t len)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (size_t i = 0; data != NULL && i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    return h;
}

void tb_rpc_dedup_init(tb_rpc_dedup_t *d)
{
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
    }
}

static bool dedup_live(const tb_rpc_dedup_entry_t *e, uint32_t now_ms)
{
    return e->used && (uint32_t)(now_ms - e->at_ms) <= TB_RPC_DEDUP_WINDOW_MS;
}

static bool dedup_match(const tb_rpc_dedup_entry_t *e, uint32_t req_id, uint32_t len, uint32_t hash)
{
    return e->used && e->req_id == req_id && e->len == len && e->hash == hash;
}

int tb_rpc_dedup_find(const tb_rpc_dedup_t *d, uint32_t req_id, const char *payload, size_t len, uint32_t now_ms)
{
    if (d == NULL || payload == NULL) {
        return -1;
    }
    uint32_t hash = tb_rpc_hash(payload, len);
    for (int i = 0; i < TB_RPC_DEDUP_SLOTS; i++) {
        if (dedup_match(&d->e[i], req_id, (uint32_t)len, hash) && dedup_live(&d->e[i], now_ms)) {
            return i;
        }
    }
    return -1;
}

int tb_rpc_dedup_insert(tb_rpc_dedup_t *d, uint32_t req_id, const char *payload, size_t len, uint32_t now_ms)
{
    if (d == NULL || payload == NULL) {
        return -1;
    }
    uint32_t hash = tb_rpc_hash(payload, len);
    int slot = -1;
    for (int i = 0; i < TB_RPC_DEDUP_SLOTS && slot < 0; i++) {
        if (dedup_match(&d->e[i], req_id, (uint32_t)len, hash)) {
            slot = i; /* same key: refresh in place */
        }
    }
    for (int i = 0; i < TB_RPC_DEDUP_SLOTS && slot < 0; i++) {
        if (!dedup_live(&d->e[i], now_ms)) {
            slot = i; /* free or expired */
        }
    }
    if (slot < 0) {
        slot = 0; /* all live: evict the oldest insert */
        for (int i = 1; i < TB_RPC_DEDUP_SLOTS; i++) {
            if ((int32_t)(d->e[i].seq - d->e[slot].seq) < 0) {
                slot = i;
            }
        }
    }
    d->e[slot] = (tb_rpc_dedup_entry_t){.used = true,
                                        .req_id = req_id,
                                        .len = (uint32_t)len,
                                        .hash = hash,
                                        .at_ms = now_ms,
                                        .seq = ++d->seq};
    return slot;
}
