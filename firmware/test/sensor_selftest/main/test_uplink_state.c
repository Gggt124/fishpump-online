#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "selftest.h"
#include "signal_registry.h"
#include "uplink_format.h"

/* Trimmed replies from the 2026-11-20 golden capture of the real board. */
static const char PUMP_JSON[] =
    "{\"ok\":true,\"system_water\":{\"running\":true,\"relay_energized\":false,\"float_state\":\"OFF\","
    "\"active_set\":1,\"phase\":\"OFF\",\"countdown_sec\":17,\"total_duration_sec\":60,"
    "\"cooldown_remaining_sec\":0,\"auto_start\":true},\"system_wash\":{\"running\":false,"
    "\"relay_energized\":false,\"phase\":\"IDLE\",\"countdown_sec\":0,\"total_duration_sec\":0,"
    "\"auto_start\":false},\"running\":true,\"initialized\":true,\"config_valid\":true,"
    "\"initial_stabilizing\":false,\"fault\":false,\"dry_run\":{\"enabled\":false,\"state\":\"disabled\","
    "\"trips\":0,\"max_retries\":3,\"wait_remaining_sec\":0,\"dry_accum_sec\":0}}";
static const char COOLING_JSON[] =
    "{\"ok\":true,\"initialized\":true,\"config_valid\":true,\"mode\":\"auto\",\"auto_enable\":true,"
    "\"temperature_c\":0,\"temperature_valid\":false,\"sensor_state\":\"fault\",\"fault\":true,"
    "\"fault_code\":\"read_failed\",\"relay_energized\":false,\"test_remaining_sec\":0,"
    "\"cooling_demand\":false,\"blocked_reason\":\"sensor_fault\"}";
static const char STATUS_JSON[] =
    "{\"ok\":true,\"chip_model\":\"ESP32-S3\",\"free_heap\":148236,\"min_free_heap\":120372,"
    "\"total_heap\":311020,\"largest_free_block\":98304,\"sta_connected\":true,\"sta_connecting\":false,"
    "\"sta_channel\":1,\"sta_rssi\":-50}";

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static bool key_ok(const char *k)
{
    if (*k == '\0') {
        return false;
    }
    for (; *k != '\0'; k++) {
        if (!(islower((unsigned char)*k) || isdigit((unsigned char)*k) || *k == '_')) {
            return false;
        }
    }
    return true;
}

static const cJSON *src_at(const cJSON *root, const char *path)
{
    const cJSON *cur = root;
    char seg[48];
    while (cur != NULL && *path != '\0') {
        const char *dot = strchr(path, '.');
        size_t len = dot != NULL ? (size_t)(dot - path) : strlen(path);
        memcpy(seg, path, len);
        seg[len] = '\0';
        cur = get(cur, seg);
        path += len + (dot != NULL ? 1 : 0);
    }
    return cur;
}

static int count_prefix(const cJSON *o, const char *p)
{
    int n = 0;
    for (const cJSON *it = o->child; it != NULL; it = it->next) {
        if (strncmp(it->string, p, strlen(p)) == 0) {
            n++;
        }
    }
    return n;
}

static bool str_is(const cJSON *o, const char *k, const char *v)
{
    const cJSON *it = get(o, k);
    return cJSON_IsString(it) && strcmp(it->valuestring, v) == 0;
}

void test_uplink_state(void)
{
    /* ---- hash ---- */
    CHECK(uplink_hash32("", 0) == 2166136261u);
    CHECK(uplink_hash32("a", 1) == 0xe40c292cu);

    /* ---- cache ---- */
    uplink_attr_cache_t c;
    uplink_attr_cache_reset(&c);
    CHECK(uplink_attr_cache_changed(&c, "cfg_pump", 1));
    CHECK(uplink_attr_cache_commit(&c, "cfg_pump", 1));
    CHECK(!uplink_attr_cache_changed(&c, "cfg_pump", 1));
    CHECK(uplink_attr_cache_changed(&c, "cfg_pump", 2));
    uplink_attr_cache_reset(&c);
    bool all8 = true;
    for (int i = 0; i < UPLINK_STATE_ATTR_MAX; i++) {
        char k[8];
        k[0] = 'k';
        k[1] = (char)('0' + i);
        k[2] = '\0';
        all8 = all8 && uplink_attr_cache_commit(&c, k, (uint32_t)i);
    }
    CHECK(all8);
    CHECK(!uplink_attr_cache_commit(&c, "k_ninth", 9));
    CHECK(!uplink_attr_cache_changed(&c, "k7", 7));
    uplink_attr_cache_reset(&c);
    /* key length boundary, on an empty cache so only the length guard can reject */
    CHECK(!uplink_attr_cache_commit(&c, "abcdefghijklmnopqrstuvwx", 1)); /* 24 chars */
    CHECK(c.n == 0);
    CHECK(uplink_attr_cache_commit(&c, "abcdefghijklmnopqrstuvw", 1)); /* 23 chars */
    CHECK(!uplink_attr_cache_changed(&c, "abcdefghijklmnopqrstuvw", 1));
    uplink_attr_cache_reset(&c);
    CHECK(uplink_attr_cache_changed(&c, "k0", 0));
    CHECK(uplink_attr_cache_changed(&c, "k7", 7));

    /* ---- due rules ---- */
    CHECK(!uplink_state_rebuild_due(false, true, 59999));
    CHECK(uplink_state_rebuild_due(false, true, 60000));
    CHECK(uplink_state_rebuild_due(true, true, 0));
    CHECK(uplink_state_rebuild_due(false, false, 0));
    CHECK(!uplink_status_poll_due(true, true, 1999));
    CHECK(uplink_status_poll_due(true, true, 2000));
    CHECK(!uplink_status_poll_due(false, true, 29999));
    CHECK(uplink_status_poll_due(false, true, 30000));
    CHECK(uplink_status_poll_due(false, false, 0));
    CHECK(!uplink_status_send_due(true, 29999, false));
    CHECK(!uplink_status_send_due(true, 1999, true));
    CHECK(uplink_status_send_due(true, 30000, false));
    CHECK(uplink_status_send_due(true, 2000, true));
    CHECK(uplink_status_send_due(false, 0, false));

    /* ---- json_without ---- */
    cJSON *src = cJSON_Parse("{\"ok\":true,\"a\":1,\"status\":{\"x\":1}}");
    CHECK(src != NULL);
    if (src != NULL) {
        const char *drop[] = {"ok", "status"};
        cJSON *w = uplink_json_without(src, drop, 2);
        CHECK(w != NULL);
        if (w != NULL) {
            char *txt = cJSON_PrintUnformatted(w);
            CHECK(txt != NULL && strcmp(txt, "{\"a\":1}") == 0);
            cJSON_free(txt);
            cJSON_Delete(w);
        }
        CHECK(cJSON_GetArraySize(src) == 3);
        cJSON_Delete(src);
    }
    cJSON *arr = cJSON_Parse("[1,2]");
    CHECK(arr != NULL && uplink_json_without(arr, NULL, 0) == NULL);
    cJSON_Delete(arr);

    /* ---- flattener ---- */
    cJSON *pump = cJSON_Parse(PUMP_JSON);
    cJSON *cool = cJSON_Parse(COOLING_JSON);
    cJSON *stat = cJSON_Parse(STATUS_JSON);
    CHECK(pump != NULL && cool != NULL && stat != NULL);
    if (pump != NULL && cool != NULL && stat != NULL) {
        const cJSON *srcs[UPLINK_SRC_COUNT] = {pump, cool, stat};
        cJSON *out = cJSON_CreateObject();
        uint32_t h0 = 0;
        int n = uplink_status_flatten(srcs, out, &h0);
        CHECK(n == (int)uplink_status_field_count);
        CHECK(str_is(out, "pump_phase", "OFF"));
        CHECK(str_is(out, "dry_run_state", "disabled"));
        CHECK(str_is(out, "cooling_mode_name", "auto"));
        CHECK(str_is(out, "cooling_blocked_reason", "sensor_fault"));
        CHECK(cJSON_IsNumber(get(out, "heap_min_free_bytes")) && get(out, "heap_min_free_bytes")->valuedouble == 120372);
        CHECK(cJSON_IsTrue(get(out, "wifi_connected")));
        CHECK(cJSON_IsFalse(get(out, "pump_fault")));

        /* a NULL pump source drops exactly the pump rows */
        int pump_rows = 0;
        for (size_t i = 0; i < uplink_status_field_count; i++) {
            if (uplink_status_fields[i].src == UPLINK_SRC_PUMP) {
                pump_rows++;
            }
        }
        CHECK(pump_rows == 16);
        const cJSON *nopump[UPLINK_SRC_COUNT] = {NULL, cool, stat};
        cJSON *out2 = cJSON_CreateObject();
        int n2 = uplink_status_flatten(nopump, out2, NULL);
        CHECK(n2 == n - pump_rows);
        CHECK(count_prefix(out2, "pump_") == 0 && count_prefix(out2, "wash_") == 0 &&
              count_prefix(out2, "dry_run_") == 0);
        cJSON_Delete(out2);
        cJSON_Delete(out);

        /* watched hash: countdown is not watched, phase and fault_code are */
        cJSON *water = cJSON_GetObjectItemCaseSensitive(pump, "system_water");
        cJSON *o = cJSON_CreateObject();
        cJSON_ReplaceItemInObjectCaseSensitive(water, "countdown_sec", cJSON_CreateNumber(3));
        uint32_t h1 = 0;
        uplink_status_flatten(srcs, o, &h1);
        CHECK(h1 == h0);
        cJSON_Delete(o);
        o = cJSON_CreateObject();
        cJSON_ReplaceItemInObjectCaseSensitive(water, "phase", cJSON_CreateString("ON"));
        uint32_t h2 = 0;
        uplink_status_flatten(srcs, o, &h2);
        CHECK(h2 != h0);
        cJSON_Delete(o);
        o = cJSON_CreateObject();
        cJSON_ReplaceItemInObjectCaseSensitive(water, "phase", cJSON_CreateString("OFF"));
        cJSON_DeleteItemFromObjectCaseSensitive(cool, "fault_code");
        uint32_t h3 = 0;
        uplink_status_flatten(srcs, o, &h3);
        CHECK(h3 != h0);
        cJSON_Delete(o);

        /* size: worst-case values */
        cJSON *bp = cJSON_Parse(PUMP_JSON);
        cJSON *bc = cJSON_Parse(COOLING_JSON);
        cJSON *bs = cJSON_Parse(STATUS_JSON);
        CHECK(bp != NULL && bc != NULL && bs != NULL);
        if (bp != NULL && bc != NULL && bs != NULL) {
            cJSON *roots[UPLINK_SRC_COUNT] = {bp, bc, bs};
            for (size_t i = 0; i < uplink_status_field_count; i++) {
                const uplink_status_field_t *f = &uplink_status_fields[i];
                const cJSON *it = src_at(roots[f->src], f->path);
                if (it == NULL) {
                    continue;
                }
                char xs[33];
                memset(xs, 'x', 32);
                xs[32] = '\0';
                /* string -> 32 x, anything else -> the widest number */
                cJSON *rep = cJSON_IsString(it) ? cJSON_CreateString(xs) : cJSON_CreateNumber(-2147483648.0);
                const char *dot = strrchr(f->path, '.');
                char parent[48] = "";
                if (dot != NULL) {
                    memcpy(parent, f->path, (size_t)(dot - f->path));
                    parent[dot - f->path] = '\0';
                }
                cJSON *par = dot != NULL ? (cJSON *)src_at(roots[f->src], parent) : roots[f->src];
                cJSON_ReplaceItemInObjectCaseSensitive(par, dot != NULL ? dot + 1 : f->path, rep);
            }
            const cJSON *bsrcs[UPLINK_SRC_COUNT] = {bp, bc, bs};
            cJSON *bo = cJSON_CreateObject();
            int bn = uplink_status_flatten(bsrcs, bo, NULL);
            CHECK(bn == (int)uplink_status_field_count);
            char *txt = cJSON_PrintUnformatted(bo);
            CHECK(txt != NULL && strlen(txt) + 7 * 40 < UPLINK_TELEMETRY_BUF);
            cJSON_free(txt);
            cJSON_Delete(bo);
        }
        cJSON_Delete(bp);
        cJSON_Delete(bc);
        cJSON_Delete(bs);
    }
    cJSON_Delete(pump);
    cJSON_Delete(cool);
    cJSON_Delete(stat);

    /* ---- table ---- */
    for (size_t i = 0; i < uplink_status_field_count; i++) {
        const uplink_status_field_t *f = &uplink_status_fields[i];
        CHECK(key_ok(f->key));
        CHECK(strncmp(f->key, "alarm_", 6) != 0);
        CHECK(signal_registry_by_key(f->key) == NULL);
        CHECK(f->src < UPLINK_SRC_COUNT);
        for (size_t j = i + 1; j < uplink_status_field_count; j++) {
            CHECK(strcmp(f->key, uplink_status_fields[j].key) != 0);
        }
    }
}
