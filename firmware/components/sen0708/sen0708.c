#include "sen0708.h"

#include <math.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sen0708_proto.h"

static const char *TAG = "sen0708";

#define SEN0708_DEFAULT_BAUD    4800
#define SEN0708_DEFAULT_ADDR    0x01
#define SEN0708_UART_BUF_SIZE   256
#define SEN0708_REPLY_TIMEOUT_MS 200
#define SEN0708_LOCK_TIMEOUT_MS 1000
/* Modbus RTU needs >= 3.5 character times of silence between frames
 * (about 8 ms at 4800 baud). 10 ms keeps a margin at every supported baud. */
#define SEN0708_FRAME_GAP_MS    10

struct sen0708_dev {
    bool in_use;
    sen0708_config_t cfg;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buf;
};

/* One SEN0708 in this system, so a single static slot (no heap). */
static struct sen0708_dev s_dev;
static portMUX_TYPE s_slot_lock = portMUX_INITIALIZER_UNLOCKED;

#if CONFIG_SEN0708_LOG_RAW_FRAMES
static void log_frame(const char *dir, const uint8_t *buf, size_t len)
{
    char line[3 * SEN0708_MAX_FRAME_LEN + 1];
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 <= sizeof(line); i++) {
        pos += (size_t)snprintf(&line[pos], sizeof(line) - pos, "%02X ", buf[i]);
    }
    line[pos] = '\0';
    ESP_LOGI(TAG, "%s %s", dir, line);
}
#else
#define log_frame(dir, buf, len) ((void)0)
#endif

static esp_err_t status_to_err(sen0708_proto_status_t st)
{
    switch (st) {
    case SEN0708_PROTO_OK: return ESP_OK;
    case SEN0708_PROTO_ERR_ARG: return ESP_ERR_INVALID_ARG;
    case SEN0708_PROTO_ERR_SHORT: return ESP_ERR_TIMEOUT;
    case SEN0708_PROTO_ERR_CRC: return ESP_ERR_INVALID_CRC;
    default: return ESP_ERR_INVALID_RESPONSE;
    }
}

/**
 * Send `req` and read the reply into `resp`. Validates the reply fully.
 * Caller must hold dev->lock.
 */
static esp_err_t transact(struct sen0708_dev *dev, const uint8_t *req, size_t req_len,
                          uint8_t *resp)
{
    const uart_port_t port = dev->cfg.uart_port;
    const uint8_t addr = dev->cfg.slave_addr;
    size_t need = sen0708_proto_reply_len(req, req_len);
    if (need == 0 || need > SEN0708_MAX_FRAME_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    vTaskDelay(pdMS_TO_TICKS(SEN0708_FRAME_GAP_MS));
    uart_flush_input(port);

    log_frame("TX", req, req_len);
    int written = uart_write_bytes(port, (const char *)req, req_len);
    if (written != (int)req_len) {
        ESP_LOGE(TAG, "UART%d addr 0x%02X: failed to write request", port, addr);
        return ESP_FAIL;
    }

    size_t total = 0;
    bool is_exception = false;
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)SEN0708_REPLY_TIMEOUT_MS * 1000;

    while (total < need) {
        int n = uart_read_bytes(port, &resp[total], need - total, pdMS_TO_TICKS(20));
        if (n > 0) {
            total += (size_t)n;
        }
        if (!is_exception && sen0708_proto_is_exception(resp, total, req[1])) {
            is_exception = true;
            need = SEN0708_EXCEPTION_LEN;
        }
        if (esp_timer_get_time() > deadline_us) {
            break;
        }
    }
    log_frame("RX", resp, total);

    if (total == 0) {
        ESP_LOGD(TAG, "UART%d addr 0x%02X: no reply", port, addr);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t exc = 0;
    sen0708_proto_status_t st = sen0708_proto_check_reply(req, req_len, resp, total, &exc);
    if (st == SEN0708_PROTO_ERR_EXCEPTION) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: Modbus exception 0x%02X for func 0x%02X", port, addr,
                 exc, req[1]);
    } else if (st != SEN0708_PROTO_OK) {
        ESP_LOGW(TAG, "UART%d addr 0x%02X: %s (%u bytes)", port, addr,
                 sen0708_proto_status_str(st), (unsigned)total);
    }
    return status_to_err(st);
}

static bool handle_valid(sen0708_handle_t h)
{
    return h == &s_dev && h->in_use;
}

static esp_err_t lock_dev(struct sen0708_dev *dev)
{
    return xSemaphoreTake(dev->lock, pdMS_TO_TICKS(SEN0708_LOCK_TIMEOUT_MS)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static esp_err_t read_regs(struct sen0708_dev *dev, uint16_t start, uint16_t count,
                           uint16_t *regs)
{
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    uint8_t resp[SEN0708_MAX_FRAME_LEN];
    size_t len = sen0708_proto_build_read(dev->cfg.slave_addr, start, count, req);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_dev(dev);
    if (err != ESP_OK) {
        return err;
    }
    err = transact(dev, req, len, resp);
    xSemaphoreGive(dev->lock);
    if (err == ESP_OK) {
        sen0708_proto_get_regs(resp, count, regs);
    }
    return err;
}

static esp_err_t write_frame(struct sen0708_dev *dev, const uint8_t *req, size_t len)
{
    uint8_t resp[SEN0708_MAX_FRAME_LEN];
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_dev(dev);
    if (err != ESP_OK) {
        return err;
    }
    err = transact(dev, req, len, resp);
    xSemaphoreGive(dev->lock);
    return err;
}

esp_err_t sen0708_new(const sen0708_config_t *config, sen0708_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    portENTER_CRITICAL(&s_slot_lock);
    if (s_dev.in_use) {
        portEXIT_CRITICAL(&s_slot_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_dev.in_use = true;
    portEXIT_CRITICAL(&s_slot_lock);

    struct sen0708_dev *dev = &s_dev;
    dev->cfg = *config;
    if (dev->cfg.baud == 0) {
        dev->cfg.baud = SEN0708_DEFAULT_BAUD;
    }
    if (dev->cfg.slave_addr == 0) {
        dev->cfg.slave_addr = SEN0708_DEFAULT_ADDR;
    }

    const uart_config_t uart_config = {
        .baud_rate = (int)dev->cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(dev->cfg.uart_port, SEN0708_UART_BUF_SIZE, 0, 0, NULL, 0);
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
        ESP_LOGE(TAG, "UART%d: config failed: %s", dev->cfg.uart_port, esp_err_to_name(err));
        uart_driver_delete(dev->cfg.uart_port);
        goto fail;
    }

    dev->lock = xSemaphoreCreateMutexStatic(&dev->lock_buf);

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d %lu 8N1, addr 0x%02X", dev->cfg.uart_port,
             dev->cfg.tx_pin, dev->cfg.rx_pin, (unsigned long)dev->cfg.baud,
             dev->cfg.slave_addr);
    *out_handle = dev;
    return ESP_OK;

fail:
    portENTER_CRITICAL(&s_slot_lock);
    s_dev.in_use = false;
    portEXIT_CRITICAL(&s_slot_lock);
    return err;
}

esp_err_t sen0708_delete(sen0708_handle_t handle)
{
    if (!handle_valid(handle)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lock_dev(handle) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    uart_driver_delete(handle->cfg.uart_port);
    xSemaphoreGive(handle->lock);
    vSemaphoreDelete(handle->lock);
    portENTER_CRITICAL(&s_slot_lock);
    handle->in_use = false;
    portEXIT_CRITICAL(&s_slot_lock);
    return ESP_OK;
}

esp_err_t sen0708_read(sen0708_handle_t handle, sen0708_reading_t *out)
{
    if (!handle_valid(handle) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t regs[2];
    esp_err_t err = read_regs(handle, SEN0708_REG_PH, 2, regs);
    if (err != ESP_OK) {
        return err;
    }
    sen0708_proto_status_t st =
        sen0708_proto_decode_reading(regs[0], regs[1], &out->ph, &out->temperature_c);
    if (st != SEN0708_PROTO_OK) {
        ESP_LOGW(TAG, "reading out of range: raw pH=%u raw temp=%d", regs[0], (int16_t)regs[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t sen0708_get_offsets(sen0708_handle_t handle, sen0708_offsets_t *out)
{
    if (!handle_valid(handle) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t regs[2];
    esp_err_t err = read_regs(handle, SEN0708_REG_PH_OFFSET, 2, regs);
    if (err == ESP_OK) {
        out->ph_offset = sen0708_proto_decode_signed(regs[0], 100.0f);
        out->temp_offset = sen0708_proto_decode_signed(regs[1], 10.0f);
    }
    return err;
}

esp_err_t sen0708_set_ph_offset(sen0708_handle_t handle, float offset)
{
    if (!handle_valid(handle) || !isfinite(offset) || fabsf(offset) > SEN0708_PH_OFFSET_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    size_t len = sen0708_proto_build_write_single(
        handle->cfg.slave_addr, SEN0708_REG_PH_OFFSET,
        sen0708_proto_encode_signed(offset, 100.0f), req);
    return write_frame(handle, req, len);
}

esp_err_t sen0708_set_temp_offset(sen0708_handle_t handle, float offset)
{
    if (!handle_valid(handle) || !isfinite(offset) ||
        fabsf(offset) > SEN0708_TEMP_OFFSET_LIMIT) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    size_t len = sen0708_proto_build_write_single(
        handle->cfg.slave_addr, SEN0708_REG_TEMP_OFFSET,
        sen0708_proto_encode_signed(offset, 10.0f), req);
    return write_frame(handle, req, len);
}

esp_err_t sen0708_calibrate(sen0708_handle_t handle, uint8_t point, float buffer_ph)
{
    if (!handle_valid(handle) || (point != 1 && point != 2) || !isfinite(buffer_ph) ||
        buffer_ph < SEN0708_PH_MIN || buffer_ph > SEN0708_PH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t values[2] = {point, (uint16_t)lroundf(buffer_ph * 100.0f)};
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    size_t len = sen0708_proto_build_write_multi(handle->cfg.slave_addr,
                                                 SEN0708_REG_CALIBRATION, 2, values, req);
    esp_err_t err = write_frame(handle, req, len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "calibration point %u = pH %.2f written", point, buffer_ph);
    }
    return err;
}
