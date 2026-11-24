#pragma once

#include <stdint.h>

/* Timed pump stop: one task wakes every second and restarts the water pump when
 * a resume armed through pump.control falls due. Safe before init: arm/cancel do
 * nothing and remaining_sec returns 0. */
void pump_resume_service_init(void);
void pump_resume_service_arm(uint16_t minutes);
void pump_resume_service_cancel(void);
uint32_t pump_resume_service_remaining_sec(void);
