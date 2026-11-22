#include "ota_key.h"

#include <string.h>

#include "psa/crypto.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool ota_key_parse_hex(const char *hex, uint8_t out[32])
{
    if (hex == NULL || strlen(hex) != OTA_KEY_HEX_LEN) {
        return false;
    }
    uint8_t tmp[32];
    for (int i = 0; i < 32; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        tmp[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out, tmp, sizeof tmp);
    return true;
}

bool ota_key_is_dev(const char *hex)
{
    uint8_t raw[32];
    if (!ota_key_parse_hex(hex, raw)) {
        return true;
    }
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) {
        any |= raw[i];
    }
    return any == 0;
}

bool ota_key_fingerprint(const char *hex, char out[OTA_KEY_FP_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t raw[32];
    uint8_t digest[32];
    size_t digest_len = 0;

    out[0] = '\0';
    if (!ota_key_parse_hex(hex, raw)) {
        return false;
    }
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, raw, sizeof raw, digest, sizeof digest, &digest_len) != PSA_SUCCESS ||
        digest_len != sizeof digest) {
        memset(raw, 0, sizeof raw);
        return false;
    }
    for (int i = 0; i < OTA_KEY_FP_LEN / 2; i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 0x0F];
    }
    out[OTA_KEY_FP_LEN] = '\0';
    memset(raw, 0, sizeof raw);
    return true;
}

size_t ota_upload_limit(size_t partition_size)
{
    return partition_size + OTA_IMAGE_IV_LEN;
}
