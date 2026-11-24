#include "pump_resume_service.h"

#include <stdbool.h>

#include "cJSON.h"
#include "device_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pump_resume.h"

static const char *TAG = "pump_resume";

#define RESUME_TASK_STACK 3072
#define RESUME_TASK_PRIORITY 2
#define RESUME_TASK_CORE 1
#define RESUME_TICK_MS 1000

static SemaphoreHandle_t s_lock;
static bool s_initialized;
static pump_resume_t s_timer;

static uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

static void resume_task(void *arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(RESUME_TICK_MS));

    bool due = false;
    if (s_lock) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      due = pump_resume_take_due(&s_timer, now_ms());
      xSemaphoreGive(s_lock);
    }
    if (!due) {
      continue;
    }

    /* The mutex is not held here: pump.start runs the dapi write hook and
     * pump_resume_service_cancel() takes s_lock. A rejected start (for example
     * dry_run_locked) is not retried. */
    cJSON *reply = NULL;
    dapi_status_t st = device_api_call("pump.start", NULL, DAPI_SRC_HTTP, &reply);
    ESP_LOGI(TAG, "timed stop over: pump.start -> %s", device_api_http_status(st));
    cJSON_Delete(reply);
  }
}

void pump_resume_service_init(void) {
  if (s_initialized) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    ESP_LOGE(TAG, "mutex create failed");
    return;
  }
  if (xTaskCreatePinnedToCore(resume_task, "pump_resume", RESUME_TASK_STACK, NULL,
                              RESUME_TASK_PRIORITY, NULL, RESUME_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "task create failed");
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    return;
  }
  s_initialized = true;
}

void pump_resume_service_arm(uint16_t minutes) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  pump_resume_arm(&s_timer, now_ms(), minutes);
  xSemaphoreGive(s_lock);
}

void pump_resume_service_cancel(void) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  pump_resume_cancel(&s_timer);
  xSemaphoreGive(s_lock);
}

uint32_t pump_resume_service_remaining_sec(void) {
  if (!s_lock) {
    return 0;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint32_t remaining = pump_resume_remaining_sec(&s_timer, now_ms());
  xSemaphoreGive(s_lock);
  return remaining;
}
