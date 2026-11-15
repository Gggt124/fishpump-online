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
void test_telemetry(void);
void test_contact_debounce(void);
void test_signal_registry(void);
void test_alarm_rules(void);
void test_alarm_eval(void);
void test_alarm_eval_wrap(void);
void test_alarm_store(void);
