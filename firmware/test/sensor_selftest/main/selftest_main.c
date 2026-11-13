#include "selftest.h"

int g_pass;
int g_fail;

void app_main(void)
{
    test_sensor_health();

    if (g_fail == 0) {
        ESP_LOGI("selftest", "SELFTEST PASS (%d checks)", g_pass);
    } else {
        ESP_LOGE("selftest", "SELFTEST FAIL (%d of %d)", g_fail, g_pass + g_fail);
    }
}
