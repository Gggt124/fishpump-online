#include "sensor_health.h"

#include <stddef.h>

void sensor_health_init(sensor_health_t *h)
{
    if (h == NULL) {
        return;
    }
    h->consecutive_failures = 0;
    h->valid = false;
    h->fault = false;
    h->ever_ok = false;
}

void sensor_health_mark_init_failed(sensor_health_t *h)
{
    if (h == NULL) {
        return;
    }
    h->valid = false;
    h->fault = true;
}

void sensor_health_record(sensor_health_t *h, bool read_ok)
{
    if (h == NULL) {
        return;
    }
    if (read_ok) {
        h->consecutive_failures = 0;
        h->valid = true;
        h->fault = false;
        h->ever_ok = true;
        return;
    }
    if (h->consecutive_failures < UINT8_MAX) {
        h->consecutive_failures++;
    }
    h->valid = false;
    if (h->consecutive_failures >= SENSOR_HEALTH_FAULT_THRESHOLD) {
        h->fault = true;
    }
}
