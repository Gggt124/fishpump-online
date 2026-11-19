#pragma once

#include <stdbool.h>

#include "device_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decisions that only the remote (ThingsBoard) path takes. Pure functions, no I/O. */

/* True when wifi.profiles.forget must be refused: the call comes from ThingsBoard and
 * names the network the board is connected to now. connected_ssid is "" or NULL when
 * the board is not connected. The local web app is never blocked. */
bool dapi_wifi_forget_blocked(dapi_source_t src, const char *ssid, const char *connected_ssid);

#ifdef __cplusplus
}
#endif
