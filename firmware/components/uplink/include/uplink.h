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

/* Whole preview as a malloc'd JSON string (caller frees), NULL on OOM or before init. */
char *uplink_preview_json(void);

#ifdef __cplusplus
}
#endif
