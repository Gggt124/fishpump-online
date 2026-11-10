#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LEDC PWM Output Driver Configuration Constants
 *
 * Hardware LEDC Timer 0, Low Speed Mode, 1 kHz frequency, 10-bit resolution.
 * Controls 0-10V DAC/inverter module on GPIO 8 (default).
 */
#define COOLING_PWM_TIMER                   LEDC_TIMER_0
#define COOLING_PWM_SPEED_MODE              LEDC_LOW_SPEED_MODE
#define COOLING_PWM_CHANNEL                 LEDC_CHANNEL_0
#define COOLING_PWM_DUTY_RES                LEDC_TIMER_10_BIT
#define COOLING_PWM_FREQ_HZ                 1000U
#define COOLING_PWM_MAX_DUTY                1023U
#define COOLING_PWM_MAX_MV                  10000U
#define COOLING_PWM_DEFAULT_GPIO            GPIO_NUM_8

/**
 * @brief Default soft slew rate limit: 1.0 V/s (1000 mV/s)
 * Used to avoid mechanical and electrical stress on inverter compressors.
 */
#define COOLING_PWM_DEFAULT_MAX_SLEW_RATE_MV_PER_SEC 1000U

/**
 * @brief Initialize the LEDC PWM output driver for 0-10V inverter speed control.
 *
 * Pre-configures the specified GPIO to LOW state before attaching to the LEDC timer
 * to prevent power-up voltage spikes/glitches. Configures LEDC Timer 0 (Low Speed Mode,
 * 1 kHz, 10-bit resolution) and Channel 0 (initial duty 0).
 *
 * @param pwm_gpio Output GPIO pin (typically GPIO 8; if GPIO_NUM_NC, defaults to GPIO 8).
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if GPIO is invalid for output
 *     - ESP_ERR_TIMEOUT if mutex lock timed out
 *     - Other ESP-IDF error codes on driver configuration failure
 */
esp_err_t cooling_pwm_output_init(gpio_num_t pwm_gpio);

/**
 * @brief Set the inverter target voltage in millivolts immediately.
 *
 * The input is clamped strictly to [0, 10000] mV (0.0V to 10.0V).
 * Duty cycle is calculated as:
 *   duty = round((mv / 10000.0) * 1023)
 * clamped to [0, 1023], and applied immediately to the hardware.
 *
 * @param mv Target voltage in millivolts (0 to 10000).
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if driver is not initialized
 *     - ESP_ERR_TIMEOUT if mutex lock timed out
 *     - Other error codes on hardware write failure
 */
esp_err_t cooling_pwm_output_set_millivolts(uint16_t mv);

/**
 * @brief Get the current voltage output setting in millivolts.
 *
 * @return Current voltage in millivolts (0 to 10000).
 */
uint16_t cooling_pwm_output_get_current_millivolts(void);

/**
 * @brief Get the current PWM duty cycle as a percentage (0.0% to 100.0%).
 *
 * Calculated directly from the current raw duty cycle:
 *   duty_percent = ((float)duty / 1023.0f) * 100.0f
 *
 * @return Duty percentage (0.0f to 100.0f).
 */
float cooling_pwm_output_get_duty_percent(void);

/**
 * @brief Stop the LEDC PWM output immediately (Fail-safe shutdown).
 *
 * Halts PWM signal and forces output level to LOW (0V).
 * Current output millivolts and duty are reset to 0.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if driver is not initialized
 *     - ESP_ERR_TIMEOUT if mutex lock timed out
 */
esp_err_t cooling_pwm_output_stop(void);

/**
 * @brief Helper function to compute the next millivolt value bounded by a step limit.
 *
 * Pure calculation function suitable for periodic control loops.
 *
 * @param current_mv Current output voltage in millivolts.
 * @param target_mv Desired target voltage in millivolts.
 * @param max_step_mv Maximum allowed change in millivolts for this step.
 * @return Next voltage in millivolts, clamped to [0, 10000].
 */
uint16_t cooling_pwm_output_calc_slew_step(uint16_t current_mv, uint16_t target_mv, uint16_t max_step_mv);

/**
 * @brief Helper function to compute slew rate limited voltage given elapsed time.
 *
 * @param current_mv Current output voltage in millivolts.
 * @param target_mv Desired target voltage in millivolts.
 * @param delta_ms Elapsed time since previous step in milliseconds.
 * @param max_rate_mv_per_sec Maximum allowed rate of change in mV/second.
 * @return Next voltage in millivolts, clamped to [0, 10000].
 */
uint16_t cooling_pwm_output_calc_slew(uint16_t current_mv, uint16_t target_mv, uint32_t delta_ms, uint32_t max_rate_mv_per_sec);

/**
 * @brief Step the output voltage towards a target value limited by max_step_mv.
 *
 * Atomically computes the stepped voltage and updates the hardware PWM duty cycle.
 *
 * @param target_mv Desired target voltage in millivolts.
 * @param max_step_mv Maximum allowed step change in millivolts.
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if driver is not initialized
 *     - ESP_ERR_TIMEOUT if mutex lock timed out
 */
esp_err_t cooling_pwm_output_step_towards(uint16_t target_mv, uint16_t max_step_mv);

/**
 * @brief Check if the cooling PWM driver is initialized.
 *
 * @return true if initialized, false otherwise.
 */
bool cooling_pwm_output_is_initialized(void);

/**
 * @brief Get the configured PWM GPIO pin number.
 *
 * @return GPIO number or GPIO_NUM_NC if not initialized.
 */
gpio_num_t cooling_pwm_output_get_gpio(void);

/**
 * @brief Get the current raw LEDC duty cycle (0 to 1023).
 *
 * @return Raw 10-bit duty cycle value.
 */
uint32_t cooling_pwm_output_get_raw_duty(void);

/**
 * @brief Deinitialize the LEDC PWM output driver and release hardware resources.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_TIMEOUT if mutex lock timed out
 */
esp_err_t cooling_pwm_output_deinit(void);

#ifdef __cplusplus
}
#endif
