#include "pump_resume.h"

#include <math.h>

void pump_resume_arm(pump_resume_t *r, uint32_t now_ms, uint16_t minutes)
{
    r->armed = true;
    r->due_ms = now_ms + (uint32_t)minutes * 60000u;
}

void pump_resume_cancel(pump_resume_t *r)
{
    r->armed = false;
}

uint32_t pump_resume_remaining_sec(const pump_resume_t *r, uint32_t now_ms)
{
    if (!r->armed) {
        return 0;
    }
    int32_t diff = (int32_t)(r->due_ms - now_ms);
    if (diff <= 0) {
        return 0;
    }
    return ((uint32_t)diff + 999u) / 1000u;
}

bool pump_resume_take_due(pump_resume_t *r, uint32_t now_ms)
{
    if (!r->armed) {
        return false;
    }
    if ((int32_t)(now_ms - r->due_ms) >= 0) {
        r->armed = false;
        return true;
    }
    return false;
}

pump_resume_param_t pump_resume_check_param(bool has_field, bool is_number, double value,
                                            bool is_stop, bool is_water, uint16_t *minutes)
{
    if (!has_field) {
        return PUMP_RESUME_PARAM_NONE;
    }
    if (!is_stop || !is_water) {
        return PUMP_RESUME_PARAM_BAD_COMBO;
    }
    if (!is_number || !isfinite(value) ||
        value < (double)PUMP_RESUME_MIN_MIN || value > (double)PUMP_RESUME_MIN_MAX ||
        (double)(uint16_t)value != value) {
        return PUMP_RESUME_PARAM_OUT_OF_RANGE;
    }
    *minutes = (uint16_t)value;
    return PUMP_RESUME_PARAM_OK;
}
