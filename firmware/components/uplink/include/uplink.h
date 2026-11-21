#pragma once

/*
 * ThingsBoard-ready uplink layer, transport-agnostic (spec 5.8). Formats the
 * periodic telemetry, alarm events, identity and rule attributes and hands them
 * to a transport "sink" when one is plugged in. With no sink nothing is sent:
 * alarm events wait in the engine's ring and telemetry is formatted for the
 * preview only. The next project adds an MQTT sink and an SNTP wall clock.
 * The task polls the snapshot and the engine; it never subscribes to the hub.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UPLINK_MSG_TELEMETRY = 0, UPLINK_MSG_ATTRIBUTES } uplink_msg_kind_t;

typedef struct {
    bool (*is_connected)(void *ctx);
    esp_err_t (*send)(void *ctx, uplink_msg_kind_t kind, const char *json, size_t len); /* ESP_OK = accepted */
    void *ctx;
} uplink_sink_t;

#define UPLINK_INTERVAL_SEC 30
#define UPLINK_MIN_GAP_MS 2000
#define UPLINK_TOPIC_TELEMETRY "v1/devices/me/telemetry" /* for the MQTT project; unused here */
#define UPLINK_TOPIC_ATTRIBUTES "v1/devices/me/attributes"

esp_err_t uplink_init(void);
/* NULL = none; copied. Waits for any send in flight; once it returns, the previous sink's ctx is never
 * called again (so the caller may free it). Must not be called from a sink callback or any context holding
 * a lock the sink's send/is_connected may take (e.g. the MQTT event handler). Transports report link state
 * via is_connected and call set_sink only at setup/teardown. The sink's send should not block
 * (e.g. esp_mqtt_client_enqueue). */
esp_err_t uplink_set_sink(const uplink_sink_t *sink);
void uplink_set_wall_clock(int64_t (*now_unix_ms)(void));      /* NULL = none */

typedef struct {
    /* One member per client attribute (e.g. "cfg_pump"); NULL = nothing this round. Called on the uplink task
     * with no uplink lock held; may take the device_api lock. The caller deletes the object. */
    cJSON *(*build_attributes)(void);
    /* Flat status telemetry; *watched_hash covers the watched keys. NULL = skip. Same calling rules. */
    cJSON *(*build_status)(uint32_t *watched_hash);
} uplink_state_source_t;

/* NULL = none; copied. ESP_ERR_INVALID_STATE before uplink_init. */
esp_err_t uplink_set_state_source(const uplink_state_source_t *src);
/* Ask for the client attributes to be rebuilt. An atomic store: never blocks, safe from any task, even
 * before uplink_init. */
void uplink_note_settings_changed(void);

/* Whole preview as a malloc'd JSON string (caller frees), NULL on OOM or before init. */
char *uplink_preview_json(void);

/* Minimum free stack of the uplink task in bytes since it started (ESP-IDF counts stack in bytes),
 * or -1 when the task was not created. */
int32_t uplink_stack_hwm(void);

#ifdef __cplusplus
}
#endif
