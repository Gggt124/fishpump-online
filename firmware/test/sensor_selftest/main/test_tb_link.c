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
}
