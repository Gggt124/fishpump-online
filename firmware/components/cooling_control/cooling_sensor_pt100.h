#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MAX31865 RTD Fault Register Bits (datasheet REG 0x07)
 */
#define COOLING_PT100_FAULT_HIGH_THRESH  (1 << 7) /**< RTD High Threshold exceeded */
#define COOLING_PT100_FAULT_LOW_THRESH   (1 << 6) /**< RTD Low Threshold exceeded */
#define COOLING_PT100_FAULT_REFIN_HIGH   (1 << 5) /**< REFIN- > 0.85 x V_BIAS */
#define COOLING_PT100_FAULT_REFIN_LOW    (1 << 4) /**< REFIN- < 0.85 x V_BIAS (FORCE- open) */
#define COOLING_PT100_FAULT_RTDIN_LOW    (1 << 3) /**< RTDIN- < 0.85 x V_BIAS (FORCE- open) */
#define COOLING_PT100_FAULT_OV_UV        (1 << 2) /**< Overvoltage or undervoltage fault */

/**
 * @brief Initialize SPI bus (if not already initialized) and MAX31865 device descriptor.
 *
 * Configures the MAX31865 for 3-wire RTD, ITS-90 standard, R_ref = 430 ohms,
 * R_nominal = 100 ohms, 50 Hz filter, and enables V_bias.
 *
 * @param host      SPI host controller (e.g. SPI2_HOST)
 * @param cs_pin    Chip Select GPIO
 * @param mosi_pin  SPI MOSI GPIO
 * @param clk_pin   SPI SCK/CLK GPIO
 * @param miso_pin  SPI MISO GPIO
 * @return ESP_OK on success, appropriate esp_err_t on error.
 */
esp_err_t cooling_sensor_pt100_init(spi_host_device_t host, gpio_num_t cs_pin,
                                    gpio_num_t mosi_pin, gpio_num_t clk_pin,
                                    gpio_num_t miso_pin);

/**
 * @brief Measure RTD temperature in degrees Celsius.
 *
 * Performs single-shot measurement (~70ms). On fault/error, queries the fault
 * status register, clears the fault, reapplies device configuration, and outputs
 * the detected fault bits in out_fault_status (if non-NULL).
 *
 * @param[out] out_temp_c        Measured temperature in °C (mandatory)
 * @param[out] out_fault_status  Fault status register bits, 0 if no fault (optional, can be NULL)
 * @return ESP_OK on success, ESP_FAIL or error code on failure.
 */
esp_err_t cooling_sensor_pt100_read(float *out_temp_c, uint8_t *out_fault_status);

/**
 * @brief Deinitialize MAX31865 SPI device and free SPI bus if owned.
 *
 * @return ESP_OK on success.
 */
esp_err_t cooling_sensor_pt100_deinit(void);

/**
 * @brief Check if MAX31865 sensor is initialized.
 *
 * @return true if initialized, false otherwise.
 */
bool cooling_sensor_pt100_is_initialized(void);

/**
 * @brief Get human-readable description of fault status bits.
 *
 * @param fault_bits Fault bits from status register
 * @return String description of fault
 */
const char *cooling_sensor_pt100_fault_str(uint8_t fault_bits);

#ifdef __cplusplus
}
#endif
