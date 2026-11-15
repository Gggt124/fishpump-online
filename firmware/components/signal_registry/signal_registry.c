#include "signal_registry.h"

#include <math.h>
#include <string.h>

#define N SIGNAL_KIND_NUMBER
#define B SIGNAL_KIND_BOOL

/* id, key, label, unit, group, kind, source, decimals, period_sec */
#define PWR(base, src, pfx, who, grp, per)                                                   \
    {(base) + 0, pfx "_voltage_v", "แรงดัน" who, "V", grp, N, src, 1, per},                  \
    {(base) + 1, pfx "_current_a", "กระแส" who, "A", grp, N, src, 3, per},                   \
    {(base) + 2, pfx "_power_w", "กำลังไฟ" who, "W", grp, N, src, 1, per},                   \
    {(base) + 3, pfx "_energy_wh", "พลังงานสะสม" who, "Wh", grp, N, src, 0, per},            \
    {(base) + 4, pfx "_frequency_hz", "ความถี่ไฟ" who, "Hz", grp, N, src, 1, per},           \
    {(base) + 5, pfx "_pf", "PF " who, "", grp, N, src, 2, per},                             \
    {(base) + 6, pfx "_meter_fault", "มิเตอร์ไฟ" who "ไม่ตอบ", "", grp, B, src, 0, per}

static const signal_def_t s_signals[] = {
    PWR(1, SIGNAL_SRC_POWER_PUMP, "pump", "ปั๊มน้ำ", "pump_power", 2),
    PWR(11, SIGNAL_SRC_POWER_CHILLER, "chiller", "Chiller", "chiller_power", 2),

    {21, "water_ph", "pH", "pH", "water", N, SIGNAL_SRC_WATER, 2, 2},
    {22, "water_temp_c", "อุณหภูมิน้ำ (หัว pH)", "°C", "water", N, SIGNAL_SRC_WATER, 1, 2},
    {23, "water_ph_stable", "ค่า pH นิ่ง", "", "water", B, SIGNAL_SRC_WATER, 0, 2},
    {24, "water_sensor_fault", "เซนเซอร์ pH ไม่ตอบ", "", "water", B, SIGNAL_SRC_WATER, 0, 2},

    {31, "pt100_temp_c", "อุณหภูมิน้ำ (PT100)", "°C", "cooling", N, SIGNAL_SRC_COOLING, 2, 1},
    {32, "cooling_duty_pct", "ระดับสั่งงาน cooling", "%", "cooling", N, SIGNAL_SRC_COOLING, 1, 1},
    {33, "cooling_output_mv", "แรงดันสั่ง inverter", "mV", "cooling", N, SIGNAL_SRC_COOLING, 0, 1},
    {34, "cooling_mode", "โหมด cooling", "", "cooling", N, SIGNAL_SRC_COOLING, 0, 1},
    {35, "cooling_lockout_sec", "เวลาหน่วงคอมเพรสเซอร์", "s", "cooling", N, SIGNAL_SRC_COOLING, 0, 1},
    {36, "pt100_fault", "เซนเซอร์ PT100 ไม่ตอบ", "", "cooling", B, SIGNAL_SRC_COOLING, 0, 1},
    {37, "cooling_on", "cooling กำลังสั่งงาน", "", "cooling", B, SIGNAL_SRC_COOLING, 0, 1},

    {41, "relay1_on", "รีเลย์ 1 (ปั๊มน้ำ) เปิด", "", "pump", B, SIGNAL_SRC_PUMP, 0, 0},
    {42, "relay2_on", "รีเลย์ 2 (ล้าง) เปิด", "", "pump", B, SIGNAL_SRC_PUMP, 0, 0},
    {43, "pump_is_running", "ระบบปั๊มทำงาน", "", "pump", B, SIGNAL_SRC_PUMP, 0, 0},
    {44, "pump_active_set", "ชุดเวลาที่ใช้", "", "pump", N, SIGNAL_SRC_PUMP, 0, 0},

    {51, "float_high", "ลูกลอยลอย", "", "float", B, SIGNAL_SRC_FLOAT, 0, 0},
    {52, "flow_on", "น้ำไหล", "", "pump", B, SIGNAL_SRC_FLOW, 0, 5},

    {61, "free_heap_bytes", "หน่วยความจำว่าง", "B", "system", N, SIGNAL_SRC_HEALTH, 0, 5},
    {62, "free_psram_bytes", "PSRAM ว่าง", "B", "system", N, SIGNAL_SRC_HEALTH, 0, 5},
    {63, "wifi_rssi_dbm", "สัญญาณ Wi-Fi", "dBm", "system", N, SIGNAL_SRC_HEALTH, 0, 5},
    {64, "uptime_sec", "เวลาทำงาน", "s", "system", N, SIGNAL_SRC_HEALTH, 0, 5},
    {65, "oom_strikes", "จำนวนครั้งหน่วยความจำต่ำ", "", "system", N, SIGNAL_SRC_HEALTH, 0, 5},
};

#define SIGNAL_COUNT (sizeof(s_signals) / sizeof(s_signals[0]))

const signal_def_t *signal_registry_all(size_t *count)
{
    if (count) {
        *count = SIGNAL_COUNT;
    }
    return s_signals;
}

const signal_def_t *signal_registry_by_id(uint8_t id)
{
    for (size_t i = 0; i < SIGNAL_COUNT; i++) {
        if (s_signals[i].id == id) {
            return &s_signals[i];
        }
    }
    return NULL;
}

const signal_def_t *signal_registry_by_key(const char *key)
{
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SIGNAL_COUNT; i++) {
        if (strcmp(s_signals[i].key, key) == 0) {
            return &s_signals[i];
        }
    }
    return NULL;
}

static void read_power(const telemetry_power_payload_t *p, unsigned field, signal_value_t *out)
{
    out->source_fault = p->fault;
    switch (field) {
    case 0: out->value = p->voltage_v; break;
    case 1: out->value = p->current_a; break;
    case 2: out->value = p->power_w; break;
    case 3: out->value = p->energy_wh; break;
    case 4: out->value = p->frequency_hz; break;
    case 5: out->value = p->pf; break;
    default: /* meter fault */
        out->value = p->fault ? 1.0 : 0.0;
        out->trusted = true;
        return;
    }
    out->trusted = p->valid && !p->fault;
}

static void read_water(const telemetry_water_payload_t *w, uint8_t id, signal_value_t *out)
{
    out->source_fault = w->fault;
    switch (id) {
    case 21: out->value = w->ph; break;
    case 22: out->value = w->temperature_c; break;
    case 23: out->value = w->stable ? 1.0 : 0.0; break;
    default: /* 24: sensor fault */
        out->value = w->fault ? 1.0 : 0.0;
        out->trusted = true;
        return;
    }
    out->trusted = w->valid && !w->fault;
}

static void read_cooling(const telemetry_cooling_payload_t *c, uint8_t id, signal_value_t *out)
{
    out->trusted = true;
    switch (id) {
    case 31:
        out->value = c->current_temp_c;
        out->trusted = c->is_sensor_valid;
        out->source_fault = !c->is_sensor_valid;
        break;
    case 32: out->value = c->duty_percent; break;
    case 33: out->value = c->output_mv; break;
    case 34: out->value = c->mode; break;
    case 35: out->value = c->lockout_remaining_sec; break;
    case 36: out->value = c->is_sensor_valid ? 0.0 : 1.0; break;
    default: /* 37: cooling_on */ out->value = c->output_mv > 0 ? 1.0 : 0.0; break;
    }
}

void signal_registry_read(const signal_def_t *def, const telemetry_snapshot_t *snap,
                          uint32_t now_ms, signal_value_t *out)
{
    memset(out, 0, sizeof(*out));
    out->age_ms = now_ms;
    if (def == NULL || snap == NULL) {
        return;
    }

    uint32_t updated = 0;
    switch (def->source) {
    case SIGNAL_SRC_POWER_PUMP:
        out->present = snap->has_power[0];
        updated = snap->power_updated_ms[0];
        break;
    case SIGNAL_SRC_POWER_CHILLER:
        out->present = snap->has_power[1];
        updated = snap->power_updated_ms[1];
        break;
    case SIGNAL_SRC_WATER:
        out->present = snap->has_water;
        updated = snap->water_updated_ms;
        break;
    case SIGNAL_SRC_COOLING:
        out->present = snap->has_cooling;
        updated = snap->cooling_updated_ms;
        break;
    case SIGNAL_SRC_PUMP:
        out->present = snap->has_pump;
        updated = snap->pump_updated_ms;
        break;
    case SIGNAL_SRC_FLOAT:
        out->present = snap->has_float;
        updated = snap->float_updated_ms;
        break;
    case SIGNAL_SRC_FLOW:
        out->present = snap->has_flow;
        updated = snap->flow_updated_ms;
        break;
    case SIGNAL_SRC_HEALTH:
        out->present = snap->has_health;
        updated = snap->health_updated_ms;
        break;
    default:
        return;
    }
    if (!out->present) {
        return;
    }
    out->age_ms = (uint32_t)(now_ms - updated);
    out->trusted = true; /* default: same as present */

    switch (def->source) {
    case SIGNAL_SRC_POWER_PUMP:
        read_power(&snap->power[0], (unsigned)(def->id - 1), out);
        break;
    case SIGNAL_SRC_POWER_CHILLER:
        read_power(&snap->power[1], (unsigned)(def->id - 11), out);
        break;
    case SIGNAL_SRC_WATER:
        read_water(&snap->water, def->id, out);
        break;
    case SIGNAL_SRC_COOLING:
        read_cooling(&snap->cooling, def->id, out);
        break;
    case SIGNAL_SRC_PUMP:
        switch (def->id) {
        case 41: out->value = snap->pump.relay_1_active ? 1.0 : 0.0; break;
        case 42: out->value = snap->pump.relay_2_active ? 1.0 : 0.0; break;
        case 43: out->value = snap->pump.is_running ? 1.0 : 0.0; break;
        default: out->value = snap->pump.active_pump; break;
        }
        break;
    case SIGNAL_SRC_FLOAT:
        out->value = snap->float_state.float_is_high ? 1.0 : 0.0;
        break;
    case SIGNAL_SRC_FLOW:
        out->value = snap->flow.flow_on ? 1.0 : 0.0;
        break;
    case SIGNAL_SRC_HEALTH:
        switch (def->id) {
        case 61: out->value = snap->health.free_heap_bytes; break;
        case 62: out->value = snap->health.free_psram_bytes; break;
        case 63:
            out->value = snap->health.wifi_rssi;
            out->trusted = snap->health.wifi_rssi != 0; /* 0 while STA is disconnected */
            break;
        case 64: out->value = snap->health.uptime_sec; break;
        default: out->value = snap->health.oom_strikes; break;
        }
        break;
    default:
        break;
    }

    /* A NaN/Infinity must never be compared or printed into JSON. */
    if (def->kind == SIGNAL_KIND_NUMBER && !isfinite(out->value)) {
        out->trusted = false;
    }
}
