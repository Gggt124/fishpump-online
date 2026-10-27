#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELEMETRY_EVT_PUMP,
    TELEMETRY_EVT_FLOAT,
    TELEMETRY_EVT_COOLING,
    TELEMETRY_EVT_HEALTH
} telemetry_event_type_t;

typedef struct {
    uint8_t active_pump;
    bool is_running;
    uint32_t elapsed_sec;
    uint32_t remaining_sec;
    bool relay_1_active;
    bool relay_2_active;
} telemetry_pump_payload_t;

typedef struct {
    bool float_is_high;
} telemetry_float_payload_t;

typedef struct {
    float current_temp_c;
    bool is_sensor_valid;
    bool cooling_relay_active;
    uint32_t lockout_remaining_sec;
} telemetry_cooling_payload_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t free_psram_bytes;
    int8_t wifi_rssi;
    uint32_t uptime_sec;
    uint8_t oom_strikes;
} telemetry_health_payload_t;

typedef struct {
    telemetry_event_type_t type;
    union {
        telemetry_pump_payload_t pump;
        telemetry_float_payload_t float_state;
        telemetry_cooling_payload_t cooling;
        telemetry_health_payload_t health;
    } data;
} telemetry_event_t;

typedef struct {
    telemetry_pump_payload_t pump;
    telemetry_float_payload_t float_state;
    telemetry_cooling_payload_t cooling;
    telemetry_health_payload_t health;
    bool has_pump;
    bool has_float;
    bool has_cooling;
    bool has_health;
} telemetry_snapshot_t;

/** @note Subscriber callbacks MUST be non-blocking. */
typedef void (*telemetry_callback_t)(const telemetry_event_t *event, void *user_ctx);

/**
 * @brief Initialize the telemetry hub and its internal mutex lock.
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails.
 */
esp_err_t telemetry_hub_init(void);

/**
 * @brief Register a callback to receive published telemetry events.
 *
 * @param cb Callback function to invoke on new events.
 * @param user_ctx Pointer to user context passed to callback.
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if cb is NULL,
 *                   ESP_ERR_INVALID_STATE if not initialized,
 *                   ESP_ERR_NO_MEM if subscriber table is full.
 */
esp_err_t telemetry_hub_subscribe(telemetry_callback_t cb, void *user_ctx);

/**
 * @brief Unregister a previously registered callback.
 *
 * @param cb Callback function to unregister.
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_FOUND if not found.
 */
esp_err_t telemetry_hub_unsubscribe(telemetry_callback_t cb);

/**
 * @brief Publish a telemetry event to all registered subscribers and update cache.
 *
 * @param event Event payload to publish and cache.
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if event is NULL,
 *                   ESP_ERR_INVALID_STATE if hub not initialized.
 */
esp_err_t telemetry_hub_publish(const telemetry_event_t *event);

/**
 * @brief Retrieve a snapshot copy of the latest telemetry state.
 *
 * @param snapshot Pointer to snapshot struct to populate.
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if snapshot is NULL,
 *                   ESP_ERR_INVALID_STATE if hub not initialized.
 */
esp_err_t telemetry_hub_get_latest(telemetry_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
