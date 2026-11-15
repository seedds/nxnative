// pad_jc1.h: the output report, the SPI calibration reply and the 0x30 IMU decode for an original Joy-Con; its buttons, stick, sideways hold, HD rumble, lights and pairing.
#include "../pad_jc1.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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
        assert(cal.gyro_origin[i] == (float)origin_g[i]);
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

    // 0x30 input: the chip's +X is the long axis and +Z out of the face on the left, out of the back on the right
    jc1_unit u; jc1_unit_init(&u);
    jc1_sample p[3];
    uint8_t r[64] = {0}; r[0] = 0x30; r[1] = 7;
    uint64_t t = 1000000000;
    {   // a short report is refused before a byte past its end is read (ASan: the buffer is exactly that long)
        uint8_t *shorty = malloc(30); memcpy(shorty, r, 30);
        assert(!jc1_input(&u, shorty, 30, 1, t, p) && !u.stream.seen);
        free(shorty);
    }
    assert(!jc1_input(&u, r, 48, 1, t, p));                 // one byte short
    assert(!jc1_input(&u, r, 49, 1, t, p));                 // zeroed IMU: the sensor is off
    assert(!jc1_live(&u.stream, t) && u.rest.n == 0);        // and nothing was learned from it
    for (int k = 0; k < 3; k++) { word(r, k, 0, 4096); word(r, k, 2, 4096); word(r, k, 3, 100 + k); word(r, k, 5, -200); }
    r[1]++;
    assert(jc1_input(&u, r, 49, 1, t, p) == 3 && jc1_live(&u.stream, t));
    for (int k = 0; k < 3; k++) {
        assert(fabsf(p[k].acc[1] + 1) < 1e-6f);             // +X (long) up: gravity along -y
        assert(fabsf(p[k].acc[2] + 1) < 1e-6f);             // +Z out of the face on the left, up: gravity along -z
        assert(fabsf(p[k].acc[0]) < 1e-6f);
        assert(fabsf(p[k].rot[0]) < 1e-6f);                                                       // gyro +X is the frame's y
        assert(fabsf(p[k].rot[1] - (float)(100 + k) * 0.061f * (float)M_PI / 180.f) < 1e-6f);
        assert(fabsf(p[k].rot[2] - (-200.f) * 0.061f * (float)M_PI / 180.f) < 1e-6f);
        assert(p[k].ns == t - (uint64_t)(2 - k) * JC1_IMU_PERIOD_NS);   // the first report anchors the clock at its arrival
    }
    assert(u.rest.n == 3);                                   // a fresh report teaches the rest
    assert(!jc1_input(&u, r, 49, 1, t, p) && u.rest.n == 3); // the same packet again: no samples, nothing learned twice
    {   // jitter: every sample still comes after the last one and none after its report arrived; an early report brings fewer
        uint64_t prev = p[2].ns, at = t; int short_reports = 0;
        const uint64_t arrivals[8] = {5, 5, 15, 15, 3, 15, 40, 15};   // ms after the one before: early, early, on time, on time, two early, late, on time
        for (int i = 0; i < 8; i++) {
            at += arrivals[i] * 1000000ull; r[1]++;
            int n = jc1_input(&u, r, 49, 1, at, p);
            assert(n >= 1 && n <= 3);
            short_reports += n < 3;
            for (int k = 0; k < n; k++) { assert(p[k].ns > prev); prev = p[k].ns; assert(p[k].ns <= at); }
        }
        assert(short_reports > 0);   // the early arrivals really did bring fewer, rather than the test passing by luck
        t = at;
    }
    // Right Joy-Con: +Z leaves the back, so the same raw Z is face down and the left's Y sign flips
    jc1_unit ur; jc1_unit_init(&ur); r[1] = 9;
    for (int k = 0; k < 3; k++) { word(r, k, 0, 0); word(r, k, 1, 4096); word(r, k, 2, 4096); word(r, k, 3, 0); word(r, k, 4, 0); word(r, k, 5, 0); }
    assert(jc1_input(&ur, r, 49, 0, t, p) == 3);
    assert(fabsf(p[0].acc[0] + 1) < 1e-6f);                 // +Y is the right's short axis away from the face: gravity -x
    assert(fabsf(p[0].acc[2] - 1) < 1e-6f);                 // +Z out of the back: gravity +z
    // A live stream that goes all-zero is the sensor switched off (a wake), not a free fall: no samples, the stream no longer live, and the watchdog asks again
    {
        uint64_t asked = t / 2, wait = 1000000000ull;
        assert(!jc1_quiet(&u.stream, asked, wait, t + 500000000ull));    // data half a second ago
        uint64_t off = t + 20000000ull;
        memset(r + 13, 0, 36); r[1] = 0x55;
        assert(!jc1_input(&u, r, 49, 1, off, p));
        assert(jc1_live(&u.stream, off));                                // 20 ms since the last data: still live, as one lost report would be
        for (uint64_t at = off; at < t + 1100000000ull; at += 15000000ull) { r[1]++; assert(!jc1_input(&u, r, 49, 1, at, p)); }
        assert(!jc1_live(&u.stream, t + 1100000000ull));
        assert(jc1_quiet(&u.stream, asked, wait, t + 1100000000ull));   // reports keep coming, but with no data: due for a handshake
        assert(!jc1_quiet(&u.stream, t + 1100000000ull, wait, t + 1500000000ull));   // and not again until wait after it
        assert(jc1_quiet(&u.stream, t + 1100000000ull, wait, t + 2100000000ull));
        jc1_stream none = {0};
        assert(jc1_quiet(&none, asked, wait, asked + wait) && !jc1_quiet(&none, asked, wait, asked + wait - 1));   // no report at all
    }
    // The calibration is kept whatever the logging: the factory origin is what the gyro is read against, and the rest is learned on top of it
    {
        jc1_unit c; jc1_unit_init(&c);
        int16_t og[3] = {5, 25, 2};
        for (int i = 0; i < 3; i++) { int16_t cg = (int16_t)(og[i] + 13371); memcpy(reply + 32 + 2 * i, &og[i], 2); memcpy(reply + 38 + 2 * i, &cg, 2); }
        uint8_t q[64] = {0}; q[0] = 0x30; q[1] = 1;
        for (int k = 0; k < 3; k++) { word(q, k, 2, 4096); word(q, k, 3, 45); word(q, k, 4, -25); word(q, k, 5, 62); }   // still: origin + 40/-50/60
        uint64_t at = 5000000000ull;
        for (int i = 0; i < 10; i++, at += 15000000ull) { q[1]++; assert(jc1_input(&c, q, 49, 1, at, p) > 0); }
        assert(c.rest.n == 30);
        assert(jc1_unit_cal(&c, reply, 44) && c.cal.gyro_origin[1] == 25.f);
        assert(c.rest.n == 0 && !c.rest.have);                         // learned against origin 0: started again
        for (int i = 0; i < JC1_REST_N / 3 + 1; i++, at += 15000000ull) { q[1]++; assert(jc1_input(&c, q, 49, 1, at, p) > 0); }
        assert(c.rest.have && fabsf(c.rest.bias[0] - 40.f) < 1e-4f && fabsf(c.rest.bias[1] + 50.f) < 1e-4f && fabsf(c.rest.bias[2] - 60.f) < 1e-4f);
        q[1]++; at += 15000000ull;
        assert(jc1_input(&c, q, 49, 1, at, p) == 3);
        for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) assert(fabsf(p[k].rot[i]) < 1e-6f);   // origin + rest taken off: still
        assert(jc1_unit_cal(&c, reply, 44) && c.rest.have);             // the same calibration again (a wake's handshake): the rest is kept
        assert(!jc1_unit_cal(&c, reply, 43) && c.cal.gyro_origin[1] == 25.f);   // a short reply changes nothing
    }
    // The gyro's still offset: a second of still samples gives their mean; moving restarts the window, and a mean past 60 counts (a slow turn) is not taken
    {
        int16_t res[6] = {10, -20, 30, 40, -50, 60};
        jc1_rest r2 = {0};
        for (int i = 0; i < JC1_REST_N - 1; i++) jc1_rest_update(&r2, res);
        res[3] = 500; jc1_rest_update(&r2, res);
        assert(!r2.have && !r2.n);
        for (int i = 0; i < JC1_REST_N; i++) { res[3] = 80; res[4] = -90; res[5] = 100; jc1_rest_update(&r2, res); }
        assert(!r2.have);   // 80 counts is 4.9 deg/s: a slow turn, not rest
        for (int i = 0; i < JC1_REST_N; i++) { res[3] = 20; res[4] = -30; res[5] = 40; jc1_rest_update(&r2, res); }
        assert(r2.have && fabsf(r2.bias[0] - 20.f) < 1e-4f);
    }
    // Buttons: each Joy-Con its own half of the report, rail buttons as the npad's own bits; the charging grip and the other side's bytes are not its buttons
    {
        uint8_t b[49] = {0}; b[0] = 0x30;
        b[4] = 0x80; assert(!jc1_joy_buttons(b, 0) && !jc1_joy_buttons(b, 1));
        b[5] = 0x01 | 0x20 | 0x40; b[4] = 0x01 | 0x08;   // left: Down, SL, L, Minus, stick press
        assert(jc1_joy_buttons(b, 0) == (DM_BTN_DOWN | DM_BTN_LSL | DM_BTN_L | DM_BTN_MINUS | DM_BTN_LS));
        assert(!jc1_joy_buttons(b, 1));
        memset(b + 3, 0, 3); b[3] = 0x08 | 0x10 | 0x80; b[4] = 0x02 | 0x04 | 0x10;   // right: A, SR, ZR, Plus, stick press, Home
        assert(jc1_joy_buttons(b, 1) == (DM_BTN_A | DM_BTN_RSR | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_RS));
        assert(!jc1_joy_buttons(b, 0) && jc1_home(b));
    }
    // Stick: a left Joy-Con's stick is the report's left one, a right Joy-Con's the right one, through its SPI calibration (pad_pro.h)
    {
        uint8_t q[49] = {0}; q[0] = 0x30;
        int lx = 2048 + 600, ly = 2048, rx = 2048, ry = 2048 - 600;   // left pushed right, right pushed down
        q[6] = lx & 0xff; q[7] = (uint8_t)(lx >> 8 | (ly & 0x0f) << 4); q[8] = (uint8_t)(ly >> 4);
        q[9] = rx & 0xff; q[10] = (uint8_t)(rx >> 8 | (ry & 0x0f) << 4); q[11] = (uint8_t)(ry >> 4);
        // the factory block: the left stick above, centre, below; the right one centre, below, above; each x and y of 12 bits in three bytes, reach 1200 each way
        uint8_t spi[64] = {0}; spi[0] = 0x21; spi[13] = 0x90; spi[14] = 0x10; spi[15] = 0x3D; spi[16] = 0x60; spi[19] = PAD_PRO_SPI_FACTORY_LEN;
        int left_vals[6] = {1200, 1200, 2048, 2048, 1200, 1200}, right_vals[6] = {2048, 2048, 1200, 1200, 1200, 1200};
        for (int k = 0; k < 3; k++) {
            uint8_t *d = spi + 20 + 3 * k; int x = left_vals[2 * k], y = left_vals[2 * k + 1];
            d[0] = x & 0xff; d[1] = (uint8_t)(x >> 8 | (y & 0x0f) << 4); d[2] = (uint8_t)(y >> 4);
            d = spi + 29 + 3 * k; x = right_vals[2 * k]; y = right_vals[2 * k + 1];
            d[0] = x & 0xff; d[1] = (uint8_t)(x >> 8 | (y & 0x0f) << 4); d[2] = (uint8_t)(y >> 4);
        }
        pad_pro sl = {0}, sr = {0};
        assert(pad_pro_spi_reply(&sl, spi, 20 + PAD_PRO_SPI_FACTORY_LEN) == 3 && pad_pro_spi_reply(&sr, spi, 20 + PAD_PRO_SPI_FACTORY_LEN) == 3);
        int32_t st[2];
        assert(jc1_joy_stick(&sl, q, 49, 0, st));
        int32_t half = (int32_t)((600 - 1200 * PAD_PRO_DEAD_PCT / 100) * (int64_t)DM_STICK_MAX / (1200 - 1200 * PAD_PRO_DEAD_PCT / 100));
        assert(st[0] == half && st[1] == 0);
        assert(jc1_joy_stick(&sr, q, 49, 1, st) && st[0] == 0 && st[1] == -half);
        assert(!jc1_joy_stick(&sl, q, 11, 0, st));   // too short for both sticks
    }
    // Held sideways: the left Joy-Con turned counterclockwise, the right one clockwise; the rail toward the top in both
    {
        int32_t up_rail[2] = {DM_STICK_MAX, 0}, out[2];   // the left Joy-Con's stick pushed toward its rail (+x upright)
        jc1_sideways_stick(up_rail, 0, out); assert(out[0] == 0 && out[1] == DM_STICK_MAX);
        int32_t to_top[2] = {0, DM_STICK_MAX};            // toward its top, which points left held sideways
        jc1_sideways_stick(to_top, 0, out); assert(out[0] == -DM_STICK_MAX && out[1] == 0);
        int32_t rail_r[2] = {-DM_STICK_MAX, 0};          // the right one's rail is on its left
        jc1_sideways_stick(rail_r, 1, out); assert(out[0] == 0 && out[1] == DM_STICK_MAX);
        jc1_sideways_stick(to_top, 1, out); assert(out[0] == DM_STICK_MAX && out[1] == 0);   // its top points right
        float g[3] = {-1, 0, 0}, v[3];   // the left one lying on its side, rail up: gravity along -x upright, so -y sideways (top up)
        jc1_sideways_vec(g, 0, v); assert(v[0] == 0 && v[1] == -1 && v[2] == 0);
        float gr[3] = {1, 0, 0};          // the right one, rail up: gravity along +x upright
        jc1_sideways_vec(gr, 1, v); assert(v[0] == 0 && v[1] == -1 && v[2] == 0);
        assert(jc1_sideways_buttons(DM_BTN_DOWN | DM_BTN_LSL | DM_BTN_LS, 0) == (DM_BTN_A | DM_BTN_L | DM_BTN_LS));
        assert(jc1_sideways_buttons(DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_LSR | DM_BTN_MINUS, 0) == (DM_BTN_B | DM_BTN_X | DM_BTN_Y | DM_BTN_R | DM_BTN_MINUS));
        assert(jc1_sideways_buttons(DM_BTN_X | DM_BTN_A | DM_BTN_B | DM_BTN_Y, 1) == (DM_BTN_A | DM_BTN_B | DM_BTN_Y | DM_BTN_X));
        assert(jc1_sideways_buttons(DM_BTN_X | DM_BTN_RS | DM_BTN_RSL | DM_BTN_RSR | DM_BTN_PLUS, 1) == (DM_BTN_A | DM_BTN_LS | DM_BTN_L | DM_BTN_R | DM_BTN_PLUS));
    }
    // A pad as one whole controller: a pair is a Pro Controller without rail buttons, one alone is held sideways, none is nothing
    {
        int have[2] = {1, 1}; uint64_t b[2] = {DM_BTN_LEFT | DM_BTN_LSL, DM_BTN_A | DM_BTN_RSR}; int32_t st[2][2] = {{100, 200}, {-300, -400}}, out[4]; uint64_t m;
        assert(jc1_pad_input(have, b, st, &m, out) && m == (DM_BTN_LEFT | DM_BTN_A) && out[0] == 100 && out[1] == 200 && out[2] == -300 && out[3] == -400);
        have[0] = 0;
        assert(jc1_pad_input(have, b, st, &m, out) && m == (DM_BTN_B | DM_BTN_R) && out[0] == -400 && out[1] == 300 && !out[2] && !out[3]);
        have[0] = 1; have[1] = 0;
        assert(jc1_pad_input(have, b, st, &m, out) && m == (DM_BTN_B | DM_BTN_L) && out[0] == -200 && out[1] == 100);
        have[0] = 0; assert(!jc1_pad_input(have, b, st, &m, out) && !m);
    }
    // Routing per device: which Joy-Con a new one pairs with, which feeds the merged stream, which motor each plays, and the gestures that split and join
    {
        uint32_t sides[4] = {3, 1, 2, 1};
        assert(jc1_pair_pick(sides, 4, 1) == 1);    // a right Joy-Con joins the first left one alone
        assert(jc1_pair_pick(sides, 4, 0) == 2);    // a left one the right one alone
        assert(jc1_pair_pick(sides, 1, 0) == -1);   // a full pair takes no third
        int have[2] = {1, 1}, live[2] = {1, 1};
        assert(jc1_merged_side(have, live, 1) == 1 && jc1_merged_side(have, live, 0) == 0);
        live[1] = 0; assert(jc1_merged_side(have, live, 1) == 0);   // the right one not streaming: the left one
        live[0] = 0; assert(jc1_merged_side(have, live, 1) == 1);   // neither: the preferred one
        have[1] = 0; assert(jc1_merged_side(have, live, 1) == 0);
        have[0] = 0; assert(jc1_merged_side(have, live, 1) == -1);
        dm_vibration m[2] = {{0.5f, 100, 0.1f, 300}, {0.2f, 150, 0.6f, 400}};
        dm_vibration v = jc1_rumble_pick(m, 0, 1); assert(v.amplitude_low == 0.5f && v.frequency_high == 300);
        v = jc1_rumble_pick(m, 1, 1); assert(v.amplitude_high == 0.6f && v.frequency_low == 150);
        v = jc1_rumble_pick(m, 1, 0); assert(v.amplitude_low == 0.5f && v.frequency_low == 100 && v.amplitude_high == 0.6f && v.frequency_high == 400);
        assert(jc1_gesture(DM_BTN_LSL | DM_BTN_LSR, 0, 1) == JC1_GESTURE_SPLIT && jc1_gesture(DM_BTN_LSL, 0, 1) == JC1_GESTURE_NONE);
        assert(jc1_gesture(DM_BTN_RSL | DM_BTN_RSR, 0, 1) == JC1_GESTURE_NONE);   // the other side's rails
        assert(jc1_gesture(DM_BTN_R, 1, 0) == JC1_GESTURE_JOIN && jc1_gesture(DM_BTN_R | DM_BTN_RSL, 1, 0) == JC1_GESTURE_NONE && jc1_gesture(DM_BTN_R, 1, 1) == JC1_GESTURE_NONE);
        jc1_hold h = {0}; uint64_t at = 1000;
        assert(!jc1_hold_update(&h, JC1_GESTURE_JOIN, at) && !jc1_hold_update(&h, JC1_GESTURE_JOIN, at + JC1_GESTURE_NS - 1) && jc1_hold_update(&h, JC1_GESTURE_JOIN, at + JC1_GESTURE_NS));
        assert(!jc1_hold_update(&h, JC1_GESTURE_NONE, at + JC1_GESTURE_NS + 1) && !jc1_hold_update(&h, JC1_GESTURE_JOIN, at + 2 * JC1_GESTURE_NS));   // a break starts again
        assert(jc1_led_pattern(0) == 0x01 && jc1_led_pattern(3) == 0x0F && jc1_led_pattern(7) == 0x06 && jc1_led_pattern(-1) == 0xF0);
    }
    // HD rumble: nothing at the default frequencies is the neutral sample the subcommand reports carry; the codes match Linux hid-nintendo's tables
    {
        uint8_t e[4];
        dm_vibration off = {0, 160, 0, 320}; jc1_rumble_encode(&off, e); assert(memcmp(e, jc1_neutral_rumble, 4) == 0 && !jc1_vibration_on(&off));
        dm_vibration full = {1, 160, 1, 320}; jc1_rumble_encode(&full, e);
        assert(e[0] == 0x00 && e[1] == 0xC9 && e[2] == 0x40 && e[3] == 0x72 && jc1_vibration_on(&full));   // 1.0: code 100, high 0xC8, low 0x0072
        // amplitude codes against the table's amplitudes (thousandths): 1 -> 10, 2 -> 12, 15 -> 112, 16 -> 117, 31 -> 225, 32 -> 230 (0.2299), 100 -> 1000
        assert(jc1_amp_code(0.010f) == 1 && jc1_amp_code(0.012f) == 2 && jc1_amp_code(0.112f) == 15 && jc1_amp_code(0.117f) == 16 && jc1_amp_code(0.231f) == 32 && jc1_amp_code(0.226f) == 31);
        assert(jc1_amp_code(0.f) == 0 && jc1_amp_code(0.005f) == 0 && jc1_amp_code(2.f) == 100 && jc1_amp_code(NAN) == 0);
        for (int k = 1, last = 0; k <= 1000; k++) { int c = jc1_amp_code((float)k / 1000.f); assert(c >= last); last = c; }   // monotone across the three ranges
        dm_vibration odd = {0.010f, 40, 0.012f, 2000}; jc1_rumble_encode(&odd, e);   // the low band's odd code sets bit 7 of byte 2; frequencies clamp to the bands
        assert(e[0] == (uint8_t)(((0xDF - 0x60) * 4) & 0xff) && e[1] == (uint8_t)(2 * 2 + 1) && e[2] == (0x01 | 0x80) && e[3] == 0x40);
        dm_vibration mid = {0, 160, 0.5f, 1000}; jc1_rumble_encode(&mid, e);   // 1000 Hz: code round(32 log2 100) = 213, (213 - 96) * 4 = 468 = 0x1D4; 0.5: code round(32 log2 4.35) = 68
        assert(e[0] == 0xD4 && e[1] == 2 * 68 + 1);
        uint8_t rep[JC1_RUMBLE_LEN]; jc1_rumble_report(rep, 0x13, e);
        assert(rep[0] == 0x10 && rep[1] == 3 && memcmp(rep + 2, e, 4) == 0 && memcmp(rep + 6, e, 4) == 0);
        // a pulse set and cleared between two reports still plays in the next one, then what is set now
        jc1_rumble_hold h = {0}; dm_vibration pulse[2] = {{0.8f, 160, 0, 320}, {0}}, quiet[2] = {{0, 160, 0, 320}, {0}}, got[2];
        jc1_rumble_set(&h, pulse); jc1_rumble_set(&h, quiet);
        jc1_rumble_take(&h, got); assert(got[0].amplitude_low == 0.8f);
        jc1_rumble_take(&h, got); assert(got[0].amplitude_low == 0.f);
    }

    puts("t_jc1: passed");
}
