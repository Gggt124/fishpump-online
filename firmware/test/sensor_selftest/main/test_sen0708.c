/*
 * SEN0708 frame layer and calibration state machine tests, ported from the
 * bench project test/sen0708_rs485/main/selftest.c. Test vectors are the
 * example frames from the DFRobot SEN0708 wiki.
 */
#include <math.h>
#include <string.h>

#include "ph_cal.h"
#include "selftest.h"
#include "sen0708_proto.h"

#define CHECK_FRAME(buf, len, ...)                                              \
    do {                                                                        \
        const uint8_t exp_[] = {__VA_ARGS__};                                   \
        CHECK((len) == sizeof(exp_) && memcmp((buf), exp_, sizeof(exp_)) == 0); \
    } while (0)

static void test_build_frames(void)
{
    uint8_t f[SEN0708_MAX_FRAME_LEN];
    size_t n;

    n = sen0708_proto_build_read(0x01, SEN0708_REG_PH, 2, f);
    CHECK_FRAME(f, n, 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0xC4, 0x0B);

    n = sen0708_proto_build_write_single(0x01, SEN0708_REG_PH_OFFSET,
                                         sen0708_proto_encode_signed(0.10f, 100.0f), f);
    CHECK_FRAME(f, n, 0x01, 0x06, 0x00, 0x50, 0x00, 0x0A, 0x09, 0xDC);

    n = sen0708_proto_build_write_single(0x01, 0x07D0, 0x0002, f);
    CHECK_FRAME(f, n, 0x01, 0x06, 0x07, 0xD0, 0x00, 0x02, 0x08, 0x86);

    const uint16_t p1[2] = {1, 401};
    n = sen0708_proto_build_write_multi(0x01, SEN0708_REG_CALIBRATION, 2, p1, f);
    CHECK_FRAME(f, n, 0x01, 0x10, 0x01, 0x20, 0x00, 0x02, 0x04, 0x00, 0x01, 0x01, 0x91, 0x6D, 0xDB);

    const uint16_t p2[2] = {2, 918};
    n = sen0708_proto_build_write_multi(0x01, SEN0708_REG_CALIBRATION, 2, p2, f);
    CHECK_FRAME(f, n, 0x01, 0x10, 0x01, 0x20, 0x00, 0x02, 0x04, 0x00, 0x02, 0x03, 0x96, 0xDD, 0x79);

    CHECK(sen0708_proto_build_read(0x01, 0, 0, f) == 0);
}

static void test_read_reply(void)
{
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    size_t rl = sen0708_proto_build_read(0x01, SEN0708_REG_PH, 2, req);
    CHECK(sen0708_proto_reply_len(req, rl) == 9);

    /* Wiki example reply. The wiki prints CRC "B8 BE", which is wrong for this
     * payload; the correct Modbus CRC is DA 25 (all other wiki frames match). */
    uint8_t resp[] = {0x01, 0x03, 0x04, 0x03, 0x16, 0x01, 0x09, 0xDA, 0x25};
    CHECK(sen0708_proto_check_reply(req, rl, resp, sizeof(resp), NULL) == SEN0708_PROTO_OK);

    uint16_t regs[2];
    sen0708_proto_get_regs(resp, 2, regs);
    float ph = 0, t = 0;
    CHECK(sen0708_proto_decode_reading(regs[0], regs[1], &ph, &t) == SEN0708_PROTO_OK);
    CHECK(fabsf(ph - 7.90f) < 0.001f);
    CHECK(fabsf(t - 26.5f) < 0.001f);

    /* Error cases */
    uint8_t bad_crc[sizeof(resp)];
    memcpy(bad_crc, resp, sizeof(resp));
    bad_crc[8] ^= 0xFF;
    CHECK(sen0708_proto_check_reply(req, rl, bad_crc, sizeof(bad_crc), NULL) == SEN0708_PROTO_ERR_CRC);

    uint8_t wrong_addr[sizeof(resp)];
    memcpy(wrong_addr, resp, sizeof(resp));
    wrong_addr[0] = 0x02;
    CHECK(sen0708_proto_check_reply(req, rl, wrong_addr, sizeof(wrong_addr), NULL) == SEN0708_PROTO_ERR_ADDR);

    CHECK(sen0708_proto_check_reply(req, rl, resp, 5, NULL) == SEN0708_PROTO_ERR_SHORT);

    /* Exception reply: 01 83 02 + CRC */
    uint8_t exc[5] = {0x01, 0x83, 0x02, 0, 0};
    uint16_t crc = sen0708_proto_crc16(exc, 3);
    exc[3] = (uint8_t)(crc & 0xFF);
    exc[4] = (uint8_t)(crc >> 8);
    uint8_t code = 0;
    CHECK(sen0708_proto_is_exception(exc, 2, 0x03));
    CHECK(sen0708_proto_check_reply(req, rl, exc, sizeof(exc), &code) == SEN0708_PROTO_ERR_EXCEPTION);
    CHECK(code == 0x02);

    /* Out-of-range values */
    CHECK(sen0708_proto_decode_reading(1500, 250, NULL, NULL) == SEN0708_PROTO_ERR_RANGE);
    CHECK(sen0708_proto_decode_reading(700, (uint16_t)(int16_t)-300, NULL, NULL) == SEN0708_PROTO_ERR_RANGE);
    CHECK(sen0708_proto_decode_reading(700, (uint16_t)(int16_t)-55, NULL, &t) == SEN0708_PROTO_OK);
    CHECK(fabsf(t + 5.5f) < 0.001f);
}

static void test_write_replies(void)
{
    uint8_t req[SEN0708_MAX_FRAME_LEN];
    size_t rl = sen0708_proto_build_write_single(0x01, SEN0708_REG_PH_OFFSET, 0x000A, req);
    /* Echo reply */
    CHECK(sen0708_proto_check_reply(req, rl, req, rl, NULL) == SEN0708_PROTO_OK);
    /* Echo with a different value is rejected */
    uint8_t other[SEN0708_MAX_FRAME_LEN];
    size_t ol = sen0708_proto_build_write_single(0x01, SEN0708_REG_PH_OFFSET, 0x000B, other);
    CHECK(sen0708_proto_check_reply(req, rl, other, ol, NULL) == SEN0708_PROTO_ERR_FORMAT);

    const uint16_t p1[2] = {1, 401};
    rl = sen0708_proto_build_write_multi(0x01, SEN0708_REG_CALIBRATION, 2, p1, req);
    const uint8_t ok[] = {0x01, 0x10, 0x01, 0x20, 0x00, 0x02, 0x41, 0xFE};
    CHECK(sen0708_proto_check_reply(req, rl, ok, sizeof(ok), NULL) == SEN0708_PROTO_OK);

    /* Signed encoding */
    CHECK(sen0708_proto_encode_signed(-0.25f, 100.0f) == 0xFFE7);
    CHECK(fabsf(sen0708_proto_decode_signed(0xFFE7, 100.0f) + 0.25f) < 0.001f);
    CHECK(sen0708_proto_encode_signed(-1.5f, 10.0f) == 0xFFF1);
}

static void test_stability(void)
{
    ph_stability_t s;
    ph_stability_reset(&s);
    for (int i = 0; i < PH_STABILITY_SAMPLES - 1; i++) {
        ph_stability_push(&s, 7.00f);
    }
    CHECK(!ph_stability_is_stable(&s)); /* not enough samples */
    ph_stability_push(&s, 7.03f);
    CHECK(ph_stability_is_stable(&s));
    ph_stability_push(&s, 7.10f);
    CHECK(!ph_stability_is_stable(&s));
}

static void test_calibration(void)
{
    ph_cal_t c;
    ph_cal_init(&c);

    /* Point 2 before point 1 */
    CHECK(ph_cal_begin_point(&c, 2, 9.18f, true, true) == PH_CAL_ERR_WRONG_STEP);
    CHECK(c.state == PH_CAL_IDLE);

    /* Range, sensor and stability checks */
    CHECK(ph_cal_begin_point(&c, 1, 6.86f, true, true) == PH_CAL_ERR_OUT_OF_RANGE);
    CHECK(ph_cal_begin_point(&c, 1, NAN, true, true) == PH_CAL_ERR_OUT_OF_RANGE);
    CHECK(ph_cal_begin_point(&c, 1, 4.01f, true, false) == PH_CAL_ERR_SENSOR_FAULT);
    CHECK(ph_cal_begin_point(&c, 1, 4.01f, false, true) == PH_CAL_ERR_NOT_STABLE);
    CHECK(c.state == PH_CAL_IDLE);

    /* Failed sensor write keeps the state */
    CHECK(ph_cal_begin_point(&c, 1, 4.01f, true, true) == PH_CAL_ERR_NONE);
    ph_cal_finish_point(&c, 1, 4.01f, false, 1000);
    CHECK(c.state == PH_CAL_IDLE && c.last_error == PH_CAL_ERR_SENSOR_WRITE);

    /* Happy path */
    CHECK(ph_cal_begin_point(&c, 1, 4.01f, true, true) == PH_CAL_ERR_NONE);
    ph_cal_finish_point(&c, 1, 4.01f, true, 1000);
    CHECK(c.state == PH_CAL_WAITING_POINT2 && c.last_error == PH_CAL_ERR_NONE);
    CHECK(ph_cal_begin_point(&c, 2, 4.01f, true, true) == PH_CAL_ERR_OUT_OF_RANGE);
    CHECK(c.state == PH_CAL_WAITING_POINT2);
    CHECK(ph_cal_begin_point(&c, 2, 9.18f, true, true) == PH_CAL_ERR_NONE);
    ph_cal_finish_point(&c, 2, 9.18f, true, 2000);
    CHECK(c.state == PH_CAL_DONE);

    /* Timeout, including across the 32-bit millisecond wrap */
    ph_cal_init(&c);
    uint32_t t0 = 0xFFFFF000u;
    CHECK(ph_cal_begin_point(&c, 1, 4.00f, true, true) == PH_CAL_ERR_NONE);
    ph_cal_finish_point(&c, 1, 4.00f, true, t0);
    ph_cal_tick(&c, t0 + PH_CAL_TIMEOUT_MS - 1);
    CHECK(c.state == PH_CAL_WAITING_POINT2);
    ph_cal_tick(&c, t0 + PH_CAL_TIMEOUT_MS);
    CHECK(c.state == PH_CAL_IDLE && c.last_error == PH_CAL_ERR_TIMEOUT);

    /* Cancel */
    CHECK(ph_cal_begin_point(&c, 1, 4.01f, true, true) == PH_CAL_ERR_NONE);
    ph_cal_finish_point(&c, 1, 4.01f, true, 0);
    ph_cal_cancel(&c);
    CHECK(c.state == PH_CAL_IDLE && ph_cal_error_str(c.last_error) == NULL);
}

void test_sen0708(void)
{
    test_build_frames();
    test_read_reply();
    test_write_replies();
    test_stability();
    test_calibration();
}
