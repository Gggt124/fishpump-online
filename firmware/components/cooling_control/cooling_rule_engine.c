#include "cooling_rule_engine.h"

#if __has_include("app_config.h")
#include "app_config.h"
#endif

#include <math.h>
#include <string.h>

#ifndef APP_TEMPLATE_COOLING_DEFAULT_P0_TEMP_X10
#define APP_TEMPLATE_COOLING_DEFAULT_P0_TEMP_X10 240
#endif

#ifndef APP_TEMPLATE_COOLING_DEFAULT_P0_MV
#define APP_TEMPLATE_COOLING_DEFAULT_P0_MV 5000
#endif

#ifndef APP_TEMPLATE_COOLING_DEFAULT_P1_TEMP_X10
#define APP_TEMPLATE_COOLING_DEFAULT_P1_TEMP_X10 350
#endif

#ifndef APP_TEMPLATE_COOLING_DEFAULT_P1_MV
#define APP_TEMPLATE_COOLING_DEFAULT_P1_MV 10000
#endif

#ifndef APP_TEMPLATE_COOLING_MIN_OFF_SEC
#define APP_TEMPLATE_COOLING_MIN_OFF_SEC 180
#endif

#ifndef APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC
#define APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC 30
#endif

#ifndef APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT
#define APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT false
#endif

void cooling_settings_set_defaults(cooling_settings_t *settings) {
  if (!settings) {
    return;
  }
  memset(settings, 0, sizeof(*settings));
  settings->auto_enable = APP_TEMPLATE_COOLING_AUTO_ENABLE_DEFAULT;
  settings->curve_type = COOLING_CURVE_LINEAR;
  settings->point_count = 2;
  settings->points[0].temp_c_x10 = APP_TEMPLATE_COOLING_DEFAULT_P0_TEMP_X10;
  settings->points[0].output_mv = APP_TEMPLATE_COOLING_DEFAULT_P0_MV;
  settings->points[1].temp_c_x10 = APP_TEMPLATE_COOLING_DEFAULT_P1_TEMP_X10;
  settings->points[1].output_mv = APP_TEMPLATE_COOLING_DEFAULT_P1_MV;
  settings->min_off_sec = APP_TEMPLATE_COOLING_MIN_OFF_SEC;
  settings->test_timeout_sec = APP_TEMPLATE_COOLING_TEST_TIMEOUT_SEC;
}

bool cooling_validate_settings(const cooling_settings_t *settings) {
  if (!settings) {
    return false;
  }
  if (settings->curve_type != COOLING_CURVE_LINEAR &&
      settings->curve_type != COOLING_CURVE_STEP) {
    return false;
  }
  if (settings->point_count < COOLING_MIN_RULE_POINTS ||
      settings->point_count > COOLING_MAX_RULE_POINTS) {
    return false;
  }
  for (uint8_t i = 0; i < settings->point_count; i++) {
    if (settings->points[i].output_mv > COOLING_RULE_MAX_OUTPUT_MV) {
      return false;
    }
    if (i > 0) {
      if (settings->points[i].temp_c_x10 <= settings->points[i - 1].temp_c_x10) {
        return false;
      }
    }
  }
  return true;
}

uint16_t cooling_evaluate_rule_curve(const cooling_settings_t *settings, float temp_c) {
  if (!cooling_validate_settings(settings)) {
    return 0;
  }
  if (!isfinite(temp_c)) {
    return 0;
  }

  uint8_t count = settings->point_count;
  float t_0 = (float)settings->points[0].temp_c_x10 / 10.0f;
  if (temp_c < t_0) {
    return 0;
  }

  float t_last = (float)settings->points[count - 1].temp_c_x10 / 10.0f;
  if (temp_c >= t_last) {
    return settings->points[count - 1].output_mv;
  }

  for (uint8_t i = 0; i < count - 1; i++) {
    float t_i = (float)settings->points[i].temp_c_x10 / 10.0f;
    float t_next = (float)settings->points[i + 1].temp_c_x10 / 10.0f;

    if (temp_c >= t_i && temp_c < t_next) {
      if (settings->curve_type == COOLING_CURVE_STEP) {
        return settings->points[i].output_mv;
      }

      /* COOLING_CURVE_LINEAR */
      float v_i = (float)settings->points[i].output_mv;
      float v_next = (float)settings->points[i + 1].output_mv;
      float span_t = t_next - t_i;
      if (span_t <= 0.0f) {
        return settings->points[i].output_mv;
      }

      float v = v_i + ((temp_c - t_i) * (v_next - v_i) / span_t);
      long v_rounded = lroundf(v);
      if (v_rounded < 0) {
        v_rounded = 0;
      } else if (v_rounded > COOLING_RULE_MAX_OUTPUT_MV) {
        v_rounded = COOLING_RULE_MAX_OUTPUT_MV;
      }
      return (uint16_t)v_rounded;
    }
  }

  return settings->points[count - 1].output_mv;
}
