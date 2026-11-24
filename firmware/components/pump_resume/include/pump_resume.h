#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PUMP_RESUME_MIN_MIN 5
#define PUMP_RESUME_MIN_MAX 120

typedef struct {
    bool armed;
    uint32_t due_ms;
} pump_resume_t;

typedef enum {
    PUMP_RESUME_PARAM_NONE = 0,
    PUMP_RESUME_PARAM_OK,
    PUMP_RESUME_PARAM_BAD_COMBO,
    PUMP_RESUME_PARAM_OUT_OF_RANGE
} pump_resume_param_t;

/* has_field: the key is present. is_number: cJSON_IsNumber. is_stop/is_water: the parsed
 * pump.control action/system. */
pump_resume_param_t pump_resume_check_param(bool has_field, bool is_number, double value,
                                            bool is_stop, bool is_water, uint16_t *minutes);

void pump_resume_arm(pump_resume_t *r, uint32_t now_ms, uint16_t minutes);
void pump_resume_cancel(pump_resume_t *r);
uint32_t pump_resume_remaining_sec(const pump_resume_t *r, uint32_t now_ms); /* 0 when not armed; rounds up */
bool pump_resume_take_due(pump_resume_t *r, uint32_t now_ms);               /* true once, then disarmed */
