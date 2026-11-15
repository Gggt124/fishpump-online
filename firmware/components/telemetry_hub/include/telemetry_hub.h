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
    TELEMETRY_EVT_HEALTH,
    TELEMETRY_EVT_POWER,
    TELEMETRY_EVT_WATER,
    TELEMETRY_EVT_FLOW,
    TELEMETRY_EVT_ALARM
} telemetry_event_type_t;

#define TELEMETRY_POWER_CHANNELS 2

/*
 * Sensor readings follow one validity contract (sensor integration spec 5.1a):
 *   valid = this poll's read succeeded
 *   fault = 3+ consecutive failures or init failed (debounced "do not trust")
 *   numeric fields hold the last good values (0 if never read)
 */
typedef struct {
    uint8_t channel; /* 0 = pump 1, 1 = pump 2 */
    bool valid;
    bool fault;
    float voltage_v;
    float current_a;
    float power_w;
    float energy_wh;
    float frequency_hz;
    float pf;
} telemetry_power_payload_t;

typedef struct {
    bool valid;
    bool fault;
    bool stable;
    float ph;
    float temperature_c;
} telemetry_water_payload_t;

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
    uint16_t output_mv;
    float duty_percent;
    uint32_t lockout_remaining_sec;
    uint8_t mode;
} telemetry_cooling_t;

typedef telemetry_cooling_t telemetry_cooling_payload_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t free_psram_bytes;
    int8_t wifi_rssi;
    uint32_t uptime_sec;
    uint8_t oom_strikes;
} telemetry_health_payload_t;

typedef struct {
    bool flow_on;
} telemetry_flow_payload_t;

typedef enum {
    TELEMETRY_ALARM_RAISED = 0,
    TELEMETRY_ALARM_CLEARED,
    TELEMETRY_ALARM_ACKED
} telemetry_alarm_type_t;

typedef struct {
    uint32_t seq;
    uint16_t rule_id;
    uint8_t type; /* telemetry_alarm_type_t */
    uint8_t severity;
    uint8_t signal_id;
    float value;
    uint8_t active_count;
    uint8_t critical_count;
    uint8_t unacked_count;
} telemetry_alarm_payload_t;

typedef struct {
    telemetry_event_type_t type;
    union {
        telemetry_pump_payload_t pump;
        telemetry_float_payload_t float_state;
        telemetry_cooling_payload_t cooling;
        telemetry_health_payload_t health;
        telemetry_power_payload_t power;
        telemetry_water_payload_t water;
        telemetry_flow_payload_t flow;
        telemetry_alarm_payload_t alarm;
    } data;
} telemetry_event_t;

typedef struct {
    telemetry_pump_payload_t pump;
    telemetry_float_payload_t float_state;
    telemetry_cooling_payload_t cooling;
    telemetry_health_payload_t health;
    telemetry_power_payload_t power[TELEMETRY_POWER_CHANNELS];
    telemetry_water_payload_t water;
    telemetry_flow_payload_t flow;
    telemetry_alarm_payload_t alarm;
    bool has_pump;
    bool has_float;
    bool has_cooling;
    bool has_health;
    bool has_power[TELEMETRY_POWER_CHANNELS];
    bool has_water;
    bool has_flow;
    bool has_alarm;
    /* Uptime ms of the last publish of each group; 0 = never published. */
    uint32_t pump_updated_ms;
    uint32_t float_updated_ms;
    uint32_t cooling_updated_ms;
    uint32_t health_updated_ms;
    uint32_t power_updated_ms[TELEMETRY_POWER_CHANNELS];
    uint32_t water_updated_ms;
    uint32_t flow_updated_ms;
    uint32_t alarm_updated_ms;
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
