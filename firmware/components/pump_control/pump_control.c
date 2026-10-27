#include "pump_control.h"

#include "app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "relay_safe_init.h"
#include "telemetry_hub.h"
#include <string.h>

#define PUMP_CONTROL_TICK_US 100000LL /* 100 ms tick */
#define MS_PER_SEC 1000LL

#define FLOAT_DEBOUNCE_MS APP_TEMPLATE_PUMP_FLOAT_DEBOUNCE_MS

#define NVS_PUMP_NAMESPACE "pump_cfg"
#define NVS_KEY_W_SET1_ON "w_set1_on"
#define NVS_KEY_W_SET1_OFF "w_set1_off"
#define NVS_KEY_W_SET1_START "w_set1_start"
#define NVS_KEY_W_SET2_ON "w_set2_on"
#define NVS_KEY_W_SET2_OFF "w_set2_off"
#define NVS_KEY_W_SET2_START "w_set2_start"
#define NVS_KEY_W_AUTOSTART "w_autostart"
#define NVS_KEY_WASH_ON "wash_on"
#define NVS_KEY_WASH_OFF "wash_off"
#define NVS_KEY_WASH_START "wash_start"
#define NVS_KEY_WASH_AUTOSTART "wash_autostart"
#define NVS_KEY_REL1_POL "rel1_pol"
#define NVS_KEY_REL2_POL "rel2_pol"
#define NVS_KEY_FLT_GPIO "flt_gpio"
#define NVS_KEY_REL1_GPIO "rel1_gpio"
#define NVS_KEY_REL2_GPIO "rel2_gpio"

static const char *TAG = "pump_control";

static SemaphoreHandle_t s_pump_mutex = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

static bool s_initialized = false;
static bool s_fault = false;
static pump_control_state_changed_cb_t s_state_changed_cb = NULL;

static void notify_state_changed(void) {
  if (s_state_changed_cb) {
    s_state_changed_cb();
  }
}

void pump_control_set_state_changed_callback(
    pump_control_state_changed_cb_t cb) {
  s_state_changed_cb = cb;
}

typedef struct {
  pump_control_timer_set_config_t water_set1;
  pump_control_timer_set_config_t water_set2;
  pump_control_timer_set_config_t wash_timer;
  bool water_auto_start;
  bool wash_auto_start;
  uint32_t min_dwell_sec;
  bool relay1_active_low;
  bool relay2_active_low;
  gpio_num_t float_gpio;
  gpio_num_t relay1_gpio;
  gpio_num_t relay2_gpio;
} pump_control_config_t;

static pump_control_config_t s_config;
static bool s_relays_configured = false;
static bool s_runtime_enabled = true;

typedef struct {
  bool running;
  bool relay_energized;
  pump_control_active_set_t active_set;
  pump_control_timer_phase_t phase;
  int64_t phase_deadline_ms;
  uint32_t total_duration_sec;
  int64_t last_set_switch_ms;
} water_runtime_state_t;

static water_runtime_state_t s_water_state;

typedef struct {
  bool running;
  bool relay_energized;
  pump_control_timer_phase_t phase;
  int64_t phase_deadline_ms;
  uint32_t total_duration_sec;
} wash_runtime_state_t;

static wash_runtime_state_t s_wash_state;

static pump_control_float_state_t s_confirmed_float_state =
    PUMP_CONTROL_FLOAT_UNKNOWN;
static pump_control_float_state_t s_pending_float_state =
    PUMP_CONTROL_FLOAT_UNKNOWN;
static int64_t s_pending_since_ms = 0;

/* Telemetry change tracking */
static telemetry_pump_payload_t s_last_published_pump = {0};
static bool s_pump_telemetry_published_once = false;

static uint32_t calc_countdown_sec(bool running, int64_t phase_deadline_ms,
                                   int64_t now_ms) {
  if (!running || phase_deadline_ms <= now_ms) {
    return 0;
  }
  int64_t rem_ms = phase_deadline_ms - now_ms;
  return (uint32_t)((rem_ms + MS_PER_SEC - 1) / MS_PER_SEC);
}

static void get_current_pump_telemetry_locked(int64_t now_ms,
                                             telemetry_pump_payload_t *out) {
  uint32_t rem = calc_countdown_sec(s_water_state.running,
                                    s_water_state.phase_deadline_ms, now_ms);
  uint32_t tot = s_water_state.running ? s_water_state.total_duration_sec : 0;
  uint32_t elap = (tot >= rem) ? (tot - rem) : 0;
  uint8_t active =
      s_water_state.running
          ? (uint8_t)s_water_state.active_set
          : (uint8_t)((s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON)
                          ? PUMP_CONTROL_SET_2
                          : PUMP_CONTROL_SET_1);

  out->active_pump = active;
  out->is_running = s_water_state.running;
  out->elapsed_sec = elap;
  out->remaining_sec = rem;
  out->relay_1_active = s_water_state.relay_energized;
  out->relay_2_active = s_wash_state.relay_energized;
}

static bool pump_telemetry_changed(const telemetry_pump_payload_t *p) {
  if (!s_pump_telemetry_published_once) {
    return true;
  }
  return (p->active_pump != s_last_published_pump.active_pump ||
          p->is_running != s_last_published_pump.is_running ||
          p->remaining_sec != s_last_published_pump.remaining_sec ||
          p->elapsed_sec != s_last_published_pump.elapsed_sec ||
          p->relay_1_active != s_last_published_pump.relay_1_active ||
          p->relay_2_active != s_last_published_pump.relay_2_active);
}

/**
 * Prepares telemetry payloads while holding the mutex, but DOES NOT publish.
 * Callers must release s_pump_mutex before calling telemetry_hub_publish().
 */
static void prepare_telemetry_if_changed_locked(
    int64_t now_ms, bool float_transitioned,
    telemetry_event_t *out_float_evt, bool *out_do_publish_float,
    telemetry_event_t *out_pump_evt, bool *out_do_publish_pump) {

  if (out_do_publish_float) {
    *out_do_publish_float = false;
  }
  if (out_do_publish_pump) {
    *out_do_publish_pump = false;
  }

  if (float_transitioned && out_float_evt && out_do_publish_float) {
    out_float_evt->type = TELEMETRY_EVT_FLOAT;
    out_float_evt->data.float_state.float_is_high =
        (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON);
    *out_do_publish_float = true;
  }

  telemetry_pump_payload_t current_pump;
  get_current_pump_telemetry_locked(now_ms, &current_pump);
  if (pump_telemetry_changed(&current_pump)) {
    s_last_published_pump = current_pump;
    s_pump_telemetry_published_once = true;
    if (out_pump_evt && out_do_publish_pump) {
      out_pump_evt->type = TELEMETRY_EVT_PUMP;
      out_pump_evt->data.pump = current_pump;
      *out_do_publish_pump = true;
    }
  }
}

static bool duration_valid(uint32_t seconds) {
  return seconds >= APP_TEMPLATE_PUMP_TIMER_MIN_SEC &&
         seconds <= APP_TEMPLATE_PUMP_TIMER_MAX_SEC;
}

static bool start_phase_valid(pump_control_start_phase_t phase) {
  return phase == PUMP_CONTROL_START_PHASE_ON ||
         phase == PUMP_CONTROL_START_PHASE_OFF;
}

static bool timer_set_config_valid(const pump_control_timer_set_config_t *set) {
  return set && duration_valid(set->on_sec) && duration_valid(set->off_sec) &&
         start_phase_valid(set->start_phase);
}

static bool config_update_valid(const pump_control_config_update_t *update) {
  if (!update)
    return false;
  return timer_set_config_valid(&update->water_set1) &&
         timer_set_config_valid(&update->water_set2) &&
         timer_set_config_valid(&update->wash_timer) &&
         update->min_dwell_sec <= APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC;
}

static bool init_context_valid(const pump_control_init_context_t *context) {
  if (!context || !GPIO_IS_VALID_GPIO(context->float_gpio) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(context->relay1_gpio) ||
      !GPIO_IS_VALID_OUTPUT_GPIO(context->relay2_gpio)) {
    return false;
  }

  return context->float_gpio != context->relay1_gpio &&
         context->float_gpio != context->relay2_gpio &&
         context->relay1_gpio != context->relay2_gpio &&
         context->min_dwell_sec <= APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_MAX_SEC;
}

static int relay_active_level(bool active_low) { return active_low ? 0 : 1; }

static int relay_inactive_level(bool active_low) { return active_low ? 1 : 0; }

static void pump_force_relays_inactive_locked(void) {
  if (s_relays_configured) {
    const relay_safe_channel_t relay1 = {
        .gpio = s_config.relay1_gpio,
        .active_low = s_config.relay1_active_low,
        .name = "pump_relay1",
    };
    const relay_safe_channel_t relay2 = {
        .gpio = s_config.relay2_gpio,
        .active_low = s_config.relay2_active_low,
        .name = "pump_relay2",
    };
    relay_safe_write_inactive(&relay1);
    relay_safe_write_inactive(&relay2);
  }

  s_water_state.running = false;
  s_water_state.relay_energized = false;
  s_water_state.phase = PUMP_CONTROL_PHASE_IDLE;
  s_water_state.phase_deadline_ms = 0;
  s_water_state.total_duration_sec = 0;
  s_water_state.last_set_switch_ms = 0;
  s_wash_state.running = false;
  s_wash_state.relay_energized = false;
  s_wash_state.phase = PUMP_CONTROL_PHASE_IDLE;
  s_wash_state.phase_deadline_ms = 0;
  s_wash_state.total_duration_sec = 0;
}

static void pump_fail_safe_locked(void) {
  if (s_tick_timer) {
    esp_timer_stop(s_tick_timer);
  }
  pump_force_relays_inactive_locked();
  s_initialized = false;
  s_fault = true;
}

static bool set_relay1_energized(bool energized) {
  bool active_low = s_config.relay1_active_low;
  int level =
      energized ? relay_active_level(active_low) : relay_inactive_level(active_low);
  if (gpio_set_level(s_config.relay1_gpio, level) != ESP_OK) {
#if !defined(APP_CONFIG_RELAY_FAULT_ENABLED) || (APP_CONFIG_RELAY_FAULT_ENABLED != 0)
    pump_force_relays_inactive_locked();
    s_fault = true;
    return false;
#endif
  }
  s_water_state.relay_energized = energized;
  return true;
}

static bool set_relay2_energized(bool energized) {
  bool active_low = s_config.relay2_active_low;
  int level =
      energized ? relay_active_level(active_low) : relay_inactive_level(active_low);
  if (gpio_set_level(s_config.relay2_gpio, level) != ESP_OK) {
#if !defined(APP_CONFIG_RELAY_FAULT_ENABLED) || (APP_CONFIG_RELAY_FAULT_ENABLED != 0)
    pump_force_relays_inactive_locked();
    s_fault = true;
    return false;
#endif
  }
  s_wash_state.relay_energized = energized;
  return true;
}

static pump_control_float_state_t read_float_state_raw(void) {
  int level = gpio_get_level(s_config.float_gpio);
  bool active_low = APP_TEMPLATE_PUMP_FLOAT_ACTIVE_LOW;
  bool active = active_low ? (level == 0) : (level != 0);
  return active ? PUMP_CONTROL_FLOAT_ON : PUMP_CONTROL_FLOAT_OFF;
}

static bool update_float_debounce(int64_t now_ms) {
  pump_control_float_state_t raw = read_float_state_raw();
  if (raw != s_pending_float_state) {
    s_pending_float_state = raw;
    s_pending_since_ms = now_ms;
    return false;
  }
  if (s_confirmed_float_state == raw) {
    return false;
  }
  if ((now_ms - s_pending_since_ms) >= FLOAT_DEBOUNCE_MS) {
    s_confirmed_float_state = raw;
    return true;
  }
  return false;
}

static const pump_control_timer_set_config_t *
get_active_water_set_config(pump_control_active_set_t set) {
  if (set == PUMP_CONTROL_SET_2) {
    return &s_config.water_set2;
  }
  return &s_config.water_set1;
}

static bool apply_water_set_start_phase(int64_t now_ms) {
  const pump_control_timer_set_config_t *cfg =
      get_active_water_set_config(s_water_state.active_set);
  pump_control_timer_phase_t phase =
      (cfg->start_phase == PUMP_CONTROL_START_PHASE_ON)
          ? PUMP_CONTROL_PHASE_ON
          : PUMP_CONTROL_PHASE_OFF;
  uint32_t duration =
      (phase == PUMP_CONTROL_PHASE_ON) ? cfg->on_sec : cfg->off_sec;

  s_water_state.phase = phase;
  s_water_state.total_duration_sec = duration;
  s_water_state.phase_deadline_ms = now_ms + ((int64_t)duration * MS_PER_SEC);
  if (!set_relay1_energized(phase == PUMP_CONTROL_PHASE_ON)) {
    return false;
  }
  notify_state_changed();
  return true;
}

static uint32_t calc_switch_cooldown_sec(int64_t now_ms) {
  if (!s_water_state.running || s_config.min_dwell_sec == 0 ||
      s_water_state.last_set_switch_ms <= 0) {
    return 0;
  }

  pump_control_active_set_t target_set =
      (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON) ? PUMP_CONTROL_SET_2
                                                         : PUMP_CONTROL_SET_1;
  if (target_set == s_water_state.active_set) {
    return 0;
  }

  int64_t deadline_ms = s_water_state.last_set_switch_ms +
                        ((int64_t)s_config.min_dwell_sec * MS_PER_SEC);
  return calc_countdown_sec(true, deadline_ms, now_ms);
}

static void load_nvs_config(void) {
  s_config.water_set1.on_sec = APP_TEMPLATE_PUMP_TIMER1_ON_SEC;
  s_config.water_set1.off_sec = APP_TEMPLATE_PUMP_TIMER1_OFF_SEC;
  s_config.water_set1.start_phase = APP_TEMPLATE_PUMP_TIMER_START_ON
                                        ? PUMP_CONTROL_START_PHASE_ON
                                        : PUMP_CONTROL_START_PHASE_OFF;

  s_config.water_set2.on_sec = APP_TEMPLATE_PUMP_TIMER2_ON_SEC;
  s_config.water_set2.off_sec = APP_TEMPLATE_PUMP_TIMER2_OFF_SEC;
  s_config.water_set2.start_phase = APP_TEMPLATE_PUMP_TIMER_START_ON
                                        ? PUMP_CONTROL_START_PHASE_ON
                                        : PUMP_CONTROL_START_PHASE_OFF;

  s_config.wash_timer.on_sec = 10;
  s_config.wash_timer.off_sec = 60;
  s_config.wash_timer.start_phase = PUMP_CONTROL_START_PHASE_ON;

  s_config.water_auto_start = APP_TEMPLATE_PUMP_AUTO_START_DEFAULT;
  s_config.wash_auto_start = false;
  s_config.min_dwell_sec = APP_TEMPLATE_PUMP_FLOAT_MIN_DWELL_SEC;

  s_config.relay1_active_low = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_ACTIVE_LOW;
  s_config.relay2_active_low = APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_ACTIVE_LOW;
  s_config.float_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_FLOAT_GPIO;
  s_config.relay1_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY1_GPIO;
  s_config.relay2_gpio = (gpio_num_t)APP_TEMPLATE_HW_DEFAULT_PUMP_RELAY2_GPIO;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_PUMP_NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "No NVS pump config found, using default values");
    return;
  }

  uint32_t val32;
  uint8_t val8;

  if (nvs_get_u32(handle, NVS_KEY_W_SET1_ON, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.water_set1.on_sec = val32;
  }
  if (nvs_get_u32(handle, NVS_KEY_W_SET1_OFF, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.water_set1.off_sec = val32;
  }
  if (nvs_get_u8(handle, NVS_KEY_W_SET1_START, &val8) == ESP_OK &&
      start_phase_valid((pump_control_start_phase_t)val8)) {
    s_config.water_set1.start_phase = (pump_control_start_phase_t)val8;
  }

  if (nvs_get_u32(handle, NVS_KEY_W_SET2_ON, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.water_set2.on_sec = val32;
  }
  if (nvs_get_u32(handle, NVS_KEY_W_SET2_OFF, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.water_set2.off_sec = val32;
  }
  if (nvs_get_u8(handle, NVS_KEY_W_SET2_START, &val8) == ESP_OK &&
      start_phase_valid((pump_control_start_phase_t)val8)) {
    s_config.water_set2.start_phase = (pump_control_start_phase_t)val8;
  }

  if (nvs_get_u8(handle, NVS_KEY_W_AUTOSTART, &val8) == ESP_OK) {
    s_config.water_auto_start = (val8 != 0);
  }

  if (nvs_get_u32(handle, NVS_KEY_WASH_ON, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.wash_timer.on_sec = val32;
  }
  if (nvs_get_u32(handle, NVS_KEY_WASH_OFF, &val32) == ESP_OK &&
      duration_valid(val32)) {
    s_config.wash_timer.off_sec = val32;
  }
  if (nvs_get_u8(handle, NVS_KEY_WASH_START, &val8) == ESP_OK &&
      start_phase_valid((pump_control_start_phase_t)val8)) {
    s_config.wash_timer.start_phase = (pump_control_start_phase_t)val8;
  }

  if (nvs_get_u8(handle, NVS_KEY_WASH_AUTOSTART, &val8) == ESP_OK) {
    s_config.wash_auto_start = (val8 != 0);
  }

  if (nvs_get_u8(handle, NVS_KEY_REL1_POL, &val8) == ESP_OK) {
    s_config.relay1_active_low = (val8 != 0);
  }

  if (nvs_get_u8(handle, NVS_KEY_REL2_POL, &val8) == ESP_OK) {
    s_config.relay2_active_low = (val8 != 0);
  }

  nvs_close(handle);
  ESP_LOGI(TAG, "NVS pump config loaded successfully");
}

static bool save_nvs_config(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(NVS_PUMP_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
    return false;
  }

  nvs_set_u32(handle, NVS_KEY_W_SET1_ON, s_config.water_set1.on_sec);
  nvs_set_u32(handle, NVS_KEY_W_SET1_OFF, s_config.water_set1.off_sec);
  nvs_set_u8(handle, NVS_KEY_W_SET1_START,
             (uint8_t)s_config.water_set1.start_phase);

  nvs_set_u32(handle, NVS_KEY_W_SET2_ON, s_config.water_set2.on_sec);
  nvs_set_u32(handle, NVS_KEY_W_SET2_OFF, s_config.water_set2.off_sec);
  nvs_set_u8(handle, NVS_KEY_W_SET2_START,
             (uint8_t)s_config.water_set2.start_phase);

  nvs_set_u8(handle, NVS_KEY_W_AUTOSTART, s_config.water_auto_start ? 1 : 0);

  nvs_set_u32(handle, NVS_KEY_WASH_ON, s_config.wash_timer.on_sec);
  nvs_set_u32(handle, NVS_KEY_WASH_OFF, s_config.wash_timer.off_sec);
  nvs_set_u8(handle, NVS_KEY_WASH_START,
             (uint8_t)s_config.wash_timer.start_phase);

  nvs_set_u8(handle, NVS_KEY_WASH_AUTOSTART, s_config.wash_auto_start ? 1 : 0);

  nvs_set_u8(handle, NVS_KEY_REL1_POL, s_config.relay1_active_low ? 1 : 0);
  nvs_set_u8(handle, NVS_KEY_REL2_POL, s_config.relay2_active_low ? 1 : 0);
  nvs_set_u8(handle, NVS_KEY_FLT_GPIO, (uint8_t)s_config.float_gpio);
  nvs_set_u8(handle, NVS_KEY_REL1_GPIO, (uint8_t)s_config.relay1_gpio);
  nvs_set_u8(handle, NVS_KEY_REL2_GPIO, (uint8_t)s_config.relay2_gpio);

  err = nvs_commit(handle);
  nvs_close(handle);
  return (err == ESP_OK);
}

static void pump_tick_cb(void *arg) {
  (void)arg;
  if (!s_pump_mutex || xSemaphoreTake(s_pump_mutex, 0) != pdTRUE) {
    return;
  }
#if defined(APP_CONFIG_RELAY_FAULT_ENABLED) && (APP_CONFIG_RELAY_FAULT_ENABLED == 0)
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return;
  }
#else
  if (!s_initialized || s_fault) {
    xSemaphoreGive(s_pump_mutex);
    return;
  }
  if (!s_runtime_enabled) {
    pump_force_relays_inactive_locked();
    xSemaphoreGive(s_pump_mutex);
    return;
  }
#endif

  int64_t now_ms = esp_timer_get_time() / 1000;
  bool float_changed = update_float_debounce(now_ms);

  /* Process Water System */
  if (s_water_state.running) {
    pump_control_active_set_t target_set =
        (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON) ? PUMP_CONTROL_SET_2
                                                           : PUMP_CONTROL_SET_1;
    if (s_water_state.active_set != target_set) {
      int64_t dwell_ms = (int64_t)s_config.min_dwell_sec * MS_PER_SEC;
      if ((now_ms - s_water_state.last_set_switch_ms) >= dwell_ms) {
        s_water_state.active_set = target_set;
        s_water_state.last_set_switch_ms = now_ms;
        if (!apply_water_set_start_phase(now_ms)) {
          goto exit_tick;
        }
      } else {
        if (now_ms >= s_water_state.phase_deadline_ms) {
          const pump_control_timer_set_config_t *cfg =
              get_active_water_set_config(s_water_state.active_set);
          if (s_water_state.phase == PUMP_CONTROL_PHASE_ON) {
            s_water_state.phase = PUMP_CONTROL_PHASE_OFF;
            s_water_state.total_duration_sec = cfg->off_sec;
          } else {
            s_water_state.phase = PUMP_CONTROL_PHASE_ON;
            s_water_state.total_duration_sec = cfg->on_sec;
          }
          s_water_state.phase_deadline_ms =
              now_ms + ((int64_t)s_water_state.total_duration_sec * MS_PER_SEC);
          if (!set_relay1_energized(s_water_state.phase ==
                                    PUMP_CONTROL_PHASE_ON)) {
            goto exit_tick;
          }
          notify_state_changed();
        }
      }
    } else {
      if (now_ms >= s_water_state.phase_deadline_ms) {
        const pump_control_timer_set_config_t *cfg =
            get_active_water_set_config(s_water_state.active_set);
        if (s_water_state.phase == PUMP_CONTROL_PHASE_ON) {
          s_water_state.phase = PUMP_CONTROL_PHASE_OFF;
          s_water_state.total_duration_sec = cfg->off_sec;
        } else {
          s_water_state.phase = PUMP_CONTROL_PHASE_ON;
          s_water_state.total_duration_sec = cfg->on_sec;
        }
        s_water_state.phase_deadline_ms =
            now_ms + ((int64_t)s_water_state.total_duration_sec * MS_PER_SEC);
        if (!set_relay1_energized(s_water_state.phase ==
                                  PUMP_CONTROL_PHASE_ON)) {
          goto exit_tick;
        }
        notify_state_changed();
      }
    }
  } else {
    s_water_state.active_set =
        (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON) ? PUMP_CONTROL_SET_2
                                                           : PUMP_CONTROL_SET_1;
  }

  /* Process Wash System */
  if (s_wash_state.running) {
    if (now_ms >= s_wash_state.phase_deadline_ms) {
      if (s_wash_state.phase == PUMP_CONTROL_PHASE_ON) {
        s_wash_state.phase = PUMP_CONTROL_PHASE_OFF;
        s_wash_state.total_duration_sec = s_config.wash_timer.off_sec;
      } else {
        s_wash_state.phase = PUMP_CONTROL_PHASE_ON;
        s_wash_state.total_duration_sec = s_config.wash_timer.on_sec;
      }
      s_wash_state.phase_deadline_ms =
          now_ms + ((int64_t)s_wash_state.total_duration_sec * MS_PER_SEC);
      if (!set_relay2_energized(s_wash_state.phase ==
                                PUMP_CONTROL_PHASE_ON)) {
        goto exit_tick;
      }
      notify_state_changed();
    }
  }

exit_tick: ;
  telemetry_event_t float_evt;
  bool do_publish_float = false;
  telemetry_event_t pump_evt;
  bool do_publish_pump = false;

  prepare_telemetry_if_changed_locked(now_ms, float_changed,
                                      &float_evt, &do_publish_float,
                                      &pump_evt, &do_publish_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_publish_float) {
    telemetry_hub_publish(&float_evt);
  }
  if (do_publish_pump) {
    telemetry_hub_publish(&pump_evt);
  }
}

bool pump_control_init(const pump_control_init_context_t *context) {
  if (!init_context_valid(context)) {
    s_runtime_enabled = false;
    if (s_pump_mutex &&
        xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      pump_fail_safe_locked();
      xSemaphoreGive(s_pump_mutex);
    }
    ESP_LOGE(TAG, "Invalid pump hardware initialization context");
    return false;
  }

  if (!s_pump_mutex) {
    s_pump_mutex = xSemaphoreCreateMutex();
    if (!s_pump_mutex) {
      ESP_LOGE(TAG, "Failed to create mutex");
      return false;
    }
  }

  if (xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }

  if (s_tick_timer) {
    esp_timer_stop(s_tick_timer);
  }

  pump_force_relays_inactive_locked();
  s_relays_configured = false;
  s_initialized = false;
  s_fault = false;

  memset(&s_water_state, 0, sizeof(s_water_state));
  memset(&s_wash_state, 0, sizeof(s_wash_state));

  load_nvs_config();

  s_config.float_gpio = context->float_gpio;
  s_config.relay1_gpio = context->relay1_gpio;
  s_config.relay2_gpio = context->relay2_gpio;
  s_config.relay1_active_low = context->relay1_active_low;
  s_config.relay2_active_low = context->relay2_active_low;
  s_config.min_dwell_sec = context->min_dwell_sec;
  s_runtime_enabled = context->allow_runtime_control;
#if defined(APP_CONFIG_RELAY_FAULT_ENABLED) && (APP_CONFIG_RELAY_FAULT_ENABLED == 0)
  if (!context->allow_auto_start) {
    s_config.water_auto_start = false;
    s_config.wash_auto_start = false;
  }
#else
  if (!context->allow_auto_start || !s_runtime_enabled) {
    s_config.water_auto_start = false;
    s_config.wash_auto_start = false;
  }
#endif

  const relay_safe_channel_t relay_channels[] = {
      {
          .gpio = s_config.relay1_gpio,
          .active_low = s_config.relay1_active_low,
          .name = "pump_relay1",
      },
      {
          .gpio = s_config.relay2_gpio,
          .active_low = s_config.relay2_active_low,
          .name = "pump_relay2",
      },
  };
  if (!relay_safe_configure(relay_channels,
                            sizeof(relay_channels) /
                                sizeof(relay_channels[0]))) {
    ESP_LOGE(TAG, "Failed to configure relay GPIOs");
#if !defined(APP_CONFIG_RELAY_FAULT_ENABLED) || (APP_CONFIG_RELAY_FAULT_ENABLED != 0)
    s_fault = true;
    xSemaphoreGive(s_pump_mutex);
    return false;
#endif
  }
  s_relays_configured = true;

  bool float_input_only = !GPIO_IS_VALID_OUTPUT_GPIO(s_config.float_gpio);
  gpio_config_t float_cfg = {
      .pin_bit_mask = (1ULL << s_config.float_gpio),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en =
          float_input_only
              ? GPIO_PULLUP_DISABLE
              : (APP_TEMPLATE_PUMP_FLOAT_ACTIVE_LOW ? GPIO_PULLUP_ENABLE
                                                    : GPIO_PULLUP_DISABLE),
      .pull_down_en =
          float_input_only
              ? GPIO_PULLDOWN_DISABLE
              : (APP_TEMPLATE_PUMP_FLOAT_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE
                                                    : GPIO_PULLDOWN_ENABLE),
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&float_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure float GPIO");
    pump_fail_safe_locked();
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  if (!s_tick_timer) {
    const esp_timer_create_args_t timer_args = {
        .callback = pump_tick_cb,
        .arg = NULL,
        .name = "pump_control_tick",
    };
    if (esp_timer_create(&timer_args, &s_tick_timer) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to create tick timer");
      pump_fail_safe_locked();
      xSemaphoreGive(s_pump_mutex);
      return false;
    }
  }

  if (esp_timer_start_periodic(s_tick_timer, PUMP_CONTROL_TICK_US) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start tick timer");
    pump_fail_safe_locked();
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  s_initialized = true;

  int64_t now_ms = esp_timer_get_time() / 1000;
  pump_control_float_state_t initial_float = read_float_state_raw();
  s_confirmed_float_state = initial_float;
  s_pending_float_state = initial_float;
  s_pending_since_ms = now_ms;

  if (s_config.water_auto_start) {
    s_water_state.running = true;
    s_water_state.active_set =
        (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON) ? PUMP_CONTROL_SET_2
                                                           : PUMP_CONTROL_SET_1;
    s_water_state.last_set_switch_ms = now_ms;
    if (!apply_water_set_start_phase(now_ms)) {
      pump_fail_safe_locked();
      xSemaphoreGive(s_pump_mutex);
      return false;
    }
  }

  if (s_config.wash_auto_start) {
    s_wash_state.running = true;
    s_wash_state.phase =
        (s_config.wash_timer.start_phase == PUMP_CONTROL_START_PHASE_ON)
            ? PUMP_CONTROL_PHASE_ON
            : PUMP_CONTROL_PHASE_OFF;
    s_wash_state.total_duration_sec =
        (s_wash_state.phase == PUMP_CONTROL_PHASE_ON)
            ? s_config.wash_timer.on_sec
            : s_config.wash_timer.off_sec;
    s_wash_state.phase_deadline_ms =
        now_ms + ((int64_t)s_wash_state.total_duration_sec * MS_PER_SEC);
    if (!set_relay2_energized(s_wash_state.phase == PUMP_CONTROL_PHASE_ON)) {
      pump_fail_safe_locked();
      xSemaphoreGive(s_pump_mutex);
      return false;
    }
  }

  s_pump_telemetry_published_once = false;
  telemetry_event_t init_float_evt;
  bool do_pub_float = false;
  telemetry_event_t init_pump_evt;
  bool do_pub_pump = false;

  prepare_telemetry_if_changed_locked(now_ms, true,
                                      &init_float_evt, &do_pub_float,
                                      &init_pump_evt, &do_pub_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_float) {
    telemetry_hub_publish(&init_float_evt);
  }
  if (do_pub_pump) {
    telemetry_hub_publish(&init_pump_evt);
  }

  return true;
}

bool pump_control_start_water(void) {
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
#if defined(APP_CONFIG_RELAY_FAULT_ENABLED) && (APP_CONFIG_RELAY_FAULT_ENABLED == 0)
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }
#else
  if (!s_initialized || s_fault || !s_runtime_enabled) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }
#endif
  if (s_water_state.running) {
    xSemaphoreGive(s_pump_mutex);
    return true;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  /* Debounce state is driven exclusively by pump_tick_cb to prevent race conditions */

  s_water_state.running = true;
  s_water_state.active_set =
      (s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON) ? PUMP_CONTROL_SET_2
                                                         : PUMP_CONTROL_SET_1;
  s_water_state.last_set_switch_ms = now_ms;
  if (!apply_water_set_start_phase(now_ms)) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  telemetry_event_t pump_evt;
  bool do_pub_pump = false;
  prepare_telemetry_if_changed_locked(now_ms, false, NULL, NULL,
                                      &pump_evt, &do_pub_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_pump) {
    telemetry_hub_publish(&pump_evt);
  }
  return true;
}

bool pump_control_stop_water(void) {
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  s_water_state.running = false;
  s_water_state.phase = PUMP_CONTROL_PHASE_IDLE;
  s_water_state.phase_deadline_ms = 0;
  s_water_state.total_duration_sec = 0;
  s_water_state.last_set_switch_ms = 0;
  bool ok = set_relay1_energized(false);
  notify_state_changed();

  telemetry_event_t pump_evt;
  bool do_pub_pump = false;
  prepare_telemetry_if_changed_locked(now_ms, false, NULL, NULL,
                                      &pump_evt, &do_pub_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_pump) {
    telemetry_hub_publish(&pump_evt);
  }
  return ok;
}

bool pump_control_start_wash(void) {
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
#if defined(APP_CONFIG_RELAY_FAULT_ENABLED) && (APP_CONFIG_RELAY_FAULT_ENABLED == 0)
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }
#else
  if (!s_initialized || s_fault || !s_runtime_enabled) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }
#endif
  if (s_wash_state.running) {
    xSemaphoreGive(s_pump_mutex);
    return true;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  s_wash_state.running = true;
  s_wash_state.phase =
      (s_config.wash_timer.start_phase == PUMP_CONTROL_START_PHASE_ON)
          ? PUMP_CONTROL_PHASE_ON
          : PUMP_CONTROL_PHASE_OFF;
  s_wash_state.total_duration_sec =
      (s_wash_state.phase == PUMP_CONTROL_PHASE_ON)
          ? s_config.wash_timer.on_sec
          : s_config.wash_timer.off_sec;
  s_wash_state.phase_deadline_ms =
      now_ms + ((int64_t)s_wash_state.total_duration_sec * MS_PER_SEC);
  if (!set_relay2_energized(s_wash_state.phase == PUMP_CONTROL_PHASE_ON)) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }
  notify_state_changed();

  telemetry_event_t pump_evt;
  bool do_pub_pump = false;
  prepare_telemetry_if_changed_locked(now_ms, false, NULL, NULL,
                                      &pump_evt, &do_pub_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_pump) {
    telemetry_hub_publish(&pump_evt);
  }
  return true;
}

bool pump_control_stop_wash(void) {
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  s_wash_state.running = false;
  s_wash_state.phase = PUMP_CONTROL_PHASE_IDLE;
  s_wash_state.phase_deadline_ms = 0;
  s_wash_state.total_duration_sec = 0;
  bool ok = set_relay2_energized(false);
  notify_state_changed();

  telemetry_event_t pump_evt;
  bool do_pub_pump = false;
  prepare_telemetry_if_changed_locked(now_ms, false, NULL, NULL,
                                      &pump_evt, &do_pub_pump);
  xSemaphoreGive(s_pump_mutex);

  /* Publish telemetry OUTSIDE mutex lock to prevent deadlock */
  if (do_pub_pump) {
    telemetry_hub_publish(&pump_evt);
  }
  return ok;
}

bool pump_control_get_status(pump_control_status_t *out) {
  if (!out)
    return false;
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  /* Read-only getter: float debounce state is not mutated here; driven exclusively by pump_tick_cb */

  out->initialized = s_initialized;
#if defined(APP_CONFIG_RELAY_FAULT_ENABLED) && (APP_CONFIG_RELAY_FAULT_ENABLED == 0)
  out->fault = false;
#else
  out->fault = s_fault || !s_runtime_enabled;
#endif

  out->water.running = s_water_state.running;
  out->water.relay_energized = s_water_state.relay_energized;
  out->water.float_state = s_confirmed_float_state;
  out->water.active_set =
      s_water_state.running
          ? s_water_state.active_set
          : ((s_confirmed_float_state == PUMP_CONTROL_FLOAT_ON)
                 ? PUMP_CONTROL_SET_2
                 : PUMP_CONTROL_SET_1);
  out->water.phase = s_water_state.phase;
  out->water.countdown_sec = calc_countdown_sec(
      s_water_state.running, s_water_state.phase_deadline_ms, now_ms);
  out->water.total_duration_sec =
      s_water_state.running ? s_water_state.total_duration_sec : 0;
  out->water.cooldown_remaining_sec = calc_switch_cooldown_sec(now_ms);
  out->water.auto_start = s_config.water_auto_start;

  out->wash.running = s_wash_state.running;
  out->wash.relay_energized = s_wash_state.relay_energized;
  out->wash.phase = s_wash_state.phase;
  out->wash.countdown_sec = calc_countdown_sec(
      s_wash_state.running, s_wash_state.phase_deadline_ms, now_ms);
  out->wash.total_duration_sec =
      s_wash_state.running ? s_wash_state.total_duration_sec : 0;
  out->wash.auto_start = s_config.wash_auto_start;

  xSemaphoreGive(s_pump_mutex);
  return true;
}

bool pump_control_update_config(const pump_control_config_update_t *update) {
  if (!config_update_valid(update)) {
    ESP_LOGE(TAG, "Invalid config update parameters");
    return false;
  }
  if (!s_pump_mutex ||
      xSemaphoreTake(s_pump_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (!s_initialized) {
    xSemaphoreGive(s_pump_mutex);
    return false;
  }

  s_config.water_set1 = update->water_set1;
  s_config.water_set2 = update->water_set2;
  s_config.wash_timer = update->wash_timer;
  s_config.water_auto_start = update->water_auto_start;
  s_config.wash_auto_start = update->wash_auto_start;
  s_config.min_dwell_sec = update->min_dwell_sec;

  bool ok = save_nvs_config();

  xSemaphoreGive(s_pump_mutex);
  return ok;
}
