#include "cooling_pwm_output.h"

#include <math.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include "app_config.h"

static const char *TAG = "cooling_pwm";

/* Driver state */
static bool s_initialized = false;
static gpio_num_t s_gpio = GPIO_NUM_NC;
static uint16_t s_current_mv = 0;
static uint32_t s_current_duty = 0;

/* Static FreeRTOS mutex to eliminate dynamic allocation and SMP race conditions */
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_pwm_mutex = NULL;
static portMUX_TYPE s_mutex_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_mutex_created(void)
{
    if (s_pwm_mutex == NULL) {
        taskENTER_CRITICAL(&s_mutex_spinlock);
        if (s_pwm_mutex == NULL) {
            s_pwm_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
        }
        taskEXIT_CRITICAL(&s_mutex_spinlock);
    }
}

/**
 * @brief Helper to convert target millivolts to 10-bit raw LEDC duty.
 * Formula: duty = round((mv / 10000.0) * 1023), clamped strictly to [0, 1023].
 */
static inline uint32_t calc_duty_from_mv(uint16_t mv)
{
    if (mv > COOLING_PWM_MAX_MV) {
        mv = COOLING_PWM_MAX_MV;
    }
    uint32_t duty = (uint32_t)lroundf(((float)mv / (float)COOLING_PWM_MAX_MV) * (float)COOLING_PWM_MAX_DUTY);
    if (duty > COOLING_PWM_MAX_DUTY) {
        duty = COOLING_PWM_MAX_DUTY;
    }
    return duty;
}

esp_err_t cooling_pwm_output_init(gpio_num_t pwm_gpio)
{
    ensure_mutex_created();
    if (s_pwm_mutex != NULL) {
        if (xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for mutex in init");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (pwm_gpio == GPIO_NUM_NC || (int)pwm_gpio < 0) {
#ifdef APP_TEMPLATE_COOLING_PWM_GPIO
        pwm_gpio = (gpio_num_t)APP_TEMPLATE_COOLING_PWM_GPIO;
#else
        pwm_gpio = COOLING_PWM_DEFAULT_GPIO;
#endif
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pwm_gpio)) {
        ESP_LOGE(TAG, "Invalid PWM GPIO: %d", (int)pwm_gpio);
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* If re-initializing, deconfigure previous channel and timer */
    if (s_initialized) {
        ledc_stop(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, 0);
        ledc_channel_config_t deconf_ch = {
            .speed_mode = COOLING_PWM_SPEED_MODE,
            .channel = COOLING_PWM_CHANNEL,
            .deconfigure = true,
        };
        ledc_channel_config(&deconf_ch);

        ledc_timer_config_t deconf_timer = {
            .speed_mode = COOLING_PWM_SPEED_MODE,
            .timer_num = COOLING_PWM_TIMER,
            .deconfigure = true,
        };
        ledc_timer_config(&deconf_timer);
        s_initialized = false;
    }

    /* Pre-configure GPIO pin LOW to prevent power-up glitch / voltage spike */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << (uint32_t)pwm_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", (int)pwm_gpio, esp_err_to_name(err));
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return err;
    }
    gpio_set_level(pwm_gpio, 0);

    /* Configure LEDC Timer 0: Low Speed Mode, 1 kHz, 10-bit resolution */
    ledc_timer_config_t timer_conf = {
        .speed_mode = COOLING_PWM_SPEED_MODE,
        .duty_resolution = COOLING_PWM_DUTY_RES,
        .timer_num = COOLING_PWM_TIMER,
        .freq_hz = COOLING_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return err;
    }

    /* Configure LEDC Channel 0: initial duty 0, initial level LOW */
    ledc_channel_config_t ch_conf = {
        .gpio_num = (int)pwm_gpio,
        .speed_mode = COOLING_PWM_SPEED_MODE,
        .channel = COOLING_PWM_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = COOLING_PWM_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = 0,
        },
        .deconfigure = false,
    };
    err = ledc_channel_config(&ch_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return err;
    }

    s_gpio = pwm_gpio;
    s_current_mv = 0;
    s_current_duty = 0;
    s_initialized = true;

    if (s_pwm_mutex != NULL) {
        xSemaphoreGive(s_pwm_mutex);
    }

    ESP_LOGI(TAG, "LEDC PWM initialized on GPIO %d (1 kHz, 10-bit, 0-10V scale)", (int)pwm_gpio);
    return ESP_OK;
}

esp_err_t cooling_pwm_output_set_millivolts(uint16_t mv)
{
    ensure_mutex_created();
    if (s_pwm_mutex != NULL) {
        if (xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for mutex in set_millivolts");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!s_initialized) {
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t target_mv = (mv > COOLING_PWM_MAX_MV) ? COOLING_PWM_MAX_MV : mv;
    uint32_t duty = calc_duty_from_mv(target_mv);

    esp_err_t err = ledc_set_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL);
    }

    if (err == ESP_OK) {
        s_current_mv = target_mv;
        s_current_duty = duty;
    } else {
        ESP_LOGE(TAG, "Failed to update LEDC duty %lu: %s", (unsigned long)duty, esp_err_to_name(err));
    }

    if (s_pwm_mutex != NULL) {
        xSemaphoreGive(s_pwm_mutex);
    }
    return err;
}

uint16_t cooling_pwm_output_get_current_millivolts(void)
{
    ensure_mutex_created();
    uint16_t mv = 0;
    if (s_pwm_mutex != NULL && xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        mv = s_current_mv;
        xSemaphoreGive(s_pwm_mutex);
    } else {
        mv = s_current_mv;
    }
    return mv;
}

float cooling_pwm_output_get_duty_percent(void)
{
    ensure_mutex_created();
    float pct = 0.0f;
    if (s_pwm_mutex != NULL && xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pct = ((float)s_current_duty / (float)COOLING_PWM_MAX_DUTY) * 100.0f;
        xSemaphoreGive(s_pwm_mutex);
    } else {
        pct = ((float)s_current_duty / (float)COOLING_PWM_MAX_DUTY) * 100.0f;
    }
    return pct;
}

esp_err_t cooling_pwm_output_stop(void)
{
    ensure_mutex_created();
    if (s_pwm_mutex != NULL) {
        if (xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for mutex in stop");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!s_initialized) {
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    /* Immediately drive duty to 0, update, and halt channel with idle level 0 */
    ledc_set_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, 0);
    ledc_update_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL);
    esp_err_t err = ledc_stop(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, 0);

    if (err == ESP_OK) {
        s_current_mv = 0;
        s_current_duty = 0;
    } else {
        ESP_LOGE(TAG, "ledc_stop failed: %s", esp_err_to_name(err));
    }

    if (s_pwm_mutex != NULL) {
        xSemaphoreGive(s_pwm_mutex);
    }
    return err;
}

uint16_t cooling_pwm_output_calc_slew_step(uint16_t current_mv, uint16_t target_mv, uint16_t max_step_mv)
{
    if (target_mv > COOLING_PWM_MAX_MV) {
        target_mv = COOLING_PWM_MAX_MV;
    }
    if (current_mv > COOLING_PWM_MAX_MV) {
        current_mv = COOLING_PWM_MAX_MV;
    }

    if (max_step_mv == 0) {
        return current_mv;
    }

    if (target_mv > current_mv) {
        uint16_t diff = target_mv - current_mv;
        if (diff > max_step_mv) {
            return current_mv + max_step_mv;
        }
        return target_mv;
    } else if (target_mv < current_mv) {
        uint16_t diff = current_mv - target_mv;
        if (diff > max_step_mv) {
            return current_mv - max_step_mv;
        }
        return target_mv;
    }

    return target_mv;
}

uint16_t cooling_pwm_output_calc_slew(uint16_t current_mv, uint16_t target_mv, uint32_t delta_ms, uint32_t max_rate_mv_per_sec)
{
    if (max_rate_mv_per_sec == 0 || delta_ms == 0) {
        return current_mv;
    }
    uint32_t max_step = (uint32_t)(((uint64_t)max_rate_mv_per_sec * delta_ms + 500ULL) / 1000ULL);
    if (max_step > COOLING_PWM_MAX_MV) {
        max_step = COOLING_PWM_MAX_MV;
    }
    return cooling_pwm_output_calc_slew_step(current_mv, target_mv, (uint16_t)max_step);
}

esp_err_t cooling_pwm_output_step_towards(uint16_t target_mv, uint16_t max_step_mv)
{
    ensure_mutex_created();
    if (s_pwm_mutex != NULL) {
        if (xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!s_initialized) {
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t next_mv = cooling_pwm_output_calc_slew_step(s_current_mv, target_mv, max_step_mv);
    uint32_t duty = calc_duty_from_mv(next_mv);

    esp_err_t err = ledc_set_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL);
    }

    if (err == ESP_OK) {
        s_current_mv = next_mv;
        s_current_duty = duty;
    } else {
        ESP_LOGE(TAG, "ledc step update failed: %s", esp_err_to_name(err));
    }

    if (s_pwm_mutex != NULL) {
        xSemaphoreGive(s_pwm_mutex);
    }
    return err;
}

bool cooling_pwm_output_is_initialized(void)
{
    ensure_mutex_created();
    bool init = false;
    if (s_pwm_mutex != NULL && xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        init = s_initialized;
        xSemaphoreGive(s_pwm_mutex);
    } else {
        init = s_initialized;
    }
    return init;
}

gpio_num_t cooling_pwm_output_get_gpio(void)
{
    ensure_mutex_created();
    gpio_num_t gpio = GPIO_NUM_NC;
    if (s_pwm_mutex != NULL && xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        gpio = s_gpio;
        xSemaphoreGive(s_pwm_mutex);
    } else {
        gpio = s_gpio;
    }
    return gpio;
}

uint32_t cooling_pwm_output_get_raw_duty(void)
{
    ensure_mutex_created();
    uint32_t duty = 0;
    if (s_pwm_mutex != NULL && xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        duty = s_current_duty;
        xSemaphoreGive(s_pwm_mutex);
    } else {
        duty = s_current_duty;
    }
    return duty;
}

esp_err_t cooling_pwm_output_deinit(void)
{
    ensure_mutex_created();
    if (s_pwm_mutex != NULL) {
        if (xSemaphoreTake(s_pwm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for mutex in deinit");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!s_initialized) {
        if (s_pwm_mutex != NULL) {
            xSemaphoreGive(s_pwm_mutex);
        }
        return ESP_OK;
    }

    ledc_stop(COOLING_PWM_SPEED_MODE, COOLING_PWM_CHANNEL, 0);

    ledc_channel_config_t ch_conf = {
        .speed_mode = COOLING_PWM_SPEED_MODE,
        .channel = COOLING_PWM_CHANNEL,
        .deconfigure = true,
    };
    ledc_channel_config(&ch_conf);

    ledc_timer_config_t timer_conf = {
        .speed_mode = COOLING_PWM_SPEED_MODE,
        .timer_num = COOLING_PWM_TIMER,
        .deconfigure = true,
    };
    ledc_timer_config(&timer_conf);

    s_initialized = false;
    s_current_mv = 0;
    s_current_duty = 0;
    s_gpio = GPIO_NUM_NC;

    if (s_pwm_mutex != NULL) {
        xSemaphoreGive(s_pwm_mutex);
    }
    ESP_LOGI(TAG, "LEDC PWM driver deinitialized");
    return ESP_OK;
}
