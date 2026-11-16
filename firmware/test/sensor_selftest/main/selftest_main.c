#include "selftest.h"

int g_pass;
int g_fail;

void app_main(void)
{
    test_sensor_health();
    test_sen0708();
    test_telemetry();
    test_contact_debounce();
    test_signal_registry();
    test_alarm_rules();
    test_alarm_eval();
    test_alarm_eval_wrap();
    test_alarm_store();
    test_uplink_format();
    test_dry_run_config();

    if (g_fail == 0) {
        ESP_LOGI("selftest", "SELFTEST PASS (%d checks)", g_pass);
    } else {
        ESP_LOGE("selftest", "SELFTEST FAIL (%d of %d)", g_fail, g_pass + g_fail);
    }
}
