#include "app_main.h"
#include "app_config.h"
#include "cooling_control.h"
#include "dns_server.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "hardware_map.h"
#include "mdns.h"
#include "nvs_store.h"
#include "pump_control.h"
#include "relay_safe_init.h"
#include "session.h"
#include "telemetry_hub.h"
#include "tft_display.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "led_strip.h"
#include <inttypes.h>
#include <stdatomic.h>

#define STATUS_RGB_BRIGHTNESS_DEFAULT 32
#define STATUS_RGB_BRIGHTNESS_DIM 16

static led_strip_handle_t s_rgb_led = NULL;

static const char *TAG = "app_main";

static void log_ota_partition_state(void) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

  if (running) {
    ESP_LOGI(TAG, "[OTA] Running: %s @ 0x%08" PRIx32, running->label,
             running->address);
  }
  if (boot) {
    ESP_LOGI(TAG, "[OTA] Boot:    %s @ 0x%08" PRIx32, boot->label,
             boot->address);
  }
  if (next) {
    ESP_LOGI(TAG, "[OTA] Update:  %s @ 0x%08" PRIx32, next->label,
             next->address);
  }

  esp_ota_img_states_t ota_state;
  if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    const char *state_name = ota_state == ESP_OTA_IMG_NEW ? "NEW"
                             : ota_state == ESP_OTA_IMG_PENDING_VERIFY
                                 ? "PENDING_VERIFY"
                             : ota_state == ESP_OTA_IMG_VALID   ? "VALID"
                             : ota_state == ESP_OTA_IMG_INVALID ? "INVALID"
                             : ota_state == ESP_OTA_IMG_ABORTED ? "ABORTED"
                                                                : "UNDEFINED";
    ESP_LOGI(TAG, "[OTA] State:   %s (%d)", state_name, (int)ota_state);
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGW(TAG, "[OTA] Firmware is PENDING_VERIFY — will mark valid after "
                    "services start");
    }
  }
}

atomic_bool s_trigger_rollback = ATOMIC_VAR_INIT(false);
atomic_bool g_cancel_rollback_timer = ATOMIC_VAR_INIT(false);
#define s_cancel_rollback g_cancel_rollback_timer

static atomic_bool s_trigger_factory_reset = ATOMIC_VAR_INIT(false);

/** Stores the restart detail string read from NVS on boot. Persists in RAM for
 * the session. */
static char s_last_restart_detail[32] = {0};

const char *app_get_last_restart_detail(void) { return s_last_restart_detail; }

static volatile app_led_state_t s_led_state = LED_STATE_OFF;

app_led_state_t app_get_led_state(void) { return s_led_state; }

const char *app_led_state_name(app_led_state_t state) {
  switch (state) {
  case LED_STATE_OFF:
    return "off";
  case LED_STATE_ON:
    return "on";
  case LED_STATE_STAGING_PENDING:
    return "staging_pending";
  case LED_STATE_BTN_HOLD_SHORT:
    return "btn_hold_short";
  case LED_STATE_RECOVERY_AP:
    return "recovery_ap";
  case LED_STATE_FACTORY_RESET:
    return "factory_reset";
  default:
    return "unknown";
  }
}

static esp_timer_handle_t s_confirm_timer = NULL;
static esp_timer_handle_t s_wifi_timer = NULL;

static void confirm_timer_cb(void *arg) {
  (void)arg;
  atomic_store(&s_trigger_rollback, true);
  ESP_LOGW(TAG, "Rollback confirm timer expired!");
}

static void wifi_timer_cb(void *arg) {
  (void)arg;
  if (!wifi_manager_is_sta_connected()) {
    atomic_store(&s_trigger_rollback, true);
    ESP_LOGW(TAG, "Wi-Fi connection staging timeout! Triggering rollback.");
  }
}

static bool s_http_server_retry = false;
static int64_t s_last_board_diag_us = 0;
static uint32_t s_last_idle_runtime[portNUM_PROCESSORS] = {0};

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power_on";
  case ESP_RST_EXT:
    return "external_reset";
  case ESP_RST_SW:
    return "software_reset";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt_watchdog";
  case ESP_RST_TASK_WDT:
    return "task_watchdog";
  case ESP_RST_WDT:
    return "other_watchdog";
  case ESP_RST_BROWNOUT:
    return "brownout";
  default:
    return "other";
  }
}

static const char *
pump_settings_load_status_name(nvs_store_pump_settings_load_status_t status) {
  switch (status) {
  case NVS_STORE_PUMP_SETTINGS_LOADED:
    return "saved";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_MISSING:
    return "defaults-missing";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_INVALID:
    return "defaults-invalid";
  case NVS_STORE_PUMP_SETTINGS_DEFAULTS_ERROR:
  default:
    return "defaults-error";
  }
}

static const char *
hardware_map_load_status_name(nvs_store_hardware_map_load_status_t status) {
  switch (status) {
  case NVS_STORE_HARDWARE_MAP_LOADED:
    return "saved";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING:
    return "defaults-missing";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_INVALID:
    return "defaults-invalid";
  case NVS_STORE_HARDWARE_MAP_DEFAULTS_ERROR:
  default:
    return "defaults-error";
  }
}

static const char *pending_hardware_map_load_status_name(
    nvs_store_pending_hardware_map_load_status_t status) {
  switch (status) {
  case NVS_STORE_PENDING_HARDWARE_MAP_LOADED:
    return "saved";
  case NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND:
    return "not-found";
  case NVS_STORE_PENDING_HARDWARE_MAP_INVALID:
    return "invalid";
  case NVS_STORE_PENDING_HARDWARE_MAP_ERROR:
  default:
    return "error";
  }
}

static const char *cooling_settings_load_status_name(
    nvs_store_cooling_settings_load_status_t status) {
  switch (status) {
  case NVS_STORE_COOLING_SETTINGS_LOADED:
    return "saved";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_MISSING:
    return "defaults-missing";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_INVALID:
    return "defaults-invalid";
  case NVS_STORE_COOLING_SETTINGS_DEFAULTS_ERROR:
  default:
    return "defaults-error";
  }
}


static const char *cooling_mode_name(cooling_control_mode_t mode) {
  switch (mode) {
  case COOLING_CONTROL_MODE_AUTO:
    return "auto";
  case COOLING_CONTROL_MODE_TEST_ON:
    return "test_on";
  case COOLING_CONTROL_MODE_FORCE_OFF:
  default:
    return "force_off";
  }
}

static const char *
cooling_sensor_state_name(cooling_control_sensor_state_t state) {
  switch (state) {
  case COOLING_CONTROL_SENSOR_OK:
    return "ok";
  case COOLING_CONTROL_SENSOR_FAULT:
    return "fault";
  case COOLING_CONTROL_SENSOR_UNKNOWN:
  default:
    return "unknown";
  }
}

static const char *cooling_fault_name(cooling_control_fault_code_t fault) {
  switch (fault) {
  case COOLING_CONTROL_FAULT_READ_FAILED:
    return "read_failed";
  case COOLING_CONTROL_FAULT_OUT_OF_RANGE:
    return "out_of_range";
  case COOLING_CONTROL_FAULT_CONFIG_INVALID:
    return "config_invalid";
  case COOLING_CONTROL_FAULT_NONE:
  default:
    return "none";
  }
}

static const char *
cooling_blocked_reason_name(cooling_control_blocked_reason_t reason) {
  switch (reason) {
  case COOLING_CONTROL_BLOCKED_COMPRESSOR_LOCKOUT:
    return "compressor_lockout";
  case COOLING_CONTROL_BLOCKED_SENSOR_FAULT:
    return "sensor_fault";
  case COOLING_CONTROL_BLOCKED_FORCE_OFF:
    return "force_off";
  case COOLING_CONTROL_BLOCKED_CONFIG_INVALID:
    return "config_invalid";
  case COOLING_CONTROL_BLOCKED_NONE:
  default:
    return "none";
  }
}

static uint32_t cpu_load_x10(uint32_t idle_delta, uint32_t elapsed_us) {
  if (elapsed_us == 0 || idle_delta >= elapsed_us) {
    return 0;
  }
  return 1000U - (uint32_t)(((uint64_t)idle_delta * 1000U) / elapsed_us);
}

static void log_board_and_hardware_diagnostics(void) {
  int64_t now_us = esp_timer_get_time();
  uint32_t idle_runtime[portNUM_PROCESSORS] = {0};
  uint32_t core_load_x10[portNUM_PROCESSORS] = {0};
  uint32_t elapsed_us = 0;
  bool cpu_stats_available = false;
  bool cpu_sample_valid = false;
  uint32_t total_load_x10 = 0;

  for (int core = 0; core < portNUM_PROCESSORS; core++) {
#if configGENERATE_RUN_TIME_STATS
    idle_runtime[core] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(core);
    cpu_stats_available = true;
#else
    idle_runtime[core] = 0;
#endif
  }

  cpu_sample_valid = cpu_stats_available && (s_last_board_diag_us != 0);

  if (cpu_sample_valid) {
    elapsed_us = (uint32_t)(now_us - s_last_board_diag_us);
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
      uint32_t idle_delta = idle_runtime[core] - s_last_idle_runtime[core];
      core_load_x10[core] = cpu_load_x10(idle_delta, elapsed_us);
      total_load_x10 += core_load_x10[core];
    }
    total_load_x10 /= portNUM_PROCESSORS;
  }

  s_last_board_diag_us = now_us;
  for (int core = 0; core < portNUM_PROCESSORS; core++) {
    s_last_idle_runtime[core] = idle_runtime[core];
  }

  multi_heap_info_t heap_info = {0};
  heap_caps_get_info(&heap_info, MALLOC_CAP_INTERNAL);
  esp_err_t twdt_status = esp_task_wdt_status(NULL);

  ESP_LOGI(
      TAG,
      "[BOARD_DIAG] uptime_ms=%llu cpu_mhz=%d cpu_sample=%s cpu_window_ms=%lu "
      "cpu_load_pct=%lu.%lu core0_pct=%lu.%lu core1_pct=%lu.%lu tasks=%lu "
      "twdt_main=%s reset=%s heap_free=%lu heap_min=%lu heap_largest=%lu "
      "heap_alloc_blocks=%lu heap_free_blocks=%lu main_stack_hwm=%lu "
      "die_temp=unsupported external_temp_source=ds18b20",
      (unsigned long long)(now_us / 1000), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      !cpu_stats_available ? "unsupported"
                           : (cpu_sample_valid ? "valid" : "warming_up"),
      (unsigned long)(elapsed_us / 1000), (unsigned long)(total_load_x10 / 10),
      (unsigned long)(total_load_x10 % 10),
      (unsigned long)(core_load_x10[0] / 10),
      (unsigned long)(core_load_x10[0] % 10),
      (unsigned long)(core_load_x10[1] / 10),
      (unsigned long)(core_load_x10[1] % 10),
      (unsigned long)uxTaskGetNumberOfTasks(), esp_err_to_name(twdt_status),
      reset_reason_name(esp_reset_reason()),
      (unsigned long)heap_info.total_free_bytes,
      (unsigned long)heap_info.minimum_free_bytes,
      (unsigned long)heap_info.largest_free_block,
      (unsigned long)heap_info.allocated_blocks,
      (unsigned long)heap_info.free_blocks,
      (unsigned long)uxTaskGetStackHighWaterMark(NULL));

  pump_control_status_t pump = {0};
  if (pump_control_get_status(&pump)) {
    ESP_LOGI(
        TAG,
        "[PUMP_DIAG] init=%s fault=%s water_running=%s water_phase=%s "
        "water_cd=%lu wash_running=%s wash_phase=%s wash_cd=%lu",
        pump.initialized ? "true" : "false",
        pump.fault ? "true" : "false",
        pump.water.running ? "true" : "false",
        pump.water.phase == PUMP_CONTROL_PHASE_ON ? "ON" : (pump.water.phase == PUMP_CONTROL_PHASE_OFF ? "OFF" : "IDLE"),
        (unsigned long)pump.water.countdown_sec,
        pump.wash.running ? "true" : "false",
        pump.wash.phase == PUMP_CONTROL_PHASE_ON ? "ON" : (pump.wash.phase == PUMP_CONTROL_PHASE_OFF ? "OFF" : "IDLE"),
        (unsigned long)pump.wash.countdown_sec);
  } else {
    ESP_LOGW(TAG, "[PUMP_DIAG] status=unavailable");
  }


  cooling_control_status_t cooling = {0};
  if (cooling_control_get_status(&cooling)) {
    ESP_LOGI(
        TAG,
        "[COOLING_DIAG] init=%s config=%s fault=%s sensor=%s fault_code=%s "
        "temp_valid=%s temp_c=%.2f ds18b20_gpio=%d mode=%s auto=%s demand=%s "
        "relay_gpio=%d relay=%s blocked=%s lockout=%s lockout_sec=%lu "
        "test_sec=%lu threshold_x10=%ld hysteresis_x10=%ld",
        cooling.initialized ? "true" : "false",
        cooling.config_valid ? "true" : "false",
        cooling.fault ? "true" : "false",
        cooling_sensor_state_name(cooling.sensor_state),
        cooling_fault_name(cooling.fault_code),
        cooling.temperature_valid ? "true" : "false", cooling.temperature_c,
        cooling.ds18b20_gpio, cooling_mode_name(cooling.mode),
        cooling.auto_enable ? "true" : "false",
        cooling.cooling_demand ? "true" : "false", cooling.cooling_relay_gpio,
        cooling.relay_energized ? "on" : "off",
        cooling_blocked_reason_name(cooling.blocked_reason),
        cooling.lockout_active ? "true" : "false",
        (unsigned long)cooling.lockout_remaining_sec,
        (unsigned long)cooling.test_remaining_sec,
        (long)cooling.threshold_c_x10, (long)cooling.hysteresis_c_x10);
  } else {
    ESP_LOGW(TAG, "[COOLING_DIAG] status=unavailable");
  }
}



static bool promote_pending_hardware_map_on_boot(
    hardware_map_t *active_hardware_map,
    nvs_store_hardware_map_load_status_t *hardware_map_status, bool *promoted) {
  if (promoted) {
    *promoted = false;
  }
  if (!active_hardware_map || !hardware_map_status) {
    return false;
  }

  hardware_map_t pending_hardware_map;
  nvs_store_pending_hardware_map_load_status_t pending_status =
      nvs_store_load_pending_hardware_map(&pending_hardware_map);
  if (pending_status == NVS_STORE_PENDING_HARDWARE_MAP_NOT_FOUND) {
    return true;
  }
  if (pending_status != NVS_STORE_PENDING_HARDWARE_MAP_LOADED) {
    ESP_LOGW(TAG, "Pending hardware map %s; keeping active map",
             pending_hardware_map_load_status_name(pending_status));
    return false;
  }
  if (hardware_map_equal(active_hardware_map, &pending_hardware_map)) {
    if (!nvs_store_clear_pending_hardware_map()) {
      ESP_LOGE(TAG, "Pending hardware map already active, but clear failed");
      return false;
    }
    ESP_LOGI(TAG, "Pending hardware map already active; cleared pending state");
    return true;
  }
  if (!nvs_store_save_hardware_map(&pending_hardware_map)) {
    ESP_LOGE(TAG, "Failed to promote pending hardware map to active map");
    return false;
  }
  if (!nvs_store_clear_pending_hardware_map()) {
    *active_hardware_map = pending_hardware_map;
    *hardware_map_status = NVS_STORE_HARDWARE_MAP_LOADED;
    if (promoted) {
      *promoted = true;
    }
    ESP_LOGE(TAG, "Pending hardware map promoted, but pending clear failed");
    return false;
  }

  *active_hardware_map = pending_hardware_map;
  *hardware_map_status = NVS_STORE_HARDWARE_MAP_LOADED;
  if (promoted) {
    *promoted = true;
  }
  ESP_LOGI(TAG, "Pending hardware map promoted to active map on boot");
  return true;
}



static cooling_control_relay_polarity_t
cooling_relay_polarity_from_hardware(hardware_relay_polarity_t polarity) {
  return polarity == HARDWARE_RELAY_ACTIVE_LOW
             ? COOLING_CONTROL_RELAY_ACTIVE_LOW
             : COOLING_CONTROL_RELAY_ACTIVE_HIGH;
}

static cooling_control_mode_t
cooling_mode_from_hardware(hardware_cooling_mode_t mode) {
  switch (mode) {
  case HARDWARE_COOLING_MODE_AUTO:
    return COOLING_CONTROL_MODE_AUTO;
  case HARDWARE_COOLING_MODE_TEST_ON:
    return COOLING_CONTROL_MODE_TEST_ON;
  case HARDWARE_COOLING_MODE_FORCE_OFF:
  default:
    return COOLING_CONTROL_MODE_FORCE_OFF;
  }
}

static void
apply_cooling_settings_to_config(cooling_control_config_t *config,
                                 const nvs_store_cooling_settings_t *settings,
                                 const hardware_map_t *hardware_map) {
  if (!config || !settings || !hardware_map) {
    return;
  }

  config->ds18b20_gpio = hardware_map->ds18b20_data_gpio;
  config->cooling_relay_gpio = hardware_map->cooling_relay_gpio;
  config->relay_polarity =
      cooling_relay_polarity_from_hardware(settings->relay_polarity);
  config->threshold_c_x10 = settings->threshold_c_x10;
  config->hysteresis_c_x10 = settings->hysteresis_c_x10;
  config->auto_enable = settings->auto_enable;
  config->mode = cooling_mode_from_hardware(settings->mode);
  config->test_timeout_sec = settings->test_timeout_sec;
  config->compressor_min_off_sec = settings->compressor_min_off_sec;
}

static void update_status_leds(app_led_state_t state, int blink_phase,
                               int ext_level) {
  // Define colors for each state
  uint8_t r = 0, g = 0, b = 0;

  switch (state) {
  case LED_STATE_OFF:
    r = 0;
    g = 0;
    b = 0;
    break;
  case LED_STATE_ON:
    r = 0;
    g = STATUS_RGB_BRIGHTNESS_DEFAULT;
    b = 0; // Green
    break;
  case LED_STATE_STAGING_PENDING:
    r = STATUS_RGB_BRIGHTNESS_DEFAULT;
    g = STATUS_RGB_BRIGHTNESS_DIM;
    b = 0; // Orange/Amber
    break;
  case LED_STATE_BTN_HOLD_SHORT:
    r = STATUS_RGB_BRIGHTNESS_DIM;
    g = STATUS_RGB_BRIGHTNESS_DIM;
    b = STATUS_RGB_BRIGHTNESS_DIM; // White
    break;
  case LED_STATE_RECOVERY_AP:
    r = 0;
    g = 0;
    b = STATUS_RGB_BRIGHTNESS_DEFAULT; // Blue
    break;
  case LED_STATE_FACTORY_RESET:
    r = STATUS_RGB_BRIGHTNESS_DEFAULT;
    g = 0;
    b = 0; // Red
    break;
  }

  // Apply blink phase to RGB LED if state calls for blinking
  if (state == LED_STATE_RECOVERY_AP || state == LED_STATE_FACTORY_RESET ||
      state == LED_STATE_STAGING_PENDING) {
    if (blink_phase == 0) {
      r = 0;
      g = 0;
      b = 0;
    }
  }

  if (s_rgb_led) {
    led_strip_set_pixel(s_rgb_led, 0, r, g, b);
    led_strip_refresh(s_rgb_led);
  }

  // External LED behaves exactly as before
  gpio_set_level(APP_CONFIG_EXT_LED_GPIO, ext_level);
}

void relay_safe_init_pins(void) {
  hardware_map_t defaults = hardware_map_defaults();
  relay_safe_channel_t channels[] = {
      {.gpio = defaults.pump_relay1_gpio, .active_low = true, .name = "pump1"},
      {.gpio = defaults.pump_relay2_gpio, .active_low = true, .name = "pump2"},
      {.gpio = defaults.cooling_relay_gpio, .active_low = true, .name = "cooling"},
  };
  relay_safe_configure(channels, sizeof(channels) / sizeof(channels[0]));
}

static bool pump_control_start_task(const pump_control_init_context_t *context) {
  return pump_control_init(context);
}

static bool cooling_control_start_task(const cooling_control_config_t *config) {
  return cooling_control_init(config);
}

static void hardware_ui_task(void *pvParameters) {
  (void)pvParameters;

  // Initialize button GPIOs as inputs with internal pull-ups
  gpio_config_t btn_cfg = {
      .pin_bit_mask = (1ULL << APP_CONFIG_BOOT_BTN_GPIO) |
                      (1ULL << APP_CONFIG_EXT_BTN_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&btn_cfg);

  // Initialize EXT LED as output
  gpio_config_t ext_led_cfg = {
      .pin_bit_mask = (1ULL << APP_CONFIG_EXT_LED_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&ext_led_cfg);
  gpio_set_level(APP_CONFIG_EXT_LED_GPIO, 0);

  // Initialize RGB LED via RMT
  led_strip_config_t strip_cfg = {
      .strip_gpio_num = APP_CONFIG_RGB_LED_GPIO,
      .max_leds = 1,
      .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
      .led_model = LED_MODEL_WS2812,
      .flags.invert_out = false,
  };
  led_strip_rmt_config_t rmt_cfg = {
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
      .flags.with_dma = false,
  };
  esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_rgb_led);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG,
             "RGB LED init FAILED (GPIO %d): %s — s_rgb_led will be NULL, no "
             "RGB output",
             APP_CONFIG_RGB_LED_GPIO, esp_err_to_name(ret));
    s_rgb_led =
        NULL; // explicit — already NULL from calloc fail, but be defensive
  } else {
    ESP_LOGI(TAG, "RGB LED init OK (GPIO %d, WS2812, RMT)",
             APP_CONFIG_RGB_LED_GPIO);
    led_strip_clear(s_rgb_led);
    ESP_LOGI(TAG, "RGB LED cleared (all pixels off)");
  }

  // Initial read to set up bootloader veto
  bool boot_btn_veto = (gpio_get_level(APP_CONFIG_BOOT_BTN_GPIO) == 0);
  bool ext_btn_veto = (gpio_get_level(APP_CONFIG_EXT_BTN_GPIO) == 0);

  if (boot_btn_veto) {
    ESP_LOGW(TAG, "Boot button active on boot; vetoing holds until released");
  }
  if (ext_btn_veto) {
    ESP_LOGW(TAG,
             "External button active on boot; vetoing holds until released");
  }

  uint32_t press_duration_ms = 0;
  bool recovery_ap_triggered = false;
  bool factory_reset_triggered = false;
  uint32_t release_ticks = 0;

  while (1) {
    int boot_level = gpio_get_level(APP_CONFIG_BOOT_BTN_GPIO);
    int ext_level = gpio_get_level(APP_CONFIG_EXT_BTN_GPIO);

    // Clear vetoes when buttons are released (level goes HIGH)
    if (boot_level == 1) {
      boot_btn_veto = false;
    }
    if (ext_level == 1) {
      ext_btn_veto = false;
    }

    bool boot_pressed = (boot_level == 0) && !boot_btn_veto;
    bool ext_pressed = (ext_level == 0) && !ext_btn_veto;

    // Mutual exclusion: if both are low, ignore both
    if (boot_pressed && ext_pressed) {
      boot_pressed = false;
      ext_pressed = false;
    }

    bool any_pressed = boot_pressed || ext_pressed;

    if (any_pressed) {
      press_duration_ms += 50;
      release_ticks = 0;

      // Wake TFT backlight on any button press
      tft_display_reset_idle_timer();

      if (press_duration_ms < 2000) {
        s_led_state = LED_STATE_BTN_HOLD_SHORT;
        update_status_leds(s_led_state, 1, 1);
      } else if (press_duration_ms < 5000) {
        if (!recovery_ap_triggered) {
          wifi_manager_start_recovery_ap();
          recovery_ap_triggered = true;
          ESP_LOGI(TAG, "Recovery AP triggered by button hold");
        }
        s_led_state = LED_STATE_RECOVERY_AP;
        int blink = (((press_duration_ms / 500) % 2) == 0);
        update_status_leds(s_led_state, blink, blink);
      } else {
        if (!factory_reset_triggered) {
          atomic_store(&s_trigger_factory_reset, true);
          factory_reset_triggered = true;
          ESP_LOGI(TAG, "Factory reset triggered by button hold");
        }
        s_led_state = LED_STATE_FACTORY_RESET;
        int blink = (((press_duration_ms / 100) % 2) == 0);
        update_status_leds(s_led_state, blink, blink);
      }
    } else {
      press_duration_ms = 0;
      recovery_ap_triggered = false;
      factory_reset_triggered = false;
      release_ticks++;

      /* PM-7: Cache staging type; refresh from NVS at most 1 Hz to avoid
       * blocking UI task during concurrent NVS writes (rollback/factory reset).
       */
      static uint8_t cached_stg_type = 0;
      static uint32_t last_stg_check_ms = 0;
      uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
      if (now_ms - last_stg_check_ms >= 1000) {
        nvs_store_get_staging_type(&cached_stg_type);
        last_stg_check_ms = now_ms;
      }
      if (cached_stg_type > 0) {
        s_led_state = LED_STATE_STAGING_PENDING;
        uint32_t phase = release_ticks % 20;
        int blink = 0;
        if (phase < 2) {
          blink = 1;
        } else if (phase < 4) {
          blink = 0;
        } else if (phase < 6) {
          blink = 1;
        } else {
          blink = 0;
        }
        update_status_leds(s_led_state, blink, blink);
      } else {
        if (wifi_manager_is_ap_active()) {
          s_led_state = LED_STATE_ON;
          update_status_leds(s_led_state, 1, 1);
        } else {
          s_led_state = LED_STATE_OFF;
          update_status_leds(s_led_state, 0, 0);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void app_main(void) {
  /* 0. Safely initialize all relay output GPIOs to inactive state before any driver init */
  relay_safe_init_pins();

  /* ── Production log-level filter ─────────────────────────────────────────
   * Global floor = WARN  (matches sdkconfig.defaults CONFIG_LOG_DEFAULT_LEVEL)
   * Noisy 3rd-party components stay at WARN.
   * Application modules raised to INFO for field observability.
   * ESP_LOGD/LOGV are compile-time stripped (CONFIG_LOG_MAXIMUM_LEVEL=INFO).
   * Runtime adjustment: esp_log_level_set("<tag>", ESP_LOG_INFO) — no reflash.
   * ─────────────────────────────────────────────────────────────────────── */
  esp_log_level_set(
      "*", ESP_LOG_WARN); /* 3rd-party: Wi-Fi, LwIP, HTTPD stay quiet */

  /* Application modules — INFO for state transitions and field diagnostics */
  esp_log_level_set("app_main", ESP_LOG_INFO);
  esp_log_level_set("pump_control", ESP_LOG_INFO);
  esp_log_level_set("cooling_control", ESP_LOG_INFO);
  esp_log_level_set("nvs_store", ESP_LOG_INFO);
  esp_log_level_set("web_server", ESP_LOG_INFO);
  esp_log_level_set("TFT_DISPLAY", ESP_LOG_INFO);
  esp_log_level_set("wifi_mgr", ESP_LOG_INFO);
  esp_log_level_set("telemetry_hub", ESP_LOG_INFO);
  esp_log_level_set("dns_server",
                    ESP_LOG_WARN); /* DNS very chatty — WARN only */
  esp_log_level_set("session",
                    ESP_LOG_WARN); /* session tokens — security: WARN only */

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  %s %s", APP_TEMPLATE_NAME, APP_TEMPLATE_FIRMWARE_VERSION);
  ESP_LOGI(TAG, "  %s", APP_TEMPLATE_PHASE_LABEL);
  ESP_LOGI(TAG, "  ESP32-S3 WROOM-1-N16R8, 240MHz, 16MB Flash, 8MB PSRAM");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "Last reset reason: %s (%d)",
           reset_reason_name(esp_reset_reason()), esp_reset_reason());

  /* 1. Initialize NVS storage */
  if (!nvs_store_init()) {
    ESP_LOGE(TAG, "NVS init failed");
    return;
  }
  ESP_LOGI(TAG, "NVS initialized");

  /* Initialize telemetry hub observer */
  esp_err_t telem_ret = telemetry_hub_init();
  if (telem_ret != ESP_OK) {
    ESP_LOGE(TAG, "Telemetry hub init failed: %s", esp_err_to_name(telem_ret));
  } else {
    ESP_LOGI(TAG, "Telemetry hub initialized");
  }

  /* Read and clear the "last words" restart detail from NVS */
  if (nvs_store_pop_restart_detail(s_last_restart_detail,
                                   sizeof(s_last_restart_detail))) {
    if (s_last_restart_detail[0]) {
      ESP_LOGW(TAG, "Last restart detail: %s", s_last_restart_detail);
    } else {
      ESP_LOGI(TAG, "Last restart detail: none");
    }
  } else {
    ESP_LOGW(TAG, "Failed to read restart detail from NVS");
  }

  log_ota_partition_state();

  uint8_t stg_type = 0;
  nvs_store_get_staging_type(&stg_type);
  if (stg_type > 0) {
    ESP_LOGW(
        TAG,
        "Staging configuration active (type=%d). Starting rollback timers.",
        stg_type);

    esp_timer_create_args_t confirm_args = {.callback = &confirm_timer_cb,
                                            .name = "confirm_timer"};
    if (esp_timer_create(&confirm_args, &s_confirm_timer) == ESP_OK) {
      esp_timer_start_once(s_confirm_timer,
                           (uint64_t)APP_CONFIG_ROLLBACK_CONFIRM_TIMEOUT_MS *
                               1000);
      ESP_LOGI(TAG, "Rollback confirmation timer started for %d ms",
               APP_CONFIG_ROLLBACK_CONFIRM_TIMEOUT_MS);
    }

    if (stg_type == 1) {
      esp_timer_create_args_t wifi_args = {.callback = &wifi_timer_cb,
                                           .name = "wifi_timer"};
      if (esp_timer_create(&wifi_args, &s_wifi_timer) == ESP_OK) {
        esp_timer_start_once(
            s_wifi_timer, (uint64_t)APP_CONFIG_ROLLBACK_WIFI_TIMEOUT_MS * 1000);
        ESP_LOGI(TAG, "Rollback Wi-Fi validation timer started for %d ms",
                 APP_CONFIG_ROLLBACK_WIFI_TIMEOUT_MS);
      }
    }
  }

  /* Initialize TFT display */
  if (tft_display_init() == ESP_OK) {
    uint8_t dim_pct = tft_display_get_idle_dim_percent();
    nvs_store_load_display_settings(&dim_pct);
    tft_display_set_idle_dim_percent(dim_pct);
    ESP_LOGI(TAG, "TFT idle dim loaded: %u%%", dim_pct);

    tft_clear(TFT_COLOR_BLACK);
    // Draw centered high-contrast "Booting..." splash screen
    tft_draw_string_x2(80, 60, "Booting...", TFT_COLOR_GREEN, TFT_COLOR_BLACK);
    tft_draw_string(44, 110, APP_TEMPLATE_NAME, TFT_COLOR_WHITE,
                    TFT_COLOR_BLACK);
    tft_draw_string(136, 135, APP_TEMPLATE_FIRMWARE_VERSION, TFT_COLOR_GRAY,
                    TFT_COLOR_BLACK);
  }

  hardware_map_t active_hardware_map;
  nvs_store_hardware_map_load_status_t hardware_map_status =
      nvs_store_load_hardware_map(&active_hardware_map);
  bool hardware_map_promoted = false;
  bool hardware_map_promotion_ok = promote_pending_hardware_map_on_boot(
      &active_hardware_map, &hardware_map_status, &hardware_map_promoted);
  bool hardware_reboot_required = false;
  bool hardware_reboot_status_ok =
      nvs_store_hardware_reboot_required(&hardware_reboot_required);

  nvs_store_pump_settings_t pump_settings;
  nvs_store_pump_settings_load_status_t pump_settings_status =
      nvs_store_load_pump_settings(&pump_settings);
  nvs_store_cooling_settings_t cooling_settings;
  nvs_store_cooling_settings_load_status_t cooling_settings_status =
      nvs_store_load_cooling_settings(&cooling_settings);

  cooling_control_config_t cooling_config = cooling_control_default_config();
  apply_cooling_settings_to_config(&cooling_config, &cooling_settings,
                                   &active_hardware_map);
  if (cooling_settings_status != NVS_STORE_COOLING_SETTINGS_LOADED) {
    cooling_config.auto_enable = false;
    cooling_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
    ESP_LOGW(TAG,
             "Cooling settings %s; forcing cooling relay off for this boot",
             cooling_settings_load_status_name(cooling_settings_status));
  }

  bool allow_auto_start =
      (pump_settings.auto_start || pump_settings.wash_auto_start);
  bool active_hardware_map_valid =
      hardware_map_validate(&active_hardware_map);
  bool hardware_map_safe =
      active_hardware_map_valid &&
      (hardware_map_status == NVS_STORE_HARDWARE_MAP_LOADED ||
       hardware_map_status == NVS_STORE_HARDWARE_MAP_DEFAULTS_MISSING);
  if (!active_hardware_map_valid) {
    allow_auto_start = false;
    ESP_LOGE(TAG,
             "Active hardware map failed validation; suppressing auto-start");
  }
  if (pump_settings_status == NVS_STORE_PUMP_SETTINGS_LOADED) {
    ESP_LOGI(TAG, "Pump settings loaded from NVS");
  } else if (pump_settings_status == NVS_STORE_PUMP_SETTINGS_DEFAULTS_MISSING) {
    ESP_LOGI(TAG, "Pump settings missing; using product defaults");
  } else {
    allow_auto_start = false;
    ESP_LOGW(TAG,
             "Pump settings %s; using defaults and suppressing auto-start for "
             "this boot",
             pump_settings_load_status_name(pump_settings_status));
  }
  if (!hardware_map_safe) {
    allow_auto_start = false;
    ESP_LOGW(TAG,
             "Hardware map %s; using defaults and suppressing auto-start for "
             "this boot",
             hardware_map_load_status_name(hardware_map_status));
  }
  if (!hardware_map_promotion_ok) {
    allow_auto_start = false;
    ESP_LOGW(TAG, "Pending hardware map promotion incomplete; suppressing "
                  "auto-start for this boot");
  }
  if (!hardware_map_safe || !hardware_map_promotion_ok) {
    cooling_config.allow_runtime_control = false;
    cooling_config.auto_enable = false;
    cooling_config.mode = COOLING_CONTROL_MODE_FORCE_OFF;
    ESP_LOGW(TAG,
             "Hardware map or promotion unsafe; forcing cooling relay off "
             "for this boot");
  } else {
    cooling_config.allow_runtime_control = true;
  }

  ESP_LOGI(TAG,
           "Active hardware map (%s): float GPIO=%d, relay1 GPIO=%d, relay2 "
           "GPIO=%d, ds18b20 GPIO=%d, cooling GPIO=%d",
           hardware_map_load_status_name(hardware_map_status),
           active_hardware_map.float_input_gpio,
           active_hardware_map.pump_relay1_gpio,
           active_hardware_map.pump_relay2_gpio,
           active_hardware_map.ds18b20_data_gpio,
           active_hardware_map.cooling_relay_gpio);
  if (hardware_map_promoted) {
    ESP_LOGI(
        TAG,
        "Active hardware map was updated from pending values during this boot");
  }
  if (hardware_reboot_status_ok) {
    ESP_LOGI(TAG, "Pending hardware map reboot_required=%s",
             hardware_reboot_required ? "true" : "false");
  } else {
    ESP_LOGW(TAG, "Pending hardware map status invalid; reboot-required state "
                  "unavailable");
  }
  ESP_LOGI(TAG,
           "Cooling settings (%s): mode=%s, threshold_x10=%ld, "
           "hysteresis_x10=%ld, auto_enable=%s, min_off=%lu sec",
           cooling_settings_load_status_name(cooling_settings_status),
           hardware_map_cooling_mode_name(cooling_settings.mode),
           (long)cooling_settings.threshold_c_x10,
           (long)cooling_settings.hysteresis_c_x10,
           cooling_settings.auto_enable ? "true" : "false",
           (unsigned long)cooling_settings.compressor_min_off_sec);

  /* 2. Initialize task watchdog (10s timeout with panic) */
  uint32_t idle_mask = 0;
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
  idle_mask |= (1 << 0);
#endif
#ifdef CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
  idle_mask |= (1 << 1);
#endif
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = APP_TEMPLATE_MAIN_WDT_TIMEOUT_MS,
      .idle_core_mask = idle_mask,
      .trigger_panic = true,
  };
  esp_err_t wdt_ret = esp_task_wdt_init(&wdt_config);
  if (wdt_ret == ESP_OK) {
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "TWDT initialized (%d ms timeout, panic on trigger)",
             APP_TEMPLATE_MAIN_WDT_TIMEOUT_MS);
  } else if (wdt_ret == ESP_ERR_INVALID_STATE) {
    esp_err_t reconf_ret = esp_task_wdt_reconfigure(&wdt_config);
    if (reconf_ret == ESP_OK) {
      ESP_LOGI(TAG, "TWDT reconfigured (%d ms timeout, panic on trigger)",
               APP_TEMPLATE_MAIN_WDT_TIMEOUT_MS);
    } else {
      ESP_LOGW(TAG, "TWDT reconfigure failed: %s", esp_err_to_name(reconf_ret));
    }
    esp_task_wdt_add(NULL);
  } else {
    ESP_LOGE(TAG, "TWDT init failed: %s (continuing without watchdog)",
             esp_err_to_name(wdt_ret));
  }

  /* Configure relay outputs safely with active map and polarities */
  relay_safe_channel_t active_relay_channels[] = {
      {.gpio = active_hardware_map.pump_relay1_gpio,
       .active_low = pump_settings.relay1_active_low,
       .name = "pump1"},
      {.gpio = active_hardware_map.pump_relay2_gpio,
       .active_low = pump_settings.relay2_active_low,
       .name = "pump2"},
      {.gpio = active_hardware_map.cooling_relay_gpio,
       .active_low =
           (cooling_settings.relay_polarity == HARDWARE_RELAY_ACTIVE_LOW),
       .name = "cooling"},
  };
  relay_safe_configure(active_relay_channels,
                       sizeof(active_relay_channels) /
                           sizeof(active_relay_channels[0]));

  if (cooling_control_start_task(&cooling_config)) {
    ESP_LOGI(TAG,
             "Cooling runtime initialized: ds18b20 GPIO=%d, relay GPIO=%d, "
             "auto_enable=%s, min_off=%lu sec",
             cooling_config.ds18b20_gpio, cooling_config.cooling_relay_gpio,
             cooling_config.auto_enable ? "true" : "false",
             (unsigned long)cooling_config.compressor_min_off_sec);
  } else {
    ESP_LOGE(TAG, "Cooling runtime init failed or safe-disabled; continuing "
                  "with Wi-Fi/web setup");
  }

  pump_control_init_context_t pump_init_context = {
      .float_gpio = active_hardware_map.float_input_gpio,
      .relay1_gpio = active_hardware_map.pump_relay1_gpio,
      .relay2_gpio = active_hardware_map.pump_relay2_gpio,
      .relay1_active_low = pump_settings.relay1_active_low,
      .relay2_active_low = pump_settings.relay2_active_low,
      .allow_auto_start = allow_auto_start,
      .allow_runtime_control =
          hardware_map_safe && hardware_map_promotion_ok,
      .min_dwell_sec = pump_settings.min_dwell_sec,
      .water_set1 =
          {
              .on_sec = pump_settings.timer1_on_sec,
              .off_sec = pump_settings.timer1_off_sec,
              .start_phase = (pump_settings.timer1_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_set2 =
          {
              .on_sec = pump_settings.timer2_on_sec,
              .off_sec = pump_settings.timer2_off_sec,
              .start_phase = (pump_settings.timer2_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .wash_timer =
          {
              .on_sec = pump_settings.wash_on_sec,
              .off_sec = pump_settings.wash_off_sec,
              .start_phase = (pump_settings.wash_start_phase == HARDWARE_TIMER_START_PHASE_ON)
                                 ? PUMP_CONTROL_START_PHASE_ON
                                 : PUMP_CONTROL_START_PHASE_OFF,
          },
      .water_auto_start = pump_settings.auto_start,
      .wash_auto_start = pump_settings.wash_auto_start,
  };
  if (pump_control_start_task(&pump_init_context)) {
    ESP_LOGI(TAG, "Pump control initialized successfully");
  } else {
    ESP_LOGE(TAG, "Pump control init failed; continuing with Wi-Fi/web setup");
  }


  /* 2. Initialize session system */
  if (!session_init()) {
    ESP_LOGE(TAG, "Session system init failed; local web login unavailable");
  } else {
    ESP_LOGI(TAG, "Session system initialized");
  }

  /* 3. Initialize Wi-Fi (AP + STA with event handlers) */
  if (!wifi_manager_init()) {
    ESP_LOGE(TAG, "Wi-Fi init failed — continuing in AP-only mode");
  } else {
    ESP_LOGI(TAG, "Wi-Fi initialized");
  }
  ESP_LOGI(TAG, "AP SSID: %s, IP: %s", APP_TEMPLATE_AP_SSID,
           wifi_manager_get_ap_ip());

  /* 3a. Migrate legacy single-credential to multi-profile store (one-time,
   * idempotent) */
  nvs_store_migrate_legacy_wifi();

  /* 3b. Auto-connect to the designated profile if one is configured */
  {
    wifi_profile_t profiles[WIFI_PROFILE_MAX] = {0};
    int prof_count = 0, auto_idx = -1;
    nvs_store_load_wifi_profiles(profiles, &prof_count, &auto_idx);
    if (auto_idx >= 0 && auto_idx < prof_count && profiles[auto_idx].ssid[0]) {
      ESP_LOGI(TAG, "Auto-connect to Wi-Fi profile[%d]: %s", auto_idx,
               profiles[auto_idx].ssid);
      wifi_manager_connect_sta(profiles[auto_idx].ssid, profiles[auto_idx].pass,
                               NULL);
    } else if (prof_count == 0) {
      /* Fallback: try legacy sta_ssid only if no profile store entries exist
       * yet */
      char sta_ssid[33] = {0}, sta_pass[65] = {0};
      if (nvs_store_load_wifi(sta_ssid, sizeof(sta_ssid), sta_pass,
                              sizeof(sta_pass)) &&
          sta_ssid[0]) {
        ESP_LOGI(TAG, "Auto-connect to legacy saved SSID: %s", sta_ssid);
        wifi_manager_connect_sta(sta_ssid, sta_pass, NULL);
      } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credential; staying in AP-only mode");
      }
    } else {
      ESP_LOGI(TAG, "Wi-Fi auto-connect is disabled; staying in AP-only mode");
    }
  }

  /* 4. Start HTTP server */
  if (!web_server_start()) {
    ESP_LOGE(TAG, "HTTP server start failed — will retry");
    s_http_server_retry = true;
  } else {
    ESP_LOGI(TAG, "HTTP server started");

    /* Mark OTA firmware as valid (cancels bootloader rollback timer) */
    esp_err_t ota_err = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_err == ESP_OK) {
      ESP_LOGI(TAG, "OTA firmware validated (PENDING_VERIFY -> VALID)");
    } else if (ota_err == ESP_ERR_INVALID_STATE) {
      ESP_LOGD(TAG, "OTA mark_valid: not in PENDING_VERIFY state (ok)");
    } else {
      ESP_LOGW(TAG, "OTA mark_valid returned: %s", esp_err_to_name(ota_err));
    }
  }

  /* 5. Disable Wi-Fi power save (prevents mMC multicast loss) */
  esp_wifi_set_ps(WIFI_PS_NONE);

  /* 6. Initialize mDNS */
  esp_err_t mdns_err = mdns_init();
  if (mdns_err != ESP_OK) {
    ESP_LOGE(TAG, "mDNS init failed: %d", mdns_err);
  } else {
    mdns_hostname_set(APP_TEMPLATE_MDNS_HOSTNAME);
    mdns_instance_name_set(APP_TEMPLATE_MDNS_INSTANCE_NAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS initialized: http://%s.local",
             APP_TEMPLATE_MDNS_HOSTNAME);
  }

  /* 7. Start captive-portal DNS server (responds all queries with AP IP) */
  if (!dns_server_start()) {
    ESP_LOGE(TAG, "DNS server start failed");
  } else {
    ESP_LOGI(TAG, "DNS fallback server started (port 53)");
  }

  /* Start background TFT status update task */
  tft_display_start_task();

  // Start hardware UI/recovery task pinned to Core 1
  xTaskCreatePinnedToCore(hardware_ui_task, "hardware_ui_task", 4096, NULL,
                          tskIDLE_PRIORITY + 1, NULL, 1);

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "  System Ready");
  ESP_LOGI(TAG, "  AP SSID: %s", APP_TEMPLATE_AP_SSID);
  ESP_LOGI(TAG, "  AP IP: %s", wifi_manager_get_ap_ip());
  ESP_LOGI(TAG, "  URL: http://%s.local (mDNS)", APP_TEMPLATE_MDNS_HOSTNAME);
  ESP_LOGI(TAG, "  URL: http://%s (direct IP - always works)",
           wifi_manager_get_ap_ip());
  if (wifi_manager_is_sta_connected()) {
    ESP_LOGI(TAG, "  STA IP: %s", wifi_manager_get_sta_ip());
    ESP_LOGI(TAG, "  URL: http://%s.local (desktop/iOS on LAN)",
             APP_TEMPLATE_MDNS_HOSTNAME);
  }
  ESP_LOGI(TAG, "  Login user: %s", APP_TEMPLATE_DEFAULT_USERNAME);

  char stored_user[64] = {0};
  char stored_pass[64] = {0};
  if (nvs_store_get_credentials(stored_user, sizeof(stored_user), stored_pass,
                                sizeof(stored_pass))) {
    if (strcmp(stored_pass, APP_TEMPLATE_DEFAULT_PASSWORD) == 0 ||
        strcmp(stored_user, APP_TEMPLATE_DEFAULT_USERNAME) == 0) {
      ESP_LOGW(TAG, "  WARNING: Change default credentials!");
    } else {
      ESP_LOGI(TAG, "  Credentials OK");
    }
  } else {
    ESP_LOGW(TAG, "  WARNING: Change default credentials!");
  }
  ESP_LOGI(TAG, "========================================");

  /* Main loop — reset WDT every 5s, retry HTTP server, log status every 30s */
  uint8_t oom_strike_count = 0;
  uint32_t loop_counter = 0;
  while (1) {
    esp_task_wdt_reset();

    if (atomic_load(&s_cancel_rollback)) {
      ESP_LOGI(TAG, "Staging confirmed. Stopping rollback timers.");
      if (s_confirm_timer) {
        esp_timer_stop(s_confirm_timer);
        esp_timer_delete(s_confirm_timer);
        s_confirm_timer = NULL;
      }
      if (s_wifi_timer) {
        esp_timer_stop(s_wifi_timer);
        esp_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
      }
      atomic_store(&s_cancel_rollback, false);
      atomic_store(&s_trigger_rollback, false);
    }

    if (atomic_load(&s_trigger_rollback)) {
      uint8_t stg_type = 0;
      if (nvs_store_get_staging_type(&stg_type) && stg_type > 0) {
        ESP_LOGW(TAG, "Staged config timeout! Rolling back changes type=%d",
                 stg_type);
        if (nvs_store_rollback_staging()) {
          /* Rollback succeeded — stg_type is now 0 in NVS, safe to restart */
          session_invalidate_all();
          nvs_store_set_restart_detail("STAGING_ROLLBACK");
          vTaskDelay(pdMS_TO_TICKS(1000));
          esp_restart();
        } else {
          /* Rollback NVS write failed — stg_type is still >0 in flash.
           * If we restart now, the boot sequence will see stg_type>0 again,
           * start a new 180s timer, fail again, and loop forever.
           * Better to keep running with the staged (unconfirmed) config
           * and let the user fix it via the web UI or reflash. */
          ESP_LOGE(TAG,
                   "FATAL: Rollback NVS write failed! "
                   "Skipping restart to avoid infinite reboot loop. "
                   "Device continues with staged config (type=%d). "
                   "Manual intervention required.",
                   stg_type);
        }
      }
      atomic_store(&s_trigger_rollback, false);
    }

    if (atomic_load(&s_trigger_factory_reset)) {
      ESP_LOGW(TAG,
               "Factory reset requested — erasing all NVS and restarting...");
      session_invalidate_all();

      /* Stop Wi-Fi to release its internal NVS handles; otherwise
       * nvs_flash_erase fails */
      esp_wifi_stop();
      esp_wifi_deinit();

      if (nvs_store_factory_reset()) {
        nvs_store_set_restart_detail("factory_reset");
        ESP_LOGI(TAG, "NVS erased. Restarting in 500ms...");
        esp_task_wdt_reset(); /* defensive: erase can take ~300ms */
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      } else {
        ESP_LOGE(TAG,
                 "Factory reset NVS erase FAILED — device continues running");
        atomic_store(&s_trigger_factory_reset, false);
      }
    }

    if (s_http_server_retry) {
      if (web_server_start()) {
        ESP_LOGI(TAG, "HTTP server started on retry");
        s_http_server_retry = false;

        /* Mark OTA firmware as valid (cancels bootloader rollback timer) */
        esp_err_t ota_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_err == ESP_OK) {
          ESP_LOGI(TAG,
                   "OTA firmware validated on retry (PENDING_VERIFY -> VALID)");
        }
      }
    }

    static uint8_t queue_full_count = 0;
    static uint8_t health_fail_count = 0;
    bool queued = web_server_queue_health_check();
    if (!queued) {
      queue_full_count++;
      if (queue_full_count >= 3) {
        ESP_LOGE(TAG, "FATAL: HTTP server queue full for 3 consecutive checks "
                      "(15s). Rebooting!");
        nvs_store_set_restart_detail("HTTP_QUEUE_FULL");
        vTaskDelay(pdMS_TO_TICKS(500)); // Allow logs to flush
        esp_restart();
      } else {
        ESP_LOGW(TAG,
                 "Health check queue full — server busy, skipping restart "
                 "(count=%d/3)",
                 queue_full_count);
      }
    } else {
      queue_full_count = 0;
      uint32_t staleness_ms = 0;
      if (!web_server_check_health(15000, &staleness_ms)) {
        health_fail_count++;
        if (health_fail_count >= APP_TEMPLATE_HTTP_HUNG_CONSECUTIVE_FAILURES) {
          ESP_LOGE(TAG,
                   "HTTP thread hung for %d consecutive checks (staleness=%lu "
                   "ms) — triggering restart",
                   health_fail_count, (unsigned long)staleness_ms);
          nvs_store_set_restart_detail("HTTP_HUNG");
          vTaskDelay(pdMS_TO_TICKS(500)); // Allow logs to flush
          esp_restart();
        } else {
          ESP_LOGW(TAG,
                   "HTTP health check stale (staleness=%lu ms, count=%d/%d)",
                   (unsigned long)staleness_ms, health_fail_count,
                   APP_TEMPLATE_HTTP_HUNG_CONSECUTIVE_FAILURES);
        }
      } else {
        if (health_fail_count > 0) {
          ESP_LOGI(TAG,
                   "HTTP health check recovered after %d stale checks "
                   "(staleness=%lu ms)",
                   health_fail_count, (unsigned long)staleness_ms);
        }
        health_fail_count = 0;
      }
    }

    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t largest_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    bool below_critical =
        (free_heap < APP_CONFIG_OOM_MIN_FREE_HEAP_BYTES ||
         largest_block < APP_CONFIG_OOM_MIN_LARGEST_BLOCK_BYTES);

    bool fully_recovered =
        (free_heap > APP_CONFIG_OOM_RECOVER_FREE_HEAP_BYTES &&
         largest_block > APP_CONFIG_OOM_RECOVER_LARGEST_BLOCK_BYTES);

    if (below_critical) {
      /* Memory is critically low — increment strike counter */
      oom_strike_count++;
      ESP_LOGW(TAG,
               "OOM WARNING: Free heap (%lu) or largest block (%lu) below "
               "critical! Strike %d/%d",
               (unsigned long)free_heap, (unsigned long)largest_block,
               oom_strike_count, APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART);

      if (oom_strike_count >= APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART) {
        ESP_LOGE(TAG,
                 "FATAL: Out of Memory condition sustained for %d intervals. "
                 "Rebooting!",
                 oom_strike_count);
        nvs_store_set_restart_detail("OOM");
        vTaskDelay(pdMS_TO_TICKS(500)); // Allow logs to flush
        esp_restart();
      }
    } else if (fully_recovered) {
      /* Memory is comfortably above recovery threshold — clear strikes */
      if (oom_strike_count > 0) {
        ESP_LOGI(TAG,
                 "OOM condition recovered. Free heap: %lu, Largest block: %lu",
                 (unsigned long)free_heap, (unsigned long)largest_block);
        oom_strike_count = 0;
      }
    } else if (oom_strike_count > 0) {
      /* Gray zone: above critical but below recovery threshold — hold steady,
       * don't increment. This prevents false-positive restarts when memory
       * briefly dips then partially recovers. */
      ESP_LOGW(TAG,
               "OOM gray zone: Free heap (%lu), Largest block (%lu). Strikes "
               "held at %d/%d",
               (unsigned long)free_heap, (unsigned long)largest_block,
               oom_strike_count, APP_CONFIG_OOM_CONSECUTIVE_FAILURES_RESTART);
    }

    /* Publish system health metrics to telemetry hub */
    int8_t sta_rssi = 0;
    if (wifi_manager_is_sta_connected()) {
      wifi_ap_record_t ap_info;
      if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        sta_rssi = ap_info.rssi;
      }
    }

    telemetry_event_t health_evt = {
        .type = TELEMETRY_EVT_HEALTH,
        .data.health = {
            .free_heap_bytes = free_heap,
            .free_psram_bytes =
                (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
            .wifi_rssi = sta_rssi,
            .uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL),
            .oom_strikes = oom_strike_count,
        },
    };
    telemetry_hub_publish(&health_evt);

    loop_counter++;
    if (loop_counter >= APP_TEMPLATE_STATUS_LOG_INTERVALS) {
      log_board_and_hardware_diagnostics();
      wifi_manager_log_diagnostics();
      web_server_log_diagnostics();
      loop_counter = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
