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
#define DAPI_F_NO_LOCK   0x10 /* runs without the call lock (a method that waits on hardware) */
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
/* *out is set to the reply object, or NULL when even the error reply could not be allocated (out of memory).
 * The caller checks for NULL and deletes it otherwise. */
dapi_status_t device_api_call(const char *method, const cJSON *params, dapi_source_t src, cJSON **out);
const char *device_api_http_status(dapi_status_t st);
/* Called after every DAPI_F_WRITE method whose function ran (any status), once the call lock is released. Not called
 * for calls refused before the function: unknown_method, local_only, TB invalid_json / invalid_body, busy.
 * It must not block. NULL means none; set once at boot (atomic pointer store). */
typedef void (*dapi_write_hook_t)(const char *method, dapi_status_t st);
void device_api_set_write_hook(dapi_write_hook_t hook);
/* {"ok":false,"error":code[,"message":message]}; message NULL -> no key. */
cJSON *dapi_error(const char *code, const char *message);

#ifdef __cplusplus
}
#endif
