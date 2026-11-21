#include <stdio.h>
#include <string.h>

#include "selftest.h"
#include "tb_link_logic.h"

#define SUM "a4710f239f3187ac0a067a1230e6c3ee7ad7191724cd49253779906ec129fd6f"
#define FIELDS                                                                       \
    "\"fw_title\":\"fishpump\",\"fw_version\":\"0.3.3\",\"fw_size\":2061648,"         \
    "\"fw_checksum_algorithm\":\"SHA256\",\"fw_checksum\":\"" SUM "\""
/* a push with a variable fw_size / algorithm / checksum */
#define PUSH(size, alg, sum)                                                         \
    "{\"fw_title\":\"fishpump\",\"fw_version\":\"0.3.3\",\"fw_size\":" size ",\"fw_checksum_algorithm\":\"" alg \
    "\",\"fw_checksum\":\"" sum "\"}"

static tb_fw_attr_t parse(const char *json, tb_fw_info_t *out)
{
    return tb_fw_parse_attrs(json, strlen(json), out);
}

static tb_fw_attr_t parse_one(const char *json)
{
    tb_fw_info_t i;
    return parse(json, &i);
}

static void test_parse(void)
{
    tb_fw_info_t i;
    CHECK(parse_one("{\"deleted\":[\"fw_tag\",\"fw_url\"]}") == TB_FW_ATTR_NONE);
    CHECK(parse("{" FIELDS "}", &i) == TB_FW_ATTR_INFO);
    CHECK(strcmp(i.title, "fishpump") == 0);
    CHECK(strcmp(i.version, "0.3.3") == 0);
    CHECK(i.size == 2061648u);
    CHECK(strcmp(i.checksum, SUM) == 0);
    memset(&i, 0, sizeof(i));
    CHECK(parse("{\"shared\":{" FIELDS "}}", &i) == TB_FW_ATTR_INFO);
    CHECK(i.size == 2061648u);
    CHECK(parse_one("{\"shared\":{}}") == TB_FW_ATTR_CLEARED);
    CHECK(parse_one("{}") == TB_FW_ATTR_CLEARED);
    CHECK(parse_one("{\"deleted\":[\"fw_title\",\"fw_version\"]}") == TB_FW_ATTR_CLEARED);

    CHECK(parse(PUSH("2061648", "MD5", SUM), &i) == TB_FW_ATTR_INVALID);
    CHECK(strcmp(i.title, "fishpump") == 0);

    CHECK(parse_one(PUSH("0", "SHA256", SUM)) == TB_FW_ATTR_INVALID);
    CHECK(parse_one(PUSH("-1", "SHA256", SUM)) == TB_FW_ATTR_INVALID);
    CHECK(parse_one(PUSH("1.5", "SHA256", SUM)) == TB_FW_ATTR_INVALID);
    CHECK(parse_one(PUSH("\"2061648\"", "SHA256", SUM)) == TB_FW_ATTR_INVALID);
    CHECK(parse_one(PUSH("4294967295", "SHA256", SUM)) == TB_FW_ATTR_INFO);
    CHECK(parse_one(PUSH("4294967296", "SHA256", SUM)) == TB_FW_ATTR_INVALID);
    /* 63 chars, and a non-hex char */
    CHECK(parse_one(PUSH("5", "SHA256", "a4710f239f3187ac0a067a1230e6c3ee7ad7191724cd49253779906ec129fd6")) ==
          TB_FW_ATTR_INVALID);
    CHECK(parse_one(PUSH("5", "SHA256", "g4710f239f3187ac0a067a1230e6c3ee7ad7191724cd49253779906ec129fd6f")) ==
          TB_FW_ATTR_INVALID);
    CHECK(parse(PUSH("5", "SHA256", "A4710F239F3187AC0A067A1230E6C3EE7AD7191724CD49253779906EC129FD6F"), &i) ==
          TB_FW_ATTR_INFO);
    CHECK(strcmp(i.checksum, SUM) == 0);

    /* missing or bad version, missing fields */
    CHECK(parse_one("{\"fw_title\":\"fishpump\",\"fw_size\":5,\"fw_checksum_algorithm\":\"SHA256\","
                    "\"fw_checksum\":\"" SUM "\"}") == TB_FW_ATTR_INVALID);
    CHECK(parse_one("{\"fw_title\":\"fishpump\",\"fw_version\":\"\",\"fw_size\":5,"
                    "\"fw_checksum_algorithm\":\"SHA256\",\"fw_checksum\":\"" SUM "\"}") == TB_FW_ATTR_INVALID);
    CHECK(parse_one("{\"fw_title\":\"fishpump\"}") == TB_FW_ATTR_INVALID);
    CHECK(parse_one("{\"fw_title\":5}") == TB_FW_ATTR_INVALID);
    /* title longer than 32 bytes: INVALID, title left empty */
    CHECK(parse("{\"fw_title\":\"123456789012345678901234567890123\"}", &i) == TB_FW_ATTR_INVALID);
    CHECK(i.title[0] == '\0');

    CHECK(parse_one("[]") == TB_FW_ATTR_INVALID);
    CHECK(parse_one("not json") == TB_FW_ATTR_INVALID);
    CHECK(parse_one("null") == TB_FW_ATTR_INVALID);
}

static void test_decide(void)
{
    tb_fw_info_t fw;
    memset(&fw, 0, sizeof(fw));
    strcpy(fw.title, "fishpump");
    strcpy(fw.version, "0.3.3");
    fw.size = 2061648;
    const char *reason = NULL;

    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, false, &reason) == TB_FW_START);
    CHECK(reason == NULL);
    strcpy(fw.title, "other");
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, false, &reason) == TB_FW_IGNORE);
    strcpy(fw.title, "fishpump");
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, true, &reason) == TB_FW_IGNORE);
    strcpy(fw.version, "v0.3.2");
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, false, &reason) == TB_FW_IGNORE);
    strcpy(fw.version, "0.3.3");
    CHECK(tb_fw_decide(&fw, "0.3.2", "0.3.3", 0x380000, false, &reason) == TB_FW_IGNORE);
    CHECK(tb_fw_decide(&fw, "0.3.2", "0.3.4", 0x380000, false, &reason) == TB_FW_START);
    fw.size = 0x380001u;
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, false, &reason) == TB_FW_FAIL);
    CHECK(reason != NULL && strcmp(reason, "too_large") == 0);
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, false, NULL) == TB_FW_FAIL);
    /* order: busy beats too_large */
    CHECK(tb_fw_decide(&fw, "0.3.2", NULL, 0x380000, true, NULL) == TB_FW_IGNORE);
}

static void test_small(void)
{
    CHECK(tb_fw_version_equal("v0.3.3", "0.3.3"));
    CHECK(tb_fw_version_equal("V1", "v1"));
    CHECK(!tb_fw_version_equal("0.3.3", "0.3.30"));
    CHECK(!tb_fw_version_equal(NULL, "x"));
    CHECK(!tb_fw_version_equal("x", NULL));
    CHECK(!tb_fw_version_equal("vv1", "v1"));

    static const char *const fin[] = {"bad_attributes", "too_large", "bad_chunk", "checksum_mismatch",
                                      "invalid_image", "rolled_back", "version_mismatch"};
    static const char *const nfin[] = {"busy",      "chunk_timeout",    "server_error",    "write_failed",
                                       "no_memory", "ota_begin_failed", "set_boot_failed", "nonsense"};
    for (size_t k = 0; k < sizeof(fin) / sizeof(fin[0]); k++) {
        CHECK(tb_fw_error_is_final(fin[k]));
    }
    for (size_t k = 0; k < sizeof(nfin) / sizeof(nfin[0]); k++) {
        CHECK(!tb_fw_error_is_final(nfin[k]));
    }
    CHECK(!tb_fw_error_is_final(NULL));

    CHECK(tb_fw_chunk_count(2061648) == 126);
    CHECK(tb_fw_chunk_count(16384) == 1);
    CHECK(tb_fw_chunk_count(16385) == 2);
    CHECK(tb_fw_chunk_len(2061648, 125) == 13648);
    CHECK(tb_fw_chunk_len(2061648, 0) == 16384);
    CHECK(tb_fw_chunk_len(2061648, 126) == 0);
    CHECK(tb_fw_chunk_len(16384, 0) == 16384);
    CHECK(tb_fw_chunk_len(16385, 1) == 1);
}

static void test_topics(void)
{
    char buf[64];
    CHECK(tb_fw_request_topic(1, 0, buf, sizeof(buf)) == 23);
    CHECK(strcmp(buf, "v2/fw/request/1/chunk/0") == 0);
    char small[10];
    CHECK(tb_fw_request_topic(1, 0, small, sizeof(small)) == -1);

    uint32_t req = 0, idx = 0;
    const char *t = "v2/fw/response/2/chunk/125";
    CHECK(tb_fw_parse_chunk_topic(t, strlen(t), &req, &idx));
    CHECK(req == 2 && idx == 125);
    t = "v2/fw/response/4294967295/chunk/4294967295";
    CHECK(tb_fw_parse_chunk_topic(t, strlen(t), &req, &idx));
    CHECK(req == 4294967295u && idx == 4294967295u);
    const char *bad[] = {"v2/fw/response/2/chunk/",
                         "v2/fw/response/2/chunk/x",
                         "v2/fw/response/2/chunk",
                         "v2/fw/error",
                         "v2/fw/response//chunk/1",
                         "v2/fw/response/4294967296/chunk/1",
                         "v2/fw/response/2/chunk/4294967296",
                         "v2/fw/response/2/chunk/1x"};
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        CHECK(!tb_fw_parse_chunk_topic(bad[k], strlen(bad[k]), &req, &idx));
    }
    /* not NUL terminated: the length decides */
    t = "v2/fw/response/2/chunk/125999";
    CHECK(tb_fw_parse_chunk_topic(t, strlen(t) - 3, &req, &idx) && idx == 125);
}

static void test_digest(void)
{
    const uint8_t d[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                           0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                           0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(tb_fw_digest_matches(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(tb_fw_digest_matches(d, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
    CHECK(!tb_fw_digest_matches(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ac"));
    CHECK(!tb_fw_digest_matches(d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a"));
    CHECK(!tb_fw_digest_matches(d, NULL));
    uint8_t e[32];
    memcpy(e, d, 32);
    e[7] ^= 0x01;
    CHECK(!tb_fw_digest_matches(e, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

static void test_state_boot(void)
{
    char buf[160];
    int n = tb_fw_state_json(buf, sizeof(buf), "DOWNLOADING", NULL, "fishpump", "0.3.2");
    const char *e1 =
        "{\"fw_state\":\"DOWNLOADING\",\"current_fw_title\":\"fishpump\",\"current_fw_version\":\"0.3.2\"}";
    CHECK(n == (int)strlen(e1) && strcmp(buf, e1) == 0);
    n = tb_fw_state_json(buf, sizeof(buf), "FAILED", "rolled_back", NULL, NULL);
    const char *e2 = "{\"fw_state\":\"FAILED\",\"fw_error\":\"rolled_back\"}";
    CHECK(n == (int)strlen(e2) && strcmp(buf, e2) == 0);
    n = tb_fw_state_json(buf, sizeof(buf), NULL, NULL, NULL, NULL);
    CHECK(n == 2 && strcmp(buf, "{}") == 0);
    char small[10];
    CHECK(tb_fw_state_json(small, sizeof(small), "DOWNLOADING", NULL, "fishpump", "0.3.2") == -1);

    CHECK(tb_fw_boot_state(NULL, "0.3.3", true) == TB_FW_BOOT_NONE);
    CHECK(tb_fw_boot_state("0.3.3", "v0.3.3", true) == TB_FW_BOOT_VERIFY);
    CHECK(tb_fw_boot_state("0.3.3", "0.3.3", false) == TB_FW_BOOT_UPDATED);
    CHECK(tb_fw_boot_state("0.3.3", "0.3.2", false) == TB_FW_BOOT_ROLLED_BACK);
    CHECK(tb_fw_boot_state("0.3.3", "0.3.2", true) == TB_FW_BOOT_ROLLED_BACK);
}

void test_tb_fw(void)
{
    test_parse();
    test_decide();
    test_small();
    test_topics();
    test_digest();
    test_state_boot();
}
