#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of PZEM-004T handles that can exist at the same time. */
#define PZEM004T_MAX_DEVICES 4

/**
 * @brief Structure holding parsed electrical measurements from PZEM-004T.
 */
typedef struct {
    float voltage;       /**< Voltage in Volts (resolution: 0.1V) */
    float current;       /**< Current in Amperes (resolution: 0.001A) */
    float power;         /**< Active power in Watts (resolution: 0.1W) */
    float energy;        /**< Accumulated active energy in Watt-hours (resolution: 1Wh) */
    float frequency;     /**< AC grid frequency in Hertz (resolution: 0.1Hz) */
    float pf;            /**< Power Factor (range: 0.00 - 1.00) */
    uint16_t alarms;     /**< Alarm register (0x0000 = normal, 0xFFFF = overload alarm) */
} pzem004t_reading_t;

/**
 * @brief Configuration parameters for one PZEM-004T module.
 */
typedef struct {
    uart_port_t uart_port; /**< UART port number (e.g. UART_NUM_1) */
    int tx_pin;            /**< ESP32 TX pin -> PZEM RX pin */
    int rx_pin;            /**< ESP32 RX pin -> PZEM TX pin */
    uint8_t slave_addr;    /**< Modbus slave address (0x01 default, 0xF8 broadcast) */
} pzem004t_config_t;

/** Opaque handle for one PZEM-004T module. */
typedef struct pzem004t_dev *pzem004t_handle_t;

/**
 * @brief Install the UART driver for a module and return its handle.
 *
 * Handles come from a static pool, so this never allocates heap memory.
 *
 * @param config      Module configuration.
 * @param out_handle  Receives the new handle on success.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_ARG on a NULL argument,
 *         ESP_ERR_NO_MEM when all PZEM004T_MAX_DEVICES slots are in use,
 *         ESP_ERR_INVALID_STATE when the UART port is already owned by another handle,
 *         or an error from the UART driver.
 */
esp_err_t pzem004t_new(const pzem004t_config_t *config, pzem004t_handle_t *out_handle);

/**
 * @brief Remove the UART driver and release the handle's slot.
 */
esp_err_t pzem004t_delete(pzem004t_handle_t handle);

/**
 * @brief Poll real-time measurement registers from the module.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on no response,
 *         ESP_ERR_INVALID_CRC on CRC error,
 *         ESP_ERR_INVALID_RESPONSE on a Modbus exception or malformed frame.
 */
esp_err_t pzem004t_read(pzem004t_handle_t handle, pzem004t_reading_t *out_reading);

/**
 * @brief Reset the accumulated energy register on the module.
 */
esp_err_t pzem004t_reset_energy(pzem004t_handle_t handle);

#ifdef __cplusplus
}
#endif
