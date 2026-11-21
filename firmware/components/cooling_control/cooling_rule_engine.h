#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOLING_MAX_RULE_POINTS 8
#define COOLING_MIN_RULE_POINTS 2
#define COOLING_RULE_MAX_OUTPUT_MV 10000

typedef enum {
  COOLING_CURVE_LINEAR = 0,
  COOLING_CURVE_STEP = 1,
} cooling_curve_type_t;

typedef struct {
  int32_t temp_c_x10;
  uint16_t output_mv;
} cooling_rule_point_t;

typedef struct {
  bool auto_enable;
  cooling_curve_type_t curve_type;
  uint8_t point_count;
  cooling_rule_point_t points[COOLING_MAX_RULE_POINTS];
  uint32_t min_off_sec;
  uint32_t test_timeout_sec;
} cooling_settings_t;

/**
 * @brief Initialize cooling settings with default 2-point rule curve.
 *
 * Defaults:
 *   - P0: 24.0 C (240 temp_c_x10) -> 5000 mV (5.0V)
 *   - P1: 35.0 C (350 temp_c_x10) -> 10000 mV (10.0V)
 *   - curve_type: COOLING_CURVE_LINEAR
 *   - point_count: 2
 *   - min_off_sec: 180
 *   - test_timeout_sec: 30
 *   - auto_enable: false
 *
 * @param[out] settings Pointer to settings structure to populate.
 */
void cooling_settings_set_defaults(cooling_settings_t *settings);

/**
 * @brief Validate cooling curve settings.
 *
 * Checks:
 *   - settings != NULL
 *   - curve_type is COOLING_CURVE_LINEAR or COOLING_CURVE_STEP
 *   - 2 <= point_count <= COOLING_MAX_RULE_POINTS (8)
 *   - strictly ascending temperatures: T0 < T1 < ... < T_{N-1}
 *   - all output_mv <= COOLING_RULE_MAX_OUTPUT_MV (10000 mV)
 *
 * @param[in] settings Pointer to settings to validate.
 * @return true if settings are valid, false otherwise.
 */
bool cooling_validate_settings(const cooling_settings_t *settings);

/**
 * @brief Evaluate rule curve for a given temperature.
 *
 * Fail-safe / Clamping:
 *   - If settings == NULL or invalid -> return 0 mV.
 *   - If temp_c is NaN -> return 0 mV.
 *   - If temp_c < T0 -> return 0 mV.
 *   - If temp_c >= T_{N-1} -> return V_{N-1}.
 *   - If Ti <= temp_c < T_{i+1}:
 *       - COOLING_CURVE_LINEAR:
 *           V = Vi + round((temp_c - Ti) * (V_{i+1} - Vi) / (T_{i+1} - Ti))
 *       - COOLING_CURVE_STEP:
 *           V = Vi
 *
 * @param[in] settings Pointer to cooling settings.
 * @param[in] temp_c Sensor temperature in degrees Celsius.
 * @return Target output in millivolts (0 to 10000 mV).
 */
uint16_t cooling_evaluate_rule_curve(const cooling_settings_t *settings, float temp_c);

#ifdef __cplusplus
}
#endif
