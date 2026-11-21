#include "sen0708_proto.h"

#include <math.h>
#include <string.h>

static void put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

uint16_t sen0708_proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* Modbus sends the CRC low byte first. */
static size_t append_crc(uint8_t *frame, size_t payload_len)
{
    uint16_t crc = sen0708_proto_crc16(frame, payload_len);
    frame[payload_len] = (uint8_t)(crc & 0xFF);
    frame[payload_len + 1] = (uint8_t)(crc >> 8);
    return payload_len + 2;
}

static bool crc_ok(const uint8_t *frame, size_t len)
{
    if (len < 3) {
        return false;
    }
    uint16_t expected = sen0708_proto_crc16(frame, len - 2);
    uint16_t actual = (uint16_t)frame[len - 2] | ((uint16_t)frame[len - 1] << 8);
    return expected == actual;
}

size_t sen0708_proto_build_read(uint8_t addr, uint16_t start, uint16_t count, uint8_t *out)
{
    if (out == NULL || count == 0 || count > SEN0708_MAX_READ_REGS) {
        return 0;
    }
    out[0] = addr;
    out[1] = SEN0708_FUNC_READ_HOLDING;
    put_u16_be(&out[2], start);
    put_u16_be(&out[4], count);
    return append_crc(out, 6);
}

size_t sen0708_proto_build_write_single(uint8_t addr, uint16_t reg, uint16_t value, uint8_t *out)
{
    if (out == NULL) {
        return 0;
    }
    out[0] = addr;
    out[1] = SEN0708_FUNC_WRITE_SINGLE;
    put_u16_be(&out[2], reg);
    put_u16_be(&out[4], value);
    return append_crc(out, 6);
}

size_t sen0708_proto_build_write_multi(uint8_t addr, uint16_t start, uint16_t count,
                                       const uint16_t *values, uint8_t *out)
{
    if (out == NULL || values == NULL || count == 0 || count > SEN0708_MAX_WRITE_REGS) {
        return 0;
    }
    out[0] = addr;
    out[1] = SEN0708_FUNC_WRITE_MULTI;
    put_u16_be(&out[2], start);
    put_u16_be(&out[4], count);
    out[6] = (uint8_t)(count * 2);
    for (uint16_t i = 0; i < count; i++) {
        put_u16_be(&out[7 + i * 2], values[i]);
    }
    return append_crc(out, 7 + (size_t)count * 2);
}

size_t sen0708_proto_reply_len(const uint8_t *req, size_t req_len)
{
    if (req == NULL || req_len < 8) {
        return 0;
    }
    switch (req[1]) {
    case SEN0708_FUNC_READ_HOLDING:
        /* addr, func, byte count, data, crc */
        return 5 + (size_t)get_u16_be(&req[4]) * 2;
    case SEN0708_FUNC_WRITE_SINGLE: /* full echo */
    case SEN0708_FUNC_WRITE_MULTI:  /* addr, func, start, count, crc */
        return 8;
    default:
        return 0;
    }
}

bool sen0708_proto_is_exception(const uint8_t *resp, size_t len, uint8_t func)
{
    return resp != NULL && len >= 2 && resp[1] == (uint8_t)(func | SEN0708_EXCEPTION_FLAG);
}

sen0708_proto_status_t sen0708_proto_check_reply(const uint8_t *req, size_t req_len,
                                                 const uint8_t *resp, size_t resp_len,
                                                 uint8_t *exception_code)
{
    size_t expected = sen0708_proto_reply_len(req, req_len);
    if (expected == 0 || resp == NULL) {
        return SEN0708_PROTO_ERR_ARG;
    }
    const uint8_t addr = req[0];
    const uint8_t func = req[1];

    if (sen0708_proto_is_exception(resp, resp_len, func)) {
        if (resp_len < SEN0708_EXCEPTION_LEN) {
            return SEN0708_PROTO_ERR_SHORT;
        }
        if (resp[0] != addr) {
            return SEN0708_PROTO_ERR_ADDR;
        }
        if (!crc_ok(resp, SEN0708_EXCEPTION_LEN)) {
            return SEN0708_PROTO_ERR_CRC;
        }
        if (exception_code != NULL) {
            *exception_code = resp[2];
        }
        return SEN0708_PROTO_ERR_EXCEPTION;
    }

    if (resp_len < expected) {
        return SEN0708_PROTO_ERR_SHORT;
    }
    if (resp[0] != addr) {
        return SEN0708_PROTO_ERR_ADDR;
    }
    if (!crc_ok(resp, expected)) {
        return SEN0708_PROTO_ERR_CRC;
    }
    if (resp[1] != func) {
        return SEN0708_PROTO_ERR_FUNC;
    }

    switch (func) {
    case SEN0708_FUNC_READ_HOLDING:
        if (resp[2] != (uint8_t)(expected - 5)) {
            return SEN0708_PROTO_ERR_FORMAT;
        }
        break;
    case SEN0708_FUNC_WRITE_SINGLE:
        /* Reply must echo register and value. */
        if (memcmp(resp, req, 6) != 0) {
            return SEN0708_PROTO_ERR_FORMAT;
        }
        break;
    case SEN0708_FUNC_WRITE_MULTI:
        /* Reply must echo start address and register count. */
        if (memcmp(&resp[2], &req[2], 4) != 0) {
            return SEN0708_PROTO_ERR_FORMAT;
        }
        break;
    default:
        return SEN0708_PROTO_ERR_FUNC;
    }
    return SEN0708_PROTO_OK;
}

void sen0708_proto_get_regs(const uint8_t *resp, uint16_t count, uint16_t *regs)
{
    for (uint16_t i = 0; i < count; i++) {
        regs[i] = get_u16_be(&resp[3 + i * 2]);
    }
}

sen0708_proto_status_t sen0708_proto_decode_reading(uint16_t raw_ph, uint16_t raw_temp,
                                                    float *ph, float *temp_c)
{
    float p = (float)raw_ph / 100.0f;
    float t = (float)(int16_t)raw_temp / 10.0f;
    if (p < SEN0708_PH_MIN || p > SEN0708_PH_MAX || t < SEN0708_TEMP_MIN_C ||
        t > SEN0708_TEMP_MAX_C) {
        return SEN0708_PROTO_ERR_RANGE;
    }
    if (ph != NULL) {
        *ph = p;
    }
    if (temp_c != NULL) {
        *temp_c = t;
    }
    return SEN0708_PROTO_OK;
}

uint16_t sen0708_proto_encode_signed(float value, float scale)
{
    long v = lroundf(value * scale);
    if (v > INT16_MAX) {
        v = INT16_MAX;
    } else if (v < INT16_MIN) {
        v = INT16_MIN;
    }
    return (uint16_t)(int16_t)v;
}

float sen0708_proto_decode_signed(uint16_t raw, float scale)
{
    return (float)(int16_t)raw / scale;
}

const char *sen0708_proto_status_str(sen0708_proto_status_t st)
{
    switch (st) {
    case SEN0708_PROTO_OK: return "ok";
    case SEN0708_PROTO_ERR_ARG: return "bad argument";
    case SEN0708_PROTO_ERR_SHORT: return "short frame";
    case SEN0708_PROTO_ERR_CRC: return "CRC mismatch";
    case SEN0708_PROTO_ERR_ADDR: return "wrong slave address";
    case SEN0708_PROTO_ERR_FUNC: return "unexpected function code";
    case SEN0708_PROTO_ERR_EXCEPTION: return "Modbus exception";
    case SEN0708_PROTO_ERR_FORMAT: return "malformed reply";
    case SEN0708_PROTO_ERR_RANGE: return "value out of range";
    }
    return "unknown";
}
