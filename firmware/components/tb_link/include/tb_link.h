#pragma once

/*
 * ThingsBoard MQTT link: one task owns the esp-mqtt client (create, start, stop, destroy), provisions the
 * device on first contact, reconnects with backoff, and plugs an uplink sink in once. Everything the link does
 * is best effort: a down or wrong broker never blocks the web app, alarms, pump or dry-run protection.
 * The token and provision secret are never logged and never returned by tb_link_get_status.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tb_link_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TB_STATE_OFF = 0,
    TB_STATE_WAIT_WIFI,
    TB_STATE_PROVISIONING,
    TB_STATE_CONNECTING,
    TB_STATE_CONNECTED,
    TB_STATE_BACKOFF
} tb_state_t;

typedef struct {
    tb_state_t state;
    char last_error[96];   /* "" = none; technical English, e.g. "provision: Provision request is not found!" */
    uint32_t connected_sec; /* 0 unless CONNECTED */
    uint32_t sent, failed;  /* sink send results since boot */
    uint32_t retry_in_sec;  /* in BACKOFF */
} tb_link_status_t;

/* Load the config from NVS, start the task, register the uplink sink (once). ESP_ERR_INVALID_STATE if repeated. */
esp_err_t tb_link_init(void);
/* Validate (ESP_ERR_INVALID_ARG), save to NVS (ESP_FAIL), then ask the task to restart with it. Returns at once. */
esp_err_t tb_link_apply_config(const tb_link_config_t *cfg);
/* Clear the stored token, save, restart the link (provisioning runs again). Returns at once. */
esp_err_t tb_link_reprovision(void);
/* Copy of the current config. Holds the token and secret: the caller must not expose them. */
void tb_link_get_config(tb_link_config_t *out);
void tb_link_get_status(tb_link_status_t *out);
/* "off","wait_wifi","provisioning","connecting","connected","backoff" */
const char *tb_link_state_name(tb_state_t s);
/* Minimum free stack of the tb_link task in bytes since it started, or -1 when it was not created. */
int32_t tb_link_stack_hwm(void);
/* Same for the tb_rpc task (runs the device_api methods), or -1 before it exists. */
int32_t tb_link_rpc_stack_hwm(void);

/* Only valid on the tb_rpc task while it runs a method (returns false anywhere else). After the reply of the
 * request being processed is acknowledged by the broker (or after TB_AFTER_REPLY_MAX_MS), the tb_rpc task calls
 * action(). A cached (duplicate) reply never runs an action. */
bool tb_link_defer_after_reply(void (*action)(void));
/* Set once at boot. Called on the tb_link task for every accepted token-session connect, before the RPC subscribe. */
void tb_link_set_connected_hook(void (*hook)(void));

typedef struct {
    const char *topic; int topic_len;  /* the message's topic, also on later fragments */
    const char *data;  int data_len;   /* this fragment */
    int offset;        int total;      /* fragment offset and the message's total length */
} tb_link_data_t;
/* Set once at boot, before tb_link_init. Called on the esp-mqtt task for every MQTT_EVENT_DATA fragment of a token
 * session whose topic is neither the RPC prefix nor the provision response (topics over 63 bytes are dropped).
 * The hook must not block. */
void tb_link_set_data_hook(void (*hook)(const tb_link_data_t *d));
/* Same. Called on the tb_link task after the subscribes of every accepted token-session connect. */
void tb_link_set_session_hook(void (*hook)(void));
/* Enqueue a message on the live token session from any task. Returns the message id, or -1 (bad argument, not
 * connected, lock timeout, no client). data need not be NUL terminated. */
int tb_link_publish(const char *topic, const char *data, int len, int qos);
bool tb_link_is_connected(void);

#ifdef __cplusplus
}
#endif
