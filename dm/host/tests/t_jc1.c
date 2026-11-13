// pad_jc1.h: the output report, the SPI calibration reply and the 0x30 IMU decode for an original Joy-Con.
#include "../pad_jc1.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void word(uint8_t *r, int k, int i, int v) { uint16_t n = (uint16_t)v; r[13 + 12 * k + 2 * i] = n; r[14 + 12 * k + 2 * i] = n >> 8; }

int main(void) {
    // Output report 0x01: id, counter, neutral rumble, subcommand, argument, zero padding; 38 argument bytes at most
    uint8_t out[JC1_REPORT_LEN], arg[40];
    for (int i = 0; i < 40; i++) arg[i] = (uint8_t)(0xa0 + i);
    jc1_output_report(out, 0x1f, JC1_SUB_SPI_READ, arg, sizeof arg);
    assert(out[0] == 0x01 && out[1] == 0x0f && memcmp(out + 2, jc1_neutral_rumble, 8) == 0 && out[10] == JC1_SUB_SPI_READ);
    assert(memcmp(out + 11, arg, 38) == 0 && out[JC1_REPORT_LEN - 1] == (uint8_t)(0xa0 + 37));   // clipped, not overrun
    memset(out, 0xff, sizeof out);
    jc1_output_report(out, 0, JC1_SUB_ENABLE_IMU, &(uint8_t){1}, 1);
    assert(out[11] == 1 && out[12] == 0 && out[48] == 0);

    // SPI 0x6020: 4 x 3 int16 (accel origin/coeff, gyro origin/coeff); scales from the notes, the gyro origin the still offset
    uint8_t reply[64] = {0};
    reply[0] = 0x21; reply[13] = 0x90; reply[14] = 0x10;
    reply[15] = 0x20; reply[16] = 0x60; reply[19] = JC1_IMU_CAL_LEN;
    int origin_a[3] = {10, -20, 30}, coeff_a[3] = {4106, 4076, 4126};      // coeff - origin = 4096: 1/1024 G per count
    int origin_g[3] = {100, 0, -13371}, coeff_g[3] = {13471, 13371, 0};   // denom 13371: 816/13371 deg/s
    for (int i = 0; i < 3; i++) {
        int16_t oa = (int16_t)origin_a[i], ca = (int16_t)coeff_a[i], og = (int16_t)origin_g[i], cg = (int16_t)coeff_g[i];
        memcpy(reply + 20 + 2 * i, &oa, 2); memcpy(reply + 26 + 2 * i, &ca, 2);
        memcpy(reply + 32 + 2 * i, &og, 2); memcpy(reply + 38 + 2 * i, &cg, 2);
    }
    jc1_cal cal; jc1_cal_default(&cal);
    assert(jc1_parse_cal(&cal, reply, 44) && cal.have);
    for (int i = 0; i < 3; i++) {
        assert(fabsf(cal.accel_scale[i] - 4.f / 4096.f) < 1e-6f);
        assert(fabsf(cal.gyro_scale[i] - 0.06103f) < 1e-5f);   // 816/13371, the default coefficient
        assert(cal.gyro_offset[i] == (float)origin_g[i]);
    }
    for (int i = 0; i < 3; i++) {   // the other encoding of the same sensitivity: 0x3BE7 where the default is 0x343B
        int16_t og = (int16_t)origin_g[i], cg = (int16_t)(origin_g[i] + 15335);
        memcpy(reply + 32 + 2 * i, &og, 2); memcpy(reply + 38 + 2 * i, &cg, 2);
    }
    assert(jc1_parse_cal(&cal, reply, 44));
    for (int i = 0; i < 3; i++) assert(fabsf(cal.gyro_scale[i] - 816.f / 15335.f * 936.f / 816.f) < 1e-6f);
    for (int i = 0; i < 3; i++) {   // back to the default coefficient
        int16_t og = (int16_t)origin_g[i], cg = (int16_t)(origin_g[i] + 13371);
        memcpy(reply + 32 + 2 * i, &og, 2); memcpy(reply + 38 + 2 * i, &cg, 2);
    }
    assert(jc1_parse_cal(&cal, reply, 44));
    reply[15] = 0x21; assert(!jc1_parse_cal(&cal, reply, 44)); reply[15] = 0x20;   // another SPI address
    assert(!jc1_parse_cal(&cal, reply, 30));                                        // short reply
    reply[0] = 0x30; assert(!jc1_parse_cal(&cal, reply, 44)); reply[0] = 0x21;

    // 0x30 decode: the chip's +X is the long axis and +Z out of the face on the left, out of the back on the right
    jc1_cal_default(&cal);
    jc1_stream s = {0}; jc1_sample p[3];
    uint8_t r[64] = {0}; r[0] = 0x30; r[1] = 7;
    uint64_t t = 1000000000;
    assert(!jc1_decode(&s, r, 48, 1, &cal, t, p));         // short
    assert(!jc1_decode(&s, r, 49, 1, &cal, t, p));         // zeroed IMU: the sensor is off, not a free fall
    assert(!s.live);
    for (int k = 0; k < 3; k++) { word(r, k, 0, 4096); word(r, k, 2, 4096); word(r, k, 3, 100 + k); word(r, k, 5, -200); }
    assert(jc1_decode(&s, r, 49, 1, &cal, t, p) == 3);
    for (int k = 0; k < 3; k++) {
        assert(fabsf(p[k].acc[1] + 1) < 1e-6f);             // +X (long) up: gravity along -y
        assert(fabsf(p[k].acc[2] + 1) < 1e-6f);             // +Z out of the face on the left, up: gravity along -z
        assert(fabsf(p[k].acc[0]) < 1e-6f);
        assert(fabsf(p[k].rot[0]) < 1e-6f);                                                       // gyro +X is the frame's y
        assert(fabsf(p[k].rot[1] - (float)(100 + k) * 0.061f * (float)M_PI / 180.f) < 1e-6f);
        assert(fabsf(p[k].rot[2] - (-200.f) * 0.061f * (float)M_PI / 180.f) < 1e-6f);
        assert(p[k].ns == t - (uint64_t)(2 - k) * JC1_IMU_PERIOD_NS);   // the first report anchors the clock at its arrival
    }
    assert(!jc1_decode(&s, r, 49, 1, &cal, t, p));          // the same packet again
    {   // jitter: every sample still comes after the last one and none after its report arrived; an early report brings fewer
        uint64_t prev = p[2].ns, at = t; int short_reports = 0;
        const uint64_t arrivals[8] = {5, 5, 15, 15, 3, 15, 40, 15};   // ms after the one before: early, early, on time, on time, two early, late, on time
        for (int i = 0; i < 8; i++) {
            at += arrivals[i] * 1000000ull; r[1]++;
            int n = jc1_decode(&s, r, 49, 1, &cal, at, p);
            assert(n >= 1 && n <= 3);
            short_reports += n < 3;
            for (int k = 0; k < n; k++) { assert(p[k].ns > prev); prev = p[k].ns; assert(p[k].ns <= at); }
        }
        assert(short_reports > 0);   // the early arrivals really did bring fewer, rather than the test passing by luck
    }
    // Right Joy-Con: +Z leaves the back, so the same raw Z is face down and the left's Y sign flips
    jc1_stream sr = {0}; r[1] = 9;
    for (int k = 0; k < 3; k++) { word(r, k, 0, 0); word(r, k, 1, 4096); word(r, k, 2, 4096); word(r, k, 3, 0); word(r, k, 5, 0); }
    assert(jc1_decode(&sr, r, 49, 0, &cal, t, p) == 3);
    assert(fabsf(p[0].acc[0] + 1) < 1e-6f);                 // +Y is the right's short axis away from the face: gravity -x
    assert(fabsf(p[0].acc[2] - 1) < 1e-6f);                 // +Z out of the back: gravity +z
    // A live stream that goes all-zero is a real free fall, until it goes stale and the sensor has to be woken again
    uint64_t fall = t + 200000000ull;   // the jitter block above left the stream receiving at about t + 113 ms
    memset(r + 13, 0, 36); r[1]++;
    assert(jc1_decode(&s, r, 49, 1, &cal, fall, p) == 3 && p[2].acc[0] == 0 && p[2].rot[2] == 0);
    assert(!jc1_decode(&s, r, 49, 1, &cal, fall + JC1_STALE_NS + 1, p));
    assert(!s.live);
    // The gyro's still offset: a second of still samples gives their mean, and the samples are taken against the factory's origin plus it
    {
        jc1_rest r = {0};
        int16_t res[6] = {10, -20, 30, 40, -50, 60};   // the accelerometer still, the gyro reading 40/-50/60 counts off
        for (int i = 0; i < JC1_REST_N; i++) jc1_rest_update(&r, res);
        assert(r.have && fabsf(r.bias[0] - 40.f) < 1e-4f && fabsf(r.bias[1] + 50.f) < 1e-4f && fabsf(r.bias[2] - 60.f) < 1e-4f);
        int16_t origin[3] = {5, 25, 2};
        jc1_rest_apply(&cal, &r, origin);
        assert(fabsf(cal.gyro_offset[0] - 45.f) < 1e-4f && fabsf(cal.gyro_offset[1] + 25.f) < 1e-4f && fabsf(cal.gyro_offset[2] - 62.f) < 1e-4f);
        // moving: the window starts again, and a mean past 60 counts (a slow turn) is not taken
        jc1_rest r2 = {0};
        for (int i = 0; i < JC1_REST_N - 1; i++) jc1_rest_update(&r2, res);
        res[3] = 500; jc1_rest_update(&r2, res);
        assert(!r2.have && !r2.n);
        for (int i = 0; i < JC1_REST_N; i++) { res[3] = 80; res[4] = -90; res[5] = 100; jc1_rest_update(&r2, res); }
        assert(!r2.have);   // 80 counts is 4.9 deg/s: a slow turn, not rest
        for (int i = 0; i < JC1_REST_N; i++) { res[3] = 20; res[4] = -30; res[5] = 40; jc1_rest_update(&r2, res); }
        assert(r2.have && fabsf(r2.bias[0] - 20.f) < 1e-4f);
        int16_t o2[3] = {0, 0, 0};
        jc1_rest_apply(&cal, &r2, o2);
        assert(fabsf(cal.gyro_offset[0] - 20.f) < 1e-4f);
    }

    puts("t_jc1: passed");
}
