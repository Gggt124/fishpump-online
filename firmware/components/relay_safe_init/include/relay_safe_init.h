#pragma once

#include "driver/gpio.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  gpio_num_t gpio;
  bool active_low;
  const char *name;
} relay_safe_channel_t;

int relay_safe_inactive_level(bool active_low);
bool relay_safe_write_inactive(const relay_safe_channel_t *channel);
bool relay_safe_configure(const relay_safe_channel_t *channels, size_t count);

#ifdef __cplusplus
}
#endif
