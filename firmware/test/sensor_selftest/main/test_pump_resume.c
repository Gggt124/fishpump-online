#include "selftest.h"
#include "pump_resume.h"

void test_pump_resume(void)
{
    uint16_t m = 0;
    CHECK(pump_resume_check_param(false, false, 0, true, true, &m) == PUMP_RESUME_PARAM_NONE);
    CHECK(pump_resume_check_param(true, true, 15, true, true, &m) == PUMP_RESUME_PARAM_OK && m == 15);
    CHECK(pump_resume_check_param(true, true, 5, true, true, &m) == PUMP_RESUME_PARAM_OK);
    CHECK(pump_resume_check_param(true, true, 120, true, true, &m) == PUMP_RESUME_PARAM_OK);
    CHECK(pump_resume_check_param(true, true, 4, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
    CHECK(pump_resume_check_param(true, true, 121, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
    CHECK(pump_resume_check_param(true, true, 15.5, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
    CHECK(pump_resume_check_param(true, false, 0, true, true, &m) == PUMP_RESUME_PARAM_OUT_OF_RANGE);
    CHECK(pump_resume_check_param(true, true, 15, false, true, &m) == PUMP_RESUME_PARAM_BAD_COMBO); /* start, incl. default action */
    CHECK(pump_resume_check_param(true, true, 15, true, false, &m) == PUMP_RESUME_PARAM_BAD_COMBO); /* wash only */

    pump_resume_t r = {0};
    CHECK(pump_resume_remaining_sec(&r, 1000) == 0 && !pump_resume_take_due(&r, 1000));
    pump_resume_arm(&r, 1000, 15);
    CHECK(pump_resume_remaining_sec(&r, 1000) == 900);
    CHECK(pump_resume_remaining_sec(&r, 1500) == 900);          /* rounds up */
    CHECK(!pump_resume_take_due(&r, 1000 + 899999u));
    CHECK(pump_resume_take_due(&r, 1000 + 900000u));
    CHECK(!r.armed && !pump_resume_take_due(&r, 1000 + 900001u)); /* once */
    pump_resume_arm(&r, 0, 30); pump_resume_cancel(&r);
    CHECK(!pump_resume_take_due(&r, 30u * 60000u) && pump_resume_remaining_sec(&r, 0) == 0);
    /* millis wrap */
    pump_resume_arm(&r, 0xFFFFF000u, 5);
    CHECK(!pump_resume_take_due(&r, 0xFFFFFFFFu));
    CHECK(pump_resume_remaining_sec(&r, 0x00000F00u) == 293u);     /* 7936 ms gone, 292.064 s left, rounds up */
    CHECK(pump_resume_take_due(&r, 0xFFFFF000u + 300000u));
}
