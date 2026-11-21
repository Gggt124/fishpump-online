#include "relay_safe_init.h"

#include "esp_err.h"
#include <stdint.h>

int relay_safe_inactive_level(bool active_low) {
  return active_low ? 1 : 0;
}

bool relay_safe_write_inactive(const relay_safe_channel_t *channel) {
  if (!channel || !channel->name ||
      !GPIO_IS_VALID_OUTPUT_GPIO(channel->gpio)) {
    return false;
  }

  return gpio_set_level(channel->gpio,
                        relay_safe_inactive_level(channel->active_low)) ==
         ESP_OK;
}

static bool relay_safe_validate_channels(
    const relay_safe_channel_t *channels, size_t count, uint64_t *mask_out) {
  if (!channels || count == 0 || !mask_out) {
    return false;
  }

  uint64_t mask = 0;
  for (size_t i = 0; i < count; i++) {
    if (!channels[i].name ||
        !GPIO_IS_VALID_OUTPUT_GPIO(channels[i].gpio)) {
      return false;
    }

    for (size_t j = 0; j < i; j++) {
      if (channels[i].gpio == channels[j].gpio) {
        return false;
      }
    }

    mask |= 1ULL << (uint32_t)channels[i].gpio;
  }

  *mask_out = mask;
  return true;
}

static void relay_safe_force_all_inactive(
    const relay_safe_channel_t *channels, size_t count) {
  if (!channels) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    relay_safe_write_inactive(&channels[i]);
  }
}

bool relay_safe_configure(const relay_safe_channel_t *channels, size_t count) {
  uint64_t mask = 0;
  if (!relay_safe_validate_channels(channels, count, &mask)) {
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    if (!relay_safe_write_inactive(&channels[i])) {
      relay_safe_force_all_inactive(channels, count);
      return false;
    }
  }

  gpio_config_t config = {
      .pin_bit_mask = mask,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&config) != ESP_OK) {
    relay_safe_force_all_inactive(channels, count);
    return false;
  }

  for (size_t i = 0; i < count; i++) {
    if (!relay_safe_write_inactive(&channels[i])) {
      relay_safe_force_all_inactive(channels, count);
      return false;
    }
  }

  return true;
}
