#include "flow_switch.h"

#include "contact_debounce.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware_map.h"
#include "telemetry_hub.h"

static const char *TAG = "flow_switch";

#define FLOW_TASK_STACK 3072
#define FLOW_TASK_PRIORITY 2 /* below the sensor tasks (3) */
#define FLOW_TASK_CORE 1

static flow_switch_status_t s_status;
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static uint32_t now_ms(void) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  return ms == 0 ? 1 : ms;
}

static bool read_closed(void) {
  return gpio_get_level(HARDWARE_FLOW_SWITCH_GPIO) == 0;
}

static void publish(bool flow_on) {
  telemetry_event_t evt = {.type = TELEMETRY_EVT_FLOW};
  evt.data.flow.flow_on = flow_on;
  telemetry_hub_publish(&evt);
}

static void flow_task(void *arg) {
  (void)arg;
  contact_debounce_t deb;
  uint32_t t = now_ms();
  bool raw = read_closed();

  /* First publish uses the initial raw level as the state. */
  contact_debounce_init(&deb, raw, t);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.flow_on = raw;
  s_status.raw_closed = raw;
  s_status.last_change_ms = t;
  xSemaphoreGive(s_lock);
  publish(raw);
  uint32_t last_pub = t;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(FLOW_SWITCH_SAMPLE_MS));
    t = now_ms();
    raw = read_closed();
    bool changed = contact_debounce_update(&deb, raw, t, FLOW_SWITCH_STABLE_MS);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.raw_closed = raw;
    if (changed) {
      s_status.flow_on = deb.state;
      s_status.last_change_ms = t;
    }
    xSemaphoreGive(s_lock);

    if (changed || (uint32_t)(t - last_pub) >= FLOW_SWITCH_HEARTBEAT_MS) {
      if (changed) {
        ESP_LOGI(TAG, "flow %s", deb.state ? "ON" : "OFF");
      }
      publish(deb.state);
      last_pub = t;
    }
  }
}

esp_err_t flow_switch_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (s_lock == NULL) {
    return ESP_ERR_NO_MEM;
  }

  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << HARDWARE_FLOW_SWITCH_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) {
    return err;
  }

  s_initialized = true; /* before the task starts so readers see a valid state */
  if (xTaskCreatePinnedToCore(flow_task, "flow_sw", FLOW_TASK_STACK, NULL,
                              FLOW_TASK_PRIORITY, NULL, FLOW_TASK_CORE) != pdPASS) {
    s_initialized = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

bool flow_switch_get_status(flow_switch_status_t *out) {
  if (!s_initialized || out == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_status;
  xSemaphoreGive(s_lock);
  return true;
}
