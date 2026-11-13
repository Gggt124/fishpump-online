#pragma once

/*
 * SEN0708 Modbus RTU frame building and parsing.
 *
 * Pure functions with no ESP-IDF or FreeRTOS dependency, so they can be
 * tested without hardware. The UART side lives in sen0708.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers (DFRobot SEN0708 wiki). */
#define SEN0708_REG_PH            0x0000 /* uint16, x100 */
#define SEN0708_REG_TEMP          0x0001 /* int16,  x10 degC */
#define SEN0708_REG_PH_OFFSET     0x0050 /* int16,  x100 */
#define SEN0708_REG_TEMP_OFFSET   0x0051 /* int16,  x10 */
#define SEN0708_REG_CALIBRATION   0x0120 /* 2 regs: point no., buffer pH x100 */

#define SEN0708_FUNC_READ_HOLDING 0x03
#define SEN0708_FUNC_WRITE_SINGLE 0x06
#define SEN0708_FUNC_WRITE_MULTI  0x10
#define SEN0708_EXCEPTION_FLAG    0x80
#define SEN0708_EXCEPTION_LEN     5

/* Largest frame this driver sends or expects. */
#define SEN0708_MAX_FRAME_LEN     32
#define SEN0708_MAX_READ_REGS     8
#define SEN0708_MAX_WRITE_REGS    4

/* Valid ranges. A reading outside them is treated as a bad frame. */
#define SEN0708_PH_MIN            0.0f
#define SEN0708_PH_MAX            14.0f
#define SEN0708_TEMP_MIN_C        (-20.0f)
#define SEN0708_TEMP_MAX_C        100.0f
#define SEN0708_PH_OFFSET_LIMIT   2.0f
#define SEN0708_TEMP_OFFSET_LIMIT 10.0f

typedef enum {
    SEN0708_PROTO_OK = 0,
    SEN0708_PROTO_ERR_ARG,       /* bad argument to a build function */
    SEN0708_PROTO_ERR_SHORT,     /* frame shorter than expected */
    SEN0708_PROTO_ERR_CRC,       /* CRC mismatch */
    SEN0708_PROTO_ERR_ADDR,      /* reply from another slave */
    SEN0708_PROTO_ERR_FUNC,      /* unexpected function code */
    SEN0708_PROTO_ERR_EXCEPTION, /* Modbus exception reply */
    SEN0708_PROTO_ERR_FORMAT,    /* byte count / echo does not match */
    SEN0708_PROTO_ERR_RANGE,     /* decoded value outside the valid range */
} sen0708_proto_status_t;

uint16_t sen0708_proto_crc16(const uint8_t *data, size_t len);

/* Build functions return the frame length (CRC included), or 0 on a bad argument.
 * `out` must hold at least SEN0708_MAX_FRAME_LEN bytes. */
size_t sen0708_proto_build_read(uint8_t addr, uint16_t start, uint16_t count, uint8_t *out);
size_t sen0708_proto_build_write_single(uint8_t addr, uint16_t reg, uint16_t value, uint8_t *out);
size_t sen0708_proto_build_write_multi(uint8_t addr, uint16_t start, uint16_t count,
                                       const uint16_t *values, uint8_t *out);

/* Length of a normal (non-exception) reply to the request `req`. 0 if unknown. */
size_t sen0708_proto_reply_len(const uint8_t *req, size_t req_len);

/* True if `resp` (at least 2 bytes) is the start of an exception reply to `func`. */
bool sen0708_proto_is_exception(const uint8_t *resp, size_t len, uint8_t func);

/*
 * Validate a complete reply against the request that caused it: slave address,
 * CRC, function code, exception, byte count, and for writes the echoed fields.
 * On SEN0708_PROTO_ERR_EXCEPTION, *exception_code (if not NULL) gets the code.
 */
sen0708_proto_status_t sen0708_proto_check_reply(const uint8_t *req, size_t req_len,
                                                 const uint8_t *resp, size_t resp_len,
                                                 uint8_t *exception_code);

/* Copy register values out of a checked 0x03 reply. */
void sen0708_proto_get_regs(const uint8_t *resp, uint16_t count, uint16_t *regs);

/* Decode pH (uint16 x100) and temperature (int16 x10). Checks the valid ranges. */
sen0708_proto_status_t sen0708_proto_decode_reading(uint16_t raw_ph, uint16_t raw_temp,
                                                    float *ph, float *temp_c);

/* Signed fixed-point helpers: value * scale, rounded, as a 16-bit register. */
uint16_t sen0708_proto_encode_signed(float value, float scale);
float sen0708_proto_decode_signed(uint16_t raw, float scale);

const char *sen0708_proto_status_str(sen0708_proto_status_t st);

#ifdef __cplusplus
}
#endif
