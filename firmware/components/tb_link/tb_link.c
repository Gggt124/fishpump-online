#include "tb_link.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "device_api.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_store.h"
#include "uplink.h"
#include "wifi_manager.h"

#include "cJSON.h"

/*
 * Threading model
 *  - tb_link task: the only code that creates, starts, stops or destroys an esp-mqtt client (the task_* helpers
 *    below are called from it only), loads and saves the token, and changes the state.
 *  - mqtt_event_handler (esp-mqtt task): posts a small command to the queue and flips the atomic `connected`
 *    flag. It never touches NVS or the uplink. The one exception is the fixed too_large / busy reply to an RPC
 *    request, published on the event's own client (esp-mqtt allows that from its handler, and the client
 *    cannot be destroyed while its handler runs). RPC requests are only copied into rpc_q; the handler never
 *    runs a method.
 *  - tb_rpc task: pops RPC requests, runs device_api_call and publishes the reply under client_lock, like
 *    sink_send, after checking the request's generation against the current one.
 *  - HTTP task: tb_link_apply_config / tb_link_reprovision save the config, update the RAM copy and raise the
 *    atomic restart flag; they never wait for the task.
 *  - uplink task: sink_is_connected / sink_send. sink_send enqueues under client_lock, which the task takes
 *    only to clear the published client pointer before it destroys the client. tb_rpc uses client_lock the
 *    same way, and the tb_link task drains rpc_q after each destroy.
 * Every command carries the session generation of the client that produced it; the generation is bumped on
 * each teardown and each new client, so events of a destroyed client never touch the next session.
 * Locks: save_lock (NVS saves + config hand-off) -> lock (config copy and status fields). Neither is ever
 * held while calling esp-mqtt or the uplink.
 */

static const char *TAG = "tb_link";

#define TB_TASK_STACK 6144
#define TB_TASK_PRIORITY 2
#define TB_TICK_MS 1000
#define TB_WIFI_RECHECK_MS 2000
#define TB_CONNECT_TIMEOUT_MS 15000
#define TB_PROV_REPLY_TIMEOUT_MS 15000
#define TB_NETWORK_TIMEOUT_MS 10000
#define TB_KEEPALIVE_SEC 30
#define TB_LOST_GRACE_MS 2000 /* CONNECTED with the connected flag down this long = a dropped DISCONNECTED */
#define TB_OUTBOX_LIMIT 16384
#define TB_QUEUE_LEN 8
#define TB_REPLY_MAX 384
#define TB_PROV_RESPONSE_TOPIC "/provision/response"
#define TB_PROV_REQUEST_TOPIC "/provision/request"
#define TB_ERR_MAX 96
#define TB_RPC_TASK_STACK 6144
#define TB_RPC_TASK_PRIORITY 2
#define TB_RPC_QUEUE_LEN 4
#define TB_RPC_LOCK_MS 200
#define TB_RPC_SUB_TOPIC TB_RPC_REQ_PREFIX "+"
#define TB_RPC_BODY_TOO_LARGE "{\"ok\":false,\"error\":\"too_large\"}"
#define TB_RPC_BODY_BUSY "{\"ok\":false,\"error\":\"busy\"}"
#define TB_RPC_BODY_INVALID_JSON "{\"ok\":false,\"error\":\"invalid_json\",\"message\":\"Expected a JSON object\"}"
#define TB_RPC_BODY_UNKNOWN_METHOD "{\"ok\":false,\"error\":\"unknown_method\"}"
#define TB_RPC_BODY_MEMORY "{\"ok\":false,\"error\":\"memory\",\"message\":\"JSON allocation failed\"}"

typedef enum {
    CMD_RESTART = 0, /* wake-up only: the restart itself is carried by the atomic flag */
    CMD_CONNECTED,
    CMD_DISCONNECTED,
    CMD_SUBSCRIBED,
    CMD_PROV_REPLY,
    CMD_ERROR
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t gen;
    int err_type;    /* CMD_ERROR: esp_mqtt_error_type_t */
    int refuse_code; /* CMD_ERROR: esp_mqtt_connect_return_code_t */
    uint32_t attempt; /* connection attempt of the client that produced it */
    size_t len;      /* CMD_PROV_REPLY: bytes in data; 0 = truncated or empty */
    char data[TB_REPLY_MAX];
} tb_cmd_t;

/* One queued RPC request. payload is heap memory (PSRAM when there is some), owned by the item. */
typedef struct {
    uint32_t req_id;
    uint32_t gen; /* session generation of the client that received it */
    size_t len;
    char *payload; /* NUL terminated */
} rpc_item_t;

static struct {
    bool inited;
    QueueHandle_t q;
    QueueHandle_t rpc_q;           /* rpc_item_t, depth TB_RPC_QUEUE_LEN */
    TaskHandle_t rpc_task;
    SemaphoreHandle_t lock;        /* cfg and the status fields */
    SemaphoreHandle_t save_lock;   /* serialises NVS saves with the config hand-off */
    SemaphoreHandle_t client_lock; /* client_pub, held by sink_send while it enqueues */
    TaskHandle_t task;

    tb_link_config_t cfg; /* lock */
    tb_state_t state;     /* lock */
    char last_error[TB_ERR_MAX]; /* lock */
    int64_t connected_since_ms;  /* lock */
    int64_t retry_deadline_ms;   /* lock */

    esp_mqtt_client_handle_t client_pub; /* client_lock: token session client, NULL otherwise */
    atomic_uint gen;     /* client identity: bumped on every teardown and new client */
    atomic_uint attempt; /* connection attempt of the current client: bumped on every (re)connect */
    atomic_bool token_session; /* token client is allowed to raise `connected` (an attempt is running) */
    atomic_bool connected;
    atomic_bool restart_req;
    atomic_uint sent;
    atomic_uint failed;
} s;

/* State owned by the task (no locking needed). */
typedef struct {
    tb_link_config_t cfg;
    esp_mqtt_client_handle_t client;
    tb_state_t state;
    uint32_t backoff_ms;
    int64_t deadline_ms;
    int64_t next_wifi_check_ms;
    bool prov_mode;
    bool prov_connected;
    bool prov_req_sent;
    uint32_t attempt;        /* mirror of s.attempt for the attempt the task waits on */
    uint32_t reconnect_fails; /* consecutive esp_mqtt_client_reconnect refusals */
    int64_t lost_since_ms;   /* CONNECTED watchdog: when the connected flag was first seen down, 0 = up */
} task_ctx_t;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

const char *tb_link_state_name(tb_state_t st)
{
    switch (st) {
    case TB_STATE_OFF: return "off";
    case TB_STATE_WAIT_WIFI: return "wait_wifi";
    case TB_STATE_PROVISIONING: return "provisioning";
    case TB_STATE_CONNECTING: return "connecting";
    case TB_STATE_CONNECTED: return "connected";
    case TB_STATE_BACKOFF: return "backoff";
    }
    return "?";
}

/* ---- uplink sink (uplink task) ---- */

static bool sink_is_connected(void *ctx)
{
    (void)ctx;
    return atomic_load(&s.connected);
}

static esp_err_t sink_send(void *ctx, uplink_msg_kind_t kind, const char *json, size_t len)
{
    (void)ctx;
    const char *topic = (kind == UPLINK_MSG_ATTRIBUTES) ? UPLINK_TOPIC_ATTRIBUTES : UPLINK_TOPIC_TELEMETRY;
    int id = -1;
    if (json != NULL && len > 0 && len < 0x7fffffffu && atomic_load(&s.connected) &&
        xSemaphoreTake(s.client_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s.client_pub != NULL) {
            /* QoS 1, stored in the (capped) outbox; the esp-mqtt task does the network work. */
            id = esp_mqtt_client_enqueue(s.client_pub, topic, json, (int)len, 1, 0, true);
        }
        xSemaphoreGive(s.client_lock);
    }
    if (id >= 0) {
        atomic_fetch_add(&s.sent, 1);
        return ESP_OK;
    }
    atomic_fetch_add(&s.failed, 1);
    return ESP_FAIL;
}

/* ---- RPC receive side (esp-mqtt task) ---- */

static void rpc_item_free(rpc_item_t *it)
{
    free(it->payload);
    it->payload = NULL;
}

/* Reply with a fixed body on the event's own client (handler context only). */
static void rpc_reply_from_handler(esp_mqtt_client_handle_t client, uint32_t req_id, const char *body)
{
    char topic[48];
    if (client == NULL || tb_rpc_response_topic(req_id, topic, sizeof(topic)) < 0) {
        return;
    }
    (void)esp_mqtt_client_enqueue(client, topic, body, (int)strlen(body), 1, 0, true);
}

/* Copy the request into rpc_q without waiting; never runs a method. Called by the handler of a live session. */
static void rpc_on_data(esp_mqtt_event_handle_t ev, uint32_t gen)
{
    uint32_t req_id = 0;
    if (!atomic_load(&s.token_session) || s.rpc_q == NULL ||
        !tb_rpc_parse_topic(ev->topic, (size_t)ev->topic_len, &req_id)) {
        return;
    }
    if (ev->current_data_offset != 0) {
        return; /* a later fragment: the first one already got its too_large */
    }
    if (ev->total_data_len > ev->data_len || ev->data_len > TB_RPC_MAX_REQUEST || ev->data_len < 0) {
        rpc_reply_from_handler(ev->client, req_id, TB_RPC_BODY_TOO_LARGE);
        return;
    }
    rpc_item_t it = {.req_id = req_id, .gen = gen, .len = (size_t)ev->data_len};
    it.payload = heap_caps_malloc(it.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (it.payload == NULL) {
        it.payload = heap_caps_malloc(it.len + 1, MALLOC_CAP_8BIT); /* boards without PSRAM */
    }
    if (it.payload == NULL) {
        rpc_reply_from_handler(ev->client, req_id, TB_RPC_BODY_BUSY);
        return;
    }
    if (it.len > 0) {
        memcpy(it.payload, ev->data, it.len);
    }
    it.payload[it.len] = '\0';
    if (xQueueSend(s.rpc_q, &it, 0) != pdTRUE) {
        rpc_item_free(&it);
        rpc_reply_from_handler(ev->client, req_id, TB_RPC_BODY_BUSY);
    }
}

/* Free every queued request (after a client destroy: their replies have no client to go to). */
static void rpc_queue_drain(void)
{
    rpc_item_t it;
    while (s.rpc_q != NULL && xQueueReceive(s.rpc_q, &it, 0) == pdTRUE) {
        rpc_item_free(&it);
    }
}

/* Message ids the broker has acknowledged (MQTT_EVENT_PUBLISHED), newest overwrites the oldest. The tb_rpc task
 * looks for its reply's id here; a ring covers an ack that lands before the id is known to the waiter. */
#define TB_ACKED_RING 8
static atomic_int s_acked_ids[TB_ACKED_RING];
static atomic_uint s_acked_next;
static void (*_Atomic s_connected_hook)(void);
static void (*_Atomic s_session_hook)(void);
static void (*_Atomic s_data_hook)(const tb_link_data_t *d);

/* Route of the message being received (esp-mqtt task only): later fragments carry no topic. */
typedef enum { ROUTE_DROP = 0, ROUTE_RPC, ROUTE_PROV, ROUTE_HOOK } data_route_t;
#define TB_HOOK_TOPIC_MAX 64
static data_route_t s_route;
static char s_route_topic[TB_HOOK_TOPIC_MAX];
static int s_route_topic_len;

static void acked_ring_clear(void)
{
    for (int i = 0; i < TB_ACKED_RING; i++) {
        atomic_store(&s_acked_ids[i], -1);
    }
}

static bool acked_ring_has(int id)
{
    for (int i = 0; i < TB_ACKED_RING; i++) {
        if (atomic_load(&s_acked_ids[i]) == id) {
            return true;
        }
    }
    return false;
}

/* ---- esp-mqtt event handler (esp-mqtt task): post a command, flip the flag, nothing else ---- */

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    if (ev == NULL || gen != atomic_load(&s.gen)) {
        return; /* event of a session that was already torn down */
    }
    tb_cmd_t c = {.gen = gen, .attempt = atomic_load(&s.attempt)};
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        if (atomic_load(&s.token_session)) {
            atomic_store(&s.connected, true);
        }
        c.type = CMD_CONNECTED;
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s.connected, false);
        c.type = CMD_DISCONNECTED;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        c.type = CMD_SUBSCRIBED;
        break;
    case MQTT_EVENT_PUBLISHED:
        atomic_store(&s_acked_ids[atomic_fetch_add(&s_acked_next, 1u) % TB_ACKED_RING], ev->msg_id);
        return;
    case MQTT_EVENT_DATA:
        if (ev->current_data_offset == 0 && ev->topic != NULL) { /* first fragment: decide the route */
            const size_t rpc_len = strlen(TB_RPC_REQ_PREFIX);
            s_route = ROUTE_DROP;
            if (ev->topic_len > (int)rpc_len && memcmp(ev->topic, TB_RPC_REQ_PREFIX, rpc_len) == 0) {
                s_route = ROUTE_RPC;
            } else if (ev->topic_len == (int)strlen(TB_PROV_RESPONSE_TOPIC) &&
                       memcmp(ev->topic, TB_PROV_RESPONSE_TOPIC, (size_t)ev->topic_len) == 0) {
                s_route = ROUTE_PROV;
            } else if (atomic_load(&s.token_session) && ev->topic_len > 0 && ev->topic_len < TB_HOOK_TOPIC_MAX) {
                memcpy(s_route_topic, ev->topic, (size_t)ev->topic_len);
                s_route_topic_len = ev->topic_len;
                s_route = ROUTE_HOOK;
            }
        }
        if (s_route == ROUTE_RPC) {
            rpc_on_data(ev, gen);
            return;
        }
        if (s_route == ROUTE_HOOK) {
            void (*dh)(const tb_link_data_t *) = atomic_load(&s_data_hook);
            if (dh != NULL) {
                tb_link_data_t d = {.topic = s_route_topic, .topic_len = s_route_topic_len, .data = ev->data,
                                    .data_len = ev->data_len, .offset = ev->current_data_offset,
                                    .total = ev->total_data_len};
                dh(&d);
            }
            return;
        }
        /* Only the first fragment carries the topic; the reply is small, a fragmented one counts as bad. */
        if (s_route != ROUTE_PROV || ev->topic == NULL) {
            return; /* dropped route, or a PROV continuation fragment: ignored as before */
        }
        c.type = CMD_PROV_REPLY;
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len && ev->data_len > 0 &&
            ev->data_len < (int)sizeof(c.data)) {
            memcpy(c.data, ev->data, (size_t)ev->data_len);
            c.len = (size_t)ev->data_len;
        }
        break;
    case MQTT_EVENT_ERROR:
        c.type = CMD_ERROR;
        if (ev->error_handle != NULL) {
            c.err_type = (int)ev->error_handle->error_type;
            c.refuse_code = (int)ev->error_handle->connect_return_code;
        }
        break;
    default:
        return;
    }
    (void)xQueueSend(s.q, &c, 0); /* full queue: the connect / reply timeouts still resolve the session */
}

/* ---- task-side helpers (called from tb_link_task only) ---- */

static void task_publish_state(task_ctx_t *tc, tb_state_t st)
{
    bool changed = (tc->state != st);
    tc->state = st;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.state = st;
    s.connected_since_ms = (st == TB_STATE_CONNECTED) ? now_ms() : 0;
    s.retry_deadline_ms = (st == TB_STATE_BACKOFF) ? tc->deadline_ms : 0;
    xSemaphoreGive(s.lock);
    if (changed) {
        ESP_LOGI(TAG, "state -> %s", tb_link_state_name(st));
    }
}

static void task_set_error(const char *msg)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    snprintf(s.last_error, sizeof(s.last_error), "%s", msg);
    xSemaphoreGive(s.lock);
    if (msg[0] != '\0') {
        ESP_LOGW(TAG, "%s", msg);
    }
}

/* Invalidate the session, unpublish the client from the sink, then stop and destroy it. After this returns
 * no handler of the old client is running or will run. */
static void task_teardown(task_ctx_t *tc)
{
    atomic_fetch_add(&s.gen, 1);
    atomic_store(&s.token_session, false);
    atomic_store(&s.connected, false);
    xSemaphoreTake(s.client_lock, portMAX_DELAY);
    s.client_pub = NULL;
    xSemaphoreGive(s.client_lock);
    esp_mqtt_client_handle_t c = tc->client;
    tc->client = NULL;
    tc->reconnect_fails = 0;
    if (c != NULL) {
        esp_mqtt_client_destroy(c);
    }
    atomic_store(&s.connected, false); /* a handler that raced the gen bump is gone now */
    rpc_queue_drain(); /* no handler of the old client can queue any more; tb_rpc drops stale replies by gen */
}

/* A failed attempt. A token session keeps its client (and so its QoS 1 outbox: alarm events already accepted
 * by sink_send are resent after the reconnect); only a provisioning session is destroyed. */
static void task_fail(task_ctx_t *tc, const char *msg, bool at_cap)
{
    atomic_store(&s.token_session, false); /* no CONNECTED of this attempt may raise the flag any more */
    atomic_store(&s.connected, false);
    if (tc->prov_mode || tc->client == NULL) {
        task_teardown(tc);
    } else {
        esp_mqtt_client_disconnect(tc->client); /* make sure an established connection is dropped */
    }
    if (msg != NULL) {
        task_set_error(msg);
    }
    uint8_t stg_type = 0;
    if (!nvs_store_get_staging_type(&stg_type)) {
        stg_type = 0;
    }
    tc->backoff_ms = tb_backoff_after_fail_ms(tc->backoff_ms, at_cap, stg_type == 1);
    tc->deadline_ms = now_ms() + tc->backoff_ms;
    ESP_LOGI(TAG, "retry in %lu s", (unsigned long)(tc->backoff_ms / 1000));
    task_publish_state(tc, TB_STATE_BACKOFF);
}

/* Wi-Fi went away under a CONNECTING/CONNECTED token link: stop reporting connected at once and wait for Wi-Fi.
 * The client (and its outbox) stays. */
static void task_wifi_lost(task_ctx_t *tc)
{
    atomic_store(&s.token_session, false);
    atomic_store(&s.connected, false);
    if (tc->client != NULL) {
        esp_mqtt_client_disconnect(tc->client);
    }
    tc->next_wifi_check_ms = now_ms() + TB_WIFI_RECHECK_MS;
    task_publish_state(tc, TB_STATE_WAIT_WIFI);
}

static void task_start_session(task_ctx_t *tc)
{
    const tb_link_config_t *cfg = &tc->cfg;
    if (cfg->host[0] == '\0') {
        task_publish_state(tc, TB_STATE_OFF);
        return;
    }
#ifdef TB_LINK_TEST_BLOCK_CONNECT
    task_set_error("test build: connect blocked");
    task_publish_state(tc, TB_STATE_OFF);
    return;
#endif
    bool token_mode = cfg->token[0] != '\0';
    if (!token_mode && (cfg->provision_key[0] == '\0' || cfg->provision_secret[0] == '\0')) {
        task_set_error("config: no token and no provision key/secret");
        task_publish_state(tc, TB_STATE_OFF);
        return;
    }

    char uri[TB_HOST_MAX + 24];
    snprintf(uri, sizeof(uri), "%s://%s:%u", cfg->tls ? "mqtts" : "mqtt", cfg->host, (unsigned)cfg->port);

    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.uri = uri;
    if (cfg->tls) {
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    mc.credentials.client_id = cfg->device_name;
    mc.credentials.username = token_mode ? cfg->token : "provision";
    mc.network.disable_auto_reconnect = true;
    mc.network.timeout_ms = TB_NETWORK_TIMEOUT_MS;
    mc.session.keepalive = TB_KEEPALIVE_SEC;
    mc.buffer.size = 2048;
    mc.outbox.limit = TB_OUTBOX_LIMIT;

    uint32_t gen = atomic_fetch_add(&s.gen, 1) + 1;
    tc->attempt = atomic_fetch_add(&s.attempt, 1) + 1;
    atomic_store(&s.token_session, token_mode);
    esp_mqtt_client_handle_t c = esp_mqtt_client_init(&mc); /* copies the strings */
    if (c == NULL) {
        task_fail(tc, "mqtt: client init failed", false);
        return;
    }
    if (esp_mqtt_client_register_event(c, ESP_EVENT_ANY_ID, mqtt_event_handler, (void *)(uintptr_t)gen) != ESP_OK ||
        esp_mqtt_client_start(c) != ESP_OK) {
        esp_mqtt_client_destroy(c);
        task_fail(tc, "mqtt: client start failed", false);
        return;
    }
    tc->client = c;
    tc->prov_mode = !token_mode;
    tc->prov_connected = false;
    tc->prov_req_sent = false;
    tc->deadline_ms = now_ms() + TB_CONNECT_TIMEOUT_MS;
    if (token_mode) {
        xSemaphoreTake(s.client_lock, portMAX_DELAY);
        s.client_pub = c;
        xSemaphoreGive(s.client_lock);
    }
    task_publish_state(tc, token_mode ? TB_STATE_CONNECTING : TB_STATE_PROVISIONING);
}

/* Wi-Fi is up: reconnect the kept token client, or create a client when there is none. */
static void task_connect(task_ctx_t *tc)
{
    if (tc->client == NULL || tc->prov_mode) {
        task_start_session(tc);
        return;
    }
    tc->attempt = atomic_fetch_add(&s.attempt, 1) + 1;
    atomic_store(&s.token_session, true);
    if (esp_mqtt_client_reconnect(tc->client) == ESP_OK) {
        tc->reconnect_fails = 0;
        tc->deadline_ms = now_ms() + TB_CONNECT_TIMEOUT_MS;
        task_publish_state(tc, TB_STATE_CONNECTING);
        return;
    }
    /* Not waiting for a reconnect yet (the old connection is still being dropped): ask again shortly. After
     * 30 refusals give the client up and build a new one (its outbox is lost, but the link cannot hang). */
    atomic_store(&s.token_session, false);
    esp_mqtt_client_disconnect(tc->client);
    if (++tc->reconnect_fails >= 30) {
        ESP_LOGW(TAG, "client does not reconnect, recreating it");
        task_teardown(tc);
        task_start_session(tc);
        return;
    }
    tc->next_wifi_check_ms = now_ms() + 1000;
    task_publish_state(tc, TB_STATE_WAIT_WIFI);
}

static void task_restart(task_ctx_t *tc)
{
    task_teardown(tc);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    tc->cfg = s.cfg;
    s.last_error[0] = '\0';
    xSemaphoreGive(s.lock);
    tc->backoff_ms = 0;
    tc->next_wifi_check_ms = 0;
    tc->deadline_ms = 0;
    task_publish_state(tc, tc->cfg.host[0] != '\0' ? TB_STATE_WAIT_WIFI : TB_STATE_OFF);
    if (tc->state == TB_STATE_OFF) {
        ESP_LOGI(TAG, "link off (no host configured)");
    }
}

/* Provisioning succeeded: persist the token (unless a newer config arrived meanwhile) and reconnect with it. */
static void task_provision_done(task_ctx_t *tc, const char *token)
{
    xSemaphoreTake(s.save_lock, portMAX_DELAY);
    if (atomic_load(&s.restart_req)) {
        xSemaphoreGive(s.save_lock); /* a new config is pending: the restart discards this result */
        return;
    }
    tb_link_config_t save;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    snprintf(s.cfg.token, sizeof(s.cfg.token), "%s", token);
    save = s.cfg;
    xSemaphoreGive(s.lock);
    bool ok = nvs_store_save_tb_link(&save);
    xSemaphoreGive(s.save_lock);
    memset(&save, 0, sizeof(save));
    if (!ok) {
        /* The device exists on the server now but we could not keep its token. */
        xSemaphoreTake(s.lock, portMAX_DELAY);
        s.cfg.token[0] = '\0';
        xSemaphoreGive(s.lock);
        task_fail(tc, "provision: could not save token", true);
        return;
    }
    ESP_LOGI(TAG, "provisioned, token saved");
    task_teardown(tc);
    snprintf(tc->cfg.token, sizeof(tc->cfg.token), "%s", token);
    task_start_session(tc);
}

static void task_handle_error(task_ctx_t *tc, const tb_cmd_t *c)
{
    switch (c->err_type) {
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        task_fail(tc, tc->prov_mode ? "connect: refused (provisioning)" : "connect: refused (token rejected?)",
                  false);
        break;
    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
        task_fail(tc, "provision: subscribe failed", true);
        break;
    default:
        task_fail(tc, "mqtt: transport error", false);
        break;
    }
}

static void task_handle_cmd(task_ctx_t *tc, const tb_cmd_t *c)
{
    if (c->type == CMD_RESTART) {
        return; /* wake-up only */
    }
    if (tc->client == NULL || c->gen != atomic_load(&s.gen) || c->attempt != tc->attempt) {
        return; /* event of a destroyed client or of an earlier connection attempt */
    }
    bool attempt_running = tc->state == TB_STATE_CONNECTING || tc->state == TB_STATE_PROVISIONING ||
                           tc->state == TB_STATE_CONNECTED;
    if (!attempt_running) {
        return; /* late event after the task already gave the attempt up */
    }
    switch (c->type) {
    case CMD_CONNECTED:
        if (!tc->prov_mode) {
            tc->lost_since_ms = 0;
            tc->backoff_ms = 0; /* accepted connect: next failure starts at 5 s again */
            task_set_error("");
            task_publish_state(tc, TB_STATE_CONNECTED);
            void (*hook)(void) = atomic_load(&s_connected_hook);
            if (hook != NULL) {
                hook(); /* before the subscribe: a staged Wi-Fi switch is confirmed or judged first */
            }
            /* Clean session: the subscription is gone after every reconnect, so subscribe on every accepted connect. */
            if (esp_mqtt_client_subscribe_single(tc->client, TB_RPC_SUB_TOPIC, 1) < 0) {
                ESP_LOGW(TAG, "rpc: subscribe failed");
            }
            static const char *const ota_subs[] = {TB_FW_ATTR_TOPIC, TB_FW_ATTR_RESP_PREFIX "+",
                                                   TB_FW_RESP_PREFIX "+/chunk/+"};
            for (size_t i = 0; i < sizeof(ota_subs) / sizeof(ota_subs[0]); i++) {
                if (esp_mqtt_client_subscribe_single(tc->client, ota_subs[i], 1) < 0) {
                    ESP_LOGW(TAG, "ota: subscribe %u failed", (unsigned)i);
                }
            }
            void (*shook)(void) = atomic_load(&s_session_hook);
            if (shook != NULL) {
                shook();
            }
        } else {
            tc->prov_connected = true;
            tc->deadline_ms = now_ms() + TB_PROV_REPLY_TIMEOUT_MS;
            if (esp_mqtt_client_subscribe_single(tc->client, TB_PROV_RESPONSE_TOPIC, 1) < 0) {
                task_fail(tc, "provision: subscribe failed", true);
            }
        }
        break;
    case CMD_SUBSCRIBED:
        if (tc->prov_mode && tc->prov_connected && !tc->prov_req_sent) {
            char req[192];
            int n = tb_provision_build_request(&tc->cfg, req, sizeof(req));
            if (n <= 0 ||
                esp_mqtt_client_enqueue(tc->client, TB_PROV_REQUEST_TOPIC, req, n, 1, 0, true) < 0) {
                task_fail(tc, "provision: request not sent", true);
            } else {
                tc->prov_req_sent = true;
            }
            memset(req, 0, sizeof(req));
        }
        break;
    case CMD_PROV_REPLY:
        if (tc->prov_mode && tc->prov_req_sent) {
            char token[TB_TOKEN_MAX] = "";
            char err[TB_ERR_MAX - 16] = "";
            char msg[TB_ERR_MAX];
            tb_prov_result_t r = (c->len == 0) ? TB_PROV_BAD_REPLY
                                               : tb_provision_parse(c->data, c->len, token, sizeof(token), err,
                                                                    sizeof(err));
            if (r == TB_PROV_OK) {
                task_provision_done(tc, token);
            } else if (r == TB_PROV_FAILURE) {
                snprintf(msg, sizeof(msg), "provision: %s", err[0] != '\0' ? err : "failure");
                task_fail(tc, msg, true);
            } else {
                task_fail(tc, "provision: bad reply", true);
            }
            memset(token, 0, sizeof(token));
        }
        break;
    case CMD_ERROR:
        task_handle_error(tc, c);
        break;
    case CMD_DISCONNECTED:
        task_fail(tc, "mqtt: disconnected", false);
        break;
    default:
        break;
    }
}

static void task_tick(task_ctx_t *tc)
{
    if (atomic_exchange(&s.restart_req, false)) {
        task_restart(tc);
        return;
    }
    int64_t now = now_ms();
    if ((tc->state == TB_STATE_CONNECTING || tc->state == TB_STATE_CONNECTED) && !wifi_manager_is_sta_connected()) {
        task_wifi_lost(tc); /* do not wait for the keepalive to notice a dead link */
        return;
    }
    switch (tc->state) {
    case TB_STATE_WAIT_WIFI:
        if (now >= tc->next_wifi_check_ms) {
            tc->next_wifi_check_ms = now + TB_WIFI_RECHECK_MS;
            if (wifi_manager_is_sta_connected()) {
                task_connect(tc);
            }
        }
        break;
    case TB_STATE_BACKOFF:
        if (now >= tc->deadline_ms) {
            tc->next_wifi_check_ms = 0;
            task_publish_state(tc, TB_STATE_WAIT_WIFI);
        }
        break;
    case TB_STATE_CONNECTING:
        if (now >= tc->deadline_ms) {
            task_fail(tc, "connect: timeout", false);
        }
        break;
    case TB_STATE_PROVISIONING:
        if (now >= tc->deadline_ms) {
            if (tc->prov_connected) {
                task_fail(tc, "provision: no reply", true);
            } else {
                task_fail(tc, "connect: timeout", false);
            }
        }
        break;
    case TB_STATE_CONNECTED:
        /* The handler only posts commands; a DISCONNECTED lost to a full queue would leave this state with the
         * flag down for good. Treat a flag that stays down past a short grace like that DISCONNECTED. */
        if (atomic_load(&s.connected)) {
            tc->lost_since_ms = 0;
        } else if (tc->lost_since_ms == 0) {
            tc->lost_since_ms = now;
        } else if (now - tc->lost_since_ms >= TB_LOST_GRACE_MS) {
            tc->lost_since_ms = 0;
            task_fail(tc, "mqtt: disconnected", false);
        }
        break;
    case TB_STATE_OFF:
    default:
        break;
    }
}

static void tb_link_task(void *arg)
{
    (void)arg;
    task_ctx_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.state = TB_STATE_OFF;
    task_restart(&tc);
    for (;;) {
        tb_cmd_t c;
        if (xQueueReceive(s.q, &c, pdMS_TO_TICKS(TB_TICK_MS)) == pdTRUE) {
            task_handle_cmd(&tc, &c);
            while (xQueueReceive(s.q, &c, 0) == pdTRUE) {
                task_handle_cmd(&tc, &c); /* drain before the tick decides on timeouts */
            }
        }
        task_tick(&tc);
    }
}

/* ---- tb_rpc task: runs device_api methods and publishes the replies ---- */

/* Publish a reply through the same guard as sink_send: client_lock, with the client pointer valid only while the
 * request's generation is still the current one. */
static bool rpc_publish(uint32_t gen, uint32_t req_id, const char *body, int *msg_id_out)
{
    char topic[48];
    int id = -1;
    if (tb_rpc_response_topic(req_id, topic, sizeof(topic)) < 0 || !atomic_load(&s.connected)) {
        return false;
    }
    if (xSemaphoreTake(s.client_lock, pdMS_TO_TICKS(TB_RPC_LOCK_MS)) == pdTRUE) {
        if (s.client_pub != NULL && gen == atomic_load(&s.gen)) {
            id = esp_mqtt_client_enqueue(s.client_pub, topic, body, (int)strlen(body), 1, 0, true);
        }
        xSemaphoreGive(s.client_lock);
    }
    if (msg_id_out != NULL) {
        *msg_id_out = id;
    }
    return id >= 0;
}

/* Action registered by the method being run on the tb_rpc task (only that task touches it). */
static void (*s_defer_action)(void);

bool tb_link_defer_after_reply(void (*action)(void))
{
    if (action == NULL || s.rpc_task == NULL || xTaskGetCurrentTaskHandle() != s.rpc_task) {
        return false;
    }
    s_defer_action = action;
    return true;
}

void tb_link_set_connected_hook(void (*hook)(void))
{
    atomic_store(&s_connected_hook, hook);
}

void tb_link_set_data_hook(void (*hook)(const tb_link_data_t *d))
{
    atomic_store(&s_data_hook, hook);
}

void tb_link_set_session_hook(void (*hook)(void))
{
    atomic_store(&s_session_hook, hook);
}

bool tb_link_is_connected(void)
{
    return atomic_load(&s.connected);
}

/* Same guard as rpc_publish; used by tb_ota (chunk requests, telemetry). Does not touch the sink's counters. */
int tb_link_publish(const char *topic, const char *data, int len, int qos)
{
    int id = -1;
    if (topic == NULL || len < 0 || !atomic_load(&s.connected)) {
        return -1;
    }
    if (xSemaphoreTake(s.client_lock, pdMS_TO_TICKS(TB_RPC_LOCK_MS)) == pdTRUE) {
        if (s.client_pub != NULL && atomic_load(&s.connected)) {
            id = esp_mqtt_client_enqueue(s.client_pub, topic, data != NULL ? data : "", len, qos, 0, true);
        }
        xSemaphoreGive(s.client_lock);
    }
    return id >= 0 ? id : -1;
}

/* Last handled requests, keyed by id + payload length + payload hash (tb_link_logic). ThingsBoard re-sends a
 * pending request after a re-subscribe, and a reply can be lost while the link is down: a repeat inside the
 * window gets the cached reply and the method does not run again. Only the tb_rpc task touches these. */
static tb_rpc_dedup_t s_rpc_dedup;
static char *s_rpc_dedup_reply[TB_RPC_DEDUP_SLOTS]; /* heap, NUL terminated; NULL = nothing cached */

static char *rpc_dup_string(const char *src)
{
    size_t n = strlen(src) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT); /* boards without PSRAM */
    }
    if (p != NULL) {
        memcpy(p, src, n);
    }
    return p;
}

/* Runs one request and sends its reply. Frees the payload. Params and replies are never logged. */
static void rpc_process(rpc_item_t *it)
{
    char method[TB_RPC_METHOD_MAX] = "";
    cJSON *params = NULL;
    cJSON *out = NULL;
    char *printed = NULL;
    const char *body = NULL;
    const char *status = "?";

    s_defer_action = NULL;
    uint32_t t_ms = (uint32_t)now_ms();
    int dup = tb_rpc_dedup_find(&s_rpc_dedup, it->req_id, it->payload, it->len, t_ms);
    tb_rpc_parse_t pr = tb_rpc_parse_request(it->payload, it->len, method, sizeof(method), &params);
    if (dup >= 0 && s_rpc_dedup_reply[dup] != NULL) {
        cJSON_Delete(params);
        (void)rpc_publish(it->gen, it->req_id, s_rpc_dedup_reply[dup], NULL); /* a cached reply never runs an action */
        ESP_LOGI(TAG, "rpc %s id=%lu -> duplicate, cached reply", method[0] != '\0' ? method : "-",
                 (unsigned long)it->req_id);
        rpc_item_free(it);
        return;
    }
    if (pr == TB_RPC_PARSE_BAD_JSON) {
        body = TB_RPC_BODY_INVALID_JSON;
        status = "invalid_json";
    } else if (pr == TB_RPC_PARSE_NO_METHOD) {
        body = TB_RPC_BODY_UNKNOWN_METHOD;
        status = "unknown_method";
    } else {
        dapi_status_t st = device_api_call(method, params, DAPI_SRC_TB, &out);
        status = device_api_http_status(st);
        if (out != NULL) {
            printed = cJSON_PrintUnformatted(out);
        }
        body = (printed != NULL) ? printed : TB_RPC_BODY_MEMORY;
    }
    cJSON_Delete(params);
    cJSON_Delete(out);

    /* Remember the request and its reply before publishing, so a dropped reply is still answered next time.
     * No memory for the copy: do not cache. */
    char *copy = rpc_dup_string(body);
    if (copy != NULL) {
        int slot = tb_rpc_dedup_insert(&s_rpc_dedup, it->req_id, it->payload, it->len, t_ms);
        if (slot >= 0) {
            free(s_rpc_dedup_reply[slot]);
            s_rpc_dedup_reply[slot] = copy;
        } else {
            free(copy);
        }
    }
    rpc_item_free(it);

    acked_ring_clear();
    int msg_id = -1;
    bool sent = rpc_publish(it->gen, it->req_id, body, &msg_id);
    ESP_LOGI(TAG, "rpc %s id=%lu -> %s%s", method[0] != '\0' ? method : "-", (unsigned long)it->req_id, status,
             sent ? "" : " (reply dropped)");
    free(printed);

    void (*action)(void) = s_defer_action;
    s_defer_action = NULL;
    if (action != NULL) {
        /* Wait for the broker to ack the reply (or the cap), so the action (a restart) does not eat it. */
        int64_t t0 = now_ms();
        for (;;) {
            bool acked = sent && acked_ring_has(msg_id);
            if (tb_after_reply_ready(acked, (uint32_t)(now_ms() - t0))) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        action();
    }
}

static void tb_rpc_task(void *arg)
{
    (void)arg;
    for (;;) {
        rpc_item_t it;
        if (xQueueReceive(s.rpc_q, &it, portMAX_DELAY) == pdTRUE) {
            rpc_process(&it);
        }
    }
}

/* ---- public API ---- */

static void post_restart(void)
{
    atomic_store(&s.restart_req, true); /* the task also polls the flag every tick */
    tb_cmd_t c = {.type = CMD_RESTART};
    (void)xQueueSend(s.q, &c, 0);
}

esp_err_t tb_link_init(void)
{
    if (s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s.lock = xSemaphoreCreateMutex();
    s.save_lock = xSemaphoreCreateMutex();
    s.client_lock = xSemaphoreCreateMutex();
    s.q = xQueueCreate(TB_QUEUE_LEN, sizeof(tb_cmd_t));
    s.rpc_q = xQueueCreate(TB_RPC_QUEUE_LEN, sizeof(rpc_item_t));
    if (s.lock == NULL || s.save_lock == NULL || s.client_lock == NULL || s.q == NULL || s.rpc_q == NULL) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    bool stored = nvs_store_load_tb_link(&s.cfg, mac);
    bool provisioned = s.cfg.token[0] != '\0';
    ESP_LOGI(TAG, "config %s: host=%s port=%u tls=%d device=%s provisioned=%d", stored ? "loaded" : "default",
             s.cfg.host[0] ? s.cfg.host : "(none)", (unsigned)s.cfg.port, (int)s.cfg.tls, s.cfg.device_name,
             (int)provisioned);
    s.state = TB_STATE_OFF;
    s.inited = true;
    if (xTaskCreate(tb_rpc_task, "tb_rpc", TB_RPC_TASK_STACK, NULL, TB_RPC_TASK_PRIORITY, &s.rpc_task) != pdPASS) {
        s.inited = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tb_link_task, "tb_link", TB_TASK_STACK, NULL, TB_TASK_PRIORITY, &s.task) != pdPASS) {
        vTaskDelete(s.rpc_task); /* nothing can have been queued: no client exists yet */
        s.rpc_task = NULL;
        s.inited = false;
        return ESP_ERR_NO_MEM;
    }
    static const uplink_sink_t sink = {.is_connected = sink_is_connected, .send = sink_send, .ctx = NULL};
    esp_err_t err = uplink_set_sink(&sink);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uplink_set_sink failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t tb_link_apply_config(const tb_link_config_t *cfg)
{
    if (cfg == NULL || !s.inited || !tb_link_config_valid(cfg, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s.save_lock, portMAX_DELAY);
    if (!nvs_store_save_tb_link(cfg)) {
        xSemaphoreGive(s.save_lock);
        return ESP_FAIL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.cfg = *cfg;
    xSemaphoreGive(s.lock);
    atomic_store(&s.restart_req, true); /* before releasing save_lock: a provisioning save sees it */
    xSemaphoreGive(s.save_lock);
    post_restart();
    return ESP_OK;
}

esp_err_t tb_link_reprovision(void)
{
    if (!s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    tb_link_config_t cfg;
    xSemaphoreTake(s.save_lock, portMAX_DELAY);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    cfg = s.cfg;
    xSemaphoreGive(s.lock);
    cfg.token[0] = '\0';
    if (!nvs_store_save_tb_link(&cfg)) {
        xSemaphoreGive(s.save_lock);
        memset(&cfg, 0, sizeof(cfg));
        return ESP_FAIL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.cfg = cfg;
    xSemaphoreGive(s.lock);
    atomic_store(&s.restart_req, true);
    xSemaphoreGive(s.save_lock);
    memset(&cfg, 0, sizeof(cfg));
    post_restart();
    return ESP_OK;
}

void tb_link_get_config(tb_link_config_t *out)
{
    if (out == NULL) {
        return;
    }
    if (!s.inited) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.cfg;
    xSemaphoreGive(s.lock);
}

void tb_link_get_status(tb_link_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->sent = atomic_load(&s.sent);
    out->failed = atomic_load(&s.failed);
    if (!s.inited) {
        return;
    }
    int64_t now = now_ms();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    out->state = s.state;
    snprintf(out->last_error, sizeof(out->last_error), "%s", s.last_error);
    if (s.state == TB_STATE_CONNECTED && s.connected_since_ms > 0 && now >= s.connected_since_ms) {
        out->connected_sec = (uint32_t)((now - s.connected_since_ms) / 1000);
    }
    if (s.state == TB_STATE_BACKOFF && s.retry_deadline_ms > now) {
        out->retry_in_sec = (uint32_t)((s.retry_deadline_ms - now + 999) / 1000);
    }
    xSemaphoreGive(s.lock);
}

int32_t tb_link_stack_hwm(void)
{
    TaskHandle_t t = s.task;
    if (t == NULL) {
        return -1;
    }
    return (int32_t)(uxTaskGetStackHighWaterMark(t) * sizeof(StackType_t));
}

int32_t tb_link_rpc_stack_hwm(void)
{
    TaskHandle_t t = s.rpc_task;
    if (t == NULL) {
        return -1;
    }
    return (int32_t)(uxTaskGetStackHighWaterMark(t) * sizeof(StackType_t));
}
