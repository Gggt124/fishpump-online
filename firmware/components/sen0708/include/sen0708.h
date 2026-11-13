#pragma once

#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for one SEN0708 on an auto-direction RS485 module.
 */
typedef struct {
    uart_port_t uart_port; /**< e.g. UART_NUM_2 */
    int tx_pin;            /**< ESP32 TX -> module RXD */
    int rx_pin;            /**< ESP32 RX <- module TXD */
    uint32_t baud;         /**< 4800 factory default; 0 = 4800 */
    uint8_t slave_addr;    /**< 0x01 factory default; 0 = 0x01 */
} sen0708_config_t;

typedef struct {
    float ph;            /**< 0.00 - 14.00 */
    float temperature_c; /**< degC, 0.1 resolution */
} sen0708_reading_t;

typedef struct {
    float ph_offset;   /**< added to pH by the sensor, 0.01 resolution */
    float temp_offset; /**< added to temperature by the sensor, 0.1 resolution */
} sen0708_offsets_t;

typedef struct sen0708_dev *sen0708_handle_t;

/**
 * @brief Install the UART driver and return a handle. Does not talk to the sensor.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE if a handle already
 *         exists (one sensor supported), or a UART driver error.
 */
esp_err_t sen0708_new(const sen0708_config_t *config, sen0708_handle_t *out_handle);
esp_err_t sen0708_delete(sen0708_handle_t handle);

/*
 * All calls below are thread-safe (one mutex per handle) and return:
 *   ESP_OK, ESP_ERR_TIMEOUT (no reply), ESP_ERR_INVALID_CRC,
 *   ESP_ERR_INVALID_RESPONSE (exception, malformed or out-of-range reply),
 *   ESP_ERR_INVALID_ARG (bad handle or value out of the allowed range).
 */

/** Read pH (reg 0x0000) and temperature (reg 0x0001). */
esp_err_t sen0708_read(sen0708_handle_t handle, sen0708_reading_t *out);

/** Read the pH and temperature offsets (regs 0x0050, 0x0051). */
esp_err_t sen0708_get_offsets(sen0708_handle_t handle, sen0708_offsets_t *out);

/** Write the pH offset, range +-2.00. */
esp_err_t sen0708_set_ph_offset(sen0708_handle_t handle, float offset);

/** Write the temperature offset, range +-10.0. */
esp_err_t sen0708_set_temp_offset(sen0708_handle_t handle, float offset);

/**
 * @brief Send one point of the two-point electrode calibration (regs 0x0120-0x0121).
 * @param point      1 or 2.
 * @param buffer_ph  pH of the buffer solution the probe is in (0 - 14).
 */
esp_err_t sen0708_calibrate(sen0708_handle_t handle, uint8_t point, float buffer_ph);

#ifdef __cplusplus
}
#endif
