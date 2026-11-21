#include <string.h>

#include "alarm_rules.h"

typedef struct {
    const char *name;
    const char *signal;
    uint8_t cond;
    float lo, hi, hyst;
    uint16_t delay;
    uint8_t sev;
    bool latch;
    const char *gate; /* NULL = none */
    bool gate_when;
} preset_t;

/* Spec 5.6. The three "placeholder" rules (7, 8, 9) are templates to be tuned
 * after the real pump and chiller are measured. All start disabled. */
static const preset_t s_presets[] = {
    {"pH ผิดช่วง", "water_ph", ALARM_COND_OUTSIDE, 6.5f, 8.5f, 0.1f, 60, ALARM_SEV_WARNING, false, NULL, false},
    {"น้ำร้อนเกิน", "pt100_temp_c", ALARM_COND_ABOVE, 32.0f, 0, 0.5f, 60, ALARM_SEV_WARNING, false, NULL, false},
    {"ไฟตก/ไฟเกิน", "pump_voltage_v", ALARM_COND_OUTSIDE, 200.0f, 245.0f, 2.0f, 5, ALARM_SEV_WARNING, false, NULL, false},
    {"รีเลย์ปั๊มน้ำค้าง", "pump_current_a", ALARM_COND_ABOVE, 0.1f, 0, 0, 10, ALARM_SEV_CRITICAL, true, "relay1_on", false},
    {"น้ำไหลทั้งที่ปั๊มปิด", "flow_on", ALARM_COND_IS_TRUE, 0, 0, 0, 30, ALARM_SEV_WARNING, false, "relay1_on", false},
    {"ปั๊มน้ำไม่ทำงานตามคำสั่ง", "pump_current_a", ALARM_COND_BELOW, 0.3f, 0, 0, 10, ALARM_SEV_CRITICAL, false, "relay1_on", true},
    {"Chiller ไม่ทำงานตามคำสั่ง", "chiller_power_w", ALARM_COND_BELOW, 50.0f, 0, 0, 180, ALARM_SEV_CRITICAL, false, "cooling_on", true},
    {"Chiller ทำงานทั้งที่สั่งหยุด", "chiller_power_w", ALARM_COND_ABOVE, 200.0f, 0, 0, 120, ALARM_SEV_WARNING, false, "cooling_on", false},
    {"เซนเซอร์ PT100 ไม่ตอบ", "pt100_fault", ALARM_COND_IS_TRUE, 0, 0, 0, 0, ALARM_SEV_CRITICAL, false, NULL, false},
    {"เซนเซอร์ pH ไม่ตอบ", "water_sensor_fault", ALARM_COND_IS_TRUE, 0, 0, 0, 0, ALARM_SEV_WARNING, false, NULL, false},
    {"มิเตอร์ไฟปั๊มน้ำไม่ตอบ", "pump_meter_fault", ALARM_COND_IS_TRUE, 0, 0, 0, 0, ALARM_SEV_WARNING, false, NULL, false},
    {"มิเตอร์ไฟ Chiller ไม่ตอบ", "chiller_meter_fault", ALARM_COND_IS_TRUE, 0, 0, 0, 0, ALARM_SEV_WARNING, false, NULL, false},
};

void alarm_presets_fill(alarm_rule_set_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t n = (uint8_t)(sizeof(s_presets) / sizeof(s_presets[0]));
    for (uint8_t i = 0; i < n; i++) {
        const preset_t *p = &s_presets[i];
        alarm_rule_t *r = &out->rules[i];
        const signal_def_t *sig = signal_registry_by_key(p->signal);
        r->id = (uint16_t)(i + 1);
        r->enabled = false;
        r->signal_id = sig != NULL ? sig->id : 0;
        r->condition = p->cond;
        r->severity = p->sev;
        r->latch = p->latch;
        r->threshold = p->lo;
        r->threshold_hi = p->hi;
        r->hysteresis = p->hyst;
        r->delay_sec = p->delay;
        if (p->gate != NULL) {
            const signal_def_t *g = signal_registry_by_key(p->gate);
            r->gate_mode = p->gate_when ? ALARM_GATE_WHEN_TRUE : ALARM_GATE_WHEN_FALSE;
            r->gate_signal_id = g != NULL ? g->id : 0;
        }
        size_t len = strnlen(p->name, ALARM_NAME_BYTES - 1);
        memcpy(r->name, p->name, len); /* r was zeroed: stays NUL-terminated */
    }
    out->count = n;
    out->next_id = (uint16_t)(n + 1);
}
