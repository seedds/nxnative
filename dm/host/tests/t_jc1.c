// pad_jc1.h: the output report, the SPI calibration reply and the 0x30 IMU decode for an original Joy-Con.
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
    // Routing by a press: the charging-grip bit is no button; one Joy-Con pressing alone, settled, with one controller pressed binds; anything less sure waits, then gives up
    {
        uint8_t b[49] = {0}; b[4] = 0x80; assert(!jc1_buttons(b));
        b[5] = 0x40; assert(jc1_buttons(b));
        const uint64_t S = JC1_PRESS_SETTLE_NS, G = JC1_PRESS_GIVE_UP_NS;
        uint64_t at = 10000000000ull;
        jc1_press all[2] = {{0}, {0}};
        assert(jc1_press_judge(all, 2, 0, 1, at) == JC1_PRESS_NONE);
        jc1_press_update(&all[0], 1, at);
        assert(jc1_press_judge(all, 2, 0, 1, at + S - 1) == JC1_PRESS_WAIT);    // GameController may not have seen it yet
        assert(jc1_press_judge(all, 2, 0, 0, at + S) == JC1_PRESS_WAIT);
        assert(jc1_press_judge(all, 2, 0, 2, at + S) == JC1_PRESS_WAIT);        // two controllers pressed: another player
        assert(jc1_press_judge(all, 2, 0, 1, at + S) == JC1_PRESS_BIND);
        jc1_press_update(&all[1], 1, at + 10);                                  // another Joy-Con pressing too: not this press's to say
        assert(jc1_press_judge(all, 2, 0, 1, at + S + 20) == JC1_PRESS_WAIT);
        assert(jc1_press_judge(all, 2, 0, 1, at + G) == JC1_PRESS_UNSURE);
        jc1_press_update(&all[1], 0, at + G);                                   // it lets go: its controller may still show the press for a moment
        assert(jc1_press_judge(all, 2, 0, 1, at + G + S - 1) == JC1_PRESS_UNSURE);
        all[0].judged = 1; assert(jc1_press_judge(all, 2, 0, 1, at + G + S) == JC1_PRESS_NONE);   // judged once
        jc1_press_update(&all[0], 0, at + G + S); jc1_press_update(&all[0], 1, at + G + 2 * S);   // a new press
        assert(!all[0].judged && jc1_press_judge(all, 2, 0, 1, at + G + 3 * S) == JC1_PRESS_BIND);
    }

    puts("t_jc1: passed");
}
