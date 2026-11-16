#include "alarm_engine.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telemetry_hub.h"

static const char *TAG = "alarm_engine";

#define ENGINE_TICK_MS 1000
#define ENGINE_TASK_STACK 4096
#define ENGINE_TASK_PRIORITY 2 /* below the sensor tasks (3) and cooling (4) */
#define ENGINE_TASK_CORE 1

/* A firmware-owned alarm (e.g. pump dry-run). Raised and cleared by its owner, never saved. */
typedef struct {
    uint16_t id;
    uint8_t severity;
    uint8_t signal_id;
    char name[ALARM_NAME_BYTES];
    alarm_runtime_t rt;
    alarm_engine_system_ack_cb_t on_ack;
} system_alarm_t;

/* One mutex guards everything in here. Hub publishes happen after it is released.
 * Acknowledge callbacks of system alarms are also called after it is released. */
static struct {
    SemaphoreHandle_t lock;
    alarm_rule_set_t set;
    alarm_runtime_t rt[ALARM_MAX_RULES];
    alarm_ring_t ring;
    alarm_counts_t counts;
    system_alarm_t sys[ALARM_ENGINE_MAX_SYSTEM];
    uint8_t sys_count;
    uint32_t boot_id;
    uint32_t last_tick_ms;
    uint32_t rules_revision;
    nvs_store_alarm_rules_load_status_t storage;
    bool running;
    bool initialized;
} s;

static uint32_t now_ms(void)
{
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ms == 0 ? 1 : ms;
}

/* Copy a rule name (always NUL-terminated, never truncated mid-buffer). */
static void copy_name(char *dst, const char *src)
{
    size_t n = strnlen(src, ALARM_NAME_BYTES - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Events produced while the lock is held, published once it is released. */
typedef struct {
    telemetry_alarm_payload_t items[ALARM_MAX_RULES + 1];
    size_t count;
} pending_t;

/* Caller holds the lock. Pushes the event into the ring and queues the hub payload. */
static void record_event_for(pending_t *p, uint16_t id, uint8_t signal_id, uint8_t severity, const char *name,
                             const alarm_transition_t *tr, uint32_t now)
{
    alarm_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = tr->type;
    evt.reason = tr->reason;
    evt.rule_id = id;
    evt.signal_id = signal_id;
    evt.severity = severity;
    evt.value = tr->value;
    evt.uptime_ms = now;
    copy_name(evt.name, name);
    uint32_t seq = alarm_ring_push(&s.ring, &evt);

    if (p->count < sizeof(p->items) / sizeof(p->items[0])) {
        telemetry_alarm_payload_t *a = &p->items[p->count++];
        memset(a, 0, sizeof(*a));
        a->seq = seq;
        a->rule_id = id;
        a->type = tr->type;
        a->severity = severity;
        a->signal_id = signal_id;
        a->value = tr->value;
    }
}

/* Caller holds the lock. */
static void record_event(pending_t *p, const alarm_rule_t *rule, const alarm_transition_t *tr, uint32_t now)
{
    record_event_for(p, rule->id, rule->signal_id, rule->severity, rule->name, tr, now);
}

/* Caller holds the lock. */
static void record_system_event(pending_t *p, const system_alarm_t *a, const alarm_transition_t *tr, uint32_t now)
{
    record_event_for(p, a->id, a->signal_id, a->severity, a->name, tr, now);
}

/* Caller holds the lock. */
static void refresh_counts(pending_t *p)
{
    alarm_counts(&s.set, s.rt, &s.counts);
    for (uint8_t i = 0; i < s.sys_count; i++) {
        alarm_counts_add(&s.counts, &s.sys[i].rt, s.sys[i].severity);
    }
    for (size_t i = 0; i < p->count; i++) {
        p->items[i].active_count = s.counts.active;
        p->items[i].critical_count = s.counts.critical;
        p->items[i].unacked_count = s.counts.unacked;
    }
}

static void publish_pending(const pending_t *p)
{
    for (size_t i = 0; i < p->count; i++) {
        telemetry_event_t e = {.type = TELEMETRY_EVT_ALARM};
        e.data.alarm = p->items[i];
        telemetry_hub_publish(&e);
        ESP_LOGI(TAG, "alarm seq=%lu rule=%u type=%u sev=%u active=%u", (unsigned long)p->items[i].seq,
                 p->items[i].rule_id, p->items[i].type, p->items[i].severity, p->items[i].active_count);
    }
}

static int find_rule(uint16_t id)
{
    for (int i = 0; i < s.set.count; i++) {
        if (s.set.rules[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* Caller holds the lock. */
static system_alarm_t *find_system(uint16_t id)
{
    for (uint8_t i = 0; i < s.sys_count; i++) {
        if (s.sys[i].id == id) {
            return &s.sys[i];
        }
    }
    return NULL;
}

static void engine_task(void *arg)
{
    (void)arg;
    pending_t pending;
    telemetry_snapshot_t snap;
    TickType_t wake = xTaskGetTickCount();
    s.running = true;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(ENGINE_TICK_MS));
        if (telemetry_hub_get_latest(&snap) != ESP_OK) {
            continue;
        }
        uint32_t now = now_ms();
        pending.count = 0;

        xSemaphoreTake(s.lock, portMAX_DELAY);
        for (int i = 0; i < s.set.count; i++) {
            alarm_transition_t tr;
            if (alarm_rule_step(&s.set.rules[i], &s.rt[i], &snap, now, &tr)) {
                record_event(&pending, &s.set.rules[i], &tr, now);
            }
        }
        refresh_counts(&pending);
        s.last_tick_ms = now;
        xSemaphoreGive(s.lock);

        publish_pending(&pending);
    }
}

esp_err_t alarm_engine_init(void)
{
    if (s.initialized) {
        return ESP_OK;
    }
    s.lock = xSemaphoreCreateMutex();
    if (s.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    alarm_ring_init(&s.ring);
    s.storage = nvs_store_load_alarm_rules(&s.set);
    memset(s.rt, 0, sizeof(s.rt));
    memset(&s.counts, 0, sizeof(s.counts));
    s.boot_id = esp_random() | 1u; /* non-zero: tells a reboot from a continuing sequence */
    s.last_tick_ms = now_ms();
    ESP_LOGI(TAG, "%u rules loaded, storage %s", s.set.count,
             s.storage == NVS_STORE_ALARM_RULES_LOADED            ? "saved"
             : s.storage == NVS_STORE_ALARM_RULES_DEFAULTS_MISSING ? "defaults-missing"
             : s.storage == NVS_STORE_ALARM_RULES_DEFAULTS_INVALID ? "defaults-invalid"
                                                                    : "defaults-error");

    if (xTaskCreatePinnedToCore(engine_task, "alarm_eng", ENGINE_TASK_STACK, NULL, ENGINE_TASK_PRIORITY, NULL,
                                ENGINE_TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s.initialized = true;
    return ESP_OK;
}

bool alarm_engine_get_summary(alarm_engine_summary_t *out)
{
    if (!s.initialized || out == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    out->counts = s.counts;
    out->last_seq = s.ring.last_seq;
    out->boot_id = s.boot_id;
    out->last_tick_ms = s.last_tick_ms;
    out->running = s.running;
    xSemaphoreGive(s.lock);
    return true;
}

bool alarm_engine_get_rules(alarm_rule_set_t *out, nvs_store_alarm_rules_load_status_t *storage)
{
    if (!s.initialized || out == NULL) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.set;
    if (storage != NULL) {
        *storage = s.storage;
    }
    xSemaphoreGive(s.lock);
    return true;
}

esp_err_t alarm_engine_upsert_rule(const alarm_rule_t *rule, bool has_id, alarm_rule_t *stored,
                                   const char **bad_field)
{
    if (!s.initialized || rule == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *dummy;
    if (bad_field == NULL) {
        bad_field = &dummy;
    }
    alarm_rule_t r = *rule;
    if (!alarm_rule_validate(&r, bad_field)) {
        return ESP_ERR_INVALID_ARG;
    }

    alarm_rule_set_t *next = malloc(sizeof(*next)); /* heap: the set is ~3 KB */
    if (next == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        free(next);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *next = s.set;
    int idx = -1;
    if (has_id) {
        idx = find_rule(r.id);
        if (idx < 0) {
            err = ESP_ERR_NOT_FOUND;
        }
    } else if (next->count >= ALARM_MAX_RULES || next->next_id == 0 || next->next_id >= ALARM_SYSTEM_ID_BASE) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        r.id = next->next_id++;
        idx = next->count++;
    }

    if (err == ESP_OK) {
        next->rules[idx] = r;
        if (!nvs_store_save_alarm_rules(next)) {
            err = ESP_FAIL; /* RAM state untouched: nothing to roll back */
        }
    }
    if (err == ESP_OK) {
        /* The edited rule starts over; close it under its old name first. */
        alarm_transition_t tr;
        if (alarm_rule_reset(&s.rt[idx], &tr)) {
            record_event(pending, &s.set.rules[idx], &tr, now_ms());
        }
        s.set = *next;
        s.storage = NVS_STORE_ALARM_RULES_LOADED;
        s.rules_revision++;
        refresh_counts(pending);
        if (stored != NULL) {
            *stored = r;
        }
    }
    xSemaphoreGive(s.lock);

    publish_pending(pending);
    free(pending);
    free(next);
    return err;
}

esp_err_t alarm_engine_delete_rule(uint16_t id)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    alarm_rule_set_t *next = malloc(sizeof(*next));
    pending_t *pending = calloc(1, sizeof(*pending));
    if (next == NULL || pending == NULL) {
        free(next);
        free(pending);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    int idx = find_rule(id);
    if (idx < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        *next = s.set;
        memmove(&next->rules[idx], &next->rules[idx + 1], (size_t)(next->count - idx - 1) * sizeof(alarm_rule_t));
        next->count--;
        memset(&next->rules[next->count], 0, sizeof(alarm_rule_t));
        if (!nvs_store_save_alarm_rules(next)) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        alarm_transition_t tr;
        if (alarm_rule_reset(&s.rt[idx], &tr)) {
            record_event(pending, &s.set.rules[idx], &tr, now_ms()); /* name still present */
        }
        s.set = *next;
        /* compact the runtime array the same way as the rules */
        memmove(&s.rt[idx], &s.rt[idx + 1], (size_t)(s.set.count - idx) * sizeof(alarm_runtime_t));
        memset(&s.rt[s.set.count], 0, sizeof(alarm_runtime_t));
        s.storage = NVS_STORE_ALARM_RULES_LOADED;
        s.rules_revision++;
        refresh_counts(pending);
    }
    xSemaphoreGive(s.lock);

    publish_pending(pending);
    free(pending);
    free(next);
    return err;
}

esp_err_t alarm_engine_ack(uint16_t rule_id)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    alarm_engine_system_ack_cb_t on_ack = NULL;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    int idx = find_rule(rule_id);
    system_alarm_t *sys = idx < 0 ? find_system(rule_id) : NULL;
    if (idx >= 0) {
        alarm_transition_t tr;
        if (alarm_rule_ack(&s.rt[idx], &tr)) {
            record_event(pending, &s.set.rules[idx], &tr, now_ms());
        }
        refresh_counts(pending);
    } else if (sys != NULL) {
        /* State is not touched: the owner decides (it may ignore the ack, or clear the alarm). */
        if (sys->rt.state == ALARM_STATE_ACTIVE) {
            on_ack = sys->on_ack;
        }
    } else {
        err = ESP_ERR_NOT_FOUND;
    }
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    if (on_ack != NULL) {
        on_ack(rule_id); /* after the unlock: it may call alarm_engine_system_clear */
    }
    return err;
}

esp_err_t alarm_engine_ack_all(size_t *acked)
{
    if (acked != NULL) {
        *acked = 0;
    }
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;
    uint32_t now = now_ms();
    struct {
        uint16_t id;
        alarm_engine_system_ack_cb_t on_ack;
    } sys_acks[ALARM_ENGINE_MAX_SYSTEM];
    size_t n_sys = 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.set.count; i++) {
        alarm_transition_t tr;
        if (alarm_rule_ack(&s.rt[i], &tr)) {
            record_event(pending, &s.set.rules[i], &tr, now);
            n++;
        }
    }
    for (uint8_t i = 0; i < s.sys_count; i++) {
        if (s.sys[i].rt.state == ALARM_STATE_ACTIVE && s.sys[i].on_ack != NULL) {
            sys_acks[n_sys].id = s.sys[i].id;
            sys_acks[n_sys].on_ack = s.sys[i].on_ack;
            n_sys++;
        }
    }
    refresh_counts(pending);
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    for (size_t i = 0; i < n_sys; i++) {
        sys_acks[i].on_ack(sys_acks[i].id); /* after the unlock: a callback may call alarm_engine_system_clear */
        n++;
    }
    if (acked != NULL) {
        *acked = n;
    }
    return ESP_OK;
}

size_t alarm_engine_get_active(alarm_active_t *out, size_t max)
{
    if (!s.initialized || out == NULL) {
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.set.count && n < max; i++) {
        uint8_t st = s.rt[i].state;
        if (st != ALARM_STATE_ACTIVE && st != ALARM_STATE_LATCHED) {
            continue;
        }
        alarm_active_t *a = &out[n++];
        memset(a, 0, sizeof(*a));
        a->rule_id = s.set.rules[i].id;
        a->signal_id = s.set.rules[i].signal_id;
        a->severity = s.set.rules[i].severity;
        a->state = st;
        a->acked = s.rt[i].acked;
        a->value = s.rt[i].last_value;
        a->since_ms = s.rt[i].active_since_ms;
        copy_name(a->name, s.set.rules[i].name);
    }
    for (uint8_t i = 0; i < s.sys_count && n < max; i++) {
        const system_alarm_t *sa = &s.sys[i];
        if (sa->rt.state != ALARM_STATE_ACTIVE) {
            continue;
        }
        alarm_active_t *a = &out[n++];
        memset(a, 0, sizeof(*a));
        a->rule_id = sa->id;
        a->signal_id = sa->signal_id;
        a->severity = sa->severity;
        a->state = sa->rt.state;
        a->acked = sa->rt.acked;
        a->value = sa->rt.last_value;
        a->since_ms = sa->rt.active_since_ms;
        copy_name(a->name, sa->name);
    }
    xSemaphoreGive(s.lock);
    return n;
}

size_t alarm_engine_get_events_since(uint32_t after_seq, alarm_event_t *out, size_t max, uint32_t *oldest_seq)
{
    if (!s.initialized || out == NULL) {
        if (oldest_seq != NULL) {
            *oldest_seq = 0;
        }
        return 0;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    size_t n = alarm_ring_since(&s.ring, after_seq, out, max, oldest_seq);
    xSemaphoreGive(s.lock);
    return n;
}

uint32_t alarm_engine_get_rules_revision(void)
{
    if (!s.initialized) {
        return 0;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    uint32_t rev = s.rules_revision;
    xSemaphoreGive(s.lock);
    return rev;
}

esp_err_t alarm_engine_system_register(uint16_t id, const char *name, uint8_t severity, uint8_t signal_id,
                                       alarm_engine_system_ack_cb_t on_ack)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (id < ALARM_SYSTEM_ID_BASE || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (find_system(id) != NULL) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s.sys_count >= ALARM_ENGINE_MAX_SYSTEM) {
        err = ESP_ERR_NO_MEM;
    } else {
        system_alarm_t *a = &s.sys[s.sys_count];
        memset(a, 0, sizeof(*a));
        a->id = id;
        a->severity = severity;
        a->signal_id = signal_id;
        a->on_ack = on_ack;
        copy_name(a->name, name);
        s.sys_count++;
    }
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t alarm_engine_system_raise(uint16_t id, float value)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    system_alarm_t *a = find_system(id);
    if (a == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        if (a->rt.state != ALARM_STATE_ACTIVE) {
            a->rt.state = ALARM_STATE_ACTIVE;
            a->rt.active_since_ms = now;
        }
        a->rt.acked = false; /* a repeat raise is a re-alert */
        a->rt.last_value = value;
        alarm_transition_t tr = {.type = ALARM_EVT_RAISED, .reason = ALARM_REASON_NONE, .value = value};
        record_system_event(pending, a, &tr, now);
        refresh_counts(pending);
    }
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    return err;
}

esp_err_t alarm_engine_system_clear(uint16_t id, uint8_t reason, float value)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    system_alarm_t *a = find_system(id);
    if (a == NULL) {
        err = ESP_ERR_NOT_FOUND;
    } else if (a->rt.state == ALARM_STATE_ACTIVE) {
        a->rt.state = ALARM_STATE_NORMAL;
        a->rt.acked = false;
        a->rt.last_value = value;
        alarm_transition_t tr = {.type = ALARM_EVT_CLEARED, .reason = reason, .value = value};
        record_system_event(pending, a, &tr, now);
        refresh_counts(pending);
    }
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    return err;
}
