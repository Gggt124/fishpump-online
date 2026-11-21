#pragma once

/*
 * Shared read-health rule for polled sensors (validity contract, spec 5.1a):
 *   valid = the latest read succeeded
 *   fault = SENSOR_HEALTH_FAULT_THRESHOLD or more consecutive failures,
 *           or the sensor failed to initialise; cleared by the next good read
 * Pure C, no ESP-IDF dependency.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_HEALTH_FAULT_THRESHOLD 3

typedef struct {
    uint8_t consecutive_failures; /* saturates at 255 */
    bool valid;
    bool fault;
    bool ever_ok;
} sensor_health_t;

void sensor_health_init(sensor_health_t *h);
void sensor_health_mark_init_failed(sensor_health_t *h);
void sensor_health_record(sensor_health_t *h, bool read_ok);

#ifdef __cplusplus
}
#endif
