#include "cooling_control.h"

#include "app_config.h"
#include "ds18b20.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "onewire_bus.h"
#include "relay_safe_init.h"
#include "telemetry_hub.h"
#include <math.h>
#include <string.h>

#define COOLING_CONTROL_TASK_STACK_BYTES 4096
#define COOLING_CONTROL_TASK_PRIORITY 4
#define COOLING_CONTROL_POLL_MS 2000
#define COOLING_CONTROL_CONVERSION_MS 750
#define COOLING_CONTROL_SENSOR_REDISCOVERY_MS 5000
#define COOLING_CONTROL_FAILED_READS_TO_FAULT 3
#define COOLING_CONTROL_SUCCESSES_TO_CLEAR_FAULT 2
#define MS_PER_SEC 1000LL

static const char *TAG = "cooling_control";

static SemaphoreHandle_t s_cooling_mutex;
static TaskHandle_t s_cooling_task;
static volatile bool s_task_stop;

static cooling_control_config_t s_config;
static bool s_initialized;
static bool s_config_valid;
static bool s_relay_configured;
static bool s_runtime_enabled = true;
static bool s_fault;
static bool s_temperature_valid;
static bool s_relay_energized;
static bool s_cooling_demand;
static bool s_auto_demand;
static bool s_lockout_active;
static float s_temperature_c;
static cooling_control_mode_t s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
static cooling_control_mode_t s_previous_mode = COOLING_CONTROL_MODE_FORCE_OFF;
static cooling_control_sensor_state_t s_sensor_state =
    COOLING_CONTROL_SENSOR_UNKNOWN;
static cooling_control_fault_code_t s_fault_code = COOLING_CONTROL_FAULT_NONE;
static cooling_control_blocked_reason_t s_blocked_reason =
    COOLING_CONTROL_BLOCKED_NONE;
static uint32_t s_failed_reads;
static uint32_t s_successful_reads;
static int64_t s_next_allowed_on_ms;
static int64_t s_test_remaining_ms;
static int64_t s_last_test_update_ms;
static int64_t s_next_sensor_discovery_ms;

static onewire_bus_handle_t s_bus;
static ds18b20_device_handle_t s_sensor;

static void deinit_sensor(void);

static bool ensure_mutex(void) {
  if (s_cooling_mutex) {
    return true;
  }
  s_cooling_mutex = xSemaphoreCreateMutex();
  return s_cooling_mutex != NULL;
}

static bool polarity_valid(cooling_control_relay_polarity_t polarity) {
  return polarity == COOLING_CONTROL_RELAY_ACTIVE_HIGH ||
         polarity == COOLING_CONTROL_RELAY_ACTIVE_LOW;
}

static bool mode_valid(cooling_control_mode_t mode) {
  return mode == COOLING_CONTROL_MODE_AUTO ||
         mode == COOLING_CONTROL_MODE_FORCE_OFF ||
         mode == COOLING_CONTROL_MODE_TEST_ON;
}

static bool gpio_valid_input(gpio_num_t gpio) {
  return GPIO_IS_VALID_GPIO(gpio);
}

static bool gpio_valid_output(gpio_num_t gpio) {
  return GPIO_IS_VALID_OUTPUT_GPIO(gpio);
}

static bool config_valid(const cooling_control_config_t *config) {
  if (!config) {
    return false;
  }
  if (!gpio_valid_input(config->ds18b20_gpio) ||
      !gpio_valid_output(config->cooling_relay_gpio) ||
      config->ds18b20_gpio == config->cooling_relay_gpio) {
    return false;
  }
  if (!polarity_valid(config->relay_polarity) || !mode_valid(config->mode)) {
    return false;
  }
  if (config->threshold_c_x10 < -550 || config->threshold_c_x10 > 1250 ||
      config->hysteresis_c_x10 <= 0 || config->hysteresis_c_x10 > 500 ||
      config->test_timeout_sec == 0 || config->test_timeout_sec > 3600 ||
      config->compressor_min_off_sec > 86400) {
    return false;
  }
  return true;
}

static int
relay_active_level_for_polarity(cooling_control_relay_polarity_t polarity) {
  return polarity == COOLING_CONTROL_RELAY_ACTIVE_HIGH ? 1 : 0;
}

static int
relay_inactive_level_for_polarity(cooling_control_relay_polarity_t polarity) {
  return relay_active_level_for_polarity(polarity) ? 0 : 1;
}

static esp_err_t
write_relay_level_for_config(const cooling_control_config_t *config,
                             bool energized) {
  if (!config || !gpio_valid_output(config->cooling_relay_gpio)) {
    return ESP_ERR_INVALID_ARG;
  }
  int level = energized
                  ? relay_active_level_for_polarity(config->relay_polarity)
                  : relay_inactive_level_for_polarity(config->relay_polarity);
  return gpio_set_level(config->cooling_relay_gpio, level);
}

static bool
force_relay_inactive_for_config(const cooling_control_config_t *config) {
  return write_relay_level_for_config(config, false) == ESP_OK;
}

static uint32_t seconds_until_deadline(int64_t now_ms, int64_t deadline_ms) {
  if (deadline_ms <= now_ms) {
    return 0;
  }
  int64_t remaining_ms = deadline_ms - now_ms;
  return (uint32_t)((remaining_ms + MS_PER_SEC - 1) / MS_PER_SEC);
}

static uint32_t lockout_remaining_locked(int64_t now_ms) {
  if (s_config.compressor_min_off_sec == 0 || s_next_allowed_on_ms <= now_ms) {
    return 0;
  }
  return seconds_until_deadline(now_ms, s_next_allowed_on_ms);
}

static uint32_t test_remaining_locked(int64_t now_ms) {
  (void)now_ms;
  if (s_mode != COOLING_CONTROL_MODE_TEST_ON || s_test_remaining_ms <= 0) {
    return 0;
  }
  return (uint32_t)((s_test_remaining_ms + MS_PER_SEC - 1) / MS_PER_SEC);
}

/**
 * Prepares cooling telemetry payload while holding s_cooling_mutex.
 * Does NOT publish directly; caller must give mutex then publish.
 */
static void get_cooling_telemetry_locked(int64_t now_ms, telemetry_event_t *out_evt) {
  if (out_evt) {
    out_evt->type = TELEMETRY_EVT_COOLING;
    out_evt->data.cooling.current_temp_c = s_temperature_c;
    out_evt->data.cooling.is_sensor_valid = s_temperature_valid;
    out_evt->data.cooling.cooling_relay_active = s_relay_energized;
    out_evt->data.cooling.lockout_remaining_sec = lockout_remaining_locked(now_ms);
  }
}

static void reset_runtime_state_locked(void) {
  s_initialized = false;
  s_config_valid = false;
  s_relay_configured = false;
  s_fault = false;
  s_temperature_valid = false;
  s_relay_energized = false;
  s_cooling_demand = false;
  s_auto_demand = false;
  s_lockout_active = false;
  s_temperature_c = 0.0f;
  s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_previous_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_sensor_state = COOLING_CONTROL_SENSOR_UNKNOWN;
  s_fault_code = COOLING_CONTROL_FAULT_NONE;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;
  s_failed_reads = 0;
  s_successful_reads = 0;
  s_next_allowed_on_ms = 0;
  s_test_remaining_ms = 0;
  s_last_test_update_ms = 0;
  s_next_sensor_discovery_ms = 0;
}

static void mark_config_invalid_locked(void) {
  s_initialized = true;
  s_config_valid = false;
  s_fault = true;
  s_temperature_valid = false;
  s_relay_energized = false;
  s_cooling_demand = false;
  s_lockout_active = false;
  s_sensor_state = COOLING_CONTROL_SENSOR_FAULT;
  s_fault_code = COOLING_CONTROL_FAULT_CONFIG_INVALID;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_CONFIG_INVALID;
  force_relay_inactive_for_config(&s_config);
}

static bool
configure_relay_gpio_locked(const cooling_control_config_t *config) {
  if (!config) {
    return false;
  }

  const relay_safe_channel_t channel = {
      .gpio = config->cooling_relay_gpio,
      .active_low =
          config->relay_polarity == COOLING_CONTROL_RELAY_ACTIVE_LOW,
      .name = "cooling_relay",
  };
  return relay_safe_configure(&channel, 1);
}

static void restart_lockout_locked(int64_t now_ms) {
  s_next_allowed_on_ms =
      now_ms + ((int64_t)s_config.compressor_min_off_sec * MS_PER_SEC);
}

static void set_relay_energized_locked(bool energized, int64_t now_ms) {
  bool was_energized = s_relay_energized;
  if (!s_relay_configured) {
    s_relay_energized = false;
    return;
  }
  if (write_relay_level_for_config(&s_config, energized) != ESP_OK) {
    write_relay_level_for_config(&s_config, false);
    s_relay_energized = false;
    s_fault = true;
    s_fault_code = COOLING_CONTROL_FAULT_CONFIG_INVALID;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_CONFIG_INVALID;
    return;
  }
  s_relay_energized = energized;
  if (was_energized && !energized) {
    restart_lockout_locked(now_ms);
  }
}

static bool temperature_in_supported_range(float temperature_c) {
  if (isnan(temperature_c) || isinf(temperature_c)) {
    return false;
  }
  if (temperature_c < -55.0f || temperature_c > 125.0f) {
    return false;
  }
  /* DS18B20 power-on default, treated as not-ready by the component contract.
   */
  if (fabsf(temperature_c - 85.0f) < 0.01f) {
    return false;
  }
  return true;
}

static bool cooling_sensor_ready_locked(void) {
  return s_temperature_valid && s_sensor_state == COOLING_CONTROL_SENSOR_OK &&
         !s_fault && s_fault_code == COOLING_CONTROL_FAULT_NONE;
}

static void update_control_locked(int64_t now_ms) {
  if (!s_initialized || !s_config_valid || !s_runtime_enabled) {
    s_cooling_demand = false;
    s_lockout_active = false;
    s_blocked_reason = s_runtime_enabled
                           ? COOLING_CONTROL_BLOCKED_CONFIG_INVALID
                           : COOLING_CONTROL_BLOCKED_FORCE_OFF;
    set_relay_energized_locked(false, now_ms);
    return;
  }

  if (s_mode == COOLING_CONTROL_MODE_TEST_ON) {
    int64_t elapsed_ms = now_ms - s_last_test_update_ms;
    if (elapsed_ms < 0) {
      elapsed_ms = 0;
    }
    if (lockout_remaining_locked(now_ms) == 0) {
      s_test_remaining_ms -= elapsed_ms;
    }
    if (s_test_remaining_ms <= 0) {
      s_test_remaining_ms = 0;
      s_mode = s_previous_mode;
    }
    s_last_test_update_ms = now_ms;
  }

  bool demand = false;
  bool may_energize = false;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;

  if (s_mode == COOLING_CONTROL_MODE_FORCE_OFF) {
    s_blocked_reason = COOLING_CONTROL_BLOCKED_FORCE_OFF;
    s_auto_demand = false;
  } else if (s_mode == COOLING_CONTROL_MODE_TEST_ON) {
    demand = true;
    may_energize = true;
    s_auto_demand = false;
  } else if (!cooling_sensor_ready_locked()) {
    s_blocked_reason = COOLING_CONTROL_BLOCKED_SENSOR_FAULT;
    s_auto_demand = false;
  } else {
    float threshold_c = (float)s_config.threshold_c_x10 / 10.0f;
    float off_c = threshold_c - ((float)s_config.hysteresis_c_x10 / 10.0f);
    if (s_auto_demand) {
      demand = s_temperature_c >= off_c;
    } else {
      demand = s_temperature_c >= threshold_c;
    }
    s_auto_demand = demand;
    may_energize = demand;
  }

  uint32_t lockout_remaining = lockout_remaining_locked(now_ms);
  s_lockout_active = lockout_remaining > 0;
  if (may_energize && s_lockout_active) {
    may_energize = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_COMPRESSOR_LOCKOUT;
  }

  s_cooling_demand = demand;
  if (!may_energize) {
    set_relay_energized_locked(false, now_ms);
    return;
  }
  set_relay_energized_locked(true, now_ms);
}

static void record_read_failure_locked(cooling_control_fault_code_t code) {
  s_failed_reads++;
  s_successful_reads = 0;
  if (s_sensor_state == COOLING_CONTROL_SENSOR_OK) {
    s_temperature_valid = false;
  }
  if (s_failed_reads >= COOLING_CONTROL_FAILED_READS_TO_FAULT) {
    s_fault = true;
    s_temperature_valid = false;
    s_sensor_state = COOLING_CONTROL_SENSOR_FAULT;
    s_fault_code = code;
  } else if (!s_temperature_valid) {
    s_sensor_state = COOLING_CONTROL_SENSOR_UNKNOWN;
    s_fault_code = COOLING_CONTROL_FAULT_NONE;
  }
}

static void record_read_success_locked(float temperature_c) {
  s_failed_reads = 0;
  s_successful_reads++;
  s_temperature_c = temperature_c;
  s_temperature_valid = true;
  if (s_fault) {
    if (s_successful_reads >= COOLING_CONTROL_SUCCESSES_TO_CLEAR_FAULT) {
      s_fault = false;
      s_fault_code = COOLING_CONTROL_FAULT_NONE;
      s_sensor_state = COOLING_CONTROL_SENSOR_OK;
    }
  } else {
    s_fault_code = COOLING_CONTROL_FAULT_NONE;
    s_sensor_state = COOLING_CONTROL_SENSOR_OK;
  }
}

static esp_err_t discover_sensor(void) {
  deinit_sensor();

  onewire_bus_config_t bus_config = {
      .bus_gpio_num = s_config.ds18b20_gpio,
  };
  onewire_bus_rmt_config_t rmt_config = {
      .max_rx_bytes = 10,
  };
  esp_err_t ret = onewire_new_bus_rmt(&bus_config, &rmt_config, &s_bus);
  if (ret != ESP_OK) {
    deinit_sensor();
    return ret;
  }

  onewire_device_iter_handle_t iter = NULL;
  ret = onewire_new_device_iter(s_bus, &iter);
  if (ret != ESP_OK) {
    deinit_sensor();
    return ret;
  }

  onewire_device_t next_onewire_device;
  ret = ESP_ERR_NOT_FOUND;
  while (onewire_device_iter_get_next(iter, &next_onewire_device) == ESP_OK) {
    ds18b20_config_t ds18b20_config = {};
    ret = ds18b20_new_device_from_enumeration(&next_onewire_device,
                                              &ds18b20_config, &s_sensor);
    if (ret == ESP_OK) {
      break;
    }
  }
  onewire_del_device_iter(iter);
  if (ret != ESP_OK) {
    deinit_sensor();
  }
  return ret;
}

static void deinit_sensor(void) {
  if (s_sensor) {
    ds18b20_del_device(s_sensor);
    s_sensor = NULL;
  }
  if (s_bus) {
    onewire_bus_del(s_bus);
    s_bus = NULL;
  }
}

static esp_err_t read_temperature_once(float *temperature_c) {
  if (!s_sensor || !temperature_c) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t ret = ds18b20_trigger_temperature_conversion(s_sensor);
  if (ret != ESP_OK) {
    return ret;
  }
  vTaskDelay(pdMS_TO_TICKS(COOLING_CONTROL_CONVERSION_MS));
  return ds18b20_get_temperature(s_sensor, temperature_c);
}

static esp_err_t read_temperature_with_recovery(float *temperature_c) {
  esp_err_t ret = read_temperature_once(temperature_c);
  if (ret == ESP_OK) {
    s_next_sensor_discovery_ms = 0;
    return ret;
  }

  if (s_task_stop) {
    return ret;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  if (s_next_sensor_discovery_ms > now_ms) {
    return ret;
  }

  s_next_sensor_discovery_ms = now_ms + COOLING_CONTROL_SENSOR_REDISCOVERY_MS;
  ESP_LOGW(TAG, "DS18B20 read failed on GPIO %d: %s; rediscovering",
           s_config.ds18b20_gpio, esp_err_to_name(ret));
  esp_err_t discover_ret = discover_sensor();
  if (discover_ret != ESP_OK) {
    ESP_LOGW(TAG, "DS18B20 rediscovery failed on GPIO %d: %s",
             s_config.ds18b20_gpio, esp_err_to_name(discover_ret));
    return ret;
  }

  ESP_LOGI(TAG, "DS18B20 rediscovered on GPIO %d", s_config.ds18b20_gpio);
  return read_temperature_once(temperature_c);
}

static void cooling_task(void *arg) {
  (void)arg;
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register cooling_task to TWDT: %s",
             esp_err_to_name(err));
  }
  while (!s_task_stop) {
    esp_task_wdt_reset();
    float temperature_c = 0.0f;
    esp_err_t ret = read_temperature_with_recovery(&temperature_c);

    telemetry_event_t evt;
    bool do_publish = false;

    if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      int64_t now_ms = esp_timer_get_time() / 1000;
      if (ret == ESP_OK && temperature_in_supported_range(temperature_c)) {
        record_read_success_locked(temperature_c);
      } else {
        cooling_control_fault_code_t code =
            ret == ESP_OK ? COOLING_CONTROL_FAULT_OUT_OF_RANGE
                          : COOLING_CONTROL_FAULT_READ_FAILED;
        record_read_failure_locked(code);
      }
      update_control_locked(now_ms);

      get_cooling_telemetry_locked(now_ms, &evt);
      do_publish = true;

      xSemaphoreGive(s_cooling_mutex);
    } else {
      ESP_LOGW(TAG, "cooling_task: failed to take mutex, skipping cycle");
    }

    /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
    if (do_publish) {
      telemetry_hub_publish(&evt);
    }

    uint32_t waited_ms = 0;
    while (!s_task_stop && waited_ms < COOLING_CONTROL_POLL_MS) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));
      waited_ms += 100;
    }
  }
  esp_err_t del_err = esp_task_wdt_delete(NULL);
  if (del_err != ESP_OK && del_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to remove cooling_task from TWDT: %s",
             esp_err_to_name(del_err));
  }
  s_cooling_task = NULL;
  vTaskDelete(NULL);
}

cooling_control_config_t cooling_control_default_config(void) {
  cooling_control_config_t config = {
      .ds18b20_gpio = APP_TEMPLATE_HW_DEFAULT_DS18B20_GPIO,
      .cooling_relay_gpio = APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_GPIO,
      .relay_polarity = APP_TEMPLATE_HW_DEFAULT_COOLING_RELAY_ACTIVE_LOW
                            ? COOLING_CONTROL_RELAY_ACTIVE_LOW
                            : COOLING_CONTROL_RELAY_ACTIVE_HIGH,
      .threshold_c_x10 = APP_TEMPLATE_COOLING_THRESHOLD_C_X10,
      .hysteresis_c_x10 = APP_TEMPLATE_COOLING_HYSTERESIS_C_X10,
      .auto_enable = APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT,
      .allow_runtime_control = true,
      .mode = COOLING_CONTROL_MODE_FORCE_OFF,
      .test_timeout_sec = APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC,
      .compressor_min_off_sec = APP_TEMPLATE_COOLING_MIN_OFF_SEC,
  };
  return config;
}

bool cooling_control_init(const cooling_control_config_t *config) {
  if (!config || !ensure_mutex()) {
    return false;
  }

  s_task_stop = true;
  for (int i = 0; s_cooling_task && i < 35; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (s_cooling_task != NULL) {
    ESP_LOGE(TAG,
             "Cannot re-init cooling control: previous task did not exit in time");
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  deinit_sensor();
  s_config = *config;
  reset_runtime_state_locked();
  s_runtime_enabled = config->allow_runtime_control;
  int64_t now_ms = esp_timer_get_time() / 1000;
  restart_lockout_locked(now_ms);

  telemetry_event_t init_evt;
  bool do_pub_init = false;

  if (!config_valid(&s_config)) {
    mark_config_invalid_locked();
    get_cooling_telemetry_locked(now_ms, &init_evt);
    do_pub_init = true;
    xSemaphoreGive(s_cooling_mutex);
    if (do_pub_init) {
      telemetry_hub_publish(&init_evt);
    }
    return false;
  }

  if (!configure_relay_gpio_locked(&s_config)) {
    mark_config_invalid_locked();
    get_cooling_telemetry_locked(now_ms, &init_evt);
    do_pub_init = true;
    xSemaphoreGive(s_cooling_mutex);
    if (do_pub_init) {
      telemetry_hub_publish(&init_evt);
    }
    return false;
  }
  s_relay_configured = true;
  s_initialized = true;
  s_config_valid = true;
  if (!s_runtime_enabled) {
    s_config.auto_enable = false;
    s_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
  }
  s_mode = s_config.auto_enable ? COOLING_CONTROL_MODE_AUTO
                                : COOLING_CONTROL_MODE_FORCE_OFF;
  s_previous_mode = s_mode == COOLING_CONTROL_MODE_TEST_ON
                        ? COOLING_CONTROL_MODE_FORCE_OFF
                        : s_mode;
  update_control_locked(now_ms);
  get_cooling_telemetry_locked(now_ms, &init_evt);
  do_pub_init = true;
  xSemaphoreGive(s_cooling_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_init) {
    telemetry_hub_publish(&init_evt);
  }

  if (!s_runtime_enabled) {
    return true;
  }

  esp_err_t ret = discover_sensor();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "DS18B20 discovery failed on GPIO %d: %s",
             s_config.ds18b20_gpio, esp_err_to_name(ret));
  }

  s_task_stop = false;
  /* Pin cooling control task to Core 1 */
  if (xTaskCreatePinnedToCore(cooling_task, "cooling_control",
                              COOLING_CONTROL_TASK_STACK_BYTES, NULL,
                              COOLING_CONTROL_TASK_PRIORITY, &s_cooling_task,
                              1) != pdPASS) {
    telemetry_event_t fail_evt;
    bool do_pub_fail = false;
    if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      mark_config_invalid_locked();
      get_cooling_telemetry_locked(now_ms, &fail_evt);
      do_pub_fail = true;
      xSemaphoreGive(s_cooling_mutex);
    }
    if (do_pub_fail) {
      telemetry_hub_publish(&fail_evt);
    }
    deinit_sensor();
    return false;
  }

  return true;
}

bool cooling_control_apply_config(const cooling_control_config_t *config) {
  if (!config || !ensure_mutex() || !config_valid(config)) {
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid ||
      config->ds18b20_gpio != s_config.ds18b20_gpio ||
      config->cooling_relay_gpio != s_config.cooling_relay_gpio ||
      config->relay_polarity != s_config.relay_polarity) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  uint32_t lockout_remaining_sec =
      seconds_until_deadline(now_ms, s_next_allowed_on_ms);
  s_config = *config;
  if (!s_runtime_enabled) {
    s_config.auto_enable = false;
    s_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
  }

  if (s_config.compressor_min_off_sec == 0) {
    s_next_allowed_on_ms = 0;
  } else if (lockout_remaining_sec > s_config.compressor_min_off_sec) {
    s_next_allowed_on_ms =
        now_ms + ((int64_t)s_config.compressor_min_off_sec * MS_PER_SEC);
  }

  s_mode = s_config.mode;
  s_previous_mode = s_mode == COOLING_CONTROL_MODE_TEST_ON
                        ? COOLING_CONTROL_MODE_FORCE_OFF
                        : s_mode;
  s_test_remaining_ms = 0;
  s_last_test_update_ms = 0;
  update_control_locked(now_ms);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(now_ms, &evt);
  xSemaphoreGive(s_cooling_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  telemetry_hub_publish(&evt);
  return true;
}

bool cooling_control_stop(void) {
  if (!ensure_mutex()) {
    return false;
  }
  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  int64_t now_ms = esp_timer_get_time() / 1000;
  set_relay_energized_locked(false, now_ms);
  s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_test_remaining_ms = 0;
  s_last_test_update_ms = 0;
  update_control_locked(now_ms);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(now_ms, &evt);
  xSemaphoreGive(s_cooling_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  telemetry_hub_publish(&evt);
  return true;
}

bool cooling_control_get_status(cooling_control_status_t *out) {
  if (!out || !ensure_mutex()) {
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  int64_t now_ms = esp_timer_get_time() / 1000;
  update_control_locked(now_ms);
  out->initialized = s_initialized;
  out->config_valid = s_config_valid;
  out->fault = s_fault;
  out->temperature_valid = s_temperature_valid;
  out->relay_energized = s_relay_energized;
  out->cooling_demand = s_cooling_demand;
  out->lockout_active = lockout_remaining_locked(now_ms) > 0;
  out->ds18b20_gpio = s_config.ds18b20_gpio;
  out->cooling_relay_gpio = s_config.cooling_relay_gpio;
  out->mode = s_mode;
  out->auto_enable = s_config.auto_enable;
  out->temperature_c = s_temperature_c;
  out->threshold_c_x10 = s_config.threshold_c_x10;
  out->hysteresis_c_x10 = s_config.hysteresis_c_x10;
  out->sensor_state = s_sensor_state;
  out->fault_code = s_fault_code;
  out->blocked_reason = s_blocked_reason;
  out->lockout_remaining_sec = lockout_remaining_locked(now_ms);
  out->test_remaining_sec = test_remaining_locked(now_ms);
  xSemaphoreGive(s_cooling_mutex);
  return true;
}

bool cooling_control_set_mode(cooling_control_mode_t mode) {
  if (!mode_valid(mode) || !ensure_mutex()) {
    return false;
  }
  if (mode == COOLING_CONTROL_MODE_TEST_ON) {
    return cooling_control_start_test();
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }
  if (!s_runtime_enabled) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }
  int64_t now_ms = esp_timer_get_time() / 1000;
  s_mode = mode;
  s_test_remaining_ms = 0;
  s_last_test_update_ms = 0;
  update_control_locked(now_ms);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(now_ms, &evt);
  xSemaphoreGive(s_cooling_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  telemetry_hub_publish(&evt);
  return true;
}

bool cooling_control_start_test(void) {
  if (!ensure_mutex()) {
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }
  if (!s_runtime_enabled) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }
  int64_t now_ms = esp_timer_get_time() / 1000;
  if (s_mode != COOLING_CONTROL_MODE_TEST_ON) {
    s_previous_mode = s_mode;
  }
  s_mode = COOLING_CONTROL_MODE_TEST_ON;
  s_test_remaining_ms = (int64_t)s_config.test_timeout_sec * MS_PER_SEC;
  s_last_test_update_ms = now_ms;
  update_control_locked(now_ms);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(now_ms, &evt);
  xSemaphoreGive(s_cooling_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  telemetry_hub_publish(&evt);
  return true;
}
