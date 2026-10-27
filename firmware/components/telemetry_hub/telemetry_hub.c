#include "telemetry_hub.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SUBSCRIBERS 4

static const char *TAG = "telemetry_hub";

typedef struct {
    telemetry_callback_t cb;
    void *user_ctx;
} subscriber_entry_t;

static subscriber_entry_t s_subscribers[MAX_SUBSCRIBERS];
static size_t s_subscriber_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static telemetry_snapshot_t s_latest_snapshot;

esp_err_t telemetry_hub_init(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "Failed to create telemetry hub mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_subscriber_count = 0;
    memset(s_subscribers, 0, sizeof(s_subscribers));
    memset(&s_latest_snapshot, 0, sizeof(s_latest_snapshot));
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Telemetry hub initialized (max subscribers: %d)", MAX_SUBSCRIBERS);
    return ESP_OK;
}

esp_err_t telemetry_hub_subscribe(telemetry_callback_t cb, void *user_ctx) {
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        ESP_LOGE(TAG, "Cannot subscribe: telemetry_hub not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].cb == cb && s_subscribers[i].user_ctx == user_ctx) {
            xSemaphoreGive(s_mutex);
            ESP_LOGD(TAG, "Subscriber already registered");
            return ESP_OK;
        }
    }

    if (s_subscriber_count >= MAX_SUBSCRIBERS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Subscriber table full (%d max)", MAX_SUBSCRIBERS);
        return ESP_ERR_NO_MEM;
    }

    s_subscribers[s_subscriber_count++] = (subscriber_entry_t){
        .cb = cb,
        .user_ctx = user_ctx
    };

    xSemaphoreGive(s_mutex);
    ESP_LOGD(TAG, "Subscriber registered, total: %zu", s_subscriber_count);
    return ESP_OK;
}

esp_err_t telemetry_hub_unsubscribe(telemetry_callback_t cb) {
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].cb == cb) {
            for (size_t j = i; j + 1 < s_subscriber_count; j++) {
                s_subscribers[j] = s_subscribers[j + 1];
            }
            s_subscriber_count--;
            s_subscribers[s_subscriber_count].cb = NULL;
            s_subscribers[s_subscriber_count].user_ctx = NULL;
            xSemaphoreGive(s_mutex);
            ESP_LOGD(TAG, "Subscriber unregistered, total: %zu", s_subscriber_count);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t telemetry_hub_publish(const telemetry_event_t *event) {
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        ESP_LOGE(TAG, "Cannot publish: telemetry_hub not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    subscriber_entry_t subs_copy[MAX_SUBSCRIBERS];
    size_t count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    switch (event->type) {
        case TELEMETRY_EVT_PUMP:
            s_latest_snapshot.pump = event->data.pump;
            s_latest_snapshot.has_pump = true;
            break;
        case TELEMETRY_EVT_FLOAT:
            s_latest_snapshot.float_state = event->data.float_state;
            s_latest_snapshot.has_float = true;
            break;
        case TELEMETRY_EVT_COOLING:
            s_latest_snapshot.cooling = event->data.cooling;
            s_latest_snapshot.has_cooling = true;
            break;
        case TELEMETRY_EVT_HEALTH:
            s_latest_snapshot.health = event->data.health;
            s_latest_snapshot.has_health = true;
            break;
        default:
            ESP_LOGW(TAG, "Unknown telemetry event type: %d", event->type);
            break;
    }

    count = s_subscriber_count;
    for (size_t i = 0; i < count; i++) {
        subs_copy[i] = s_subscribers[i];
    }

    xSemaphoreGive(s_mutex);

    // Dispatch outside mutex lock to prevent deadlock or callback blocking
    for (size_t i = 0; i < count; i++) {
        if (subs_copy[i].cb) {
            subs_copy[i].cb(event, subs_copy[i].user_ctx);
        }
    }

    return ESP_OK;
}

esp_err_t telemetry_hub_get_latest(telemetry_snapshot_t *snapshot) {
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex) {
        ESP_LOGE(TAG, "Cannot get snapshot: telemetry_hub not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *snapshot = s_latest_snapshot;
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}
