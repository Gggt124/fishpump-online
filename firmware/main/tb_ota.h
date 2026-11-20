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
