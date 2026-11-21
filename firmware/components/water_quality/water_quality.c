#include "water_quality.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hardware_map.h"
#include "ph_cal.h"
#include "sen0708.h"
#include "sen0708_proto.h"
#include "sensor_health.h"
#include "telemetry_hub.h"

static const char *TAG = "water_quality";

#define WATER_POLL_INTERVAL_MS 2000
#define WATER_QUEUE_LEN 4
#define WATER_TASK_STACK 4096
#define WATER_TASK_PRIORITY 3 /* below cooling_control (4), same as power_mon */
#define WATER_TASK_CORE 1

typedef enum {
  WQ_CMD_CAL_POINT,
  WQ_CMD_CAL_CANCEL,
  WQ_CMD_PH_OFFSET,
  WQ_CMD_TEMP_OFFSET,
} wq_cmd_type_t;

typedef struct {
  wq_cmd_type_t type;
  uint8_t point;
  float value;
} wq_cmd_t;

/* Owned by the sensor task only. */
static sen0708_handle_t s_sensor;
static sensor_health_t s_health;
static ph_stability_t s_stability;
static ph_cal_t s_cal;

/* Shared with readers; guarded by s_lock. */
static water_quality_status_t s_status;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static bool s_initialized;

static uint32_t now_ms(void) {
  uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
  return ms == 0 ? 1 : ms;
}

static const char *cal_error_thai(ph_cal_error_t err) {
  switch (err) {
  case PH_CAL_ERR_OUT_OF_RANGE:
    return "ค่าน้ำยาบัฟเฟอร์อยู่นอกช่วงของจุดนี้";
  case PH_CAL_ERR_NOT_STABLE:
    return "ค่ายังไม่นิ่ง";
  case PH_CAL_ERR_WRONG_STEP:
    return "ต้องทำจุดที่ 1 ก่อน";
  case PH_CAL_ERR_SENSOR_FAULT:
    return "เซนเซอร์ไม่ตอบ";
  case PH_CAL_ERR_SENSOR_WRITE:
    return "เซนเซอร์ไม่ยืนยันการเขียน";
  case PH_CAL_ERR_TIMEOUT:
    return "หมดเวลารอจุดที่ 2";
  default:
    return "";
  }
}

static void set_notice(const char *text, bool ok) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  snprintf(s_status.notice, sizeof(s_status.notice), "%s", text);
  s_status.notice_ok = ok;
  xSemaphoreGive(s_lock);
}

/* Copy task-owned state into the shared status and publish one event. */
static void publish(void) {
  telemetry_event_t evt = {.type = TELEMETRY_EVT_WATER};
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.valid = s_health.valid;
  s_status.fault = s_health.fault;
  s_status.stable = s_health.valid && ph_stability_is_stable(&s_stability);
  s_status.cal_state = ph_cal_state_str(s_cal.state);
  s_status.cal_error = ph_cal_error_str(s_cal.last_error);
  evt.data.water = (telemetry_water_payload_t){
      .valid = s_status.valid,
      .fault = s_status.fault,
      .stable = s_status.stable,
      .ph = s_status.ph,
      .temperature_c = s_status.temperature_c,
  };
  xSemaphoreGive(s_lock);
  telemetry_hub_publish(&evt);
}

static void refresh_offsets(void) {
  sen0708_offsets_t off;
  if (sen0708_get_offsets(s_sensor, &off) != ESP_OK) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_status.ph_offset = off.ph_offset;
  s_status.temp_offset = off.temp_offset;
  s_status.offsets_known = true;
  xSemaphoreGive(s_lock);
}

static void poll_sensor(void) {
  bool was_fault = s_health.fault;
  sen0708_reading_t r;
  bool ok = false;

  if (s_sensor != NULL) {
    esp_err_t err = sen0708_read(s_sensor, &r);
    ok = (err == ESP_OK);
    if (!ok) {
      ESP_LOGD(TAG, "read failed: %s", esp_err_to_name(err));
    }
    sensor_health_record(&s_health, ok);
  }

  if (ok) {
    ph_stability_push(&s_stability, r.ph);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.ph = r.ph;
    s_status.temperature_c = r.temperature_c;
    s_status.last_ok_ms = now_ms();
    bool need_offsets = !s_status.offsets_known;
    xSemaphoreGive(s_lock);
    if (need_offsets) {
      refresh_offsets();
    }
  } else if (s_health.fault) {
    ph_stability_reset(&s_stability);
  }

  if (s_health.fault != was_fault) {
    ESP_LOGW(TAG, "SEN0708 %s", s_health.fault ? "FAULT (no reply)" : "recovered");
  }
}

static void handle_cal_point(uint8_t point, float buffer_ph) {
  char msg[192];
  ph_cal_error_t pre = ph_cal_begin_point(&s_cal, point, buffer_ph,
                                          ph_stability_is_stable(&s_stability),
                                          s_sensor != NULL && s_health.valid);
  if (pre != PH_CAL_ERR_NONE) {
    snprintf(msg, sizeof(msg), "ไม่ได้เขียนจุดที่ %u: %s", point, cal_error_thai(pre));
    set_notice(msg, false);
    return;
  }

  esp_err_t err = sen0708_calibrate(s_sensor, point, buffer_ph);
  ph_cal_finish_point(&s_cal, point, buffer_ph, err == ESP_OK, now_ms());
  if (err == ESP_OK) {
    /* The reading jumps after calibration; require it to settle again. */
    ph_stability_reset(&s_stability);
    snprintf(msg, sizeof(msg), "บันทึกจุดที่ %u = pH %.2f แล้ว", point, buffer_ph);
    set_notice(msg, true);
  } else {
    ESP_LOGW(TAG, "calibration point %u failed: %s", point, esp_err_to_name(err));
    snprintf(msg, sizeof(msg), "คาลิเบรตจุดที่ %u ล้มเหลว", point);
    set_notice(msg, false);
  }
}

static void handle_offset(bool is_ph, float value) {
  char msg[192];
  if (s_sensor == NULL || s_health.fault) {
    set_notice("เซนเซอร์ไม่ตอบ ตั้ง offset ไม่ได้", false);
    return;
  }
  esp_err_t err = is_ph ? sen0708_set_ph_offset(s_sensor, value)
                        : sen0708_set_temp_offset(s_sensor, value);
  if (err == ESP_OK) {
    if (is_ph) {
      snprintf(msg, sizeof(msg), "ตั้ง pH offset = %+.2f แล้ว", value);
    } else {
      snprintf(msg, sizeof(msg), "ตั้ง offset อุณหภูมิ = %+.1f °C แล้ว", value);
    }
    ph_stability_reset(&s_stability);
    refresh_offsets();
  } else {
    ESP_LOGW(TAG, "%s offset write failed: %s", is_ph ? "pH" : "temp",
             esp_err_to_name(err));
    snprintf(msg, sizeof(msg), "ตั้ง offset %s ล้มเหลว", is_ph ? "pH" : "อุณหภูมิ");
  }
  set_notice(msg, err == ESP_OK);
}

static void handle_cmd(const wq_cmd_t *cmd) {
  switch (cmd->type) {
  case WQ_CMD_CAL_POINT:
    handle_cal_point(cmd->point, cmd->value);
    break;
  case WQ_CMD_CAL_CANCEL:
    ph_cal_cancel(&s_cal);
    set_notice("ยกเลิกการคาลิเบรตแล้ว", true);
    break;
  case WQ_CMD_PH_OFFSET:
    handle_offset(true, cmd->value);
    break;
  case WQ_CMD_TEMP_OFFSET:
    handle_offset(false, cmd->value);
    break;
  }
  publish();
}

/* Only task that talks to the sensor, so web commands never collide with a poll. */
static void water_task(void *arg) {
  (void)arg;
  uint32_t next_poll = now_ms();

  for (;;) {
    if ((int32_t)(now_ms() - next_poll) >= 0) {
      poll_sensor();
      publish();
      next_poll += WATER_POLL_INTERVAL_MS;
      if ((int32_t)(now_ms() - next_poll) > 0) {
        next_poll = now_ms() + WATER_POLL_INTERVAL_MS; /* fell behind; don't burst */
      }
    }

    ph_cal_state_t before = s_cal.state;
    ph_cal_tick(&s_cal, now_ms());
    if (s_cal.state != before) {
      publish();
    }

    int32_t wait = (int32_t)(next_poll - now_ms());
    wq_cmd_t cmd;
    if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(wait > 0 ? wait : 0)) == pdTRUE) {
      handle_cmd(&cmd);
    }
  }
}

esp_err_t water_quality_init(void) {
  if (s_initialized) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  s_queue = xQueueCreate(WATER_QUEUE_LEN, sizeof(wq_cmd_t));
  if (s_lock == NULL || s_queue == NULL) {
    return ESP_ERR_NO_MEM;
  }

  sensor_health_init(&s_health);
  ph_stability_reset(&s_stability);
  ph_cal_init(&s_cal);
  s_status.cal_state = ph_cal_state_str(s_cal.state);

  const hardware_bus_pin_t *b = &hardware_map_fixed_bus_pins(NULL)[HARDWARE_BUS_RS485_PH];
  sen0708_config_t cfg = {
      .uart_port = (uart_port_t)b->uart_port,
      .tx_pin = b->tx_gpio,
      .rx_pin = b->rx_gpio,
      .baud = b->baud,
      .slave_addr = b->modbus_addr,
  };
  esp_err_t err = sen0708_new(&cfg, &s_sensor);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SEN0708 init on UART%u failed: %s", b->uart_port, esp_err_to_name(err));
    s_sensor = NULL;
    sensor_health_mark_init_failed(&s_health);
    s_status.fault = true;
  }

  if (xTaskCreatePinnedToCore(water_task, "water_q", WATER_TASK_STACK, NULL,
                              WATER_TASK_PRIORITY, NULL, WATER_TASK_CORE) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_initialized = true;
  return ESP_OK;
}

bool water_quality_get_status(water_quality_status_t *out) {
  if (!s_initialized || out == NULL) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_status;
  xSemaphoreGive(s_lock);
  return true;
}

static esp_err_t enqueue(const wq_cmd_t *cmd) {
  if (!s_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  return xQueueSend(s_queue, cmd, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t water_quality_request_calibrate(uint8_t point, float buffer_ph) {
  if ((point != 1 && point != 2) || !isfinite(buffer_ph) || buffer_ph < SEN0708_PH_MIN ||
      buffer_ph > SEN0708_PH_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  wq_cmd_t cmd = {.type = WQ_CMD_CAL_POINT, .point = point, .value = buffer_ph};
  return enqueue(&cmd);
}

esp_err_t water_quality_request_cancel(void) {
  wq_cmd_t cmd = {.type = WQ_CMD_CAL_CANCEL};
  return enqueue(&cmd);
}

esp_err_t water_quality_request_ph_offset(float offset) {
  if (!isfinite(offset) || fabsf(offset) > SEN0708_PH_OFFSET_LIMIT) {
    return ESP_ERR_INVALID_ARG;
  }
  wq_cmd_t cmd = {.type = WQ_CMD_PH_OFFSET, .value = offset};
  return enqueue(&cmd);
}

esp_err_t water_quality_request_temp_offset(float offset) {
  if (!isfinite(offset) || fabsf(offset) > SEN0708_TEMP_OFFSET_LIMIT) {
    return ESP_ERR_INVALID_ARG;
  }
  wq_cmd_t cmd = {.type = WQ_CMD_TEMP_OFFSET, .value = offset};
  return enqueue(&cmd);
}
