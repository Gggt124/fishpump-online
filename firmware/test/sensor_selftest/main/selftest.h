#pragma once

#include "esp_log.h"

extern int g_pass;
extern int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            g_pass++;                                                   \
        } else {                                                        \
            g_fail++;                                                   \
            ESP_LOGE("selftest", "FAIL %s:%d: %s", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

void test_sensor_health(void);
void test_sen0708(void);
