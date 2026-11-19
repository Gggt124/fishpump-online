#include "dapi_guards.h"

#include <string.h>

bool dapi_wifi_forget_blocked(dapi_source_t src, const char *ssid, const char *connected_ssid)
{
    if (src != DAPI_SRC_TB || !ssid || !connected_ssid || connected_ssid[0] == '\0') {
        return false;
    }
    return strcmp(ssid, connected_ssid) == 0;
}
