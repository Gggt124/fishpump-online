#include <stdio.h>
#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void heartbeat_task(void *pvParameters)
{
    uint32_t count = 0;
    while (1) {
        ESP_LOGI(TAG, "Heartbeat count: %" PRIu32, count++);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== FishPump ESP32-S3 Controller Initializing ===");

    // 1. Log Firmware Information from app descriptor
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware Project Name: %s", app_desc->project_name);
    ESP_LOGI(TAG, "Firmware Version     : %s", app_desc->version);
    ESP_LOGI(TAG, "IDF Version          : %s", app_desc->idf_ver);
    ESP_LOGI(TAG, "Compile Date & Time  : %s %s", app_desc->date, app_desc->time);

    // 2. Log Chip Information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip Model           : ESP32-S3 (cores: %d, revision: %d)", chip_info.cores, chip_info.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash Size           : %" PRIu32 " MB", flash_size / (1024 * 1024));
    }

    // 3. Start Heartbeat Background Task
    xTaskCreate(heartbeat_task, "heartbeat_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "System initialization complete. Running tasks...");
}
