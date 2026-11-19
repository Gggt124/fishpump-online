#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "selftest.h"
#include "tb_link_logic.h"

static const uint8_t MAC[6] = {0x24, 0x6f, 0x28, 0xa1, 0xb2, 0xc3};

static bool merge(const char *json, const tb_link_config_t *cur, tb_link_config_t *out, const char **bad)
{
    cJSON *b = cJSON_Parse(json);
    bool ok = tb_link_config_from_json(b, cur, out, bad);
    cJSON_Delete(b);
    return ok;
}

void test_tb_link(void)
{
    char name[TB_NAME_MAX];
    tb_default_device_name(MAC, name, sizeof(name));
    CHECK(strcmp(name, "fishpump-a1b2c3") == 0);

    tb_link_config_t cfg;
    tb_link_config_defaults(&cfg, MAC);
    const char *bad = NULL;
    CHECK(cfg.host[0] == 0);
    CHECK(cfg.port == 1883);
    CHECK(!cfg.tls);
    CHECK(strcmp(cfg.device_name, "fishpump-a1b2c3") == 0);
    CHECK(tb_link_config_valid(&cfg, &bad));
    CHECK(tb_link_config_valid(&cfg, NULL));

    /* host */
    strcpy(cfg.host, "192.168.1.51");
    strcpy(cfg.token, "tok_1");
    CHECK(tb_link_config_valid(&cfg, &bad));
    strcpy(cfg.host, "bad host");
    bad = NULL;
    CHECK(!tb_link_config_valid(&cfg, &bad));
    CHECK(bad && strcmp(bad, "host") == 0);
    memset(cfg.host, 'a', 63);
    cfg.host[63] = 0;
    CHECK(tb_link_config_valid(&cfg, &bad));
    {
        /* 64 chars cannot be stored in host[]; the JSON path must reject it */
        char body[160];
        char h64[65];
        memset(h64, 'a', 64);
        h64[64] = 0;
        snprintf(body, sizeof(body), "{\"host\":\"%s\"}", h64);
        tb_link_config_t out;
        bad = NULL;
        CHECK(!merge(body, &cfg, &out, &bad));
        CHECK(bad && strcmp(bad, "host") == 0);
    }

    /* token / provisioning requirement */
    tb_link_config_defaults(&cfg, MAC);
    strcpy(cfg.host, "192.168.1.51");
    bad = NULL;
    CHECK(!tb_link_config_valid(&cfg, &bad));
    CHECK(bad && strcmp(bad, "token") == 0);
    strcpy(cfg.provision_key, "k1");
    strcpy(cfg.provision_secret, "s1");
    CHECK(tb_link_config_valid(&cfg, &bad));

    /* from_json */
    tb_link_config_t cur, out;
    tb_link_config_defaults(&cur, MAC);
    strcpy(cur.host, "tb.example.com");
    strcpy(cur.token, "oldtoken");
    strcpy(cur.provision_key, "k1");
    strcpy(cur.provision_secret, "s1");

    bad = NULL;
    CHECK(!merge("{\"port\":0}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "port") == 0);
    bad = NULL;
    CHECK(!merge("{\"port\":\"1883\"}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "port") == 0);
    bad = NULL;
    CHECK(!merge("{\"port\":65536}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "port") == 0);
    bad = NULL;
    CHECK(!merge("{\"port\":1883.5}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "port") == 0);
    bad = NULL;
    CHECK(!merge("{\"tls\":1}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "tls") == 0);
    bad = NULL;
    CHECK(!merge("[1]", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "body") == 0);

    CHECK(merge("{\"token\":\"\"}", &cur, &out, &bad));
    CHECK(out.token[0] == 0);
    CHECK(strcmp(out.provision_secret, "s1") == 0); /* absent = keep */
    CHECK(strcmp(out.host, "tb.example.com") == 0);

    CHECK(merge("{\"port\":8883,\"tls\":true,\"provision_secret\":\"\",\"unknown\":5}", &cur, &out, &bad));
    CHECK(out.port == 8883);
    CHECK(out.tls);
    CHECK(out.provision_secret[0] == 0);
    CHECK(strcmp(out.token, "oldtoken") == 0);

    /* merged result must be valid: clearing token and secret -> "token" */
    bad = NULL;
    CHECK(!merge("{\"token\":\"\",\"provision_secret\":\"\"}", &cur, &out, &bad));
    CHECK(bad && strcmp(bad, "token") == 0);

    /* to_json */
    {
        cJSON *j = tb_link_config_to_json(&cur);
        CHECK(j != NULL);
        const cJSON *ht = cJSON_GetObjectItemCaseSensitive(j, "has_token");
        CHECK(cJSON_IsTrue(ht));
        const cJSON *hs = cJSON_GetObjectItemCaseSensitive(j, "has_provision_secret");
        CHECK(cJSON_IsTrue(hs));
        const cJSON *pk = cJSON_GetObjectItemCaseSensitive(j, "provision_key");
        CHECK(cJSON_IsString(pk) && strcmp(pk->valuestring, "k1") == 0);
        char *txt = cJSON_PrintUnformatted(j);
        CHECK(txt != NULL);
        if (txt) {
            CHECK(strstr(txt, "\"token\":") == NULL);
            CHECK(strstr(txt, "\"provision_secret\":") == NULL);
            CHECK(strstr(txt, "oldtoken") == NULL);
            free(txt);
        }
        cJSON_Delete(j);
    }

    /* provisioning request */
    {
        tb_link_config_t p;
        tb_link_config_defaults(&p, MAC);
        strcpy(p.device_name, "n");
        strcpy(p.provision_key, "k");
        strcpy(p.provision_secret, "s");
        char buf[200];
        int n = tb_provision_build_request(&p, buf, sizeof(buf));
        CHECK(n > 0 && (size_t)n == strlen(buf));
        cJSON *j = cJSON_Parse(buf);
        CHECK(j != NULL);
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "deviceName");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(j, "provisionDeviceKey");
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "provisionDeviceSecret");
        CHECK(cJSON_IsString(a) && strcmp(a->valuestring, "n") == 0);
        CHECK(cJSON_IsString(b) && strcmp(b->valuestring, "k") == 0);
        CHECK(cJSON_IsString(c) && strcmp(c->valuestring, "s") == 0);
        cJSON_Delete(j);
        char small[10];
        CHECK(tb_provision_build_request(&p, small, sizeof(small)) == -1);
    }

    /* provisioning parse */
    {
        char tok[TB_TOKEN_MAX];
        char err[80];
        const char *ok = "{\"status\":\"SUCCESS\",\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"abc123\"}";
        CHECK(tb_provision_parse(ok, strlen(ok), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_OK);
        CHECK(strcmp(tok, "abc123") == 0);

        const char *fail = "{\"status\":\"FAILURE\",\"errorMsg\":\"Provision request is not found!\"}";
        CHECK(tb_provision_parse(fail, strlen(fail), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_FAILURE);
        CHECK(strcmp(err, "Provision request is not found!") == 0);

        const char *fail2 = "{\"status\":\"FAILURE\"}";
        strcpy(err, "x");
        CHECK(tb_provision_parse(fail2, strlen(fail2), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_FAILURE);
        CHECK(err[0] == 0);

        /* any non-SUCCESS status is a failure, e.g. ThingsBoard NOT_FOUND for a wrong key/secret */
        const char *nf = "{\"status\":\"NOT_FOUND\",\"errorMsg\":\"Provision data was not found!\"}";
        strcpy(err, "x");
        CHECK(tb_provision_parse(nf, strlen(nf), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_FAILURE);
        CHECK(strcmp(err, "Provision data was not found!") == 0);

        const char *odd = "{\"status\":\"WEIRD\"}";
        strcpy(err, "x");
        CHECK(tb_provision_parse(odd, strlen(odd), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_FAILURE);
        CHECK(strcmp(err, "WEIRD") == 0);

        char tiny[8];
        CHECK(tb_provision_parse(nf, strlen(nf), tok, sizeof(tok), tiny, sizeof(tiny)) == TB_PROV_FAILURE);
        CHECK(strlen(tiny) == sizeof(tiny) - 1 && strncmp(tiny, "Provisi", 7) == 0);
        CHECK(tb_provision_parse(odd, strlen(odd), tok, sizeof(tok), NULL, 0) == TB_PROV_FAILURE);

        const char *basic = "{\"status\":\"SUCCESS\",\"credentialsType\":\"MQTT_BASIC\",\"credentialsValue\":\"abc123\"}";
        CHECK(tb_provision_parse(basic, strlen(basic), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_BAD_REPLY);
        const char *junk = "not json";
        CHECK(tb_provision_parse(junk, strlen(junk), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_BAD_REPLY);
        const char *nostatus = "{\"credentialsType\":\"ACCESS_TOKEN\"}";
        CHECK(tb_provision_parse(nostatus, strlen(nostatus), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_BAD_REPLY);

        char longtok[160];
        char v70[71];
        memset(v70, 'a', 70);
        v70[70] = 0;
        snprintf(longtok, sizeof(longtok),
                 "{\"status\":\"SUCCESS\",\"credentialsType\":\"ACCESS_TOKEN\",\"credentialsValue\":\"%s\"}", v70);
        strcpy(tok, "keep");
        CHECK(tb_provision_parse(longtok, strlen(longtok), tok, sizeof(tok), err, sizeof(err)) == TB_PROV_BAD_REPLY);
        CHECK(strcmp(tok, "keep") == 0);
    }

    /* backoff */
    CHECK(tb_backoff_next_ms(0) == 5000u);
    CHECK(tb_backoff_next_ms(5000) == 10000u);
    CHECK(tb_backoff_next_ms(10000) == 20000u);
    CHECK(tb_backoff_next_ms(20000) == 40000u);
    CHECK(tb_backoff_next_ms(40000) == 60000u);
    CHECK(tb_backoff_next_ms(60000) == 60000u);
    /* RPC topic and envelope parsing */
    {
        uint32_t id = 0;
        const char *t = "v1/devices/me/rpc/request/17";
        CHECK(tb_rpc_parse_topic(t, strlen(t), &id) && id == 17);
        t = "v1/devices/me/rpc/request/4294967295";
        CHECK(tb_rpc_parse_topic(t, strlen(t), &id) && id == 4294967295u);
        t = "v1/devices/me/rpc/request/";
        CHECK(!tb_rpc_parse_topic(t, strlen(t), &id));
        t = "v1/devices/me/rpc/request/1a";
        CHECK(!tb_rpc_parse_topic(t, strlen(t), &id));
        t = "v1/devices/me/rpc/request/4294967296";
        CHECK(!tb_rpc_parse_topic(t, strlen(t), &id));
        t = "v1/devices/me/rpc/response/17";
        CHECK(!tb_rpc_parse_topic(t, strlen(t), &id));
        t = "v1/devices/me/rpc/request/17";
        CHECK(!tb_rpc_parse_topic(t, strlen(t) - 1 > 26 ? 26 : strlen(t) - 1, &id));
        CHECK(tb_rpc_parse_topic(t, strlen(t) - 1, &id) && id == 1); /* topic_len, not the NUL, ends the id */
        CHECK(!tb_rpc_parse_topic(NULL, 0, &id));

        char method[TB_RPC_METHOD_MAX];
        cJSON *params = (cJSON *)1;
        const char *r = "{\"method\":\"alarms.status\"}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_OK);
        CHECK(strcmp(method, "alarms.status") == 0 && params == NULL);
        r = "{\"method\":\"x\",\"params\":{\"a\":1}}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_OK);
        CHECK(params != NULL && cJSON_IsObject(params));
        cJSON_Delete(params);
        r = "{\"method\":\"x\",\"params\":[1]}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_OK);
        CHECK(params != NULL && cJSON_IsArray(params));
        cJSON_Delete(params);

        params = (cJSON *)1;
        r = "{\"params\":{}}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_NO_METHOD);
        CHECK(params == NULL);
        r = "{\"method\":3}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_NO_METHOD);
        char j48[80];
        snprintf(j48, sizeof(j48), "{\"method\":\"%s\"}", "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv");
        CHECK(strlen("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuv") == TB_RPC_METHOD_MAX);
        CHECK(tb_rpc_parse_request(j48, strlen(j48), method, sizeof(method), &params) == TB_RPC_PARSE_NO_METHOD);
        r = "{\"method\":\"\"}";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_NO_METHOD);
        r = "not json";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_BAD_JSON);
        r = "[]";
        CHECK(tb_rpc_parse_request(r, strlen(r), method, sizeof(method), &params) == TB_RPC_PARSE_BAD_JSON);
        CHECK(tb_rpc_parse_request(NULL, 0, method, sizeof(method), &params) == TB_RPC_PARSE_BAD_JSON);

        char buf[64];
        CHECK(tb_rpc_response_topic(17, buf, sizeof(buf)) == 29 && strcmp(buf, "v1/devices/me/rpc/response/17") == 0);
        CHECK(tb_rpc_response_topic(17, buf, 29) == -1);
        CHECK(tb_rpc_response_topic(17, buf, 30) == 29);
        CHECK(tb_rpc_response_topic(4294967295u, buf, sizeof(buf)) == 37);
    }

    /* RPC duplicate suppression */
    {
        const char *pa = "{\"method\":\"pump.stop\"}";
        const char *pb = "{\"method\":\"pump.start\"}";
        size_t la = strlen(pa), lb = strlen(pb);
        CHECK(tb_rpc_hash(pa, la) != tb_rpc_hash(pb, lb));
        CHECK(tb_rpc_hash(pa, la) == tb_rpc_hash(pa, la));
        tb_rpc_dedup_t d;
        tb_rpc_dedup_init(&d);
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 1000) == -1); /* empty ring */
        int s90 = tb_rpc_dedup_insert(&d, 90, pa, la, 1000);
        CHECK(s90 >= 0 && s90 < TB_RPC_DEDUP_SLOTS);
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 1000) == s90);
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 1000 + 29999) == s90);       /* inside the window */
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 1000 + 30000) == s90);       /* edge: 30 s old still hits */
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 1000 + 30001) == -1);        /* after 30 s */
        CHECK(tb_rpc_dedup_find(&d, 90, pb, lb, 1000) == -1);                /* same id, other payload */
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la - 1, 1000) == -1);            /* same id, other length */
        CHECK(tb_rpc_dedup_find(&d, 91, pa, la, 1000) == -1);                /* other id, same payload */
        /* re-insert of the same key reuses its slot and refreshes the time */
        CHECK(tb_rpc_dedup_insert(&d, 90, pa, la, 20000) == s90);
        CHECK(tb_rpc_dedup_find(&d, 90, pa, la, 49000) == s90);

        /* eviction: four slots, the fifth key evicts the oldest */
        tb_rpc_dedup_init(&d);
        int sl[5];
        for (uint32_t i = 0; i < 4; i++) {
            sl[i] = tb_rpc_dedup_insert(&d, 100 + i, pa, la, 5000 + i);
        }
        CHECK(sl[0] != sl[1] && sl[1] != sl[2] && sl[2] != sl[3] && sl[0] != sl[3]);
        sl[4] = tb_rpc_dedup_insert(&d, 104, pa, la, 5010);
        CHECK(sl[4] == sl[0]);
        CHECK(tb_rpc_dedup_find(&d, 100, pa, la, 5010) == -1);
        CHECK(tb_rpc_dedup_find(&d, 101, pa, la, 5010) == sl[1]);
        CHECK(tb_rpc_dedup_find(&d, 104, pa, la, 5010) == sl[4]);
        /* an expired entry's slot is taken before a live one is evicted */
        tb_rpc_dedup_init(&d);
        int e0 = tb_rpc_dedup_insert(&d, 1, pa, la, 0);
        (void)tb_rpc_dedup_insert(&d, 2, pa, la, 20000);
        (void)tb_rpc_dedup_insert(&d, 3, pa, la, 20001);
        (void)tb_rpc_dedup_insert(&d, 4, pa, la, 20002);
        CHECK(tb_rpc_dedup_insert(&d, 5, pa, la, 40003) == e0);
        CHECK(tb_rpc_dedup_find(&d, 2, pa, la, 40003) >= 0);
        /* uint32 ms wrap: subtraction still works */
        tb_rpc_dedup_init(&d);
        int w = tb_rpc_dedup_insert(&d, 7, pa, la, 0xffffff00u);
        CHECK(tb_rpc_dedup_find(&d, 7, pa, la, 0x00000100u) == w);
        CHECK(tb_rpc_dedup_find(&d, 7, NULL, 0, 0) == -1);
    }
}
