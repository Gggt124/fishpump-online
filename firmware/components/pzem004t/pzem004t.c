#include "pzem004t.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pzem004t";

#define PZEM_BAUD_RATE        9600
#define PZEM_BUF_SIZE         256
#define PZEM_TIMEOUT_MS       500
#define PZEM_LOCK_TIMEOUT_MS  1000
#define PZEM_BROADCAST_ADDR   0xF8

#define PZEM_FUNC_READ_INPUT  0x04
#define PZEM_FUNC_RESET_ENERGY 0x42
#define PZEM_EXCEPTION_FLAG   0x80
#define PZEM_EXCEPTION_LEN    5

#define PZEM_READ_REG_COUNT   10
#define PZEM_READ_RESP_LEN    (3 + PZEM_READ_REG_COUNT * 2 + 2) /* 25 bytes */
#define PZEM_RESET_RESP_LEN   4

struct pzem004t_dev {
    bool in_use;
    pzem004t_config_t cfg;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buf;
};

static struct pzem004t_dev s_devices[PZEM004T_MAX_DEVICES];
static portMUX_TYPE s_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t pzem_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static bool pzem_crc_ok(const uint8_t *frame, size_t len)
{
    if (len < 3) {
        return false;
    }
    uint16_t expected = pzem_crc16(frame, len - 2);
    uint16_t actual = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    return expected == actual;
}

static void pzem_append_crc(uint8_t *frame, size_t payload_len)
{
    uint16_t crc = pzem_crc16(frame, payload_len);
    frame[payload_len] = (uint8_t)(crc & 0xFF);
    frame[payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);
}

/**
 * Send a request and receive a reply of resp_len bytes, or a 5-byte Modbus
 * exception reply (func | 0x80), whichever arrives. Validates address,
 * function code and CRC. Caller must hold dev->lock.
 */
static esp_err_t pzem_transact(struct pzem004t_dev *dev, const uint8_t *req, size_t req_len,
                               uint8_t *resp, size_t resp_len, uint8_t func)
{
    const uart_port_t port = dev->cfg.uart_port;
    const uint8_t addr = dev->cfg.slave_addr;

    uart_flush_input(port);

    int written = uart_write_bytes(port, (const char *)req, req_len);
    if (written != (int)req_len) {
        ESP_LOGE(TAG, "UART%d addr 0x%02X: failed to write request", port, addr);
        return ESP_FAIL;
    }

    size_t need = resp_len;
    size_t total = 0;
    bool is_exception = false;
    int64_t deadline_ms = esp_timer_get_time() / 1000 + PZEM_TIMEOUT_MS;

    while (total < need) {
        int n = uart_read_bytes(port, &resp[total], need - total, pdMS_TO_TICKS(50));
        if (n > 0) {
            total += (size_t)n;
        }
        if (!is_exception && total >= 2 && resp[1] == (func | PZEM_EXCEPTION_FLAG)) {
            is_exception = true;
            need = PZEM_EXCEPTION_LEN;
        }
        if (esp_timer_get_time() / 1000 > deadline_ms) {
            break;
        }
    }

    if (total < need) {
        return ESP_ERR_TIMEOUT;
    }

    if (resp[0] != addr && addr != PZEM_BROADCAST_ADDR) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: unexpected slave addr 0x%02X", port, addr, resp[0]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!pzem_crc_ok(resp, need)) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: CRC mismatch", port, addr);
        return ESP_ERR_INVALID_CRC;
    }

    if (is_exception) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: Modbus exception 0x%02X for func 0x%02X",
                 port, addr, resp[2], func);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (resp[1] != func) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: unexpected func 0x%02X", port, addr, resp[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static bool pzem_handle_valid(pzem004t_handle_t handle)
{
    for (int i = 0; i < PZEM004T_MAX_DEVICES; i++) {
        if (handle == &s_devices[i]) {
            return handle->in_use;
        }
    }
    return false;
}

esp_err_t pzem004t_new(const pzem004t_config_t *config, pzem004t_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    struct pzem004t_dev *dev = NULL;

    portENTER_CRITICAL(&s_pool_lock);
    for (int i = 0; i < PZEM004T_MAX_DEVICES; i++) {
        if (s_devices[i].in_use && s_devices[i].cfg.uart_port == config->uart_port) {
            portEXIT_CRITICAL(&s_pool_lock);
            ESP_LOGE(TAG, "UART%d is already used by another PZEM handle", config->uart_port);
            return ESP_ERR_INVALID_STATE;
        }
    }
    for (int i = 0; i < PZEM004T_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) {
            dev = &s_devices[i];
            dev->in_use = true; // reserve the slot
            break;
        }
    }
    portEXIT_CRITICAL(&s_pool_lock);

    if (dev == NULL) {
        ESP_LOGE(TAG, "No free PZEM slot (max %d)", PZEM004T_MAX_DEVICES);
        return ESP_ERR_NO_MEM;
    }

    dev->cfg = *config;
    if (dev->cfg.slave_addr == 0) {
        dev->cfg.slave_addr = 0x01;
    }

    uart_config_t uart_config = {
        .baud_rate = PZEM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(dev->cfg.uart_port, PZEM_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: uart_driver_install failed: %s", dev->cfg.uart_port,
                 esp_err_to_name(err));
        goto fail;
    }

    err = uart_param_config(dev->cfg.uart_port, &uart_config);
    if (err == ESP_OK) {
        err = uart_set_pin(dev->cfg.uart_port, dev->cfg.tx_pin, dev->cfg.rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: config/pin setup failed: %s", dev->cfg.uart_port,
                 esp_err_to_name(err));
        uart_driver_delete(dev->cfg.uart_port);
        goto fail;
    }

    dev->lock = xSemaphoreCreateMutexStatic(&dev->lock_buf);
    uart_flush_input(dev->cfg.uart_port);

    ESP_LOGI(TAG, "PZEM-004T ready on UART%d (TX: %d, RX: %d, Slave: 0x%02X)",
             dev->cfg.uart_port, dev->cfg.tx_pin, dev->cfg.rx_pin, dev->cfg.slave_addr);

    *out_handle = dev;
    return ESP_OK;

fail:
    portENTER_CRITICAL(&s_pool_lock);
    dev->in_use = false;
    portEXIT_CRITICAL(&s_pool_lock);
    return err;
}

esp_err_t pzem004t_delete(pzem004t_handle_t handle)
{
    if (!pzem_handle_valid(handle)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Wait for any in-flight transaction to finish before tearing down.
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    uart_driver_delete(handle->cfg.uart_port);
    vSemaphoreDelete(handle->lock);
    handle->lock = NULL;

    portENTER_CRITICAL(&s_pool_lock);
    handle->in_use = false;
    portEXIT_CRITICAL(&s_pool_lock);
    return ESP_OK;
}

esp_err_t pzem004t_read(pzem004t_handle_t handle, pzem004t_reading_t *out_reading)
{
    if (!pzem_handle_valid(handle) || out_reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Modbus-RTU request: read 10 input registers starting at 0x0000
    uint8_t req[8] = {
        handle->cfg.slave_addr, PZEM_FUNC_READ_INPUT,
        0x00, 0x00,
        0x00, PZEM_READ_REG_COUNT,
    };
    pzem_append_crc(req, 6);

    uint8_t resp[PZEM_READ_RESP_LEN];

    if (xSemaphoreTake(handle->lock, pdMS_TO_TICKS(PZEM_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = pzem_transact(handle, req, sizeof(req), resp, sizeof(resp),
                                  PZEM_FUNC_READ_INPUT);
    xSemaphoreGive(handle->lock);

    if (err != ESP_OK) {
        return err;
    }

    if (resp[2] != PZEM_READ_REG_COUNT * 2) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: unexpected byte count %d",
                 handle->cfg.uart_port, handle->cfg.slave_addr, resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Parse measurement registers (data starts at offset 3).
    // 32-bit values are sent low word first, each word big-endian.
    #define REG16(off) (((uint16_t)resp[(off)] << 8) | resp[(off) + 1])
    #define REG32(off) (((uint32_t)REG16((off) + 2) << 16) | REG16(off))

    out_reading->voltage = REG16(3) * 0.1f;          // 0x0000: 0.1V
    out_reading->current = REG32(5) * 0.001f;        // 0x0001-0x0002: 0.001A
    out_reading->power = REG32(9) * 0.1f;            // 0x0003-0x0004: 0.1W
    out_reading->energy = (float)REG32(13);          // 0x0005-0x0006: 1Wh
    out_reading->frequency = REG16(17) * 0.1f;       // 0x0007: 0.1Hz
    out_reading->pf = REG16(19) * 0.01f;             // 0x0008: 0.01
    out_reading->alarms = REG16(21);                 // 0x0009: alarm status

    #undef REG32
    #undef REG16

    return ESP_OK;
}

esp_err_t pzem004t_reset_energy(pzem004t_handle_t handle)
{
    if (!pzem_handle_valid(handle)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t req[4] = { handle->cfg.slave_addr, PZEM_FUNC_RESET_ENERGY };
    pzem_append_crc(req, 2);

    // Buffer sized for the larger of the normal (4) and exception (5) replies.
    uint8_t resp[PZEM_EXCEPTION_LEN];

    if (xSemaphoreTake(handle->lock, pdMS_TO_TICKS(PZEM_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = pzem_transact(handle, req, sizeof(req), resp, PZEM_RESET_RESP_LEN,
                                  PZEM_FUNC_RESET_ENERGY);
    xSemaphoreGive(handle->lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART%d addr 0x%02X: energy accumulator reset",
                 handle->cfg.uart_port, handle->cfg.slave_addr);
    }
    return err;
}
