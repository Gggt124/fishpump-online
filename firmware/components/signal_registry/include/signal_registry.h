#pragma once

/*
 * One table naming every value in the telemetry snapshot. Shared by the alarm
 * engine, the REST API and the uplink so a signal has one name everywhere.
 * Ids are stored in NVS rule blobs: never renumber or reuse them.
 * Pure C: depends only on the telemetry_hub types and libc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetry_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SIGNAL_KIND_NUMBER = 0, SIGNAL_KIND_BOOL } signal_kind_t;

typedef enum {
    SIGNAL_SRC_POWER_PUMP = 0,
    SIGNAL_SRC_POWER_CHILLER,
    SIGNAL_SRC_WATER,
    SIGNAL_SRC_COOLING,
    SIGNAL_SRC_PUMP,
    SIGNAL_SRC_FLOAT,
    SIGNAL_SRC_FLOW,
    SIGNAL_SRC_HEALTH,
    SIGNAL_SRC_COUNT
} signal_source_t;

typedef struct {
    uint8_t id;
    const char *key;      /* snake_case: REST API, rule JSON and uplink key */
    const char *label_th;
    const char *unit;
    const char *group;
    signal_kind_t kind;
    signal_source_t source;
    uint8_t decimals;
    uint8_t period_sec;   /* how often the source publishes; 0 = on change only */
} signal_def_t;

typedef struct {
    bool present;       /* source published at least once */
    bool trusted;       /* safe to compare / send */
    bool source_fault;  /* debounced "sensor is gone" for this source */
    double value;       /* bools are 0.0 / 1.0 */
    uint32_t age_ms;    /* now - source updated_ms; now_ms itself if never published */
} signal_value_t;

const signal_def_t *signal_registry_all(size_t *count);
const signal_def_t *signal_registry_by_id(uint8_t id);       /* NULL if unknown */
const signal_def_t *signal_registry_by_key(const char *key);  /* NULL if unknown */
void signal_registry_read(const signal_def_t *def, const telemetry_snapshot_t *snap,
                          uint32_t now_ms, signal_value_t *out);

#ifdef __cplusplus
}
#endif
