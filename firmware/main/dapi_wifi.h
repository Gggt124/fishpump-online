#pragma once

/* Largest JSON bodies the Wi-Fi POSTs accept (HTTP buffer sizes and the
 * max_params_bytes of the write methods). */
#define WIFI_CONNECT_BODY_BYTES 512
/* forget and setauto */
#define WIFI_PROFILE_BODY_BYTES 256
#define WIFI_SAVE_BODY_BYTES 384

void dapi_wifi_register(void);
