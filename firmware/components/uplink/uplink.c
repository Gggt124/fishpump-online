#include "uplink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_engine.h"
#include "alarm_rules.h"
#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telemetry_hub.h"
#include "uplink_format.h"

static const char *TAG = "uplink";

#define UPLINK_TICK_MS 1000
#define UPLINK_TASK_STACK 4096
#define UPLINK_TASK_PRIORITY 2 /* not in TWDT, below the sensor tasks (3) */
#define UPLINK_TASK_CORE 1

/* Fields the change-driven telemetry send watches. */
typedef struct {
    bool valid;
    bool relay1, relay2, float_high, flow;
    uint32_t alarm_seq;
} watched_t;

/*
 * Locks. send_lock serializes every use of s.sink (is_connected, send) with uplink_set_sink, so once
 * set_sink returns, the old ctx is never called again. lock guards the preview buffers and counters the
 * HTTP handler reads, so it is never held across a sink call. The HTTP preview never takes send_lock
 * (httpd must not wait on sink I/O); it reads connected_cache, which the uplink task refreshes each tick.
 * Lock order when both are needed: send_lock first, then lock.
 */
static struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t send_lock;
    bool initialized;

    uplink_sink_t sink;
    bool has_sink;
    int64_t (*wall_clock)(void);

    /* Persistent buffers (PSRAM): the last formatted telemetry / event, kept for the preview. */
    char *tel_buf;
    int tel_len;
    char *ev_buf;
    int ev_len;
    alarm_event_t *events;

    uplink_forward_t fwd;
    uint32_t last_summary_seq;
    uint32_t pending_events;
    bool waiting_for_clock; /* under lock: events are held until the wall clock is set */
    bool connected_cache;   /* under lock: sink_ready() as of the last tick, for the preview */
    /* uplink task only: clock-hold state. released latches once the hold ends with the clock still unset
     * (ring margin or time cap) so events go out without ts until the clock is set or the sink drops. */
    bool hold_active;
    bool hold_released;
    uint32_t hold_start_ms;

    watched_t last_watched;
    uint32_t last_tel_ms;
    bool tel_ever;

    bool was_connected;
    bool want_identity;
    bool want_rules;
    uint32_t rules_sent_rev;
} s;

static uint32_t now_ms(void)
{
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ms == 0 ? 1 : ms;
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT); /* boards without PSRAM */
    }
    return p;
}

static watched_t watched_from(const telemetry_snapshot_t *snap, uint32_t alarm_seq)
{
    watched_t w = {.valid = true,
                   .relay1 = snap->has_pump && snap->pump.relay_1_active,
                   .relay2 = snap->has_pump && snap->pump.relay_2_active,
                   .float_high = snap->has_float && snap->float_state.float_is_high,
                   .flow = snap->has_flow && snap->flow.flow_on,
                   .alarm_seq = alarm_seq};
    return w;
}

static bool watched_changed(const watched_t *a, const watched_t *b)
{
    return a->relay1 != b->relay1 || a->relay2 != b->relay2 || a->float_high != b->float_high ||
           a->flow != b->flow || a->alarm_seq != b->alarm_seq;
}

/* Caller holds send_lock. */
static bool sink_ready(void)
{
    return s.has_sink && s.sink.is_connected != NULL && s.sink.send != NULL && s.sink.is_connected(s.sink.ctx);
}

/* Telemetry: periodic, or sooner (>= UPLINK_MIN_GAP_MS apart) when a watched value or the alarm sequence changes. */
static void do_telemetry(const telemetry_snapshot_t *snap, const alarm_engine_summary_t *sm, bool have_sm,
                         uint32_t now)
{
    watched_t cur = watched_from(snap, have_sm ? sm->last_seq : 0);
    uint32_t since = (uint32_t)(now - s.last_tel_ms);
    bool due = !s.tel_ever || since >= (uint32_t)UPLINK_INTERVAL_SEC * 1000u ||
               (s.last_watched.valid && watched_changed(&s.last_watched, &cur) && since >= UPLINK_MIN_GAP_MS);
    if (!due) {
        return;
    }

    alarm_counts_t counts = {0};
    if (have_sm) {
        counts = sm->counts;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    int len = uplink_format_telemetry(snap, now, &counts, s.tel_buf, UPLINK_TELEMETRY_BUF);
    s.tel_len = len > 0 ? len : 0;
    xSemaphoreGive(s.lock);
    if (len <= 0) {
        ESP_LOGW(TAG, "telemetry does not fit its buffer");
        return;
    }
    s.last_watched = cur;
    s.last_tel_ms = now;
    s.tel_ever = true;

    /* A failed or skipped send is not retried: telemetry is not backfilled. */
    xSemaphoreTake(s.send_lock, portMAX_DELAY);
    if (sink_ready()) {
        esp_err_t err = s.sink.send(s.sink.ctx, UPLINK_MSG_TELEMETRY, s.tel_buf, (size_t)len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "telemetry send failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s.send_lock);
}

/* Events: store-and-forward in seq order; advance only when the sink accepts. */
static void do_events(const alarm_engine_summary_t *sm, uint32_t now)
{
    uint32_t oldest = 0;
    alarm_engine_get_events_since(s.fwd.last_forwarded_seq, s.events, 0, &oldest);
    uint32_t gap = uplink_forward_account_gap(&s.fwd, oldest);
    if (gap > 0) {
        ESP_LOGW(TAG, "%lu unsent alarm event(s) overwritten", (unsigned long)gap);
    }

    size_t n = alarm_engine_get_events_since(s.fwd.last_forwarded_seq, s.events, ALARM_HISTORY_LEN, &oldest);
    int64_t unix_now = s.wall_clock != NULL ? s.wall_clock() : 0;
    uint32_t pending = sm->last_seq >= s.fwd.last_forwarded_seq ? sm->last_seq - s.fwd.last_forwarded_seq : 0;
    if (n > pending) {
        pending = (uint32_t)n; /* the fresh fetch is the better count */
    }
    xSemaphoreTake(s.send_lock, portMAX_DELAY);
    bool ready = sink_ready();
    bool clock_set = unix_now != 0;
    bool hold = false;
    if (!ready || clock_set) {
        if (s.hold_active) {
            ESP_LOGI(TAG, "alarm events no longer held for the clock (%s)", clock_set ? "clock set" : "sink gone");
        }
        s.hold_active = false;
        s.hold_released = false;
    } else if (n > 0 && !s.hold_released) {
        uint32_t held_ms = s.hold_active ? (uint32_t)(now - s.hold_start_ms) : 0;
        hold = uplink_forward_hold_for_clock(false, pending, ALARM_HISTORY_LEN, held_ms);
        if (hold && !s.hold_active) {
            s.hold_active = true;
            s.hold_start_ms = now;
            ESP_LOGI(TAG, "holding alarm events until the wall clock is set");
        } else if (!hold) {
            ESP_LOGI(TAG, "sending alarm events without ts (%s)",
                     held_ms >= UPLINK_CLOCK_HOLD_MAX_MS ? "time cap" : "ring margin");
            s.hold_active = false;
            s.hold_released = true;
        }
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.waiting_for_clock = hold;
    xSemaphoreGive(s.lock);
    if (n > 0 && (!ready || hold)) {
        /* No transport, or waiting for the clock: keep the newest pending event formatted for the preview;
         * the rest wait in the ring (a ts-less send happens at the ring margin or after UPLINK_CLOCK_HOLD_MAX_MS). */
        const alarm_event_t *e = &s.events[n - 1];
        xSemaphoreTake(s.lock, portMAX_DELAY);
        int len = uplink_format_event(e, sm->boot_id, unix_now != 0 ? uplink_event_ts(unix_now, now, e->uptime_ms) : 0,
                                      s.ev_buf, UPLINK_EVENT_BUF);
        s.ev_len = len > 0 ? len : 0;
        xSemaphoreGive(s.lock);
        n = 0;
    }
    for (size_t i = 0; i < n; i++) {
        const alarm_event_t *e = &s.events[i];
        int64_t ts = unix_now != 0 ? uplink_event_ts(unix_now, now, e->uptime_ms) : 0;
        xSemaphoreTake(s.lock, portMAX_DELAY);
        int len = uplink_format_event(e, sm->boot_id, ts, s.ev_buf, UPLINK_EVENT_BUF);
        s.ev_len = len > 0 ? len : 0;
        xSemaphoreGive(s.lock);
        if (len <= 0) {
            ESP_LOGW(TAG, "alarm event %lu does not fit its buffer", (unsigned long)e->seq);
            s.fwd.last_forwarded_seq = e->seq; /* never block the queue on one bad event */
            continue;
        }
        if (!sink_ready()) {
            break; /* the sink went away mid-batch: events wait in the engine ring */
        }
        if (s.sink.send(s.sink.ctx, UPLINK_MSG_TELEMETRY, s.ev_buf, (size_t)len) != ESP_OK) {
            break;
        }
        s.fwd.last_forwarded_seq = e->seq;
    }
    xSemaphoreGive(s.send_lock);
    s.pending_events = sm->last_seq - s.fwd.last_forwarded_seq;
}

/* Caller holds send_lock. */
static bool send_attribute(char *(*build)(size_t *), const char *what)
{
    size_t len = 0;
    char *json = build(&len);
    if (json == NULL) {
        return false;
    }
    esp_err_t err = s.sink.send(s.sink.ctx, UPLINK_MSG_ATTRIBUTES, json, len);
    free(json);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s attribute send failed: %s", what, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static char *build_identity(size_t *len_out)
{
    char *buf = psram_alloc(UPLINK_IDENTITY_BUF);
    if (buf == NULL) {
        return NULL;
    }
    int n = uplink_format_identity(APP_TEMPLATE_FIRMWARE_VERSION, buf, UPLINK_IDENTITY_BUF);
    if (n <= 0) {
        free(buf);
        return NULL;
    }
    *len_out = (size_t)n;
    return buf;
}

static char *build_rules(size_t *len_out)
{
    alarm_rule_set_t *set = malloc(sizeof(*set));
    char *buf = psram_alloc(UPLINK_RULES_BUF);
    int n = -1;
    if (set != NULL && buf != NULL && alarm_engine_get_rules(set, NULL)) {
        n = uplink_format_rules(set, buf, UPLINK_RULES_BUF);
    }
    free(set);
    if (n <= 0) {
        free(buf);
        return NULL;
    }
    *len_out = (size_t)n;
    return buf;
}

/* Attributes: identity then rules when the sink connects; rules again when they change. */
static void do_attributes(void)
{
    xSemaphoreTake(s.send_lock, portMAX_DELAY);
    bool connected = sink_ready();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.connected_cache = connected;
    xSemaphoreGive(s.lock);
    if (!connected) {
        s.was_connected = false;
        s.want_identity = true;
        s.want_rules = true;
        xSemaphoreGive(s.send_lock);
        return;
    }
    if (!s.was_connected) {
        s.was_connected = true;
        s.want_identity = true;
        s.want_rules = true;
    }
    if (s.want_identity && send_attribute(build_identity, "identity")) {
        s.want_identity = false;
    }
    uint32_t rev = alarm_engine_get_rules_revision();
    if (!s.want_identity && (s.want_rules || rev != s.rules_sent_rev)) {
        if (send_attribute(build_rules, "rules")) {
            s.want_rules = false;
            s.rules_sent_rev = rev;
        }
    }
    xSemaphoreGive(s.send_lock);
}

static void uplink_task(void *arg)
{
    (void)arg;
    telemetry_snapshot_t snap;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(UPLINK_TICK_MS));
        if (telemetry_hub_get_latest(&snap) != ESP_OK) {
            continue;
        }
        alarm_engine_summary_t sm;
        bool have_sm = alarm_engine_get_summary(&sm);
        uint32_t now = now_ms();

        do_telemetry(&snap, &sm, have_sm, now);
        if (have_sm) {
            do_events(&sm, now);
        }
        do_attributes();
    }
}

esp_err_t uplink_init(void)
{
    if (s.initialized) {
        return ESP_OK;
    }
    s.lock = xSemaphoreCreateMutex();
    s.send_lock = xSemaphoreCreateMutex();
    s.tel_buf = psram_alloc(UPLINK_TELEMETRY_BUF);
    s.ev_buf = psram_alloc(UPLINK_EVENT_BUF);
    s.events = psram_alloc(ALARM_HISTORY_LEN * sizeof(alarm_event_t));
    if (s.lock == NULL || s.send_lock == NULL || s.tel_buf == NULL || s.ev_buf == NULL || s.events == NULL) {
        goto fail;
    }
    s.tel_buf[0] = '\0';
    s.ev_buf[0] = '\0';
    s.want_identity = true;
    s.want_rules = true;
    s.initialized = true; /* before the task starts: the preview may be asked at once */
    if (xTaskCreatePinnedToCore(uplink_task, "uplink", UPLINK_TASK_STACK, NULL, UPLINK_TASK_PRIORITY, NULL,
                                UPLINK_TASK_CORE) != pdPASS) {
        s.initialized = false;
        goto fail;
    }
    ESP_LOGI(TAG, "uplink ready (no transport; interval %d s)", UPLINK_INTERVAL_SEC);
    return ESP_OK;

fail:
    if (s.lock != NULL) {
        vSemaphoreDelete(s.lock);
        s.lock = NULL;
    }
    if (s.send_lock != NULL) {
        vSemaphoreDelete(s.send_lock);
        s.send_lock = NULL;
    }
    free(s.tel_buf);
    free(s.ev_buf);
    free(s.events);
    s.tel_buf = NULL;
    s.ev_buf = NULL;
    s.events = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t uplink_set_sink(const uplink_sink_t *sink)
{
    if (!s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s.send_lock, portMAX_DELAY); /* waits out any send in flight */
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.connected_cache = false;
    if (sink == NULL) {
        s.has_sink = false;
        memset(&s.sink, 0, sizeof(s.sink));
    } else {
        s.sink = *sink;
        s.has_sink = true;
    }
    xSemaphoreGive(s.lock);
    xSemaphoreGive(s.send_lock);
    return ESP_OK;
}

void uplink_set_wall_clock(int64_t (*now_unix_ms)(void))
{
    s.wall_clock = now_unix_ms;
}

char *uplink_preview_json(void)
{
    if (!s.initialized) {
        return NULL;
    }
    size_t id_len = 0, rules_len = 0;
    char *identity = build_identity(&id_len);
    char *rules = build_rules(&rules_len);

    size_t cap = 512 + UPLINK_TELEMETRY_BUF + UPLINK_EVENT_BUF + id_len + rules_len;
    char *out = psram_alloc(cap);
    if (out == NULL) {
        free(identity);
        free(rules);
        return NULL;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool connected = s.connected_cache;
    bool has_sink = s.has_sink;
    bool waiting = s.waiting_for_clock;
    uint32_t pending = s.pending_events;
    uint32_t dropped = s.fwd.dropped;
    int n = snprintf(out, cap,
                     "{\"ok\":true,\"sink\":\"%s\",\"connected\":%s,\"interval_sec\":%d,"
                     "\"pending_events\":%lu,\"waiting_for_clock\":%s,\"dropped\":%lu,\"last_telemetry\":%s,\"last_event\":%s,"
                     "\"identity\":%s,\"rules\":%s}",
                     has_sink ? "set" : "none", connected ? "true" : "false", UPLINK_INTERVAL_SEC,
                     (unsigned long)pending, waiting ? "true" : "false", (unsigned long)dropped,
                     s.tel_len > 0 ? s.tel_buf : "null", s.ev_len > 0 ? s.ev_buf : "null",
                     identity != NULL ? identity : "null", rules != NULL ? rules : "null");
    xSemaphoreGive(s.lock);
    free(identity);
    free(rules);
    if (n < 0 || (size_t)n >= cap) {
        free(out);
        return NULL;
    }
    return out;
}
