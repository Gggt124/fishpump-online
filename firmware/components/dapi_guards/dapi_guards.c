#include "dapi_guards.h"

#include <string.h>

bool dapi_wifi_forget_blocked(dapi_source_t src, const char *ssid, const char *connected_ssid)
{
    if (src != DAPI_SRC_TB || !ssid || !connected_ssid || connected_ssid[0] == '\0') {
        return false;
    }
    return strcmp(ssid, connected_ssid) == 0;
}

bool dapi_wifi_setauto_blocked(dapi_source_t src, int target_index, int connected_index)
{
    if (src != DAPI_SRC_TB) {
        return false;
    }
    return !(target_index >= 0 && connected_index >= 0 && target_index == connected_index);
}

bool dapi_wifi_save_password_blocked(dapi_source_t src, const char *password, const char *active_password)
{
    if (src != DAPI_SRC_TB || password == NULL || password[0] == '\0') {
        return false;
    }
    return active_password == NULL || strcmp(password, active_password) != 0;
}

bool dapi_polarity_write_blocked(dapi_source_t src, const char *requested, const char *stored)
{
    if (src != DAPI_SRC_TB || requested == NULL) {
        return false;
    }
    /* Only a real polarity value is a write attempt; anything else the method's own parser rejects. */
    if (strcmp(requested, "active_low") != 0 && strcmp(requested, "active_high") != 0) {
        return false;
    }
    return stored == NULL || strcmp(requested, stored) != 0;
}

const char *dapi_wifi_connect_refusal(dapi_source_t src, bool static_ip, bool staging_active, bool measuring)
{
    if (src != DAPI_SRC_TB) {
        return NULL;
    }
    if (static_ip) {
        return "local_only";
    }
    if (staging_active) {
        return "staging_active";
    }
    if (measuring) {
        return "measure_running";
    }
    return NULL;
}

bool dapi_wifi_save_empty_blocked(dapi_source_t src, const char *password, bool profile_exists)
{
    return src == DAPI_SRC_TB && (password == NULL || password[0] == '\0') && !profile_exists;
}
