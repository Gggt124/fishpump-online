#pragma once

#include <stdbool.h>

#include "tb_link.h"

/*
 * ThingsBoard firmware OTA: reacts to the fw_* shared attributes, downloads the image in 16 KB chunks over MQTT,
 * checks its SHA-256, writes the next OTA slot and reboots into it. One update at a time (tb_ota_busy), and never
 * while the web upload holds the OTA lock.
 */

/* tb_link data hook (esp-mqtt task). Never blocks. */
void tb_ota_on_data(const tb_link_data_t *d);
/* True from the start of an update until its task has finished (a successful update never clears it). */
bool tb_ota_busy(void);

/* app_main, before web_server_start: resolves the boot state (pending version vs running version); on a fresh
 * image that is still PENDING_VERIFY it starts the 5 min rollback timer. */
void tb_ota_boot_init(void);
/* True while the boot state is VERIFY: app_main must not mark the image valid yet. */
bool tb_ota_boot_holds_valid(void);
/* tb_link session hook (tb_link task, after the subscriptions): reports the boot result, marks the image valid,
 * reports the current title and version and requests the shared attributes. */
void tb_ota_on_session(void);
