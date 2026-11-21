#include "cooling_sensor_pt100.h"

#include <string.h>
#include <math.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <max31865.h>

static const char *TAG = "pt100_max31865";

/* MAX31865 hardware constants for PT100 RTD sensor */
#define PT100_R_REF_OHMS      430.0f
#define PT100_R_NOMINAL_OHMS  100.0f
#define PT100_SETTLE_DELAY_MS 50

static max31865_t s_dev;
static max31865_config_t s_config;
static spi_host_device_t s_spi_host = SPI2_HOST;
static bool s_bus_initialized = false;
static bool s_device_initialized = false;

/* Static FreeRTOS mutex to eliminate dynamic allocation and SMP race conditions */
static StaticSemaphore_t s_mutex_buffer;
static SemaphoreHandle_t s_pt100_mutex = NULL;
static portMUX_TYPE s_mutex_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_mutex_created(void)
{
    if (s_pt100_mutex == NULL) {
        taskENTER_CRITICAL(&s_mutex_spinlock);
        if (s_pt100_mutex == NULL) {
            s_pt100_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
        }
        taskEXIT_CRITICAL(&s_mutex_spinlock);
    }
}

esp_err_t cooling_sensor_pt100_init(spi_host_device_t host, gpio_num_t cs_pin,
                                    gpio_num_t mosi_pin, gpio_num_t clk_pin,
                                    gpio_num_t miso_pin)
{
    ensure_mutex_created();
    if (s_pt100_mutex != NULL) {
        if (xSemaphoreTake(s_pt100_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout waiting for MAX31865 mutex in init");
            return ESP_ERR_TIMEOUT;
        }
    }

    if (!GPIO_IS_VALID_OUTPUT_GPIO(cs_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(mosi_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(clk_pin) ||
        !GPIO_IS_VALID_GPIO(miso_pin)) {
        ESP_LOGE(TAG, "Invalid GPIO configuration: CS=%d, MOSI=%d, CLK=%d, MISO=%d",
                 (int)cs_pin, (int)mosi_pin, (int)clk_pin, (int)miso_pin);
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* Clean up any prior device registration before reinitializing */
    if (s_device_initialized) {
        max31865_free_desc(&s_dev);
        s_device_initialized = false;
        memset(&s_dev, 0, sizeof(s_dev));
    }

    /* Initialize SPI bus on designated host if not already initialized */
    spi_bus_config_t buscfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = clk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_DISABLED);
    if (err == ESP_OK) {
        s_bus_initialized = true;
        s_spi_host = host;
        ESP_LOGI(TAG, "SPI bus initialized on host %d (MOSI=%d, MISO=%d, CLK=%d)",
                 (int)host, (int)mosi_pin, (int)miso_pin, (int)clk_pin);
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* Bus was already initialized by another component or earlier call; share it.
         * If we previously initialized the bus, preserve s_bus_initialized ownership. */
        s_spi_host = host;
        ESP_LOGI(TAG, "SPI bus on host %d already active; sharing bus", (int)host);
    } else {
        ESP_LOGE(TAG, "Failed to initialize SPI bus on host %d: %s", (int)host, esp_err_to_name(err));
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return err;
    }

    /* Add MAX31865 device descriptor onto SPI bus */
    memset(&s_dev, 0, sizeof(s_dev));
    err = max31865_init_desc(&s_dev, host, MAX31865_MAX_CLOCK_SPEED_HZ, cs_pin);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "max31865_init_desc failed (CS=%d): %s", (int)cs_pin, esp_err_to_name(err));
        if (s_bus_initialized) {
            spi_bus_free(host);
            s_bus_initialized = false;
        }
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return err;
    }
    s_device_initialized = true;

    /* Configure RTD parameters: ITS-90 standard, R_ref = 430.0 Ohm, PT100 nominal = 100.0 Ohm */
    s_dev.standard = MAX31865_ITS90;
    s_dev.r_ref = PT100_R_REF_OHMS;
    s_dev.rtd_nominal = PT100_R_NOMINAL_OHMS;

    /* Configure operating mode: 3-wire RTD, V_bias enabled, 50Hz filter, single-shot mode */
    s_config.mode = MAX31865_MODE_SINGLE;
    s_config.connection = MAX31865_3WIRE;
    s_config.v_bias = true;
    s_config.filter = MAX31865_FILTER_50HZ;

    err = max31865_set_config(&s_dev, &s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "max31865_set_config failed: %s", esp_err_to_name(err));
        max31865_free_desc(&s_dev);
        s_device_initialized = false;
        if (s_bus_initialized) {
            spi_bus_free(host);
            s_bus_initialized = false;
        }
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return err;
    }

    /* Chip presence verification: read back configuration register to ensure device is responding */
    max31865_config_t readback_cfg;
    err = max31865_get_config(&s_dev, &readback_cfg);
    if (err != ESP_OK ||
        readback_cfg.connection != s_config.connection ||
        readback_cfg.v_bias != s_config.v_bias ||
        readback_cfg.filter != s_config.filter ||
        readback_cfg.mode != s_config.mode) {
        ESP_LOGE(TAG, "MAX31865 chip presence verification failed (readback mismatch or communication error)");
        max31865_free_desc(&s_dev);
        s_device_initialized = false;
        if (s_bus_initialized) {
            spi_bus_free(host);
            s_bus_initialized = false;
        }
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return (err != ESP_OK) ? err : ESP_ERR_NOT_FOUND;
    }

    /* Check and clear any power-on fault status */
    uint8_t initial_fault = 0;
    if (max31865_get_fault_status(&s_dev, &initial_fault) == ESP_OK && initial_fault != 0) {
        ESP_LOGI(TAG, "Clearing initial MAX31865 fault bits 0x%02X: %s",
                 initial_fault, cooling_sensor_pt100_fault_str(initial_fault));
        (void)max31865_clear_fault_status(&s_dev);
        (void)max31865_set_config(&s_dev, &s_config);
    }

    /* Settle delay to allow input capacitors to charge with V_bias enabled */
    vTaskDelay(pdMS_TO_TICKS(PT100_SETTLE_DELAY_MS));

    ESP_LOGI(TAG, "MAX31865 PT100 sensor initialized (Host=%d, CS=%d, 3-Wire, Rref=%.1f, 50Hz filter)",
             (int)host, (int)cs_pin, PT100_R_REF_OHMS);

    if (s_pt100_mutex != NULL) {
        xSemaphoreGive(s_pt100_mutex);
    }
    return ESP_OK;
}

esp_err_t cooling_sensor_pt100_read(float *out_temp_c, uint8_t *out_fault_status)
{
    if (out_temp_c == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_fault_status != NULL) {
        *out_fault_status = 0;
    }

    ensure_mutex_created();
    if (s_pt100_mutex != NULL) {
        if (xSemaphoreTake(s_pt100_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGE(TAG, "Timeout acquiring MAX31865 mutex in read");
            return ESP_ERR_TIMEOUT;
        }
    }

    /* Verify device initialized state safely within mutex lock to prevent race with deinit */
    if (!s_device_initialized) {
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return ESP_ERR_INVALID_STATE;
    }

    /* Run full single-shot measurement cycle (~70ms conversion wait) */
    esp_err_t err = max31865_measure(&s_dev, out_temp_c);
    if (err != ESP_OK) {
        uint8_t fault_bits = 0;
        esp_err_t fault_err = max31865_get_fault_status(&s_dev, &fault_bits);
        if (fault_err == ESP_OK) {
            if (out_fault_status != NULL) {
                *out_fault_status = fault_bits;
            }
            ESP_LOGW(TAG, "MAX31865 fault reading (code 0x%02X: %s)",
                     fault_bits, cooling_sensor_pt100_fault_str(fault_bits));
        } else {
            ESP_LOGW(TAG, "Failed to retrieve MAX31865 fault status register: %s",
                     esp_err_to_name(fault_err));
        }

        /* Clear fault status and reapply configuration as required by datasheet */
        esp_err_t clr_err = max31865_clear_fault_status(&s_dev);
        if (clr_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to clear MAX31865 fault: %s", esp_err_to_name(clr_err));
        }

        esp_err_t reconfig_err = max31865_set_config(&s_dev, &s_config);
        if (reconfig_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to reapply config post-fault clear: %s", esp_err_to_name(reconfig_err));
        }

        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return err;
    }

    /* Sanity check valid PT100 temperature range (-200°C to +850°C) and NaN */
    if (isnan(*out_temp_c) || *out_temp_c < -200.0f || *out_temp_c > 850.0f) {
        ESP_LOGW(TAG, "MAX31865 measurement out of physical range: %.2f C", *out_temp_c);
        if (s_pt100_mutex != NULL) {
            xSemaphoreGive(s_pt100_mutex);
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (out_fault_status != NULL) {
        *out_fault_status = 0;
    }

    if (s_pt100_mutex != NULL) {
        xSemaphoreGive(s_pt100_mutex);
    }
    return ESP_OK;
}

esp_err_t cooling_sensor_pt100_deinit(void)
{
    ensure_mutex_created();
    if (s_pt100_mutex != NULL) {
        xSemaphoreTake(s_pt100_mutex, portMAX_DELAY);
    }

    esp_err_t ret = ESP_OK;

    if (s_device_initialized) {
        esp_err_t err = max31865_free_desc(&s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "max31865_free_desc failed: %s", esp_err_to_name(err));
            ret = err;
        }
        s_device_initialized = false;
        memset(&s_dev, 0, sizeof(s_dev));
    }

    if (s_bus_initialized) {
        esp_err_t err = spi_bus_free(s_spi_host);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "spi_bus_free on host %d failed: %s", (int)s_spi_host, esp_err_to_name(err));
            if (ret == ESP_OK) {
                ret = err;
            }
        }
        s_bus_initialized = false;
    }

    if (s_pt100_mutex != NULL) {
        xSemaphoreGive(s_pt100_mutex);
    }
    return ret;
}

bool cooling_sensor_pt100_is_initialized(void)
{
    ensure_mutex_created();
    bool init = false;
    if (s_pt100_mutex != NULL) {
        if (xSemaphoreTake(s_pt100_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            init = s_device_initialized;
            xSemaphoreGive(s_pt100_mutex);
            return init;
        }
    }
    return s_device_initialized;
}

const char *cooling_sensor_pt100_fault_str(uint8_t fault_bits)
{
    if (fault_bits == 0) {
        return "No fault";
    }
    if (fault_bits & COOLING_PT100_FAULT_HIGH_THRESH) {
        return "RTD high threshold exceeded";
    }
    if (fault_bits & COOLING_PT100_FAULT_LOW_THRESH) {
        return "RTD low threshold exceeded";
    }
    if (fault_bits & COOLING_PT100_FAULT_REFIN_HIGH) {
        return "REFIN- > 0.85 x V_BIAS";
    }
    if (fault_bits & COOLING_PT100_FAULT_REFIN_LOW) {
        return "REFIN- < 0.85 x V_BIAS (FORCE- open)";
    }
    if (fault_bits & COOLING_PT100_FAULT_RTDIN_LOW) {
        return "RTDIN- < 0.85 x V_BIAS (FORCE- open)";
    }
    if (fault_bits & COOLING_PT100_FAULT_OV_UV) {
        return "Overvoltage / undervoltage";
    }
    return "Unknown fault";
}
