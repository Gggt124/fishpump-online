#include "cooling_control.h"
#include "cooling_sensor_pt100.h"
#include "cooling_pwm_output.h"
#include "cooling_rule_engine.h"

#include "app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "telemetry_hub.h"
#include <math.h>
#include <string.h>

#define COOLING_CONTROL_TASK_STACK_BYTES 4096
#define COOLING_CONTROL_TASK_PRIORITY 4
#define COOLING_CONTROL_LOOP_PERIOD_MS 1000
#define COOLING_CONTROL_FAILED_READS_TO_FAULT 3
#define COOLING_CONTROL_SUCCESSES_TO_CLEAR_FAULT 2

static const char *TAG = "cooling_control";

static SemaphoreHandle_t s_cooling_mutex = NULL;
static StaticSemaphore_t s_cooling_mutex_buf;
static TaskHandle_t s_cooling_task = NULL;
static volatile bool s_task_stop = false;

static cooling_control_config_t s_config;
static bool s_initialized = false;
static bool s_config_valid = false;
static bool s_runtime_enabled = true;
static bool s_fault = false;
static bool s_temperature_valid = false;
static float s_temperature_c = 0.0f;
static cooling_control_mode_t s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
static cooling_control_mode_t s_previous_mode = COOLING_CONTROL_MODE_FORCE_OFF;
static cooling_control_sensor_state_t s_sensor_state = COOLING_CONTROL_SENSOR_UNKNOWN;
static cooling_control_fault_code_t s_fault_code = COOLING_CONTROL_FAULT_NONE;
static cooling_control_blocked_reason_t s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;

static uint32_t s_failed_reads = 0;
static uint32_t s_successful_reads = 0;
static uint32_t s_lockout_remaining_sec = 0;
static uint32_t s_test_remaining_sec = 0;
static uint16_t s_test_target_mv = COOLING_RULE_MAX_OUTPUT_MV;

static uint16_t s_current_output_mv = 0;
static float s_current_duty_percent = 0.0f;
static bool s_cooling_demand = false;

static bool ensure_mutex(void) {
  if (s_cooling_mutex != NULL) {
    return true;
  }
  s_cooling_mutex = xSemaphoreCreateMutexStatic(&s_cooling_mutex_buf);
  return s_cooling_mutex != NULL;
}

static bool mode_valid(cooling_control_mode_t mode) {
  return mode == COOLING_CONTROL_MODE_AUTO ||
         mode == COOLING_CONTROL_MODE_FORCE_OFF ||
         mode == COOLING_CONTROL_MODE_TEST_ON;
}

static bool config_valid(const cooling_control_config_t *config) {
  if (!config) {
    return false;
  }
  if (!mode_valid(config->mode)) {
    return false;
  }
  gpio_num_t cs_pin = config->max31865_cs_gpio;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(cs_pin)) {
    cs_pin = config->ds18b20_gpio;
  }
  gpio_num_t pwm_pin = config->cooling_pwm_gpio;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(pwm_pin)) {
    pwm_pin = config->cooling_relay_gpio;
  }
  if (!GPIO_IS_VALID_OUTPUT_GPIO(cs_pin) || !GPIO_IS_VALID_OUTPUT_GPIO(pwm_pin)) {
    return false;
  }
  if (cs_pin == pwm_pin) {
    return false;
  }
  return true;
}

static bool temperature_in_supported_range(float temp_c) {
  if (!isfinite(temp_c)) {
    return false;
  }
  /* Supported range for PT100 in chiller/aquaculture system */
  return (temp_c >= -50.0f && temp_c <= 150.0f);
}

static bool cooling_sensor_ready_locked(void) {
  return s_temperature_valid &&
         s_sensor_state == COOLING_CONTROL_SENSOR_OK &&
         !s_fault &&
         s_fault_code == COOLING_CONTROL_FAULT_NONE;
}

static void trigger_lockout_locked(void) {
  uint32_t off_sec = s_config.settings.min_off_sec > 0
                         ? s_config.settings.min_off_sec
                         : (s_config.compressor_min_off_sec > 0
                                ? s_config.compressor_min_off_sec
                                : APP_TEMPLATE_COOLING_MIN_OFF_SEC);
  s_lockout_remaining_sec = off_sec;
  ESP_LOGI(TAG, "Compressor lockout triggered for %lu sec", (unsigned long)s_lockout_remaining_sec);
}

static void apply_pwm_output_locked(uint16_t target_mv) {
  uint16_t was_mv = s_current_output_mv;
  if (target_mv == 0) {
    /* Immediate fail-safe shutdown to protect compressor and enforce safety cutoff */
    cooling_pwm_output_set_millivolts(0);
  } else {
    /* Soft slew rate ramp to avoid mechanical/electrical inverter stress */
    cooling_pwm_output_step_towards(target_mv, COOLING_PWM_DEFAULT_MAX_SLEW_RATE_MV_PER_SEC);
  }
  s_current_output_mv = cooling_pwm_output_get_current_millivolts();
  s_current_duty_percent = cooling_pwm_output_get_duty_percent();

  /* If output transitions from > 0 mV to 0 mV, trigger lockout */
  if (was_mv > 0 && s_current_output_mv == 0) {
    trigger_lockout_locked();
  }
}

static void get_cooling_telemetry_locked(telemetry_event_t *out_evt) {
  if (out_evt) {
    out_evt->type = TELEMETRY_EVT_COOLING;
    out_evt->data.cooling.current_temp_c = s_temperature_c;
    out_evt->data.cooling.is_sensor_valid = s_temperature_valid;
    out_evt->data.cooling.output_mv = s_current_output_mv;
    out_evt->data.cooling.duty_percent = s_current_duty_percent;
    out_evt->data.cooling.lockout_remaining_sec = s_lockout_remaining_sec;
    out_evt->data.cooling.mode = (uint8_t)s_mode;
  }
}

static void reset_runtime_state_locked(void) {
  s_initialized = false;
  s_config_valid = false;
  s_fault = false;
  s_temperature_valid = false;
  s_temperature_c = 0.0f;
  s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_previous_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_sensor_state = COOLING_CONTROL_SENSOR_UNKNOWN;
  s_fault_code = COOLING_CONTROL_FAULT_NONE;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;
  s_failed_reads = 0;
  s_successful_reads = 0;
  s_lockout_remaining_sec = 0;
  s_test_remaining_sec = 0;
  s_test_target_mv = COOLING_RULE_MAX_OUTPUT_MV;
  s_current_output_mv = 0;
  s_current_duty_percent = 0.0f;
  s_cooling_demand = false;
}

static void update_control_locked(void) {
  if (!s_initialized || !s_config_valid || !s_runtime_enabled) {
    s_cooling_demand = false;
    s_blocked_reason = s_runtime_enabled
                           ? COOLING_CONTROL_BLOCKED_CONFIG_INVALID
                           : COOLING_CONTROL_BLOCKED_FORCE_OFF;
    apply_pwm_output_locked(0);
    return;
  }

  if (s_mode == COOLING_CONTROL_MODE_FORCE_OFF) {
    s_cooling_demand = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_FORCE_OFF;
    apply_pwm_output_locked(0);
    return;
  }

  if (s_mode == COOLING_CONTROL_MODE_TEST_ON) {
    s_cooling_demand = true;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;
    apply_pwm_output_locked(s_test_target_mv);
    return;
  }

  /* AUTO mode */
  if (!cooling_sensor_ready_locked()) {
    s_cooling_demand = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_SENSOR_FAULT;
    apply_pwm_output_locked(0);
    return;
  }

  uint16_t target_mv = cooling_evaluate_rule_curve(&s_config.settings, s_temperature_c);
  if (target_mv > 0) {
    s_cooling_demand = true;
    if (s_lockout_remaining_sec > 0) {
      s_blocked_reason = COOLING_CONTROL_BLOCKED_COMPRESSOR_LOCKOUT;
      apply_pwm_output_locked(0);
    } else {
      s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;
      apply_pwm_output_locked(target_mv);
    }
  } else {
    s_cooling_demand = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;
    apply_pwm_output_locked(0);
  }
}

static void cooling_task(void *arg) {
  (void)arg;
  esp_err_t err = esp_task_wdt_add(NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register cooling_task to TWDT: %s", esp_err_to_name(err));
  }

  while (!s_task_stop) {
    esp_task_wdt_reset();

    /* 1. Read PT100 temperature via cooling_sensor_pt100_read */
    float temp_c = 0.0f;
    uint8_t fault_status = 0;
    esp_err_t ret = cooling_sensor_pt100_read(&temp_c, &fault_status);

    telemetry_event_t evt;
    bool do_publish = false;

    if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      /* 2. Process sensor reading status & consecutive failures */
      if (ret == ESP_OK && temperature_in_supported_range(temp_c)) {
        s_failed_reads = 0;
        s_successful_reads++;
        s_temperature_c = temp_c;
        s_temperature_valid = true;
        if (s_fault) {
          if (s_successful_reads >= COOLING_CONTROL_SUCCESSES_TO_CLEAR_FAULT) {
            s_fault = false;
            s_fault_code = COOLING_CONTROL_FAULT_NONE;
            s_sensor_state = COOLING_CONTROL_SENSOR_OK;
            ESP_LOGI(TAG, "PT100 sensor fault cleared (temp=%.2f C)", temp_c);
          }
        } else {
          s_sensor_state = COOLING_CONTROL_SENSOR_OK;
          s_fault_code = COOLING_CONTROL_FAULT_NONE;
        }
      } else {
        s_failed_reads++;
        s_successful_reads = 0;
        if (s_failed_reads >= COOLING_CONTROL_FAILED_READS_TO_FAULT) {
          const bool entering_fault = !s_fault; /* log once per fault onset, not every second */
          s_fault = true;
          s_temperature_valid = false;
          s_sensor_state = COOLING_CONTROL_SENSOR_FAULT;
          s_fault_code = (ret == ESP_OK) ? COOLING_CONTROL_FAULT_OUT_OF_RANGE
                                         : COOLING_CONTROL_FAULT_READ_FAILED;
          if (entering_fault) {
            ESP_LOGW(TAG, "PT100 sensor fault: %u consecutive failures (code=%d)",
                     (unsigned int)s_failed_reads, s_fault_code);
          }
        }
      }

      /* 3. Decrement 1-sec countdown timers */
      if (s_lockout_remaining_sec > 0) {
        s_lockout_remaining_sec--;
      }
      if (s_mode == COOLING_CONTROL_MODE_TEST_ON) {
        if (s_test_remaining_sec > 0) {
          s_test_remaining_sec--;
        }
        if (s_test_remaining_sec == 0) {
          ESP_LOGI(TAG, "Cooling test timeout reached; reverting to previous mode (%d)", s_previous_mode);
          s_mode = s_previous_mode;
        }
      }

      /* 4. Evaluate control logic and update PWM output */
      update_control_locked();

      /* 5. Prepare telemetry */
      get_cooling_telemetry_locked(&evt);
      do_publish = true;

      xSemaphoreGive(s_cooling_mutex);
    } else {
      ESP_LOGW(TAG, "cooling_task: failed to take mutex, skipping cycle");
    }

    /* 6. Publish telemetry OUTSIDE mutex lock to prevent deadlock */
    if (do_publish) {
      telemetry_hub_publish(&evt);
    }

    /* Sleep 1000 ms in 100 ms increments to keep stop responsive and WDT fed */
    uint32_t waited_ms = 0;
    while (!s_task_stop && waited_ms < COOLING_CONTROL_LOOP_PERIOD_MS) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(100));
      waited_ms += 100;
    }
  }

  esp_err_t del_err = esp_task_wdt_delete(NULL);
  if (del_err != ESP_OK && del_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to remove cooling_task from TWDT: %s", esp_err_to_name(del_err));
  }
  s_cooling_task = NULL;
  vTaskDelete(NULL);
}

cooling_control_config_t cooling_control_default_config(void) {
  cooling_control_config_t config = {
      .ds18b20_gpio = APP_TEMPLATE_MAX31865_CS_GPIO,
      .cooling_relay_gpio = APP_TEMPLATE_COOLING_PWM_GPIO,
      .relay_polarity = COOLING_CONTROL_RELAY_ACTIVE_HIGH,
      .threshold_c_x10 = APP_TEMPLATE_COOLING_THRESHOLD_C_X10,
      .hysteresis_c_x10 = APP_TEMPLATE_COOLING_HYSTERESIS_C_X10,
      .auto_enable = APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT,
      .allow_runtime_control = true,
      .mode = COOLING_CONTROL_MODE_FORCE_OFF,
      .test_timeout_sec = APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC,
      .compressor_min_off_sec = APP_TEMPLATE_COOLING_MIN_OFF_SEC,
      .spi_host = APP_TEMPLATE_MAX31865_SPI_HOST,
      .max31865_cs_gpio = APP_TEMPLATE_MAX31865_CS_GPIO,
      .max31865_mosi_gpio = APP_TEMPLATE_MAX31865_MOSI_GPIO,
      .max31865_clk_gpio = APP_TEMPLATE_MAX31865_CLK_GPIO,
      .max31865_miso_gpio = APP_TEMPLATE_MAX31865_MISO_GPIO,
      .cooling_pwm_gpio = APP_TEMPLATE_COOLING_PWM_GPIO,
  };
  cooling_settings_set_defaults(&config.settings);
  return config;
}

bool cooling_control_init(const cooling_control_config_t *config) {
  if (!config || !ensure_mutex()) {
    return false;
  }

  /* Stop running task if any */
  s_task_stop = true;
  for (int i = 0; s_cooling_task && i < 35; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (s_cooling_task != NULL) {
    ESP_LOGE(TAG, "Cannot re-init cooling control: previous task did not exit in time");
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }

  cooling_sensor_pt100_deinit();
  cooling_pwm_output_deinit();

  reset_runtime_state_locked();

  if (!config_valid(config)) {
    ESP_LOGE(TAG, "Invalid cooling configuration supplied");
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }

  s_config = *config;

  /* Resolve pins with fallbacks */
  gpio_num_t cs_pin = config->max31865_cs_gpio;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(cs_pin)) {
    cs_pin = config->ds18b20_gpio;
  }
  gpio_num_t pwm_pin = config->cooling_pwm_gpio;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(pwm_pin)) {
    pwm_pin = config->cooling_relay_gpio;
  }
  spi_host_device_t host = config->spi_host ? config->spi_host : APP_TEMPLATE_MAX31865_SPI_HOST;
  gpio_num_t mosi_pin = GPIO_IS_VALID_OUTPUT_GPIO(config->max31865_mosi_gpio)
                            ? config->max31865_mosi_gpio
                            : APP_TEMPLATE_MAX31865_MOSI_GPIO;
  gpio_num_t clk_pin = GPIO_IS_VALID_OUTPUT_GPIO(config->max31865_clk_gpio)
                           ? config->max31865_clk_gpio
                           : APP_TEMPLATE_MAX31865_CLK_GPIO;
  gpio_num_t miso_pin = GPIO_IS_VALID_GPIO(config->max31865_miso_gpio)
                            ? config->max31865_miso_gpio
                            : APP_TEMPLATE_MAX31865_MISO_GPIO;

  s_config.max31865_cs_gpio = cs_pin;
  s_config.cooling_pwm_gpio = pwm_pin;
  s_config.ds18b20_gpio = cs_pin;
  s_config.cooling_relay_gpio = pwm_pin;
  s_config.spi_host = host;
  s_config.max31865_mosi_gpio = mosi_pin;
  s_config.max31865_clk_gpio = clk_pin;
  s_config.max31865_miso_gpio = miso_pin;

  if (!cooling_validate_settings(&s_config.settings)) {
    cooling_settings_set_defaults(&s_config.settings);
  }

  uint32_t min_off = s_config.settings.min_off_sec > 0
                         ? s_config.settings.min_off_sec
                         : (s_config.compressor_min_off_sec > 0
                                ? s_config.compressor_min_off_sec
                                : APP_TEMPLATE_COOLING_MIN_OFF_SEC);
  s_config.settings.min_off_sec = min_off;
  s_config.compressor_min_off_sec = min_off;

  /* Initialize MAX31865 SPI PT100 sensor */
  esp_err_t sensor_err = cooling_sensor_pt100_init(host, cs_pin, mosi_pin, clk_pin, miso_pin);
  if (sensor_err != ESP_OK) {
    ESP_LOGW(TAG, "MAX31865 PT100 init warning: %s (will retry during read loop)",
             esp_err_to_name(sensor_err));
  }

  /* Initialize LEDC PWM output driver on GPIO 8 */
  esp_err_t pwm_err = cooling_pwm_output_init(pwm_pin);
  if (pwm_err != ESP_OK) {
    ESP_LOGE(TAG, "LEDC PWM output init failed on GPIO %d: %s", pwm_pin, esp_err_to_name(pwm_err));
    cooling_sensor_pt100_deinit();
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }
  cooling_pwm_output_set_millivolts(0);

  s_runtime_enabled = config->allow_runtime_control;
  if (!s_runtime_enabled) {
    s_config.auto_enable = false;
    s_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
  }

  s_initialized = true;
  s_config_valid = true;
  s_mode = s_config.auto_enable ? COOLING_CONTROL_MODE_AUTO : COOLING_CONTROL_MODE_FORCE_OFF;
  s_previous_mode = s_mode;
  s_lockout_remaining_sec = min_off; /* Start with initial safety lockout on boot */

  update_control_locked();

  telemetry_event_t init_evt;
  get_cooling_telemetry_locked(&init_evt);
  xSemaphoreGive(s_cooling_mutex);

  telemetry_hub_publish(&init_evt);

  if (!s_runtime_enabled) {
    return true;
  }

  s_task_stop = false;
  if (xTaskCreatePinnedToCore(cooling_task, "cooling_control",
                              COOLING_CONTROL_TASK_STACK_BYTES, NULL,
                              COOLING_CONTROL_TASK_PRIORITY, &s_cooling_task,
                              1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start cooling_control task on Core 1");
    cooling_pwm_output_stop();
    cooling_pwm_output_deinit();
    cooling_sensor_pt100_deinit();
    return false;
  }

  ESP_LOGI(TAG, "Cooling control coordinator initialized (CS=%d, PWM=%d, MinOff=%lu s)",
           cs_pin, pwm_pin, (unsigned long)min_off);
  return true;
}

bool cooling_control_apply_config(const cooling_control_config_t *config) {
  if (!config || !ensure_mutex() || !config_valid(config)) {
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }

  s_config = *config;
  if (!cooling_validate_settings(&s_config.settings)) {
    cooling_settings_set_defaults(&s_config.settings);
  }
  if (!s_runtime_enabled) {
    s_config.auto_enable = false;
    s_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
  }

  uint32_t min_off = s_config.settings.min_off_sec > 0
                         ? s_config.settings.min_off_sec
                         : s_config.compressor_min_off_sec;
  s_config.settings.min_off_sec = min_off;
  s_config.compressor_min_off_sec = min_off;

  if (s_lockout_remaining_sec > min_off) {
    s_lockout_remaining_sec = min_off;
  }

  s_mode = s_config.mode;
  s_previous_mode = s_mode == COOLING_CONTROL_MODE_TEST_ON
                        ? COOLING_CONTROL_MODE_FORCE_OFF
                        : s_mode;
  s_test_remaining_sec = 0;

  if (s_cooling_task != NULL) {
    update_control_locked();
  } else if (s_mode == COOLING_CONTROL_MODE_FORCE_OFF) {
    s_cooling_demand = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_FORCE_OFF;
    apply_pwm_output_locked(0);
  }

  telemetry_event_t evt;
  get_cooling_telemetry_locked(&evt);
  xSemaphoreGive(s_cooling_mutex);

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

  s_mode = COOLING_CONTROL_MODE_FORCE_OFF;
  s_test_remaining_sec = 0;
  s_cooling_demand = false;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_FORCE_OFF;
  apply_pwm_output_locked(0);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(&evt);
  xSemaphoreGive(s_cooling_mutex);

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

  out->initialized = s_initialized;
  out->config_valid = s_config_valid;
  out->fault = s_fault;
  out->temperature_valid = s_temperature_valid;
  out->temperature_c = s_temperature_c;
  out->sensor_state = s_sensor_state;
  out->fault_code = s_fault_code;
  out->blocked_reason = s_blocked_reason;
  out->mode = s_mode;
  out->auto_enable = s_config.auto_enable;
  out->cooling_demand = s_cooling_demand;
  out->relay_energized = (s_current_output_mv > 0);
  out->lockout_active = (s_lockout_remaining_sec > 0);
  out->lockout_remaining_sec = s_lockout_remaining_sec;
  out->test_remaining_sec = s_test_remaining_sec;
  out->output_mv = s_current_output_mv;
  out->duty_percent = s_current_duty_percent;

  out->ds18b20_gpio = s_config.max31865_cs_gpio;
  out->cooling_relay_gpio = s_config.cooling_pwm_gpio;
  out->max31865_cs_gpio = s_config.max31865_cs_gpio;
  out->cooling_pwm_gpio = s_config.cooling_pwm_gpio;

  if (s_config.settings.point_count >= COOLING_MIN_RULE_POINTS &&
      s_config.settings.point_count <= COOLING_MAX_RULE_POINTS) {
    out->curve_type = s_config.settings.curve_type;
    out->point_count = s_config.settings.point_count;
    memcpy(out->points, s_config.settings.points, sizeof(out->points));
    out->threshold_c_x10 = s_config.settings.points[0].temp_c_x10;
    out->hysteresis_c_x10 = 10;
  } else {
    cooling_settings_t def;
    cooling_settings_set_defaults(&def);
    out->curve_type = def.curve_type;
    out->point_count = def.point_count;
    memcpy(out->points, def.points, sizeof(out->points));
    out->threshold_c_x10 = def.points[0].temp_c_x10;
    out->hysteresis_c_x10 = 10;
  }

  xSemaphoreGive(s_cooling_mutex);
  return true;
}

bool cooling_control_set_mode(cooling_control_mode_t mode) {
  if (!mode_valid(mode) || !ensure_mutex()) {
    return false;
  }
  if (mode == COOLING_CONTROL_MODE_TEST_ON) {
    return cooling_control_start_test(COOLING_RULE_MAX_OUTPUT_MV);
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid || !s_runtime_enabled) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }

  s_mode = mode;
  s_test_remaining_sec = 0;
  if (s_cooling_task != NULL) {
    update_control_locked();
  } else if (mode == COOLING_CONTROL_MODE_FORCE_OFF) {
    s_cooling_demand = false;
    s_blocked_reason = COOLING_CONTROL_BLOCKED_FORCE_OFF;
    apply_pwm_output_locked(0);
  }

  telemetry_event_t evt;
  get_cooling_telemetry_locked(&evt);
  xSemaphoreGive(s_cooling_mutex);

  telemetry_hub_publish(&evt);
  return true;
}

bool (cooling_control_start_test)(uint16_t test_mv) {
  if (!ensure_mutex()) {
    return false;
  }

  if (xSemaphoreTake(s_cooling_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized || !s_config_valid || !s_runtime_enabled) {
    xSemaphoreGive(s_cooling_mutex);
    return false;
  }

  if (test_mv > COOLING_RULE_MAX_OUTPUT_MV) {
    test_mv = COOLING_RULE_MAX_OUTPUT_MV;
  }

  if (s_mode != COOLING_CONTROL_MODE_TEST_ON) {
    s_previous_mode = s_mode;
  }
  s_mode = COOLING_CONTROL_MODE_TEST_ON;
  s_test_target_mv = test_mv;
  uint32_t tmo_sec = s_config.settings.test_timeout_sec > 0
                         ? s_config.settings.test_timeout_sec
                         : (s_config.test_timeout_sec > 0
                                ? s_config.test_timeout_sec
                                : APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC);
  s_test_remaining_sec = tmo_sec;
  s_cooling_demand = true;
  s_blocked_reason = COOLING_CONTROL_BLOCKED_NONE;

  apply_pwm_output_locked(test_mv);

  telemetry_event_t evt;
  get_cooling_telemetry_locked(&evt);
  xSemaphoreGive(s_cooling_mutex);

  telemetry_hub_publish(&evt);
  return true;
}

bool cooling_control_start_test_default(void) {
  return (cooling_control_start_test)(COOLING_RULE_MAX_OUTPUT_MV);
}
