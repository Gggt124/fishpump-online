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

/* True when wifi.profiles.setauto must be refused. From ThingsBoard the auto-connect profile may only be set to
 * the profile of the network the board is connected to now: target_index is the resolved profile index (-1 =
 * clear, unknown ssid or out of range), connected_index the index of the connected network's saved profile
 * (-1 = not connected or not saved). Anything else could leave the board AP-only after a power cycle. */
bool dapi_wifi_setauto_blocked(dapi_source_t src, int target_index, int connected_index);

/* True when wifi.profiles.save must be refused: from ThingsBoard a non-empty password that differs from the
 * station's active password would overwrite the working credentials. An empty password keeps the saved one.
 * active_password NULL (could not be read) blocks, because safety cannot be proven. */
bool dapi_wifi_save_password_blocked(dapi_source_t src, const char *password, const char *active_password);

/* True when a relay polarity write must be refused: from ThingsBoard a polarity can only be re-sent equal to the
 * stored value (flipping it blind inverts the relay). requested NULL = key absent. A value that is not
 * "active_low" or "active_high" is not blocked here: the parser answers it with invalid_relay_polarity. */
bool dapi_polarity_write_blocked(dapi_source_t src, const char *requested, const char *stored);

/* Why wifi.connect from ThingsBoard must be refused, or NULL when nothing blocks it (always NULL for the local
 * web app). Checked in order: "local_only" (static IP fields), "staging_active", "measure_running". */
const char *dapi_wifi_connect_refusal(dapi_source_t src, bool static_ip, bool staging_active, bool measuring);

/* True when wifi.profiles.save must be refused: from ThingsBoard a new network (no saved profile) needs a
 * password, an empty or NULL one would store an open profile blind. */
bool dapi_wifi_save_empty_blocked(dapi_source_t src, const char *password, bool profile_exists);

#ifdef __cplusplus
}
#endif
