#include "selftest.h"
#include "sensor_health.h"

void test_sensor_health(void)
{
    sensor_health_t h;
    sensor_health_init(&h);
    CHECK(!h.valid && !h.fault && !h.ever_ok);

    sensor_health_record(&h, false);
    sensor_health_record(&h, false);
    CHECK(!h.valid && !h.fault); /* 2 misses: not yet fault */

    sensor_health_record(&h, false);
    CHECK(!h.valid && h.fault); /* 3rd miss: fault */

    sensor_health_record(&h, true);
    CHECK(h.valid && !h.fault && h.ever_ok && h.consecutive_failures == 0);

    sensor_health_record(&h, false);
    CHECK(!h.valid && !h.fault); /* single miss after a good read: transient */

    sensor_health_mark_init_failed(&h);
    CHECK(h.fault && !h.valid);

    for (int i = 0; i < 300; i++) {
        sensor_health_record(&h, false);
    }
    CHECK(h.fault && h.consecutive_failures == 255); /* saturates, no wrap */
}
