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
#define ALARM_SYSTEM_ID_BASE 65000u /* user rule ids stay below this; at or above = firmware-owned system alarms */
#define ALARM_SYSTEM_ID_DRY_RUN 65001u

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

/* Saved to NVS as a raw record. Fields are append-only (never reorder or
 * resize), and 0 must mean the default, so an older, shorter record loads. */
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

/* ---- History ring (also the uplink's store-and-forward buffer) ---- */

#define ALARM_HISTORY_LEN 32
typedef struct {
    uint32_t seq;
    uint8_t type;
    uint8_t reason;
    uint16_t rule_id;
    uint8_t signal_id;
    uint8_t severity;
    float value;
    uint32_t uptime_ms;
    char name[ALARM_NAME_BYTES];
} alarm_event_t;

typedef struct {
    alarm_event_t items[ALARM_HISTORY_LEN];
    uint8_t head;
    uint8_t count;
    uint32_t last_seq;
} alarm_ring_t;

void alarm_ring_init(alarm_ring_t *ring);
/* Assigns and returns seq = ++last_seq. */
uint32_t alarm_ring_push(alarm_ring_t *ring, const alarm_event_t *evt);
/* Copies events with seq > after_seq, oldest first, up to max. *oldest_seq = lowest seq still held (0 if empty). */
size_t alarm_ring_since(const alarm_ring_t *ring, uint32_t after_seq, alarm_event_t *out, size_t max,
                        uint32_t *oldest_seq);

/* ---- Presets and storage blob ---- */

/* 12 rules, ids 1..12, all enabled=false, next_id=13. */
void alarm_presets_fill(alarm_rule_set_t *out);

#define ALARM_BLOB_VERSION 2
#define ALARM_BLOB_V1 1
#define ALARM_BLOB_HEADER_BYTES 6
/* Write size. */
#define ALARM_BLOB_MAX_BYTES (ALARM_BLOB_HEADER_BYTES + ALARM_MAX_RULES * sizeof(alarm_rule_t))
/* Read limit: leaves room for a future, larger record so it still loads. */
#define ALARM_BLOB_READ_MAX_BYTES (ALARM_BLOB_HEADER_BYTES + ALARM_MAX_RULES * (sizeof(alarm_rule_t) + 64))
/* v2 layout: [2][count][next_id u16 LE][rec_size u16 LE][count x rec_size].
 * v1 (read only): [1][count][next_id u16 LE][count x sizeof(alarm_rule_t)].
 * Encode returns 0 if it does not fit. */
size_t alarm_blob_encode(const alarm_rule_set_t *set, uint8_t *buf, size_t size);
/* false = blob unusable (wrong version, bad length, count > max). A rule that fails
 * alarm_rule_validate() is dropped and counted in *dropped; the rest are kept. */
bool alarm_blob_decode(const uint8_t *buf, size_t len, alarm_rule_set_t *out, uint8_t *dropped);

/* ---- Evaluation (pure state machine, one tick per rule) ---- */

typedef enum {
    ALARM_STATE_NORMAL = 0,
    ALARM_STATE_PENDING,
    ALARM_STATE_ACTIVE,
    ALARM_STATE_LATCHED
} alarm_state_t;

/* A PENDING rule whose signal gives no information for this long restarts its delay. */
#define ALARM_HOLD_RESET_MS 5000u

typedef struct {
    uint8_t state;
    bool acked;
    bool holding;           /* last tick was VERDICT_HOLD */
    uint32_t hold_since_ms; /* first tick of the current hold run */
    uint32_t pending_since_ms;
    uint32_t active_since_ms;
    float last_value;
} alarm_runtime_t;

typedef enum { ALARM_EVT_RAISED = 0, ALARM_EVT_CLEARED, ALARM_EVT_ACKED } alarm_event_type_t; /* == telemetry_alarm_type_t */
typedef enum {
    ALARM_REASON_NONE = 0,
    ALARM_REASON_CONDITION,
    ALARM_REASON_ACKED,
    ALARM_REASON_RULE_CHANGED
} alarm_reason_t;

typedef struct {
    uint8_t type;
    uint8_t reason;
    float value;
} alarm_transition_t;

/* One tick for one rule. Returns true and fills *out when an event is produced
 * (at most one per call). A disabled rule, or one whose signal id is unknown, is
 * treated as not violating. All time math is unsigned subtraction (wrap-safe). */
bool alarm_rule_step(const alarm_rule_t *rule, alarm_runtime_t *rt, const telemetry_snapshot_t *snap,
                     uint32_t now_ms, alarm_transition_t *out);
/* Acknowledge. Returns false when the rule is NORMAL/PENDING or already acked. */
bool alarm_rule_ack(alarm_runtime_t *rt, alarm_transition_t *out);
/* Rule edited or deleted: state -> NORMAL; emits cleared/rule_changed if it was ACTIVE or LATCHED. */
bool alarm_rule_reset(alarm_runtime_t *rt, alarm_transition_t *out);

typedef struct {
    uint8_t active, critical, unacked, critical_unacked, max_severity;
    uint8_t warning_unacked, info_unacked;
} alarm_counts_t;

typedef enum {
    ALARM_LED_NONE = 0,
    ALARM_LED_INFO,
    ALARM_LED_WARNING,
    ALARM_LED_CRITICAL
} alarm_led_level_t;
/* Highest severity that still has an unacknowledged alarm (acked alarms never light the LED). */
alarm_led_level_t alarm_counts_led_level(const alarm_counts_t *c);
void alarm_counts(const alarm_rule_set_t *set, const alarm_runtime_t rt[ALARM_MAX_RULES], alarm_counts_t *out);
/* Adds one ACTIVE/LATCHED runtime of the given severity into counts (same rules as alarm_counts). */
void alarm_counts_add(alarm_counts_t *counts, const alarm_runtime_t *rt, uint8_t severity);

#ifdef __cplusplus
}
#endif
