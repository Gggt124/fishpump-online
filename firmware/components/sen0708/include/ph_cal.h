#pragma once

/*
 * Two-point pH calibration state machine and reading-stability tracker.
 *
 * Pure C, no ESP-IDF dependency, so it can be tested without hardware.
 * The caller does the actual sensor write between ph_cal_begin_point()
 * and ph_cal_finish_point().
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accepted buffer ranges per point (point 1 = acid, e.g. 4.01; point 2 = alkaline, e.g. 9.18). */
#define PH_CAL_POINT1_MIN 3.0f
#define PH_CAL_POINT1_MAX 5.0f
#define PH_CAL_POINT2_MIN 8.5f
#define PH_CAL_POINT2_MAX 10.5f

/* waiting_point2 falls back to idle after this long. */
#define PH_CAL_TIMEOUT_MS (10u * 60u * 1000u)

/* A reading is stable when the last N readings differ by less than the band. */
#define PH_STABILITY_SAMPLES 5
#define PH_STABILITY_BAND    0.05f

typedef enum {
    PH_CAL_IDLE,
    PH_CAL_WAITING_POINT2,
    PH_CAL_DONE,
} ph_cal_state_t;

typedef enum {
    PH_CAL_ERR_NONE,
    PH_CAL_ERR_OUT_OF_RANGE, /* buffer value outside the range for this point */
    PH_CAL_ERR_NOT_STABLE,   /* reading still moving */
    PH_CAL_ERR_WRONG_STEP,   /* point 2 before point 1 */
    PH_CAL_ERR_SENSOR_FAULT, /* sensor not answering */
    PH_CAL_ERR_SENSOR_WRITE, /* sensor rejected or did not confirm the write */
    PH_CAL_ERR_TIMEOUT,      /* point 2 not done in time */
} ph_cal_error_t;

typedef struct {
    ph_cal_state_t state;
    ph_cal_error_t last_error;
    float point1_buffer;
    uint32_t point1_time_ms;
} ph_cal_t;

typedef struct {
    float samples[PH_STABILITY_SAMPLES];
    uint8_t count;
    uint8_t next;
} ph_stability_t;

void ph_cal_init(ph_cal_t *cal);

/**
 * Check whether `point` may be written now. Point 1 may always (re)start a
 * calibration; point 2 needs a finished point 1. Returns PH_CAL_ERR_NONE when
 * the caller should write to the sensor. On any other result last_error is set
 * and the state is unchanged.
 */
ph_cal_error_t ph_cal_begin_point(ph_cal_t *cal, uint8_t point, float buffer_ph, bool stable,
                                  bool sensor_ok);

/** Report the result of the sensor write that followed a successful begin. */
void ph_cal_finish_point(ph_cal_t *cal, uint8_t point, float buffer_ph, bool write_ok,
                         uint32_t now_ms);

void ph_cal_cancel(ph_cal_t *cal);

/** Call periodically; handles the waiting_point2 timeout. */
void ph_cal_tick(ph_cal_t *cal, uint32_t now_ms);

const char *ph_cal_state_str(ph_cal_state_t state);
const char *ph_cal_error_str(ph_cal_error_t err); /* NULL for PH_CAL_ERR_NONE */

void ph_stability_reset(ph_stability_t *s);
void ph_stability_push(ph_stability_t *s, float ph);
bool ph_stability_is_stable(const ph_stability_t *s);

#ifdef __cplusplus
}
#endif
