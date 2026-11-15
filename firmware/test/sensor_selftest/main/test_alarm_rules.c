#include <string.h>

#include "alarm_rules.h"
#include "cJSON.h"
#include "selftest.h"

#define K8 "กกกกกกกก" /* 8 x 3 bytes */
#define NAME_96 K8 K8 K8 K8

/* Each name is a JSON text, defined once so the checks below stay readable. */
#define R_PH_RANGE                                                                         \
    "{\"name\":\"pH ผิดช่วง\",\"signal\":\"water_ph\",\"condition\":\"outside\","         \
    "\"lo\":6.5,\"hi\":8.5,\"hysteresis\":0.1,\"delay_sec\":60,\"severity\":\"warning\"," \
    "\"gate\":{\"signal\":\"relay1_on\",\"when\":true}}"
#define R_ABOVE "{\"name\":\"a\",\"signal\":\"pump_current_a\",\"condition\":\"above\",\"threshold\":5,\"severity\":\"info\"}"
#define R_BELOW "{\"id\":9,\"name\":\"b\",\"signal\":\"pump_current_a\",\"condition\":\"below\",\"threshold\":0.3,\"severity\":\"critical\",\"latch\":true,\"enabled\":false}"
#define R_INSIDE "{\"name\":\"i\",\"signal\":\"water_temp_c\",\"condition\":\"inside\",\"lo\":10,\"hi\":20,\"hysteresis\":1,\"severity\":\"info\"}"
#define R_IS_TRUE "{\"name\":\"t\",\"signal\":\"flow_on\",\"condition\":\"is_true\",\"severity\":\"warning\"}"
#define R_IS_FALSE "{\"name\":\"f\",\"signal\":\"flow_on\",\"condition\":\"is_false\",\"delay_sec\":15,\"severity\":\"critical\",\"gate\":{\"signal\":\"relay1_on\",\"when\":false}}"
#define R_STALE_HEALTH(n) "{\"name\":\"s\",\"signal\":\"uptime_sec\",\"condition\":\"stale\",\"stale_sec\":" #n ",\"severity\":\"info\"}"
#define STALE_ON_RELAY1 "{\"name\":\"s\",\"signal\":\"relay1_on\",\"condition\":\"stale\",\"stale_sec\":30,\"severity\":\"info\"}"
#define ABOVE_ON_FLOW_ON "{\"name\":\"x\",\"signal\":\"flow_on\",\"condition\":\"above\",\"threshold\":1,\"severity\":\"info\"}"
#define IS_TRUE_ON_WATER_PH "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"is_true\",\"severity\":\"info\"}"
#define UNKNOWN_SIGNAL "{\"name\":\"x\",\"signal\":\"nope\",\"condition\":\"above\",\"threshold\":1,\"severity\":\"info\"}"
#define LO_NOT_BELOW_HI "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"outside\",\"lo\":8,\"hi\":8,\"severity\":\"info\"}"
#define HYST_HALF_SPAN "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"outside\",\"lo\":6,\"hi\":8,\"hysteresis\":1,\"severity\":\"info\"}"
#define NEGATIVE_HYST "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":5,\"hysteresis\":-1,\"severity\":\"info\"}"
#define DELAY_3601 "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":5,\"delay_sec\":3601,\"severity\":\"info\"}"
#define THRESHOLD_1E40 "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":1e40,\"severity\":\"info\"}"
#define THRESHOLD_AS_STRING "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":\"5\",\"severity\":\"info\"}"
#define NAME_AS_NUMBER "{\"name\":5,\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":5,\"severity\":\"info\"}"
#define NO_SEVERITY "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":5}"
#define ABOVE_WITHOUT_THRESHOLD "{\"name\":\"x\",\"signal\":\"water_ph\",\"condition\":\"above\",\"severity\":\"info\"}"
#define GATE_ON_OWN_SIGNAL "{\"name\":\"x\",\"signal\":\"flow_on\",\"condition\":\"is_true\",\"severity\":\"info\",\"gate\":{\"signal\":\"flow_on\",\"when\":true}}"
#define GATE_ON_NUMBER_SIGNAL "{\"name\":\"x\",\"signal\":\"flow_on\",\"condition\":\"is_true\",\"severity\":\"info\",\"gate\":{\"signal\":\"water_ph\",\"when\":true}}"
#define NAMED(n) "{\"name\":\"" n "\",\"signal\":\"water_ph\",\"condition\":\"above\",\"threshold\":5,\"severity\":\"info\"}"

static alarm_json_err_t parse(const char *json, alarm_rule_t *r, const char **f)
{
    bool has_id;
    *f = "";
    cJSON *obj = cJSON_Parse(json);
    if (obj == NULL) {
        return ALARM_JSON_INVALID_TYPE;
    }
    alarm_json_err_t e = alarm_rule_from_json(obj, r, &has_id, f);
    cJSON_Delete(obj);
    return e;
}

void test_alarm_rules(void)
{
    alarm_rule_t r;
    const char *f;

    /* round trip, Thai name, range + gate */
    CHECK(parse(R_PH_RANGE, &r, &f) == ALARM_JSON_OK);
    CHECK(r.signal_id == 21 && r.condition == ALARM_COND_OUTSIDE && r.threshold == 6.5f && r.threshold_hi == 8.5f
          && r.delay_sec == 60 && r.severity == ALARM_SEV_WARNING && r.enabled && !r.latch
          && r.gate_mode == ALARM_GATE_WHEN_TRUE && r.gate_signal_id == 41);
    CHECK(strcmp(r.name, "pH ผิดช่วง") == 0);

    /* to_json then from_json gives the same rule */
    cJSON *back = alarm_rule_to_json(&r);
    CHECK(back != NULL);
    if (back != NULL) {
        alarm_rule_t r2;
        bool has_id = true;
        CHECK(alarm_rule_from_json(back, &r2, &has_id, &f) == ALARM_JSON_OK && !has_id);
        CHECK(memcmp(&r, &r2, sizeof(r)) == 0);
        cJSON_Delete(back);
    }

    /* every condition parses and round-trips */
    const char *all_ok[] = {R_ABOVE, R_BELOW, R_INSIDE, R_IS_TRUE, R_IS_FALSE, R_STALE_HEALTH(15), R_STALE_HEALTH(3600)};
    for (size_t i = 0; i < sizeof(all_ok) / sizeof(all_ok[0]); i++) {
        alarm_rule_t a, b;
        bool has_id;
        CHECK(parse(all_ok[i], &a, &f) == ALARM_JSON_OK);
        cJSON *j = alarm_rule_to_json(&a);
        CHECK(j != NULL && alarm_rule_from_json(j, &b, &has_id, &f) == ALARM_JSON_OK && memcmp(&a, &b, sizeof(a)) == 0);
        cJSON_Delete(j);
    }
    CHECK(parse(R_BELOW, &r, &f) == ALARM_JSON_OK && r.id == 9 && r.latch && !r.enabled
          && r.severity == ALARM_SEV_CRITICAL && r.condition == ALARM_COND_BELOW && r.threshold == 0.3f);
    CHECK(parse(R_IS_FALSE, &r, &f) == ALARM_JSON_OK && r.gate_mode == ALARM_GATE_WHEN_FALSE && r.delay_sec == 15);

    CHECK(parse(R_STALE_HEALTH(10), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "stale_sec")); /* min 15 */
    CHECK(parse(R_STALE_HEALTH(15), &r, &f) == ALARM_JSON_OK);
    CHECK(parse(R_STALE_HEALTH(3601), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "stale_sec"));
    CHECK(parse(STALE_ON_RELAY1, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition")); /* period 0 */
    CHECK(parse(ABOVE_ON_FLOW_ON, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition"));
    CHECK(parse(IS_TRUE_ON_WATER_PH, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "condition"));
    CHECK(parse(UNKNOWN_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "signal"));
    CHECK(parse(LO_NOT_BELOW_HI, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hi"));
    CHECK(parse(HYST_HALF_SPAN, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hysteresis"));
    CHECK(parse(NEGATIVE_HYST, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "hysteresis"));
    CHECK(parse(DELAY_3601, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "delay_sec"));
    CHECK(parse(THRESHOLD_1E40, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "threshold")); /* inf as float */
    CHECK(parse(THRESHOLD_AS_STRING, &r, &f) == ALARM_JSON_INVALID_TYPE && !strcmp(f, "threshold"));
    CHECK(parse(NAME_AS_NUMBER, &r, &f) == ALARM_JSON_INVALID_TYPE && !strcmp(f, "name"));
    CHECK(parse(NO_SEVERITY, &r, &f) == ALARM_JSON_MISSING_FIELD && !strcmp(f, "severity"));
    CHECK(parse(ABOVE_WITHOUT_THRESHOLD, &r, &f) == ALARM_JSON_MISSING_FIELD && !strcmp(f, "threshold"));
    CHECK(parse(GATE_ON_OWN_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "gate"));
    CHECK(parse(GATE_ON_NUMBER_SIGNAL, &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "gate"));

    /* names: Review Focus 3 */
    CHECK(parse(NAMED(""), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
    CHECK(parse(NAMED(NAME_96), &r, &f) == ALARM_JSON_OK && strlen(r.name) == 96);
    CHECK(parse(NAMED(NAME_96 "a"), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
    CHECK(parse(NAMED("a\\\"b"), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
    CHECK(parse(NAMED("a\\\\b"), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));
    CHECK(parse(NAMED("a\\nb"), &r, &f) == ALARM_JSON_INVALID_VALUE && !strcmp(f, "name"));

    /* invalid UTF-8 cannot come through cJSON; call alarm_rule_validate directly */
    CHECK(parse(R_ABOVE, &r, &f) == ALARM_JSON_OK && alarm_rule_validate(&r, &f));
    alarm_rule_t bad = r;
    memcpy(bad.name, "\xE0\xB8", 3); /* truncated sequence + NUL */
    CHECK(!alarm_rule_validate(&bad, &f) && !strcmp(f, "name"));
    bad = r;
    memcpy(bad.name, "\xC0\x80", 3); /* overlong encoding */
    CHECK(!alarm_rule_validate(&bad, &f) && !strcmp(f, "name"));
    bad = r;
    bad.threshold = 1.0f / 0.0f;
    CHECK(!alarm_rule_validate(&bad, &f) && !strcmp(f, "threshold"));

    CHECK(!strcmp(alarm_cond_name(ALARM_COND_STALE), "stale") && !strcmp(alarm_severity_name(3), "critical"));
}
