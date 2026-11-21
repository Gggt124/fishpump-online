#pragma once

/*
 * Paddle flow switch on a fixed GPIO (hardware_map: GPIO 9 -> switch (NO) -> GND,
 * internal pull-up). Contact closed (pin low) = water flowing. The raw level is
 * sampled every 100 ms and a change is accepted after 1 s stable. Publishes
 * TELEMETRY_EVT_FLOW on every accepted change and every 5 s as a heartbeat.
 * A bare contact has no fault state; an unconnected input reads "no flow".
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLOW_SWITCH_SAMPLE_MS 100
#define FLOW_SWITCH_STABLE_MS 1000
#define FLOW_SWITCH_HEARTBEAT_MS 5000

typedef struct {
  bool flow_on;
  bool raw_closed;
  uint32_t last_change_ms;
} flow_switch_status_t;

/** Configures the GPIO and starts the task. Never blocks. */
esp_err_t flow_switch_init(void);

/** false before init. */
bool flow_switch_get_status(flow_switch_status_t *out);

#ifdef __cplusplus
}
#endif
