#pragma once

/*
 * Pure helpers around the OTA encryption key (64 hex characters = AES-256).
 * Nothing here ever prints or stores the key itself; the fingerprint is the
 * safe way to compare "which key is this board / build using".
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_KEY_HEX_LEN 64
#define OTA_KEY_FP_LEN 8     /* fingerprint characters; buffers need +1 for the NUL */
#define OTA_IMAGE_IV_LEN 16  /* web OTA files start with a 16-byte IV */

/* Decode 64 hex characters (either case) into 32 bytes. False for NULL, wrong length or non-hex. */
bool ota_key_parse_hex(const char *hex, uint8_t out[32]);

/* True when the key is all zero or cannot be parsed: the unconfigured "dev" key. */
bool ota_key_is_dev(const char *hex);

/* Lower-case hex of the first 4 bytes of SHA-256(raw 32-byte key). False if the key cannot be parsed. */
bool ota_key_fingerprint(const char *hex, char out[OTA_KEY_FP_LEN + 1]);

/* Largest web OTA upload for a partition: the partition size plus the 16-byte IV. */
size_t ota_upload_limit(size_t partition_size);
