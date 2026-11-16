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
    uint32_t last_critical_seq; /* seq of the newest raised critical event this boot; 0 if none */
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
/* ESP_OK | ESP_ERR_NOT_FOUND | ESP_ERR_INVALID_STATE | ESP_ERR_NO_MEM. A registered system id is found. */
esp_err_t alarm_engine_ack(uint16_t rule_id);
/* ESP_OK (*acked = number acknowledged) | ESP_ERR_INVALID_STATE | ESP_ERR_NO_MEM.
 * *acked is 0 on every error; acked may be NULL. */
esp_err_t alarm_engine_ack_all(size_t *acked);
size_t alarm_engine_get_active(alarm_active_t *out, size_t max); /* ACTIVE and LATCHED rules and system alarms: size out for ALARM_MAX_RULES + ALARM_ENGINE_MAX_SYSTEM */
size_t alarm_engine_get_events_since(uint32_t after_seq, alarm_event_t *out, size_t max,
                                     uint32_t *oldest_seq);
uint32_t alarm_engine_get_rules_revision(void); /* +1 on every successful upsert or delete; 0 before init */

/* ---- System alarms: owned by a firmware module, not by the rule set ----
 * They count, list, record history, publish SSE and uplink exactly like rule alarms, and are never
 * saved or edited. Ack of an active system alarm does not change its state: it calls on_ack (after the
 * engine lock is released, so on_ack may call alarm_engine_system_clear) and the owner decides. */
typedef void (*alarm_engine_system_ack_cb_t)(uint16_t id);
#define ALARM_ENGINE_MAX_SYSTEM 2
/* ESP_OK | ESP_ERR_INVALID_ARG (id < ALARM_SYSTEM_ID_BASE, or NULL name) | ESP_ERR_NO_MEM (table full)
 * | ESP_ERR_INVALID_STATE (engine not initialised, or id already registered). on_ack may be NULL. */
esp_err_t alarm_engine_system_register(uint16_t id, const char *name, uint8_t severity, uint8_t signal_id,
                                       alarm_engine_system_ack_cb_t on_ack);
/* Not active: ACTIVE + acked=false + RAISED event. Already active: acked=false, value updated,
 * new RAISED event (re-alert). ESP_ERR_NOT_FOUND if not registered; ESP_ERR_INVALID_STATE before init;
 * ESP_ERR_NO_MEM if the event buffer cannot be allocated. */
esp_err_t alarm_engine_system_raise(uint16_t id, float value);
/* Active: NORMAL + CLEARED event with `reason` (ALARM_REASON_*). Not active: ESP_OK, no event.
 * Same error codes as raise. */
esp_err_t alarm_engine_system_clear(uint16_t id, uint8_t reason, float value);

#ifdef __cplusplus
}
#endif
