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

/* One mutex guards everything in here. Hub publishes happen after it is released. */
static struct {
    SemaphoreHandle_t lock;
    alarm_rule_set_t set;
    alarm_runtime_t rt[ALARM_MAX_RULES];
    alarm_ring_t ring;
    alarm_counts_t counts;
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
static void record_event(pending_t *p, const alarm_rule_t *rule, const alarm_transition_t *tr, uint32_t now)
{
    alarm_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = tr->type;
    evt.reason = tr->reason;
    evt.rule_id = rule->id;
    evt.signal_id = rule->signal_id;
    evt.severity = rule->severity;
    evt.value = tr->value;
    evt.uptime_ms = now;
    copy_name(evt.name, rule->name);
    uint32_t seq = alarm_ring_push(&s.ring, &evt);

    if (p->count < sizeof(p->items) / sizeof(p->items[0])) {
        telemetry_alarm_payload_t *a = &p->items[p->count++];
        memset(a, 0, sizeof(*a));
        a->seq = seq;
        a->rule_id = rule->id;
        a->type = tr->type;
        a->severity = rule->severity;
        a->signal_id = rule->signal_id;
        a->value = tr->value;
    }
}

/* Caller holds the lock. */
static void refresh_counts(pending_t *p)
{
    alarm_counts(&s.set, s.rt, &s.counts);
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
    } else if (next->count >= ALARM_MAX_RULES || next->next_id == 0) {
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
    xSemaphoreTake(s.lock, portMAX_DELAY);
    int idx = find_rule(rule_id);
    if (idx < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        alarm_transition_t tr;
        if (alarm_rule_ack(&s.rt[idx], &tr)) {
            record_event(pending, &s.set.rules[idx], &tr, now_ms());
        }
        refresh_counts(pending);
    }
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    return err;
}

size_t alarm_engine_ack_all(void)
{
    if (!s.initialized) {
        return 0;
    }
    pending_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return 0;
    }
    size_t n = 0;
    uint32_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (int i = 0; i < s.set.count; i++) {
        alarm_transition_t tr;
        if (alarm_rule_ack(&s.rt[i], &tr)) {
            record_event(pending, &s.set.rules[i], &tr, now);
            n++;
        }
    }
    refresh_counts(pending);
    xSemaphoreGive(s.lock);
    publish_pending(pending);
    free(pending);
    return n;
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
