#pragma once

/*
 * Generic alarm rules. Pure C (plus cJSON): no NVS, GPIO, Wi-Fi, HTTP or
 * FreeRTOS. The same rule model, validator and JSON are used by the web API
 * now and by a ThingsBoard downlink later. See
 * docs/superpowers/specs/2026-11-15-alarm-rules-and-uplink-design.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "signal_registry.h"
#include "telemetry_hub.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX_RULES 24
#define ALARM_NAME_BYTES 97 /* 96 bytes UTF-8 + NUL */

typedef enum {
    ALARM_COND_ABOVE = 0,
    ALARM_COND_BELOW,
    ALARM_COND_OUTSIDE,
    ALARM_COND_INSIDE,
    ALARM_COND_IS_TRUE,
    ALARM_COND_IS_FALSE,
    ALARM_COND_STALE,
    ALARM_COND_COUNT
} alarm_cond_t;

typedef enum {
    ALARM_SEV_INFO = 1,
    ALARM_SEV_WARNING = 2,
    ALARM_SEV_CRITICAL = 3
} alarm_severity_t;

typedef enum {
    ALARM_GATE_NONE = 0,
    ALARM_GATE_WHEN_TRUE,
    ALARM_GATE_WHEN_FALSE
} alarm_gate_mode_t;

typedef struct {
    uint16_t id;
    bool enabled;
    uint8_t signal_id;
    uint8_t condition;
    uint8_t severity;
    bool latch;
    uint8_t gate_mode;
    uint8_t gate_signal_id;
    float threshold;    /* above/below; low bound for ranges */
    float threshold_hi; /* ranges only */
    float hysteresis;
    uint16_t delay_sec;
    uint16_t stale_sec;
    char name[ALARM_NAME_BYTES];
} alarm_rule_t;

typedef struct {
    uint16_t next_id;
    uint8_t count;
    alarm_rule_t rules[ALARM_MAX_RULES];
} alarm_rule_set_t;

/* true if valid; otherwise *bad_field names the JSON field ("name", "signal",
 * "condition", "threshold", "lo", "hi", "hysteresis", "delay_sec",
 * "stale_sec", "severity", "gate"). */
bool alarm_rule_validate(const alarm_rule_t *rule, const char **bad_field);

typedef enum {
    ALARM_JSON_OK = 0,
    ALARM_JSON_MISSING_FIELD,
    ALARM_JSON_INVALID_TYPE,
    ALARM_JSON_INVALID_VALUE
} alarm_json_err_t;

/* Parses and validates. *has_id is true when the object carried "id". */
alarm_json_err_t alarm_rule_from_json(const cJSON *obj, alarm_rule_t *out, bool *has_id,
                                      const char **bad_field);
cJSON *alarm_rule_to_json(const alarm_rule_t *rule); /* caller deletes; NULL on OOM */
const char *alarm_cond_name(uint8_t cond);           /* "above" ... "stale" */
const char *alarm_severity_name(uint8_t sev);        /* "info", "warning", "critical" */

#ifdef __cplusplus
}
#endif
