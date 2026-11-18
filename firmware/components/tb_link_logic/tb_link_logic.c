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
        } else if (strcmp(status->valuestring, "FAILURE") == 0) {
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(r, "errorMsg");
            if (cJSON_IsString(msg) && msg->valuestring && err && err_size) {
                snprintf(err, err_size, "%s", msg->valuestring);
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
