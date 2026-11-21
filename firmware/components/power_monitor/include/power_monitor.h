#pragma once

/*
 * Two-channel PZEM-004T power monitor (channel 0 = pump 1, 1 = pump 2).
 * Owns both UARTs from hardware_map_fixed_bus_pins(), polls every 2 s in its
 * own task and publishes TELEMETRY_EVT_POWER per channel per poll.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POWER_MONITOR_CHANNELS 2

typedef struct {
  bool valid; /* this poll's read succeeded */
  bool fault; /* 3+ consecutive failures or init failed */
  float voltage_v, current_a, power_w, energy_wh, frequency_hz, pf;
  uint16_t alarms;     /* PZEM's own alarm register */
  uint32_t last_ok_ms; /* uptime of last good read; 0 = never */
} power_monitor_channel_t;

typedef struct {
  power_monitor_channel_t ch[POWER_MONITOR_CHANNELS];
  char notice[128]; /* last energy-reset result (UTF-8 Thai), "" if none */
  bool notice_ok;
} power_monitor_status_t;

/** Install UARTs and start the task. Never talks to the sensors. */
esp_err_t power_monitor_init(void);

/** Copy the current status. Returns false before init. */
bool power_monitor_get_status(power_monitor_status_t *out);

/** Queue an energy reset. ESP_ERR_INVALID_ARG for channel >= 2,
 *  ESP_ERR_INVALID_STATE before init, ESP_ERR_NO_MEM when the queue is full. */
esp_err_t power_monitor_request_reset_energy(uint8_t channel);

#ifdef __cplusplus
}
#endif
