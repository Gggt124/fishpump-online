#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_api.h"
#include "selftest.h"

static int s_seen_empty_object;

static dapi_status_t fake_read(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)params;
    (void)src;
    *out = cJSON_Parse("{\"ok\":true,\"n\":1}");
    return DAPI_OK;
}

static dapi_status_t fake_echo(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)src;
    s_seen_empty_object = cJSON_IsObject(params) && params->child == NULL;
    *out = cJSON_Duplicate(params, true);
    return DAPI_OK;
}

static dapi_status_t fake_null_out(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)params;
    (void)src;
    *out = NULL;
    return DAPI_OK;
}

static const dapi_method_t M_READ = {"fake_read", DAPI_F_READ, 0, fake_read};
static const dapi_method_t M_LOCAL = {"fake_local", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY, 32, fake_echo};
static const dapi_method_t M_NULL = {"fake_null", DAPI_F_WRITE, 0, fake_null_out};
static const dapi_method_t M_CAP = {"fake_cap", DAPI_F_WRITE, 32, fake_echo};

/* Calls the normal method and a NO_LOCK method while the caller holds (or does not hold) the lock. */
static dapi_status_t fake_nested(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)params;
    (void)src;
    cJSON *inner = NULL;
    dapi_status_t st = device_api_call("fake_read", NULL, DAPI_SRC_HTTP, &inner);
    cJSON_Delete(inner);
    *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(*out, "inner_ok", st == DAPI_OK);
    cJSON_AddBoolToObject(*out, "inner_busy", st == DAPI_UNAVAILABLE);
    return DAPI_OK;
}

static const dapi_method_t M_NESTED_LOCKED = {"fake_nested_locked", DAPI_F_READ, 0, fake_nested};
static const dapi_method_t M_NESTED_FREE = {"fake_nested_free", DAPI_F_READ | DAPI_F_NO_LOCK, 0, fake_nested};
static const dapi_method_t M_FREE_LOCAL = {"fake_free_local", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY | DAPI_F_NO_LOCK, 32, fake_echo};

static bool body_is(const cJSON *out, const char *expect)
{
    if (!out) {
        return false;
    }
    char *s = cJSON_PrintUnformatted(out);
    bool eq = s && strcmp(s, expect) == 0;
    free(s);
    return eq;
}

/* Calls, checks status and body, then frees the body. */
static void call_expect(const char *method, const cJSON *params, dapi_source_t src,
                        dapi_status_t st, const char *body)
{
    cJSON *out = NULL;
    dapi_status_t got = device_api_call(method, params, src, &out);
    CHECK(got == st);
    CHECK(body_is(out, body));
    cJSON_Delete(out);
}

static int s_hook_count;
static char s_hook_method[32];
static dapi_status_t s_hook_status;

static void test_hook(const char *method, dapi_status_t st)
{
    s_hook_count++;
    strncpy(s_hook_method, method, sizeof(s_hook_method) - 1);
    s_hook_status = st;
}

static dapi_status_t hook_ok(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)params;
    (void)src;
    *out = cJSON_CreateObject();
    return DAPI_OK;
}

static dapi_status_t hook_fail(const cJSON *params, dapi_source_t src, cJSON **out)
{
    (void)params;
    (void)src;
    *out = dapi_error("nope", NULL);
    return DAPI_BAD_REQUEST;
}

static const dapi_method_t M_HOOK[] = {
    {"hook.read", DAPI_F_READ, 0, hook_ok},
    {"hook.write", DAPI_F_WRITE, 512, hook_fail},
    {"hook.local", DAPI_F_WRITE | DAPI_F_LOCAL_ONLY, 0, hook_ok},
};

static void hook_call(const char *method, const cJSON *params, dapi_source_t src)
{
    cJSON *out = NULL;
    device_api_call(method, params, src, &out);
    cJSON_Delete(out);
}

void test_device_api(void)
{
    /* status lines */
    CHECK(strcmp(device_api_http_status(DAPI_OK), "200 OK") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_ACCEPTED), "202 Accepted") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_BAD_REQUEST), "400 Bad Request") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_LOCAL_ONLY), "403 Forbidden") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_NOT_FOUND), "404 Not Found") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_CONFLICT), "409 Conflict") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_UNAVAILABLE), "503 Service Unavailable") == 0);
    CHECK(strcmp(device_api_http_status(DAPI_INTERNAL), "500 Internal Server Error") == 0);

    /* dapi_error */
    cJSON *e = dapi_error("a", NULL);
    CHECK(body_is(e, "{\"ok\":false,\"error\":\"a\"}"));
    cJSON_Delete(e);
    e = dapi_error("a", "b");
    CHECK(body_is(e, "{\"ok\":false,\"error\":\"a\",\"message\":\"b\"}"));
    cJSON_Delete(e);

    /* registration */
    CHECK(device_api_init() == ESP_OK);
    CHECK(device_api_init() == ESP_OK);
    CHECK(device_api_find("nope") == NULL);
    CHECK(device_api_register(&M_READ, 1) == ESP_OK);
    CHECK(device_api_count() == 1);
    CHECK(device_api_register(&M_READ, 1) == ESP_ERR_INVALID_STATE);
    CHECK(device_api_count() == 1);
    CHECK(device_api_register(&M_LOCAL, 1) == ESP_OK);
    CHECK(device_api_register(&M_NULL, 1) == ESP_OK);
    CHECK(device_api_register(&M_CAP, 1) == ESP_OK);
    CHECK(device_api_count() == 4);
    CHECK(device_api_find("fake_read") != NULL);
    CHECK(device_api_find("nope") == NULL);

    /* unknown method */
    call_expect("nope", NULL, DAPI_SRC_HTTP, DAPI_NOT_FOUND,
                "{\"ok\":false,\"error\":\"unknown_method\"}");

    /* read method */
    call_expect("fake_read", NULL, DAPI_SRC_TB, DAPI_OK, "{\"ok\":true,\"n\":1}");

    /* local only */
    cJSON *p = cJSON_Parse("{\"k\":1}");
    call_expect("fake_local", p, DAPI_SRC_TB, DAPI_LOCAL_ONLY,
                "{\"ok\":false,\"error\":\"local_only\",\"message\":\"Use the local web app\"}");
    call_expect("fake_local", p, DAPI_SRC_HTTP, DAPI_OK, "{\"k\":1}");
    cJSON_Delete(p);

    /* TB params must be an object */
    const char *bad_json_body =
        "{\"ok\":false,\"error\":\"invalid_json\",\"message\":\"Expected a JSON object\"}";
    const char *bad[] = {"[]", "\"x\"", "3", "null"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cJSON *bp = cJSON_Parse(bad[i]);
        call_expect("fake_null", bp, DAPI_SRC_TB, DAPI_BAD_REQUEST, bad_json_body);
        cJSON_Delete(bp);
    }

    /* NULL params become {} */
    s_seen_empty_object = 0;
    call_expect("fake_local", NULL, DAPI_SRC_HTTP, DAPI_OK, "{}");
    CHECK(s_seen_empty_object == 1);

    /* params size cap applies to TB only */
    cJSON *big = cJSON_CreateObject();
    cJSON_AddStringToObject(big, "s", "0123456789012345678901234567890123456789");
    call_expect("fake_cap", big, DAPI_SRC_HTTP, DAPI_OK,
                "{\"s\":\"0123456789012345678901234567890123456789\"}");
    call_expect("fake_cap", big, DAPI_SRC_TB, DAPI_BAD_REQUEST,
                "{\"ok\":false,\"error\":\"invalid_body\","
                "\"message\":\"Request body is missing or too large\"}");
    cJSON_Delete(big);
    p = cJSON_Parse("{\"k\":1}");
    call_expect("fake_cap", p, DAPI_SRC_TB, DAPI_OK, "{\"k\":1}");
    cJSON_Delete(p);

    /* a NO_LOCK method runs without the lock: a normal method called inside it is not blocked */
    CHECK(device_api_register(&M_NESTED_FREE, 1) == ESP_OK);
    CHECK(device_api_register(&M_NESTED_LOCKED, 1) == ESP_OK);
    CHECK(device_api_register(&M_FREE_LOCAL, 1) == ESP_OK);
    call_expect("fake_nested_free", NULL, DAPI_SRC_HTTP, DAPI_OK, "{\"inner_ok\":true,\"inner_busy\":false}");
    /* a normal method holds the lock: the same inner call is refused with busy */
    call_expect("fake_nested_locked", NULL, DAPI_SRC_HTTP, DAPI_OK, "{\"inner_ok\":false,\"inner_busy\":true}");
    /* NO_LOCK skips only the lock: local-only and the params cap still apply */
    p = cJSON_Parse("{\"k\":1}");
    call_expect("fake_free_local", p, DAPI_SRC_TB, DAPI_LOCAL_ONLY,
                "{\"ok\":false,\"error\":\"local_only\",\"message\":\"Use the local web app\"}");
    call_expect("fake_free_local", p, DAPI_SRC_HTTP, DAPI_OK, "{\"k\":1}");
    cJSON_Delete(p);

    /* method returns no object */
    call_expect("fake_null", NULL, DAPI_SRC_HTTP, DAPI_INTERNAL,
                "{\"ok\":false,\"error\":\"memory\",\"message\":\"JSON allocation failed\"}");

    /* write hook: runs for every WRITE method whose function ran, never for refused calls */
    CHECK(device_api_register(M_HOOK, 3) == ESP_OK);
    device_api_set_write_hook(test_hook);
    hook_call("hook.read", NULL, DAPI_SRC_HTTP);
    CHECK(s_hook_count == 0);
    hook_call("hook.write", NULL, DAPI_SRC_HTTP);
    CHECK(s_hook_count == 1);
    CHECK(strcmp(s_hook_method, "hook.write") == 0);
    CHECK(s_hook_status == DAPI_BAD_REQUEST);
    hook_call("hook.local", NULL, DAPI_SRC_TB);
    CHECK(s_hook_count == 1);
    hook_call("hook.local", NULL, DAPI_SRC_HTTP);
    CHECK(s_hook_count == 2);
    cJSON *hbig = cJSON_CreateObject();
    char *longstr = malloc(601);
    memset(longstr, 'a', 600);
    longstr[600] = 0;
    cJSON_AddStringToObject(hbig, "s", longstr);
    free(longstr);
    hook_call("hook.write", hbig, DAPI_SRC_TB);
    CHECK(s_hook_count == 2);
    cJSON_Delete(hbig);
    device_api_set_write_hook(NULL);
    hook_call("hook.write", NULL, DAPI_SRC_HTTP);
    CHECK(s_hook_count == 2);
}
