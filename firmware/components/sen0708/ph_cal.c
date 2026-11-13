#include "ph_cal.h"

#include <math.h>
#include <string.h>

void ph_cal_init(ph_cal_t *cal)
{
    memset(cal, 0, sizeof(*cal));
    cal->state = PH_CAL_IDLE;
    cal->last_error = PH_CAL_ERR_NONE;
}

static bool in_range(uint8_t point, float buffer_ph)
{
    if (!isfinite(buffer_ph)) {
        return false;
    }
    if (point == 1) {
        return buffer_ph >= PH_CAL_POINT1_MIN && buffer_ph <= PH_CAL_POINT1_MAX;
    }
    if (point == 2) {
        return buffer_ph >= PH_CAL_POINT2_MIN && buffer_ph <= PH_CAL_POINT2_MAX;
    }
    return false;
}

ph_cal_error_t ph_cal_begin_point(ph_cal_t *cal, uint8_t point, float buffer_ph, bool stable,
                                  bool sensor_ok)
{
    ph_cal_error_t err = PH_CAL_ERR_NONE;

    if (point != 1 && point != 2) {
        err = PH_CAL_ERR_WRONG_STEP;
    } else if (point == 2 && cal->state != PH_CAL_WAITING_POINT2) {
        err = PH_CAL_ERR_WRONG_STEP;
    } else if (!in_range(point, buffer_ph)) {
        err = PH_CAL_ERR_OUT_OF_RANGE;
    } else if (!sensor_ok) {
        err = PH_CAL_ERR_SENSOR_FAULT;
    } else if (!stable) {
        err = PH_CAL_ERR_NOT_STABLE;
    }

    cal->last_error = err;
    return err;
}

void ph_cal_finish_point(ph_cal_t *cal, uint8_t point, float buffer_ph, bool write_ok,
                         uint32_t now_ms)
{
    if (!write_ok) {
        cal->last_error = PH_CAL_ERR_SENSOR_WRITE;
        return;
    }
    cal->last_error = PH_CAL_ERR_NONE;
    if (point == 1) {
        cal->state = PH_CAL_WAITING_POINT2;
        cal->point1_buffer = buffer_ph;
        cal->point1_time_ms = now_ms;
    } else {
        cal->state = PH_CAL_DONE;
    }
}

void ph_cal_cancel(ph_cal_t *cal)
{
    cal->state = PH_CAL_IDLE;
    cal->last_error = PH_CAL_ERR_NONE;
}

void ph_cal_tick(ph_cal_t *cal, uint32_t now_ms)
{
    /* Unsigned subtraction handles the 32-bit millisecond wrap. */
    if (cal->state == PH_CAL_WAITING_POINT2 &&
        (uint32_t)(now_ms - cal->point1_time_ms) >= PH_CAL_TIMEOUT_MS) {
        cal->state = PH_CAL_IDLE;
        cal->last_error = PH_CAL_ERR_TIMEOUT;
    }
}

const char *ph_cal_state_str(ph_cal_state_t state)
{
    switch (state) {
    case PH_CAL_IDLE: return "idle";
    case PH_CAL_WAITING_POINT2: return "waiting_point2";
    case PH_CAL_DONE: return "done";
    }
    return "idle";
}

const char *ph_cal_error_str(ph_cal_error_t err)
{
    switch (err) {
    case PH_CAL_ERR_NONE: return NULL;
    case PH_CAL_ERR_OUT_OF_RANGE: return "out_of_range";
    case PH_CAL_ERR_NOT_STABLE: return "not_stable";
    case PH_CAL_ERR_WRONG_STEP: return "wrong_step";
    case PH_CAL_ERR_SENSOR_FAULT: return "sensor_fault";
    case PH_CAL_ERR_SENSOR_WRITE: return "sensor_write";
    case PH_CAL_ERR_TIMEOUT: return "timeout";
    }
    return "unknown";
}

void ph_stability_reset(ph_stability_t *s)
{
    memset(s, 0, sizeof(*s));
}

void ph_stability_push(ph_stability_t *s, float ph)
{
    s->samples[s->next] = ph;
    s->next = (uint8_t)((s->next + 1) % PH_STABILITY_SAMPLES);
    if (s->count < PH_STABILITY_SAMPLES) {
        s->count++;
    }
}

bool ph_stability_is_stable(const ph_stability_t *s)
{
    if (s->count < PH_STABILITY_SAMPLES) {
        return false;
    }
    float lo = s->samples[0];
    float hi = s->samples[0];
    for (int i = 1; i < PH_STABILITY_SAMPLES; i++) {
        lo = fminf(lo, s->samples[i]);
        hi = fmaxf(hi, s->samples[i]);
    }
    return (hi - lo) < PH_STABILITY_BAND;
}
