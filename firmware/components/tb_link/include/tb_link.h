#pragma once

/*
 * ThingsBoard MQTT link: one task owns the esp-mqtt client (create, start, stop, destroy), provisions the
 * device on first contact, reconnects with backoff, and plugs an uplink sink in once. Everything the link does
 * is best effort: a down or wrong broker never blocks the web app, alarms, pump or dry-run protection.
 * The token and provision secret are never logged and never returned by tb_link_get_status.
 */

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

#ifdef __cplusplus
}
#endif
