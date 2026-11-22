#include "tb_state.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "alarm_engine.h"
#include "app_config.h"
#include "ota_key.h"
#include "cJSON.h"
#include "device_api.h"
#include "dry_run_guard.h"
#include "esp_log.h"
#include "tb_link.h"
#include "uplink.h"
#include "uplink_format.h"

static const char *TAG = "tb_state";

/* One read of a device_api method as the ThingsBoard source. NULL on any status other than DAPI_OK; a failure is
 * logged (method and status only, never the reply). The caller deletes the result. */
static cJSON *read_method(const char *method)
{
    cJSON *reply = NULL;
    dapi_status_t st = device_api_call(method, NULL, DAPI_SRC_TB, &reply);
    if (st == DAPI_OK && reply != NULL) {
        /* Some methods reply with a Raw node, which is not walkable: make it a real tree. */
        reply = uplink_json_materialize(reply);
        if (reply != NULL) {
            return reply;
        }
        ESP_LOGW(TAG, "%s: read failed (parse)", method);
        return NULL;
    }
    ESP_LOGW(TAG, "%s: read failed (status %d)", method, (int)st);
    cJSON_Delete(reply);
    return NULL;
}

/* Adds the method's reply minus the drop list under key. Returns true when the key was added. */
static bool add_filtered(cJSON *attrs, const char *key, const char *method, const char *const *drop, size_t ndrop)
{
    cJSON *reply = read_method(method);
    if (reply == NULL) {
        return false;
    }
    if (!cJSON_IsObject(reply)) {
        cJSON_Delete(reply);
        return false;
    }
    /* The reply is ours: drop the keys in place instead of deep-copying it (hardware.map.get is large). */
    for (size_t i = 0; i < ndrop; i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(reply, drop[i]);
    }
    if (!cJSON_AddItemToObject(attrs, key, reply)) {
        cJSON_Delete(reply);
        return false;
    }
    return true;
}

static void copy_string(cJSON *dst, const char *dst_key, const cJSON *src, const char *src_key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(src, src_key);
    if (cJSON_IsString(it) && it->valuestring != NULL) {
        cJSON_AddStringToObject(dst, dst_key, it->valuestring);
    }
}

static void copy_bool(cJSON *dst, const char *dst_key, const cJSON *src, const char *src_key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(src, src_key);
    if (cJSON_IsBool(it)) {
        cJSON_AddBoolToObject(dst, dst_key, cJSON_IsTrue(it));
    }
}

static void copy_number(cJSON *dst, const char *dst_key, const cJSON *src, const char *src_key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(src, src_key);
    if (cJSON_IsNumber(it)) {
        cJSON_AddNumberToObject(dst, dst_key, it->valuedouble);
    }
}

/* Allow-list only: nothing is copied wholesale from the replies. */
static bool add_wifi(cJSON *attrs)
{
    cJSON *wifi = cJSON_CreateObject();
    if (wifi == NULL) {
        return false;
    }
    bool any = false;

    cJSON *st = read_method("status.get");
    if (st != NULL) {
        copy_string(wifi, "ssid", st, "sta_ssid");
        copy_string(wifi, "ip", st, "sta_ip");
        copy_string(wifi, "mode", st, "wifi_mode");
        copy_bool(wifi, "ap_enabled", st, "ap_enabled");
        any = true;
        cJSON_Delete(st);
    }

    cJSON *prof = read_method("wifi.profiles.get");
    if (prof != NULL) {
        any = true;
        copy_number(wifi, "auto_index", prof, "auto_index");
        cJSON *list = cJSON_AddArrayToObject(wifi, "profiles");
        bool auto_connected = false;
        const cJSON *p = NULL;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(prof, "profiles"))
        {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "connected"))) {
                auto_connected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "auto"));
            }
            cJSON *e = cJSON_CreateObject();
            if (e == NULL) {
                continue;
            }
            copy_string(e, "ssid", p, "ssid");
            copy_bool(e, "auto", p, "auto");
            copy_bool(e, "connected", p, "connected");
            if (list == NULL || !cJSON_AddItemToArray(list, e)) {
                cJSON_Delete(e);
            }
        }
        cJSON_AddBoolToObject(wifi, "auto", auto_connected);
        cJSON_Delete(prof);
    }

    if (!any || !cJSON_AddItemToObject(attrs, "wifi", wifi)) {
        cJSON_Delete(wifi);
        return false;
    }
    return true;
}

static bool add_tb_link(cJSON *attrs)
{
    /* The config copy is larger than a task stack should hold: heap it, read the device name, wipe it. */
    tb_link_config_t *link_cfg = calloc(1, sizeof(*link_cfg));
    if (link_cfg == NULL) {
        return false;
    }
    tb_link_get_config(link_cfg);
    cJSON *obj = cJSON_CreateObject();
    bool ok = false;
    if (obj != NULL) {
        cJSON_AddStringToObject(obj, "device_name", link_cfg->device_name);
        cJSON_AddStringToObject(obj, "fw_version", APP_TEMPLATE_FIRMWARE_VERSION);
        char key_fp[OTA_KEY_FP_LEN + 1] = "";
        ota_key_fingerprint(APP_CONFIG_OTA_ENCRYPTION_KEY, key_fp);
        cJSON_AddStringToObject(obj, "ota_key_fp", key_fp);
        cJSON_AddBoolToObject(obj, "ota_key_dev", ota_key_is_dev(APP_CONFIG_OTA_ENCRYPTION_KEY));
        ok = cJSON_AddItemToObject(attrs, "tb_link", obj);
        if (!ok) {
            cJSON_Delete(obj);
        }
    }
    memset(link_cfg, 0, sizeof(*link_cfg));
    free(link_cfg);
    return ok;
}

static cJSON *build_attributes(void)
{
    cJSON *attrs = cJSON_CreateObject();
    if (attrs == NULL) {
        return NULL;
    }
    static const char *const drop_ok[] = {"ok"};
    static const char *const drop_cooling[] = {"ok", "status"};
    static const char *const drop_prot[] = {"ok", "pump_status"};
    static const char *const drop_hw[] = {"ok", "options"};
    int n = 0;
    n += add_filtered(attrs, "cfg_pump", "pump.config.get", drop_ok, 1) ? 1 : 0;
    n += add_filtered(attrs, "cfg_cooling", "cooling.config.get", drop_cooling, 2) ? 1 : 0;
    n += add_filtered(attrs, "cfg_protection", "protection.get", drop_prot, 2) ? 1 : 0;
    n += add_filtered(attrs, "hardware_map", "hardware.map.get", drop_hw, 2) ? 1 : 0;
    n += add_wifi(attrs) ? 1 : 0;
    n += add_tb_link(attrs) ? 1 : 0;
    if (n == 0) {
        cJSON_Delete(attrs);
        return NULL;
    }
    return attrs;
}

static void add_hwm(cJSON *out, const char *key, int32_t hwm)
{
    if (hwm >= 0) {
        cJSON_AddNumberToObject(out, key, (double)hwm);
    }
}

static cJSON *build_status(uint32_t *watched_hash)
{
    cJSON *pump = read_method("pump.status");
    cJSON *cooling = read_method("cooling.status");
    cJSON *status = read_method("status.get");
    cJSON *out = cJSON_CreateObject();
    if (out != NULL) {
        const cJSON *srcs[UPLINK_SRC_COUNT] = {0};
        srcs[UPLINK_SRC_PUMP] = pump;
        srcs[UPLINK_SRC_COOLING] = cooling;
        srcs[UPLINK_SRC_STATUS] = status;
        uplink_status_flatten(srcs, out, watched_hash);
        add_hwm(out, "stack_hwm_alarm", alarm_engine_stack_hwm());
        add_hwm(out, "stack_hwm_uplink", uplink_stack_hwm());
        add_hwm(out, "stack_hwm_dry_run", dry_run_guard_stack_hwm());
        add_hwm(out, "stack_hwm_tb_link", tb_link_stack_hwm());
        add_hwm(out, "stack_hwm_tb_rpc", tb_link_rpc_stack_hwm());
        tb_link_status_t ls;
        tb_link_get_status(&ls);
        cJSON_AddNumberToObject(out, "tb_sent", (double)ls.sent);
        cJSON_AddNumberToObject(out, "tb_failed", (double)ls.failed);
    }
    cJSON_Delete(pump);
    cJSON_Delete(cooling);
    cJSON_Delete(status);
    return out;
}

static void on_write(const char *method, dapi_status_t st)
{
    (void)method;
    (void)st;
    uplink_note_settings_changed();
}

esp_err_t tb_state_init(void)
{
    static const uplink_state_source_t src = {.build_attributes = build_attributes, .build_status = build_status};
    device_api_set_write_hook(on_write);
    return uplink_set_state_source(&src);
}
