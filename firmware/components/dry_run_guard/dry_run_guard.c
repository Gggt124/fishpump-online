#include "dry_run_guard.h"

#include <string.h>

#include "alarm_engine.h"
#include "alarm_rules.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "flow_switch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_store.h"
#include "pump_control.h"

static const char *TAG = "dry_run_guard";

#define GUARD_TASK_STACK 4096 /* trip path publishes SSE/telemetry from this task */
#define GUARD_TASK_PRIORITY 2
#define GUARD_TASK_CORE 1
#define GUARD_TICK_MS 1000
#define GUARD_FLOW_SIGNAL_ID 52 /* flow_on */

typedef struct {
    dry_run_measure_state_t state;
    uint32_t t0_ms;
    uint32_t elapsed_ms;
    uint16_t suggested_prime_sec;
    bool pending_off; /* relay 1 must still be switched off (pump_control refused or was busy) */
} measure_t;

/* What one state-machine call changed, captured under s_lock so it can be applied outside it. */
typedef struct {
    dry_run_out_t out;
    uint8_t old_state;
    uint8_t new_state;
    uint8_t trips;
    uint8_t max_retries;
    uint32_t wait_sec;
} change_t;

/* s_apply serialises "mutate then act" sequences (tick, ack, settings save, measure) so actions reach
 * pump_control and alarm_engine in the order the state machine produced them. s_lock only guards the
 * data and is never held across an outside call. Order: s_apply, then s_lock. */
static SemaphoreHandle_t s_apply;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static dry_run_config_t s_cfg;
static dry_run_rt_t s_rt;
static measure_t s_meas;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- logging: one line per state change, never per tick ---- */

static const char *state_name(uint8_t s) {
    switch (s) {
    case DRY_RUN_DISABLED: return "disabled";
    case DRY_RUN_MONITOR: return "monitoring";
    case DRY_RUN_WAIT: return "waiting";
    case DRY_RUN_LOCKED: return "locked";
    default: return "?";
    }
}

static void log_state_changed(const change_t *c) {
    if (c->new_state == DRY_RUN_WAIT) {
        ESP_LOGW(TAG, "dry-run trip %u/%u, pump stopped, retry in %u s", (unsigned)c->trips,
                 (unsigned)c->max_retries, (unsigned)c->wait_sec);
    } else if (c->new_state == DRY_RUN_LOCKED) {
        ESP_LOGW(TAG, "dry-run locked after %u trips, pump stopped until acknowledged",
                 (unsigned)c->trips);
    } else {
        ESP_LOGI(TAG, "dry-run %s -> %s (trips %u)", state_name(c->old_state), state_name(c->new_state),
                 (unsigned)c->trips);
    }
}

static void log_transition_persist_failed(bool locked) {
    ESP_LOGE(TAG, "could not persist locked=%d to NVS", (int)locked);
}

static void log_transition_measure(const char *what, uint32_t elapsed_ms) {
    ESP_LOGI(TAG, "measure %s (%u ms)", what, (unsigned)elapsed_ms);
}

static void log_transition_config(bool enabled) {
    ESP_LOGI(TAG, "dry-run settings saved, protection %s", enabled ? "on" : "off");
}

/* ---- applying state-machine actions (call with NO lock held except s_apply) ---- */

/* Caller holds s_lock. */
static void capture_change(change_t *c, const dry_run_out_t *out, uint8_t old_state, uint32_t now) {
    c->out = *out;
    c->old_state = old_state;
    c->new_state = s_rt.state;
    c->trips = s_rt.trips;
    c->max_retries = s_cfg.max_retries;
    c->wait_sec = dry_run_wait_remaining_sec(&s_rt, now);
}

static void apply_change(const change_t *c) {
    uint8_t act = c->out.actions;

    /* Inhibit first: the pump is the safety-relevant part. Guard enum values equal state values. */
    if (act & (DRY_RUN_ACT_INHIBIT | DRY_RUN_ACT_RELEASE | DRY_RUN_ACT_RELEASE_RESUME)) {
        (void)pump_control_set_water_guard((pump_control_water_guard_t)c->new_state,
                                           (act & DRY_RUN_ACT_RELEASE_RESUME) != 0);
    }
    if (act & DRY_RUN_ACT_RAISE) {
        (void)alarm_engine_system_raise(ALARM_SYSTEM_ID_DRY_RUN, c->out.value);
    }
    if (act & DRY_RUN_ACT_CLEAR) {
        (void)alarm_engine_system_clear(ALARM_SYSTEM_ID_DRY_RUN, c->out.clear_reason, c->out.value);
    }
    /* Every entry to and exit from LOCKED is persisted, whatever flags the machine set. */
    bool was_locked = c->old_state == DRY_RUN_LOCKED;
    bool is_locked = c->new_state == DRY_RUN_LOCKED;
    if (was_locked != is_locked && !nvs_store_save_dry_run_locked(is_locked)) {
        log_transition_persist_failed(is_locked);
    }
    if (act & DRY_RUN_ACT_STATE_CHANGED) {
        log_state_changed(c);
    }
}

/* ---- measurement helpers ---- */

/* Try to switch relay 1 off; clear pending_off once it is really off. Caller holds s_apply. */
static void measure_relay_off(void) {
    bool ok = pump_control_water_test(false);
    if (!ok) {
        pump_control_status_t ps;
        ok = pump_control_get_status(&ps) && !ps.water.test_active;
    }
    if (ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_meas.pending_off = false;
        xSemaphoreGive(s_lock);
    }
}

/* ---- ack callback ---- */

/* Runs in the HTTP task after the engine released its lock, possibly late: it acts on the guard's
 * current state only (dry_run_ack ignores the ack when the alarm is no longer active). */
static void on_alarm_ack(uint16_t id) {
    (void)id;
    if (!s_initialized) {
        return;
    }
    xSemaphoreTake(s_apply, portMAX_DELAY);
    pump_control_status_t ps;
    bool water_running = pump_control_get_status(&ps) && ps.water.running;
    uint32_t now = now_ms();
    dry_run_out_t out;
    change_t ch;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t old_state = s_rt.state;
    dry_run_ack(&s_cfg, &s_rt, water_running, now, &out);
    capture_change(&ch, &out, old_state, now);
    xSemaphoreGive(s_lock);
    apply_change(&ch);
    xSemaphoreGive(s_apply);
}

/* ---- the task ---- */

static void guard_tick(void) {
    xSemaphoreTake(s_apply, portMAX_DELAY);

    pump_control_status_t ps;
    memset(&ps, 0, sizeof(ps));
    bool pump_ok = pump_control_get_status(&ps);
    flow_switch_status_t fs;
    memset(&fs, 0, sizeof(fs));
    bool flow_ok = flow_switch_get_status(&fs);
    uint32_t now = now_ms();

    /* A previous measurement end could not switch relay 1 off: keep trying every tick. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool retry_off = s_meas.pending_off;
    uint8_t cur_state = s_rt.state;
    xSemaphoreGive(s_lock);
    if (retry_off) {
        measure_relay_off();
    }
    /* Self-heal: if a guard call failed or raced, the pump must still follow the machine. */
    if (pump_ok && ps.initialized && ps.water.guard != cur_state) {
        (void)pump_control_set_water_guard((pump_control_water_guard_t)cur_state, false);
    }

    dry_run_out_t out;
    change_t ch;
    bool finished_off = false;
    const char *finished = NULL;
    uint32_t finished_ms = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    dry_run_input_t in = {
        .relay_on = pump_ok && ps.water.relay_energized,
        .flow_on = flow_ok && fs.flow_on,
        .test_active = s_meas.state == DRY_RUN_MEASURE_RUNNING || s_meas.pending_off ||
                       (pump_ok && ps.water.test_active),
        .water_running = pump_ok && ps.water.running,
    };
    uint8_t old_state = s_rt.state;
    dry_run_step(&s_cfg, &s_rt, &in, now, &out);
    capture_change(&ch, &out, old_state, now);

    if (s_meas.state == DRY_RUN_MEASURE_RUNNING) {
        uint32_t since_t0 = now - s_meas.t0_ms;
        if (flow_ok && fs.flow_on) {
            /* Use the debounced flow-on instant only if that change happened after t0 (signed
             * unsigned-subtraction test). Flow already on before the relay was energised would
             * otherwise yield a huge wrapped value; then the best bound is now - t0. */
            uint32_t flow_since_t0 = fs.last_change_ms - s_meas.t0_ms;
            s_meas.elapsed_ms = (flow_since_t0 < 0x80000000u) ? flow_since_t0 : since_t0;
            s_meas.suggested_prime_sec = dry_run_suggest_prime_sec(s_meas.elapsed_ms);
            s_meas.state = DRY_RUN_MEASURE_DONE;
            s_meas.pending_off = true;
            finished_off = true;
            finished = "done";
            finished_ms = s_meas.elapsed_ms;
        } else if (since_t0 >= DRY_RUN_MEASURE_TIMEOUT_MS) {
            s_meas.elapsed_ms = since_t0;
            s_meas.state = DRY_RUN_MEASURE_NO_FLOW;
            s_meas.pending_off = true;
            finished_off = true;
            finished = "no flow";
            finished_ms = since_t0;
        } else if (pump_ok && !ps.water.test_active) {
            /* pump_control ended the test itself (backstop, guard, fault): relay is already off. */
            s_meas.elapsed_ms = since_t0;
            s_meas.state = DRY_RUN_MEASURE_STOPPED;
            finished = "ended by pump control";
            finished_ms = since_t0;
        }
    }
    xSemaphoreGive(s_lock);

    apply_change(&ch);
    if (finished) {
        log_transition_measure(finished, finished_ms);
    }
    if (finished_off) {
        measure_relay_off();
    }
    xSemaphoreGive(s_apply);
}

static void guard_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(GUARD_TICK_MS));
        guard_tick();
    }
}

/* ---- public API ---- */

esp_err_t dry_run_guard_init(void) {
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_apply = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    if (!s_apply || !s_lock) {
        ESP_LOGE(TAG, "mutex create failed");
        goto fail_mutex;
    }

    dry_run_config_t cfg;
    bool stored_locked = false;
    (void)nvs_store_load_dry_run(&cfg, &stored_locked);
    if (stored_locked && !cfg.enabled) {
        /* Protection is off: a stale lock must not outlive it. Clear the stored flag once. */
        (void)nvs_store_save_dry_run_locked(false);
        stored_locked = false;
    }

    uint32_t now = now_ms();
    s_cfg = cfg;
    memset(&s_meas, 0, sizeof(s_meas));
    dry_run_begin(&s_rt, &s_cfg, stored_locked, now);
    s_initialized = true; /* before the alarm is registered: a late ack must find a valid state */

    esp_err_t err = alarm_engine_system_register(ALARM_SYSTEM_ID_DRY_RUN, "ปั๊มน้ำเดินแห้ง", ALARM_SEV_CRITICAL,
                                                 GUARD_FLOW_SIGNAL_ID, on_alarm_ack);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "alarm register failed: %s", esp_err_to_name(err));
        goto fail_init;
    }

    /* Make the pump match the machine (boot lock is also passed via water_guard_initial). */
    (void)pump_control_set_water_guard((pump_control_water_guard_t)s_rt.state, false);
    if (s_rt.state == DRY_RUN_LOCKED) {
        (void)alarm_engine_system_raise(ALARM_SYSTEM_ID_DRY_RUN, (float)s_rt.trips);
    }

    if (xTaskCreatePinnedToCore(guard_task, "dry_run", GUARD_TASK_STACK, NULL, GUARD_TASK_PRIORITY, NULL,
                                GUARD_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        err = ESP_ERR_NO_MEM;
        goto fail_init;
    }

    ESP_LOGI(TAG, "init: protection %s, state %s, prime %u s, trip %u s, wait %u min, retries %u",
             s_cfg.enabled ? "on" : "off", state_name(s_rt.state), (unsigned)s_cfg.prime_sec,
             (unsigned)s_cfg.dry_trip_sec, (unsigned)s_cfg.retry_wait_min, (unsigned)s_cfg.max_retries);
    return ESP_OK;

fail_init:
    s_initialized = false;
    vSemaphoreDelete(s_apply);
    vSemaphoreDelete(s_lock);
    s_apply = NULL;
    s_lock = NULL;
    return err;
fail_mutex:
    if (s_apply) {
        vSemaphoreDelete(s_apply);
        s_apply = NULL;
    }
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    return ESP_ERR_NO_MEM;
}

bool dry_run_guard_get_status(dry_run_guard_status_t *out) {
    if (!out || !s_initialized) {
        return false;
    }
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->cfg = s_cfg;
    out->state = s_rt.state;
    out->trips = s_rt.trips;
    out->wait_remaining_sec = dry_run_wait_remaining_sec(&s_rt, now);
    out->dry_accum_sec = s_rt.dry_accum_ms / 1000u;
    out->alarm_active = s_rt.alarm_active;
    xSemaphoreGive(s_lock);
    return true;
}

esp_err_t dry_run_guard_set_config(const dry_run_config_t *cfg, const char **bad_field) {
    if (!cfg || !dry_run_config_validate(cfg, bad_field)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_apply, portMAX_DELAY);
    if (!nvs_store_save_dry_run_config(cfg)) { /* no lock held across NVS */
        xSemaphoreGive(s_apply);
        return ESP_FAIL;
    }
    uint32_t now = now_ms();
    dry_run_out_t out;
    change_t ch;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dry_run_config_t old_cfg = s_cfg;
    uint8_t old_state = s_rt.state;
    s_cfg = *cfg;
    dry_run_config_changed(&old_cfg, &s_cfg, &s_rt, now, &out);
    capture_change(&ch, &out, old_state, now);
    xSemaphoreGive(s_lock);
    apply_change(&ch); /* disabling while LOCKED persists locked=false via the state comparison */
    log_transition_config(cfg->enabled);
    xSemaphoreGive(s_apply);
    return ESP_OK;
}

esp_err_t dry_run_guard_measure_start(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_apply, portMAX_DELAY);
    pump_control_status_t ps;
    bool pump_ok = pump_control_get_status(&ps);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_meas.state == DRY_RUN_MEASURE_RUNNING || s_meas.pending_off;
    bool inhibited = s_rt.state == DRY_RUN_WAIT || s_rt.state == DRY_RUN_LOCKED;
    xSemaphoreGive(s_lock);
    if (!pump_ok || busy || inhibited || ps.water.running) {
        xSemaphoreGive(s_apply);
        return ESP_ERR_INVALID_STATE;
    }
    flow_switch_status_t fs;
    memset(&fs, 0, sizeof(fs));
    if (flow_switch_get_status(&fs) && fs.flow_on) {
        /* Flow already reads on before the pump starts (stuck switch / jumper left in): a measure
         * would finish instantly with a bogus prime time. */
        xSemaphoreGive(s_apply);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!pump_control_water_test(true)) {
        xSemaphoreGive(s_apply);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t t0 = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_meas.state = DRY_RUN_MEASURE_RUNNING;
    s_meas.t0_ms = t0;
    s_meas.elapsed_ms = 0;
    s_meas.suggested_prime_sec = 0;
    s_meas.pending_off = false;
    xSemaphoreGive(s_lock);
    log_transition_measure("started", 0);
    xSemaphoreGive(s_apply);
    return ESP_OK;
}

esp_err_t dry_run_guard_measure_stop(void) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_apply, portMAX_DELAY);
    uint32_t now = now_ms();
    bool was_running = false;
    uint32_t elapsed = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_meas.state == DRY_RUN_MEASURE_RUNNING) {
        was_running = true;
        elapsed = now - s_meas.t0_ms;
        s_meas.elapsed_ms = elapsed;
        s_meas.state = DRY_RUN_MEASURE_STOPPED;
        s_meas.pending_off = true;
    }
    xSemaphoreGive(s_lock);
    if (was_running) {
        log_transition_measure("stopped", elapsed);
        measure_relay_off(); /* if refused, the next tick retries */
    }
    xSemaphoreGive(s_apply);
    return ESP_OK;
}

bool dry_run_guard_get_measure(dry_run_measure_status_t *out) {
    if (!out || !s_initialized) {
        return false;
    }
    uint32_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->state = (uint8_t)s_meas.state;
    out->elapsed_ms = (s_meas.state == DRY_RUN_MEASURE_RUNNING) ? (now - s_meas.t0_ms) : s_meas.elapsed_ms;
    out->suggested_prime_sec = (s_meas.state == DRY_RUN_MEASURE_DONE) ? s_meas.suggested_prime_sec : 0;
    xSemaphoreGive(s_lock);
    return true;
}
