#include "session.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "session";

#define NUM_SLOTS 4

typedef struct {
  char token[SESSION_TOKEN_LEN];
  char username[32];
  char ip[64];
  bool active;
  int64_t last_used;
} session_slot_t;

static session_slot_t s_slots[NUM_SLOTS];
static SemaphoreHandle_t s_session_mutex = NULL;

static bool secure_str_eq(const char *a, const char *b) {
  size_t len_a = strlen(a);
  size_t len_b = strlen(b);
  if (len_a != len_b || len_a == 0)
    return false;

  volatile uint8_t result = 0;
  for (size_t i = 0; i < len_a; i++) {
    result |= (a[i] ^ b[i]);
  }
  return result == 0;
}

bool session_init(void) {
  if (s_session_mutex == NULL) {
    s_session_mutex = xSemaphoreCreateMutex();
    if (s_session_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create session mutex");
      return false;
    }
  }

  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in init");
    return false;
  }
  for (int i = 0; i < NUM_SLOTS; i++) {
    s_slots[i].active = false;
    s_slots[i].last_used = 0;
    memset(s_slots[i].token, 0, sizeof(s_slots[i].token));
    memset(s_slots[i].username, 0, sizeof(s_slots[i].username));
    memset(s_slots[i].ip, 0, sizeof(s_slots[i].ip));
  }
  xSemaphoreGive(s_session_mutex);
  return true;
}

bool session_create(const char *username, const char *client_ip,
                    char token_out[SESSION_TOKEN_LEN]) {
  if (!s_session_mutex || !username || !client_ip || !token_out)
    return false;

  // Generate 32 random bytes (64 hex chars)
  uint8_t rand_bytes[32];
  esp_fill_random(rand_bytes, sizeof(rand_bytes));

  char hex_token[SESSION_TOKEN_LEN];
  for (int i = 0; i < 32; i++) {
    snprintf(&hex_token[i * 2], 3, "%02x", rand_bytes[i]);
  }

  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in create");
    return false;
  }

  int target_idx = -1;
  int64_t oldest_time = INT64_MAX;

  // Find empty slot or oldest slot
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (!s_slots[i].active) {
      target_idx = i;
      break;
    }
    if (s_slots[i].last_used < oldest_time) {
      oldest_time = s_slots[i].last_used;
      target_idx = i;
    }
  }

  if (target_idx >= 0 && target_idx < NUM_SLOTS) {
    strlcpy(s_slots[target_idx].token, hex_token,
            sizeof(s_slots[target_idx].token));

    strlcpy(s_slots[target_idx].username, username,
            sizeof(s_slots[target_idx].username));

    strlcpy(s_slots[target_idx].ip, client_ip,
            sizeof(s_slots[target_idx].ip));

    s_slots[target_idx].last_used = esp_timer_get_time();
    s_slots[target_idx].active = true;

    strlcpy(token_out, hex_token, SESSION_TOKEN_LEN);
    xSemaphoreGive(s_session_mutex);
    return true;
  }

  xSemaphoreGive(s_session_mutex);
  return false;
}

bool session_validate(const char *token, const char *client_ip) {
  if (!s_session_mutex || !token || !client_ip)
    return false;

  bool valid = false;
  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in validate");
    return false;
  }
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (s_slots[i].active && secure_str_eq(s_slots[i].token, token) &&
        strcmp(s_slots[i].ip, client_ip) == 0) {
      int64_t now = esp_timer_get_time();
      if ((now - s_slots[i].last_used) >
          (int64_t)SESSION_MAX_AGE_SEC * 1000000) {
        s_slots[i].active = false;
        valid = false;
      } else {
        s_slots[i].last_used = now;
        valid = true;
      }
      break;
    }
  }
  xSemaphoreGive(s_session_mutex);
  return valid;
}

void session_destroy(const char *token) {
  if (!s_session_mutex || !token)
    return;

  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in destroy");
    return;
  }
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (s_slots[i].active && secure_str_eq(s_slots[i].token, token)) {
      s_slots[i].active = false;
      break;
    }
  }
  xSemaphoreGive(s_session_mutex);
}

void session_invalidate_all(void) {
  if (!s_session_mutex)
    return;

  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in invalidate_all");
    return;
  }
  for (int i = 0; i < NUM_SLOTS; i++) {
    s_slots[i].active = false;
  }
  xSemaphoreGive(s_session_mutex);
}

void session_invalidate_others(const char *current_token) {
  if (!s_session_mutex)
    return;

  if (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire session mutex in invalidate_others");
    return;
  }
  for (int i = 0; i < NUM_SLOTS; i++) {
    if (s_slots[i].active) {
      if (current_token == NULL ||
          !secure_str_eq(s_slots[i].token, current_token)) {
        s_slots[i].active = false;
      }
    }
  }
  xSemaphoreGive(s_session_mutex);
}
