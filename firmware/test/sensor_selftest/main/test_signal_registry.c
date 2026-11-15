#include <math.h>
#include <string.h>

#include "selftest.h"
#include "signal_registry.h"

void test_signal_registry(void)
{
    size_t n;
    const signal_def_t *all = signal_registry_all(&n);
    CHECK(n == 36);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            CHECK(all[i].id != all[j].id && strcmp(all[i].key, all[j].key) != 0);
        }
    }
    CHECK(signal_registry_by_key("pump_current_a")->id == 2 && signal_registry_by_id(52)->kind == SIGNAL_KIND_BOOL);
    CHECK(signal_registry_by_key("pump1_current_a") == NULL && signal_registry_by_id(200) == NULL);

    telemetry_snapshot_t s = {0};
    signal_value_t v;
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(!v.present && !v.trusted && v.age_ms == 5000);

    s.has_power[0] = true;
    s.power_updated_ms[0] = 4000;
    s.power[0] = (telemetry_power_payload_t){.channel = 0, .valid = true, .current_a = 1.5f};
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(v.present && v.trusted && v.value == 1.5 && v.age_ms == 1000);
    signal_registry_read(signal_registry_by_key("chiller_current_a"), &s, 5000, &v);
    CHECK(!v.present); /* channels are independent */

    s.power[0].valid = false; /* transient miss */
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(v.present && !v.trusted && !v.source_fault);
    s.power[0].fault = true; /* debounced fault */
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(!v.trusted && v.source_fault);
    signal_registry_read(signal_registry_by_key("pump_meter_fault"), &s, 5000, &v);
    CHECK(v.trusted && v.value == 1.0); /* the fault signal itself is trusted */

    s.power[0] = (telemetry_power_payload_t){.valid = true, .current_a = NAN};
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(!v.trusted); /* Review Focus 4 */
    s.power[0].current_a = INFINITY;
    signal_registry_read(signal_registry_by_key("pump_current_a"), &s, 5000, &v);
    CHECK(!v.trusted);

    s.has_cooling = true;
    s.cooling = (telemetry_cooling_payload_t){.is_sensor_valid = false, .output_mv = 3000};
    signal_registry_read(signal_registry_by_key("pt100_temp_c"), &s, 5000, &v);
    CHECK(!v.trusted && v.source_fault);
    signal_registry_read(signal_registry_by_key("pt100_fault"), &s, 5000, &v);
    CHECK(v.trusted && v.value == 1.0);
    signal_registry_read(signal_registry_by_key("cooling_on"), &s, 5000, &v);
    CHECK(v.trusted && v.value == 1.0);

    s.has_health = true;
    s.health.wifi_rssi = 0;
    signal_registry_read(signal_registry_by_key("wifi_rssi_dbm"), &s, 5000, &v);
    CHECK(v.present && !v.trusted);
    s.health.wifi_rssi = -61;
    signal_registry_read(signal_registry_by_key("wifi_rssi_dbm"), &s, 5000, &v);
    CHECK(v.trusted && v.value == -61.0);

    s.has_flow = true;
    s.flow.flow_on = true;
    s.flow_updated_ms = 0xFFFFFFF0u; /* age across the wrap */
    signal_registry_read(signal_registry_by_key("flow_on"), &s, 0x00000010u, &v);
    CHECK(v.trusted && v.value == 1.0 && v.age_ms == 0x20);
}
