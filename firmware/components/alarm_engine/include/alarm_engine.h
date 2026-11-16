#pragma once

/*
 * Alarm engine: holds the rule set and per-rule runtime state, evaluates every
 * rule once a second from the telemetry snapshot (no network needed), keeps a
 * 32-event history ring and publishes TELEMETRY_EVT_ALARM on every event.
 * Alarms only report: nothing here commands a relay or the cooling output.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alarm_rules.h"
#include "esp_err.h"
#include "nvs_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    alarm_counts_t counts;
    uint32_t last_seq;
    uint32_t boot_id;
    uint32_t last_tick_ms;
    bool running;
} alarm_engine_summary_t;

typedef struct {
    uint16_t rule_id;
    uint8_t signal_id;
    uint8_t severity;
    uint8_t state; /* ALARM_STATE_ACTIVE or ALARM_STATE_LATCHED */
    bool acked;
    float value;
    uint32_t since_ms; /* uptime ms when it was raised */
    char name[ALARM_NAME_BYTES];
} alarm_active_t;

#define ALARM_ENGINE_STALL_MS 5000

esp_err_t alarm_engine_init(void);
bool alarm_engine_get_summary(alarm_engine_summary_t *out); /* false before init */
bool alarm_engine_get_rules(alarm_rule_set_t *out, nvs_store_alarm_rules_load_status_t *storage);
/* ESP_OK | ESP_ERR_INVALID_ARG (validation; *bad_field set) | ESP_ERR_NOT_FOUND (has_id, unknown id)
 * | ESP_ERR_INVALID_SIZE (rule set full: ALARM_MAX_RULES rules, or ids exhausted)
 * | ESP_ERR_NO_MEM (allocation failed) | ESP_ERR_INVALID_STATE (engine not initialised) | ESP_FAIL (NVS write failed; RAM state rolled back) */
esp_err_t alarm_engine_upsert_rule(const alarm_rule_t *rule, bool has_id, alarm_rule_t *stored,
                                   const char **bad_field);
esp_err_t alarm_engine_delete_rule(uint16_t id);         /* ESP_OK | ESP_ERR_NOT_FOUND | ESP_FAIL */
esp_err_t alarm_engine_ack(uint16_t rule_id);            /* ESP_OK | ESP_ERR_NOT_FOUND */
size_t alarm_engine_ack_all(void);                       /* number acknowledged */
size_t alarm_engine_get_active(alarm_active_t *out, size_t max); /* ACTIVE and LATCHED rules */
size_t alarm_engine_get_events_since(uint32_t after_seq, alarm_event_t *out, size_t max,
                                     uint32_t *oldest_seq);
uint32_t alarm_engine_get_rules_revision(void); /* +1 on every successful upsert or delete; 0 before init */

#ifdef __cplusplus
}
#endif
