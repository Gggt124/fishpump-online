#include "api_json.h"

#include "alarm_engine.h"
#include "dry_run_guard.h"
#include "esp_timer.h"

uint32_t sensors_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *dry_run_state_name(uint8_t state) {
  switch (state) {
  case DRY_RUN_MONITOR:
    return "monitor";
  case DRY_RUN_WAIT:
    return "wait";
  case DRY_RUN_LOCKED:
    return "locked";
  default:
    return "disabled";
  }
}

const char *alarms_type_name(uint8_t type) {
  switch (type) {
  case ALARM_EVT_RAISED:
    return "raised";
  case ALARM_EVT_CLEARED:
    return "cleared";
  default:
    return "acked";
  }
}
