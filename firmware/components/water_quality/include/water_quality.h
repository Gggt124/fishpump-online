#pragma once

/*
 * SEN0708 pH / water temperature owner. Owns UART2 from
 * hardware_map_fixed_bus_pins(), polls every 2 s in its own task, publishes
 * TELEMETRY_EVT_WATER after every poll and every command, and runs the
 * two-point calibration and offset commands queued from the web UI.
 * Calibration lives inside the sensor; nothing is stored in NVS.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool valid;  /* this poll's read succeeded */
  bool fault;  /* 3+ consecutive failures or init failed */
  bool stable; /* last 5 readings within 0.05 pH */
  float ph, temperature_c;
  float ph_offset, temp_offset;
  bool offsets_known;
  const char *cal_state; /* "idle" | "waiting_point2" | "done" */
  const char *cal_error; /* NULL or ph_cal_error_str() code */
  char notice[192];      /* result of the last command (UTF-8 Thai), "" if none */
  bool notice_ok;
  uint32_t last_ok_ms; /* uptime of last good read; 0 = never */
} water_quality_status_t;

/** Install the UART and start the task. Never talks to the sensor. */
esp_err_t water_quality_init(void);

/** Copy the current status. Returns false before init. */
bool water_quality_get_status(water_quality_status_t *out);

/*
 * Queue a command for the sensor task. All return ESP_OK when queued,
 * ESP_ERR_INVALID_ARG for an obviously bad value, ESP_ERR_INVALID_STATE
 * before init and ESP_ERR_NO_MEM when the queue (length 4) is full.
 */
esp_err_t water_quality_request_calibrate(uint8_t point, float buffer_ph); /* point 1|2, pH 0-14 */
esp_err_t water_quality_request_cancel(void);
esp_err_t water_quality_request_ph_offset(float offset);   /* +-2.00 */
esp_err_t water_quality_request_temp_offset(float offset); /* +-10.0 */

#ifdef __cplusplus
}
#endif
