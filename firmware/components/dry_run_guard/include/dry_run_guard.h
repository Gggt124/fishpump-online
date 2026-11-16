#pragma once

/*
 * Glue between the pure dry_run_logic state machine and the firmware: reads pump_control and
 * flow_switch once a second, applies the machine's actions (pump guard, system alarm, NVS flag),
 * and runs the "measure" (วัดให้) flow-time test. See
 * docs/superpowers/specs/2026-11-16-pump-dry-run-protection-design.md.
 *
 * Lock rule: the guard never holds its data mutex while calling pump_control, alarm_engine,
 * flow_switch or nvs_store.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dry_run_logic.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    dry_run_config_t cfg;
    uint8_t state;              /* dry_run_state_t */
    uint8_t trips;
    uint32_t wait_remaining_sec;
    uint32_t dry_accum_sec;
    bool alarm_active;
} dry_run_guard_status_t;

typedef enum {
    DRY_RUN_MEASURE_IDLE = 0,
    DRY_RUN_MEASURE_RUNNING,
    DRY_RUN_MEASURE_DONE,
    DRY_RUN_MEASURE_NO_FLOW,
    DRY_RUN_MEASURE_STOPPED
} dry_run_measure_state_t;

typedef struct {
    uint8_t state;                /* dry_run_measure_state_t */
    uint32_t elapsed_ms;          /* running: so far; done: relay-on to flow_on */
    uint16_t suggested_prime_sec; /* done only */
} dry_run_measure_status_t;

/* After alarm_engine_init and pump_control_init: loads settings, registers + (re)raises the system
 * alarm when LOCKED at boot, starts the 1 s task. On failure returns the error and leaves nothing
 * allocated; the caller may continue booting without protection. */
esp_err_t dry_run_guard_init(void);

/* false before init. */
bool dry_run_guard_get_status(dry_run_guard_status_t *out);

/* ESP_OK | ESP_ERR_INVALID_ARG (*bad_field set) | ESP_ERR_INVALID_STATE (not initialised)
 * | ESP_FAIL (NVS write failed; old settings kept). */
esp_err_t dry_run_guard_set_config(const dry_run_config_t *cfg, const char **bad_field);

/* ESP_OK | ESP_ERR_INVALID_STATE (not initialised, water running, WAIT/LOCKED, already measuring,
 * or pump_control refused the test). */
esp_err_t dry_run_guard_measure_start(void);
/* Switches the test relay off and reports STOPPED. ESP_OK also when nothing was running. */
esp_err_t dry_run_guard_measure_stop(void);
/* false before init. */
bool dry_run_guard_get_measure(dry_run_measure_status_t *out);

#ifdef __cplusplus
}
#endif
