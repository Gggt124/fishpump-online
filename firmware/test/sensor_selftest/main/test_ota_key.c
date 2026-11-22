#include <string.h>

#include "ota_key.h"
#include "selftest.h"

#define KEY_ZERO "0000000000000000000000000000000000000000000000000000000000000000"
#define KEY_NIST_LOWER "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"
#define KEY_NIST_UPPER "603DEB1015CA71BE2B73AEF0857D77811F352C073B6108D72D9810A30914DFF4"
/* 63 and 65 characters */
#define KEY_SHORT "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff"
#define KEY_LONG "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4a"
#define KEY_NOT_HEX "zz3deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4"

void test_ota_key(void)
{
    uint8_t raw[32];
    char fp[OTA_KEY_FP_LEN + 1];
    char fp_upper[OTA_KEY_FP_LEN + 1];

    /* The all-zero key is the "dev" key every unconfigured build falls back to. */
    CHECK(ota_key_is_dev(KEY_ZERO));
    CHECK(ota_key_fingerprint(KEY_ZERO, fp));
    CHECK(strcmp(fp, "66687aad") == 0);

    /* A real key is not dev; upper and lower case give the same fingerprint. */
    CHECK(!ota_key_is_dev(KEY_NIST_LOWER));
    CHECK(ota_key_fingerprint(KEY_NIST_LOWER, fp));
    CHECK(ota_key_fingerprint(KEY_NIST_UPPER, fp_upper));
    CHECK(strcmp(fp, fp_upper) == 0);
    CHECK(strcmp(fp, "66687aad") != 0);
    CHECK(ota_key_parse_hex(KEY_NIST_UPPER, raw));
    CHECK(raw[0] == 0x60 && raw[1] == 0x3d && raw[31] == 0xf4);

    /* Wrong length or non-hex characters: rejected, and treated as dev. */
    CHECK(!ota_key_parse_hex(KEY_SHORT, raw));
    CHECK(!ota_key_parse_hex(KEY_LONG, raw));
    CHECK(!ota_key_parse_hex(KEY_NOT_HEX, raw));
    CHECK(!ota_key_parse_hex(NULL, raw));
    CHECK(ota_key_is_dev(KEY_SHORT));
    CHECK(ota_key_is_dev(KEY_NOT_HEX));
    CHECK(!ota_key_fingerprint(KEY_SHORT, fp));

    /* Web OTA upload limit = partition size + 16-byte IV. */
    CHECK(ota_upload_limit(0x380000) == 0x380010);
    CHECK(ota_upload_limit(0) == 16);
}
