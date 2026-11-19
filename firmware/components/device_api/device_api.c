#include "device_api.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "device_api";

static dapi_method_t s_methods[DAPI_MAX_METHODS];
static size_t s_count;
static SemaphoreHandle_t s_lock;

cJSON *dapi_error(const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    if (!cJSON_AddBoolToObject(root, "ok", false) ||
        !cJSON_AddStringToObject(root, "error", code ? code : "") ||
        (message && !cJSON_AddStringToObject(root, "message", message))) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

esp_err_t device_api_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

const dapi_method_t *device_api_find(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_methods[i].name, name) == 0) {
            return &s_methods[i];
        }
    }
    return NULL;
}

size_t device_api_count(void)
{
    return s_count;
}

esp_err_t device_api_register(const dapi_method_t *methods, size_t count)
{
    if (!methods && count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count + count > DAPI_MAX_METHODS) {
        return ESP_ERR_INVALID_STATE;
    }
    /* validate everything first so a failure adds nothing */
    for (size_t i = 0; i < count; i++) {
        if (!methods[i].name || !methods[i].name[0] || !methods[i].fn) {
            return ESP_ERR_INVALID_ARG;
        }
        if (device_api_find(methods[i].name)) {
            return ESP_ERR_INVALID_STATE;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(methods[i].name, methods[j].name) == 0) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        s_methods[s_count++] = methods[i];
    }
    return ESP_OK;
}

const char *device_api_http_status(dapi_status_t st)
{
    switch (st) {
    case DAPI_OK:          return "200 OK";
    case DAPI_ACCEPTED:    return "202 Accepted";
    case DAPI_BAD_REQUEST: return "400 Bad Request";
    case DAPI_LOCAL_ONLY:  return "403 Forbidden";
    case DAPI_NOT_FOUND:   return "404 Not Found";
    case DAPI_CONFLICT:    return "409 Conflict";
    case DAPI_UNAVAILABLE: return "503 Service Unavailable";
    case DAPI_INTERNAL:
    default:               return "500 Internal Server Error";
    }
}

static dapi_status_t fail(cJSON **out, dapi_status_t st, const char *code, const char *message)
{
    *out = dapi_error(code, message);
    return st;
}

dapi_status_t device_api_call(const char *method, const cJSON *params, dapi_source_t src, cJSON **out)
{
    static const char *const SRC_NAME[] = {"http", "tb"};
    cJSON *empty = NULL;
    dapi_status_t st;

    if (!out) {
        return DAPI_INTERNAL;
    }
    *out = NULL;

    const dapi_method_t *m = device_api_find(method);
    if (!m) {
        return fail(out, DAPI_NOT_FOUND, "unknown_method", NULL);
    }
    if ((m->flags & DAPI_F_LOCAL_ONLY) && src == DAPI_SRC_TB) {
        return fail(out, DAPI_LOCAL_ONLY, "local_only", "Use the local web app");
    }
    if (src == DAPI_SRC_TB && params) {
        if (!cJSON_IsObject(params)) {
            return fail(out, DAPI_BAD_REQUEST, "invalid_json", "Expected a JSON object");
        }
        if (m->max_params_bytes) {
            char *printed = cJSON_PrintUnformatted(params);
            if (!printed) {
                return fail(out, DAPI_INTERNAL, "memory", "JSON allocation failed");
            }
            size_t len = strlen(printed);
            free(printed);
            if (len >= m->max_params_bytes) {
                return fail(out, DAPI_BAD_REQUEST, "invalid_body",
                            "Request body is missing or too large");
            }
        }
    }
    if (!params) {
        empty = cJSON_CreateObject();
        if (!empty) {
            return fail(out, DAPI_INTERNAL, "memory", "JSON allocation failed");
        }
        params = empty;
    }

    bool locked = !(m->flags & DAPI_F_NO_LOCK);
    if (locked && (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(DAPI_LOCK_TIMEOUT_MS)) != pdTRUE)) {
        cJSON_Delete(empty);
        return fail(out, DAPI_UNAVAILABLE, "busy", "Device busy, try again");
    }
    if (m->flags & DAPI_F_WRITE) {
        ESP_LOGI(TAG, "write %s src=%s", m->name, SRC_NAME[src == DAPI_SRC_TB]);
    }
    st = m->fn(params, src, out);
    if (locked) {
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(empty);

    if (!*out) {
        return fail(out, DAPI_INTERNAL, "memory", "JSON allocation failed");
    }
    return st;
}
