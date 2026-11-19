#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAPI_OK = 0,
    DAPI_ACCEPTED,
    DAPI_BAD_REQUEST,
    DAPI_LOCAL_ONLY,
    DAPI_NOT_FOUND,
    DAPI_CONFLICT,
    DAPI_UNAVAILABLE,
    DAPI_INTERNAL
} dapi_status_t;

typedef enum { DAPI_SRC_HTTP = 0, DAPI_SRC_TB } dapi_source_t;

#define DAPI_F_READ       0x01
#define DAPI_F_WRITE      0x02
#define DAPI_F_LOCAL_ONLY 0x04 /* refused when src == DAPI_SRC_TB */
#define DAPI_F_STAGED     0x08 /* reserved for M4 */
#define DAPI_MAX_METHODS  64
#define DAPI_LOCK_TIMEOUT_MS 5000

typedef dapi_status_t (*dapi_fn_t)(const cJSON *params, dapi_source_t src, cJSON **out);

typedef struct {
    const char *name;
    uint8_t flags;
    uint16_t max_params_bytes;
    dapi_fn_t fn;
} dapi_method_t;

/* Creates the mutex; a second call returns ESP_OK. */
esp_err_t device_api_init(void);
/* Duplicate name or table full: ESP_ERR_INVALID_STATE, nothing added. */
esp_err_t device_api_register(const dapi_method_t *methods, size_t count);
const dapi_method_t *device_api_find(const char *name);
/* Number of registered methods. */
size_t device_api_count(void);
/* *out is always set; the caller deletes it. */
dapi_status_t device_api_call(const char *method, const cJSON *params, dapi_source_t src, cJSON **out);
const char *device_api_http_status(dapi_status_t st);
/* {"ok":false,"error":code[,"message":message]}; message NULL -> no key. */
cJSON *dapi_error(const char *code, const char *message);

#ifdef __cplusplus
}
#endif
