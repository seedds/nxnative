#include "../motion_pro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void word(uint8_t *r, int k, int i, int v) { uint16_t n = (uint16_t)v; r[13 + 12 * k + 2 * i] = n; r[14 + 12 * k + 2 * i] = n >> 8; }
int main(void) {
    pro_motion s = {0}; pro_motion_sample out[3]; uint8_t r[64] = {0x30}; uint64_t t = 1000000000;
    assert(!pro_motion_decode(&s, r, 48, t, out));
    r[0] = 0x21; assert(!pro_motion_decode(&s, r, 49, t, out)); r[0] = 0x30;
    assert(!pro_motion_decode(&s, r, 49, t, out));   // empty sensor stream
    r[1]++;
    for (int k = 0; k < 3; k++) { word(r, k, 0, -4096); word(r, k, 1, 2048); word(r, k, 2, 4096); word(r, k, 3, 100 + k); word(r, k, 4, -200); word(r, k, 5, 300); }
    assert(pro_motion_decode(&s, r, 49, t, out) == 3);
    for (int k = 0; k < 3; k++) {
        assert(out[k].acc[0] == .5f && out[k].acc[1] == 1 && out[k].acc[2] == -1);
        assert(fabsf(out[k].rot[0] - 14.f * (float)M_PI / 180) < 1e-6f);
        assert(fabsf(out[k].rot[1] - (100 + k) * .070f * (float)M_PI / 180) < 1e-6f);
        assert(fabsf(out[k].rot[2] - 21.f * (float)M_PI / 180) < 1e-6f);
        assert(out[k].ns == t - (2 - k) * PRO_MOTION_PERIOD_NS);
    }
    assert(!pro_motion_decode(&s, r, 49, t + 15000000, out));   // duplicate packet
    memset(r + 13, 0, 36); r[1]++;
    assert(pro_motion_decode(&s, r, 64, t + 15000000, out) == 3);   // real free fall after live data
    assert(out[2].acc[2] == 0 && out[2].rot[2] == 0);
    assert(!pro_motion_decode(&s, r, 49, t + PRO_MOTION_STALE_NS + 15000001, out));   // stale stream must become live again
    r[1] = 255; word(r, 2, 2, -32768); word(r, 2, 5, -32768);
    assert(pro_motion_decode(&s, r, 49, t + 400000000, out) == 3 && out[2].acc[2] == 8 && out[2].rot[2] < 0);
    r[1] = 0; assert(pro_motion_decode(&s, r, 49, t + 415000000, out) == 3);   // report counter wrap
    for (int k = 0; k < 3; k++) word(r, k, 5, 20);
    r[1]++; assert(pro_motion_decode(&s, r, 49, t + 430000000, out) == 3 && out[2].rot[2] > 0);   // slow turn is not learned as bias
    assert(pro_motion_player(1, 1, 2) == 2);
    assert(pro_motion_player(1, 1, -1) == -1 && pro_motion_player(0, 1, 0) == -1);
    assert(pro_motion_player(2, 1, 0) == -1 && pro_motion_player(1, 2, 0) == -1);
    assert(!pro_live(0, t));                                                          // an entry that never sent report 0x30
    assert(pro_live(t, t) && pro_live(t, t + PRO_SILENT_NS - 1));                     // still streaming
    assert(!pro_live(t, t + PRO_SILENT_NS) && !pro_live(t + 1, t));                   // silent for a second; a stamp ahead of the clock
    pro_motion d = {0}; uint8_t q[78] = {0x11}; const float rd = 2000.f / 32767.f * (float)M_PI / 180.f;
    assert(!ds4_motion_decode(&d, q, 77, t, out) && !ds4_motion_decode(&d, q, 78, t, out));   // short report; empty sensor stream
    int16_t w[6] = {1000, -500, 250, 0, 8192, 0};   // rotation x, y, z, acceleration: lying face up
    for (int i = 0; i < 6; i++) { q[15 + 2 * i] = (uint8_t)w[i]; q[16 + 2 * i] = (uint8_t)((uint16_t)w[i] >> 8); }
    q[0] = 0x01; assert(!ds4_motion_decode(&d, q, 78, t, out)); q[0] = 0x11;
    assert(ds4_motion_decode(&d, q, 78, t, out) == 1 && out[0].ns == t);
    assert(out[0].acc[0] == 0 && out[0].acc[1] == 0 && out[0].acc[2] == -1);
    assert(fabsf(out[0].rot[0] - 1000 * rd) < 1e-6f && fabsf(out[0].rot[1] + 250 * rd) < 1e-6f && fabsf(out[0].rot[2] + 500 * rd) < 1e-6f);
    memset(q + 15, 0, 12); q[26] = 0x20;   // acceleration z +8192: standing on its shoulder buttons, grips up (Switch +1 G on y, as gravity on the Pro)
    assert(ds4_motion_decode(&d, q, 78, t + 4000000, out) == 1 && out[0].acc[0] == 0 && out[0].acc[1] == 1 && out[0].acc[2] == 0);
    memset(q + 15, 0, 12);
    assert(ds4_motion_decode(&d, q, 78, t + 8000000, out) == 1);   // real free fall after live data
    assert(!ds4_motion_decode(&d, q, 78, t + 8000000 + PRO_MOTION_STALE_NS + 1, out));   // stale stream must become live again
    q[17] = 0x01; assert(ds4_motion_decode(&d, q, 78, t + 8000000 + PRO_MOTION_STALE_NS + 4000001, out) == 1);
    // Joy-Con 1: subcommand framing, SPI gyro calibration (factory 0x6020 and user 0x8026), and Joy-Con (R) flipped PCB axes
    uint8_t cmd[16], arg = 1;
    assert(pro_subcmd_build(cmd, 0x15, 0x40, &arg, 1) == 12 && cmd[0] == 0x01 && cmd[1] == 0x05 && cmd[10] == 0x40 && cmd[11] == 1);
    assert(!pro_subcmd_build(cmd, 0, 0x10, cmd, 6));
    uint8_t spi[64] = {0x21}; spi[13] = 0x90; spi[14] = 0x10; spi[15] = 0x20; spi[16] = 0x60; spi[19] = 24;
    int16_t goff[3] = {-30, 10, -5}, gsens[3] = {15335, 13371, 13371};
    for (int i = 0; i < 3; i++) {
        spi[32 + 2 * i] = (uint8_t)goff[i]; spi[33 + 2 * i] = (uint8_t)((uint16_t)goff[i] >> 8);
        spi[38 + 2 * i] = (uint8_t)gsens[i]; spi[39 + 2 * i] = (uint8_t)((uint16_t)gsens[i] >> 8);
    }
    pro_imu_cal cal = {0};
    assert(!pro_imu_cal_parse(&cal, spi, 43));   // truncated (len < 20 + r[19])
    assert(pro_imu_cal_parse(&cal, spi, 44) && cal.valid == 1 && cal.gyro_off[0] == -30);
    // Out-of-range denom on axis 1 must fail without corrupting already-valid axis 0
    uint8_t bad[64]; memcpy(bad, spi, sizeof bad); bad[32] = 99; bad[40] = 0; bad[41] = 0;
    assert(!pro_imu_cal_parse(&cal, bad, 44) && cal.valid == 1 && cal.gyro_off[0] == -30);
    // User calibration (0x8026 with magic 0xB2 0xA1) overrides factory; factory then does not overwrite user
    uint8_t uspi[64] = {0x21}; uspi[13] = 0x90; uspi[14] = 0x10; uspi[15] = 0x26; uspi[16] = 0x80; uspi[19] = 26; uspi[20] = 0xb2; uspi[21] = 0xa1;
    for (int i = 0; i < 3; i++) {
        int16_t uo = (int16_t)(i + 1), us = 13371;
        uspi[34 + 2 * i] = (uint8_t)uo; uspi[35 + 2 * i] = (uint8_t)((uint16_t)uo >> 8);
        uspi[40 + 2 * i] = (uint8_t)us; uspi[41 + 2 * i] = (uint8_t)((uint16_t)us >> 8);
    }
    assert(pro_imu_cal_parse(&cal, uspi, 46) && cal.valid == 2 && cal.gyro_off[0] == 1);
    assert(!pro_imu_cal_parse(&cal, spi, 44) && cal.valid == 2 && cal.gyro_off[0] == 1);
    // Joy-Con (R) flipped PCB: level face-up has raw z = -4096 (acc z = -1), yaw left has raw gz = -1000 (+Switch Z)
    pro_motion jr = {.flip_r = 1}; assert(pro_imu_cal_parse(&jr.cal, spi, 44));
    memset(r, 0, sizeof r); r[0] = 0x30; r[1] = 1;
    for (int k = 0; k < 3; k++) { word(r, k, 2, -4096); word(r, k, 3, -30); word(r, k, 4, 10); word(r, k, 5, -1005); }
    assert(pro_motion_decode(&jr, r, 49, t, out) == 3);
    assert(out[0].acc[0] == 0 && out[0].acc[1] == 0 && out[0].acc[2] == -1);
    assert(fabsf(out[0].rot[0]) < 1e-6f && fabsf(out[0].rot[1]) < 1e-6f);
    assert(fabsf(out[0].rot[2] - 1000.f * jr.cal.gyro_rad[2]) < 1e-5f);
    pro_motion_reset(&jr); assert(!jr.live && !jr.seen && jr.flip_r == 1 && jr.cal.valid == 1);
    puts("t_motion_pro: passed");
}
