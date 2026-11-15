#include "power_monitor.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

/* PZEM pump 1 owns UART0 (GPIO4/42). A console left on UART0 by a stale
 * sdkconfig would be silently rerouted into the meter, so refuse to build. */
#if CONFIG_ESP_CONSOLE_UART && (CONFIG_ESP_CONSOLE_UART_NUM == 0)
#error "Console is on UART0, which PZEM pump 1 uses: delete firmware/sdkconfig and rebuild (console must be USB-Serial-JTAG)"
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware_map.h"
#include "pzem004t.h"
#include "sensor_health.h"
#include "telemetry_hub.h"

static const char *TAG = "power_monitor";

#define POWER_POLL_INTERVAL_MS 2000
#define POWER_QUEUE_LEN 2
#define POWER_TASK_STACK 4096
#define POWER_TASK_PRIORITY 3 /* below cooling_control (4) */
#define POWER_TASK_CORE 1

_Static_assert(POWER_MONITOR_CHANNELS == TELEMETRY_POWER_CHANNELS,
               "power channel count mismatch");

static const uint8_t s_bus_index[POWER_MONITOR_CHANNELS] = {
    HARDWARE_BUS_PZEM_PUMP, HARDWARE_BUS_PZEM_CHILLER};

static pzem004t_handle_t s_handles[POWER_MONITOR_CHANNELS];
static sensor_health_t s_health[POWER_MONITOR_CHANNELS];
static power_monitor_status_t s_status;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static bool s_initialized;

static uint32_t now_ms(void) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  return ms == 0 ? 1 : ms;
}

static void publish_channel(uint8_t ch) {
  telemetry_event_t evt = {.type = TELEMETRY_EVT_POWER};
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const power_monitor_channel_t *c = &s_status.ch[ch];
  evt.data.power = (telemetry_power_payload_t){
      .channel = ch,
      .valid = c->valid,
      .fault = c->fault,
      .voltage_v = c->voltage_v,
      .current_a = c->current_a,
      .power_w = c->power_w,
      .energy_wh = c->energy_wh,
      .frequency_hz = c->frequency_hz,
      .pf = c->pf,
  };
  xSemaphoreGive(s_lock);
  telemetry_hub_publish(&evt);
}

static void poll_channel(uint8_t ch) {
  bool was_fault = s_health[ch].fault;
  pzem004t_reading_t r = {0};
  bool ok = false;

  if (s_handles[ch] != NULL) {
    esp_err_t err = pzem004t_read(s_handles[ch], &r);
    ok = (err == ESP_OK);
    if (!ok) {
      ESP_LOGD(TAG, "pump %u read failed: %s", ch + 1, esp_err_to_name(err));
    }
    sensor_health_record(&s_health[ch], ok);
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  power_monitor_channel_t *c = &s_status.ch[ch];
  if (ok) {
    c->voltage_v = r.voltage;
    c->current_a = r.current;
    c->power_w = r.power;
    c->energy_wh = r.energy;
    c->frequency_hz = r.frequency;
    c->pf = r.pf;
    c->alarms = r.alarms;
    c->last_ok_ms = now_ms();
  }
  c->valid = s_health[ch].valid;
  c->fault = s_health[ch].fault;
  xSemaphoreGive(s_lock);

  if (s_health[ch].fault != was_fault) {
    ESP_LOGW(TAG, "pump %u PZEM %s", ch + 1,
             s_health[ch].fault ? "FAULT (no reply)" : "recovered");
  }
  publish_channel(ch);
}

static void set_notice(const char *text, bool ok) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(s_status.notice, sizeof(s_status.notice), "%s", text);
  s_status.notice_ok = ok;
  xSemaphoreGive(s_lock);
}

static void handle_reset(uint8_t ch) {
  char text[128];
  if (s_handles[ch] == NULL || s_health[ch].fault) {
    snprintf(text, sizeof(text), "ปั๊ม %u: เซนเซอร์ไม่ตอบ รีเซ็ต kWh ไม่ได้",
             ch + 1);
    set_notice(text, false);
    return;
  }
  esp_err_t err = pzem004t_reset_energy(s_handles[ch]);
  if (err == ESP_OK) {
    snprintf(text, sizeof(text), "ปั๊ม %u: รีเซ็ต kWh แล้ว", ch + 1);
    set_notice(text, true);
  } else {
    ESP_LOGW(TAG, "pump %u energy reset failed: %s", ch + 1,
             esp_err_to_name(err));
    snprintf(text, sizeof(text), "ปั๊ม %u: รีเซ็ต kWh ล้มเหลว", ch + 1);
    set_notice(text, false);
  }
}

static void power_task(void *arg) {
  (void)arg;
  TickType_t next = xTaskGetTickCount();
  for (;;) {
    for (uint8_t ch = 0; ch < POWER_MONITOR_CHANNELS; ch++) {
      poll_channel(ch);
    }
    next += pdMS_TO_TICKS(POWER_POLL_INTERVAL_MS);

    /* Serve energy-reset commands until the next poll is due. */
    for (;;) {
      TickType_t now = xTaskGetTickCount();
      int32_t remaining = (int32_t)(next - now);
      if (remaining <= 0) {
        next = now; /* fell behind: poll now without bursting */
        break;
      }
      uint8_t ch;
      if (xQueueReceive(s_queue, &ch, (TickType_t)remaining) != pdTRUE) {
        break;
      }
      handle_reset(ch);
    }
  }
}

esp_err_t power_monitor_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  s_queue = xQueueCreate(POWER_QUEUE_LEN, sizeof(uint8_t));
  if (s_lock == NULL || s_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }

  const hardware_bus_pin_t *bus = hardware_map_fixed_bus_pins(NULL);
  for (uint8_t ch = 0; ch < POWER_MONITOR_CHANNELS; ch++) {
    const hardware_bus_pin_t *b = &bus[s_bus_index[ch]];
    pzem004t_config_t cfg = {
        .uart_port = (uart_port_t)b->uart_port,
        .tx_pin = b->tx_gpio,
        .rx_pin = b->rx_gpio,
        .slave_addr = b->modbus_addr,
    };
    sensor_health_init(&s_health[ch]);
    esp_err_t err = pzem004t_new(&cfg, &s_handles[ch]);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "pump %u: PZEM init on UART%u failed: %s", ch + 1,
               b->uart_port, esp_err_to_name(err));
      s_handles[ch] = NULL;
      sensor_health_mark_init_failed(&s_health[ch]);
      s_status.ch[ch].fault = true;
    }
  }

  if (xTaskCreatePinnedToCore(power_task, "power_mon", POWER_TASK_STACK, NULL,
                              POWER_TASK_PRIORITY, NULL,
                              POWER_TASK_CORE) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_initialized = true;
  return ESP_OK;
}

bool power_monitor_get_status(power_monitor_status_t *out) {
  if (!s_initialized || out == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_status;
  xSemaphoreGive(s_lock);
  return true;
}

esp_err_t power_monitor_request_reset_energy(uint8_t channel) {
  if (channel >= POWER_MONITOR_CHANNELS) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return xQueueSend(s_queue, &channel, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
