// Switch 2 controllers over Bluetooth LE (switch2_proto.h): reports and replies recorded from a left and a right Joy-Con 2 (macOS 27), command framing, the advertisement,
// stick calibration, and how one or two controllers make a pad
#include <stdio.h>
#include <stdlib.h>
#include "../switch2_proto.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t unhex(const char *h, uint8_t *out) { size_t n = 0; for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; } return n; }

int main(void) {
    uint8_t b[128]; size_t n; s2_report r = {0};
    // The first report of the left Joy-Con (stick at rest; nothing pressed)
    n = unhex("b3240000000000e0ff0f14d882fff77f000000000000000000edff12ffd6fda70d0015080000000000012ee30200fdff000000000000000000000000000000", b);
    CHECK(n == 63 && s2_report_parse(b, n, &r));
    CHECK(r.buttons == 0xE0000000u && r.lx == 2068 && r.ly == 2093 && r.rx == 2047 && r.ry == 2047 && r.battery_mv == s2_u16(b + 0x1F) && r.battery_mv > 3000 && r.battery_mv < 4400);
    CHECK(!s2_report_parse(b, 0x3B, &r) && !s2_report_parse(b, 0x60, &r) && !s2_report_parse(NULL, 63, &r));
    // The left Joy-Con with the d-pad's Down pressed, the right Joy-Con with A pressed
    n = unhex("48520000000001e0ff0f13e882fff77f000000000000000000b1ff5bfef1fda70d00110800000000000116a9b300fdff8d005d0aef0bf0ffefffe7ff000000", b);
    CHECK(s2_report_parse(b, n, &r) && (r.buttons & S2_DOWN) && !(r.buttons & S2_UP));
    CHECK(r.raw[0] == s2_s16(b + 0x30) && r.raw[5] == s2_s16(b + 0x3A));
    n = unhex("722e0000080000e0ff0ffff77ff2a77d000000000000000000a000160002ff880d002608000000000001af628a00fdff000000000000000000000000000000", b);
    CHECK(s2_report_parse(b, n, &r) && r.buttons == (0xE0000000u | S2_A) && r.rx == 2034 && r.ry == 2010 && r.lx == 2047 && r.ly == 2047);

    // Command frames
    uint8_t f[64], p[8];
    s2_memory_payload(p, 0x40, S2_ADDR_INFO);
    CHECK(p[0] == 0x40 && p[1] == 0x7E && p[4] == 0x00 && p[5] == 0x30 && p[6] == 0x01 && p[7] == 0);
    CHECK(s2_command(f, sizeof f, S2_CMD_MEMORY, S2_SUB_MEMORY_READ, p, 8) == 16 && f[0] == 0x02 && f[1] == 0x91 && f[2] == 0x01 && f[3] == 0x04 && f[5] == 8 && f[8] == 0x40);
    CHECK(s2_command(f, 7, 1, 1, NULL, 0) == 0 && s2_command(f, 8, 1, 1, NULL, 0) == 8);
    uint8_t rep[] = {0x0c, 0x01, 0x01, 0x04, 0x10, 0x78, 0, 0, 0, 0, 0, 0};
    CHECK(s2_reply_is(rep, sizeof rep, 0x0c) && !s2_reply_is(rep, sizeof rep, 0x09) && !s2_reply_is(rep, 7, 0x0c));
    rep[1] = 3; CHECK(!s2_reply_is(rep, sizeof rep, 0x0c));

    // Advertisements as captured: payload after the company id 0x0553; pairing mode (no host)
    s2_advert a = {0};
    n = unhex("0100037e0566200001000000000000000f00000000000000", b);
    CHECK(s2_advert_parse(b, n, &a) && a.vid == 0x057E && a.pid == S2_PID_JOYCON_R && a.pairing);
    n = unhex("0100037e0567200001000000000000000f00000000000000", b);
    CHECK(s2_advert_parse(b, n, &a) && a.pid == S2_PID_JOYCON_L);
    b[12] = 0x42; CHECK(s2_advert_parse(b, n, &a) && !a.pairing);   // a reconnect host
    b[3] = 0x7f; CHECK(!s2_advert_parse(b, n, &a));
    CHECK(!s2_advert_parse(b, 15, &a));

    // What the host does with them. This Mac's address c8:89:f3:b1:20:2f, little endian, as the right Joy-Con's wake advertisement carried it after bonding (2026-11-05)
    const uint8_t mac[6] = {0x2f, 0x20, 0xb1, 0xf3, 0x89, 0xc8}, other[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    n = unhex("0100037e0566200001002f20b1f389c80f00000000000000", b);
    CHECK(s2_advert_parse(b, n, &a) && !a.pairing && !memcmp(a.host, mac, 6));
    CHECK(s2_advert_use(&a, mac) == S2_ADV_WAKE);
    CHECK(s2_advert_use(&a, other) == S2_ADV_IGNORE);   // woken for another host (a Switch 2 nearby): left alone
    CHECK(s2_advert_use(&a, NULL) == S2_ADV_IGNORE);    // this host's address unknown: a wake can be nobody's but its own host's
    n = unhex("0100037e0566200001000000000000000f00000000000000", b);
    CHECK(s2_advert_parse(b, n, &a) && a.pairing && s2_advert_use(&a, mac) == S2_ADV_PAIR && s2_advert_use(&a, NULL) == S2_ADV_TAKE);   // Sync held

    // Bonding: the host address command (0x00, 0x02, the address twice) and a key (0x00, then 16 bytes), framed as sent
    uint8_t hp[S2_PAIR_HOST_LEN], kp[S2_PAIR_KEY_LEN], key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0xA0 + i);
    s2_pair_host_payload(hp, mac); s2_pair_key_payload(kp, key);
    n = unhex("1591010100" "0e0000" "00022f20b1f389c82f20b1f389c8", b);
    CHECK(s2_command(f, sizeof f, S2_CMD_PAIR, S2_SUB_PAIR_HOST, hp, sizeof hp) == n && !memcmp(f, b, n));
    CHECK(kp[0] == 0 && kp[1] == 0xA0 && kp[16] == 0xAF);
    CHECK(s2_command(f, sizeof f, S2_CMD_PAIR, S2_SUB_PAIR_KEY1, kp, sizeof kp) == 8 + 17 && f[3] == 0x04 && f[5] == 17);
    uint8_t done_rep[] = {0x15, 0x01, 0x01, 0x03, 0x10, 0x78, 0x00, 0x00, 0x01};   // the controller's answer to the last step, as recorded
    CHECK(s2_reply_is(done_rep, sizeof done_rep, S2_CMD_PAIR));

    // The info block of the right Joy-Con (after the reply's 16 header bytes): serial at 2, ids at 18 and 20
    uint16_t vid, pid; char serial[15];
    n = unhex("0100484357353130303432333437323400007e0566200108023232", b);
    CHECK(s2_info_parse(b, n, &vid, &pid, serial) && vid == 0x057E && pid == S2_PID_JOYCON_R && !strncmp(serial, "HCW", 3) && strlen(serial) == 14);
    CHECK(!s2_info_parse(b, 10, &vid, &pid, serial));

    // Calibration of the right Joy-Con (factory, first slot): centre 2095, 1999; travel 1175, 1225 up and 1181, 1129 down
    s2_cal c; n = unhex("2ff87c97944c9d9446ffff", b);
    CHECK(!s2_cal_blank(b, n) && s2_cal_parse(b, n, &c) && c.cx == 2095 && c.cy == 1999 && c.maxx == 1175 && c.maxy == 1225 && c.minx == 1181 && c.miny == 1129);
    float x, y;
    s2_cal_apply(&c, 3270, 1999, 40, &x, &y); CHECK(x == 1.f && y == 0.f);
    s2_cal_apply(&c, 2095 + 20, 1999 - 20, 40, &x, &y); CHECK(x == 0.f && y == 0.f);       // inside the dead zone
    s2_cal_apply(&c, 2095 - 1181, 1999 + 2000, 40, &x, &y); CHECK(x == -1.f && y == 1.f);   // past the travel clamps
    s2_cal_apply(NULL, 2048, 2048, 40, &x, &y); CHECK(x == 0.f && y == 0.f);
    s2_cal blank; n = unhex("ffffffffffffffffffffff", b); s2_cal_default(&blank);
    CHECK(s2_cal_blank(b, n) && !s2_cal_parse(b, n, &blank) && !blank.valid);
    s2_cal_apply(&blank, 2048 + 1200, 2048, 40, &x, &y); CHECK(x == 1.f);   // an uncalibrated stick uses the typical travel

    // Pads
    s2_pad_state pad; uint64_t m; int32_t st[4];
    memset(&pad, 0, sizeof pad); CHECK(!s2_pad_input(&pad, &m, st) && !s2_pad_motion_dev(&pad, 1));
    // A Pro Controller 2: its own buttons, both sticks
    pad.pro.have = 1; pad.pro.pid = S2_PID_PRO; s2_cal_default(&pad.pro.cal[0]); s2_cal_default(&pad.pro.cal[1]);
    pad.pro.r.buttons = S2_A | S2_ZL | S2_SL_L; pad.pro.r.lx = 2048 + 1200; pad.pro.r.ly = 2048; pad.pro.r.rx = 2048; pad.pro.r.ry = 2048 - 600;
    CHECK(s2_pad_input(&pad, &m, st) && m == (DM_BTN_A | DM_BTN_ZL) && st[0] == DM_STICK_MAX && st[1] == 0 && st[2] == 0 && st[3] == -(int32_t)lrintf(600.f / 1200.f * DM_STICK_MAX));
    CHECK(s2_pad_motion_dev(&pad, 1) == &pad.pro && s2_pad_motion_dev(&pad, 0) == &pad.pro);
    // Two Joy-Cons: the left's d-pad and stick, the right's face buttons and stick
    memset(&pad, 0, sizeof pad);
    pad.left.have = 1; pad.left.pid = S2_PID_JOYCON_L; s2_cal_default(&pad.left.cal[0]); pad.left.r.buttons = S2_UP | S2_L | S2_SR_L; pad.left.r.lx = 2048 - 1200; pad.left.r.ly = 2048;
    pad.right.have = 1; pad.right.pid = S2_PID_JOYCON_R; s2_cal_default(&pad.right.cal[0]); pad.right.r.buttons = S2_B | S2_ZR | S2_PLUS | S2_SL_R; pad.right.r.rx = 2048; pad.right.r.ry = 2048 + 1200;
    pad.left.r.rx = pad.left.r.ry = 4095; pad.right.r.lx = pad.right.r.ly = 4095;   // the other fields of a Joy-Con's report are not its stick
    CHECK(s2_pad_input(&pad, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS));   // rail buttons are not shoulders in a pair
    CHECK(st[0] == -DM_STICK_MAX && st[1] == 0 && st[2] == 0 && st[3] == DM_STICK_MAX);
    CHECK(s2_pad_motion_dev(&pad, 1) == &pad.right && s2_pad_motion_dev(&pad, 0) == &pad.left);
    // One Joy-Con alone is held sideways: rail buttons are L / R, its stick the left stick
    pad.left.have = 0; CHECK(s2_pad_input(&pad, &m, st) && m == (DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_L) && st[0] == 0 && st[1] == DM_STICK_MAX && st[2] == 0 && st[3] == 0);
    CHECK(s2_pad_motion_dev(&pad, 0) == &pad.right);
    pad.right.have = 0; pad.left.have = 1; CHECK(s2_pad_input(&pad, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_R) && st[0] == -DM_STICK_MAX);

    // Motion. A right Joy-Con 2 held upright, face up and still: gravity reads +1 G on raw z, which is the Switch frame's (0, 0, -1)
    int16_t raw[6] = {100, 200, 4096, 10, 20, 30}; float acc[3], rot[3];
    s2_motion(raw, s2_map_pair, NULL, S2_GYRO_RAD, acc, rot);
    CHECK(fabsf(acc[0] + 100.f / 4096.f) < 1e-6f && fabsf(acc[1] + 200.f / 4096.f) < 1e-6f && fabsf(acc[2] + 1.f) < 1e-6f);
    CHECK(fabsf(rot[0] - 10 * S2_GYRO_RAD) < 1e-6f && fabsf(rot[1] - 20 * S2_GYRO_RAD) < 1e-6f && fabsf(rot[2] - 30 * S2_GYRO_RAD) < 1e-6f);   // identity for rotation
    float bias[3] = {10, -20, 5}; s2_motion(raw, s2_map_pair, bias, S2_GYRO_RAD, acc, rot);
    CHECK(fabsf(rot[0]) < 1e-6f && fabsf(rot[1] - 40 * S2_GYRO_RAD) < 1e-6f && fabsf(rot[2] - 25 * S2_GYRO_RAD) < 1e-6f && fabsf(acc[2] + 1.f) < 1e-6f);   // the offset leaves acceleration alone
    // The recorded movements: tilting the top away from you is gyro x -83 degrees (rot x < 0 = nose down), turning left gyro z +115 (rot z > 0), right side down gyro y +104 (rot y > 0)
    // Right alone is the Pro Controller 1's map: raw x toward the shoulders, y left, z out of the face
    int16_t sw[6] = {0, 0, 4096, 0, 0, 0}; s2_motion(sw, s2_map_right_alone, NULL, S2_GYRO_RAD, acc, rot); CHECK(fabsf(acc[2] + 1.f) < 1e-6f);
    sw[2] = 0; sw[0] = 4096; s2_motion(sw, s2_map_right_alone, NULL, S2_GYRO_RAD, acc, rot); CHECK(fabsf(acc[1] + 1.f) < 1e-6f && fabsf(acc[0]) < 1e-6f);   // its raw x (the top) pointing up: gravity along -y?
    memset(&pad, 0, sizeof pad); pad.right.have = 1;
    CHECK(s2_pad_map(&pad, &pad.right) == s2_map_right_alone);
    pad.left.have = 1; CHECK(s2_pad_map(&pad, &pad.right) == s2_map_pair && s2_pad_map(&pad, &pad.left) == s2_map_pair);
    pad.right.have = 0; CHECK(s2_pad_map(&pad, &pad.left) == s2_map_left_alone);
    pad.pro.have = 1; CHECK(s2_pad_map(&pad, &pad.pro) == s2_map_pro);
    // Rest offset: a second of nearly constant samples is learned, motion resets, a large offset is not rest
    s2_rest rs; memset(&rs, 0, sizeof rs); int16_t q[6] = {0, 0, 4096, 18, -20, -4};
    for (int i = 0; i < S2_REST_N - 1; i++) { q[3] = 18 + (i & 1); s2_rest_update(&rs, q); }
    CHECK(!rs.have); q[3] = 18; s2_rest_update(&rs, q);
    CHECK(rs.have && fabsf(rs.bias[0] - 18.5f) < 1.f && fabsf(rs.bias[1] + 20.f) < 0.01f && fabsf(rs.bias[2] + 4.f) < 0.01f);
    for (int i = 0; i < S2_REST_N * 2; i++) { q[3] = i % 2 ? 500 : -500; s2_rest_update(&rs, q); }   // shaking: never learned
    CHECK(fabsf(rs.bias[0] - 18.5f) < 1.f);
    s2_rest big; memset(&big, 0, sizeof big); int16_t z[6] = {0, 0, 4096, 200, 0, 0};
    for (int i = 0; i < S2_REST_N; i++) s2_rest_update(&big, z);
    CHECK(!big.have);   // 200 counts (14 degrees per second) is a turn, not rest
    int8_t mp[6] = {1, 2, 3, 4, 5, 6};
    CHECK(s2_map_parse("3,-2,1,6,-5,4", mp) && mp[0] == 3 && mp[1] == -2 && mp[2] == 1 && mp[3] == 6 && mp[4] == -5 && mp[5] == 4);
    CHECK(!s2_map_parse("1,2,3,4,5", mp) && !s2_map_parse("1,2,3,4,5,7", mp) && !s2_map_parse("1,2,3,4,5,0", mp) && !s2_map_parse("1,2,3,4,5,6,7", mp) && !s2_map_parse("x", mp) && !s2_map_parse(NULL, mp) && mp[0] == 3);

    // Rumble: the frequency code is logarithmic (code 333 at 110 Hz, 96 an octave); 320 Hz is 0x1E1, the high-band default other drivers send
    CHECK(s2_freq_code(110) == 333 && s2_freq_code(220) == 429 && s2_freq_code(160) == 385 && s2_freq_code(320) == 0x1E1);
    CHECK(s2_freq_code(1) == 0 && s2_freq_code(1e6f) == 1023 && s2_freq_code(NAN) == 0 && fabsf(s2_freq_hz(429) - 220.f) < 0.01f);
    // A sample: low frequency, low amplitude, high frequency, high amplitude, 10 bits each, little endian
    s2_haptic hz = {385, 1023, 481, 1023}; uint8_t s5[5]; s2_haptic_pack(&hz, s5);
    uint64_t packed = (uint64_t)s5[0] | (uint64_t)s5[1] << 8 | (uint64_t)s5[2] << 16 | (uint64_t)s5[3] << 24 | (uint64_t)s5[4] << 32;
    CHECK((packed & 0x3FF) == 385 && (packed >> 10 & 0x3FF) == 1023 && (packed >> 20 & 0x3FF) == 481 && (packed >> 30 & 0x3FF) == 1023);
    uint8_t pk[33]; CHECK(s2_rumble_packet(pk, &hz, &hz, 0xd) == 33 && pk[0] == 0 && pk[1] == 0x5d && pk[2] == s5[0] && pk[17] == 0x5d && pk[32] == s5[4]);
    s2_haptic quiet = s2_haptic_from(0, 160, 0, 320); CHECK(!s2_haptic_on(&quiet) && quiet.lf_hz == 385 && quiet.hf_hz == 481);
    s2_haptic full = s2_haptic_from(1, 160, 1, 320); CHECK(full.lf_amp == 1023 && full.hf_amp == 1023 && s2_haptic_on(&full));
    s2_haptic wild = s2_haptic_from(5, 9000, -1, 0); CHECK(wild.lf_amp == 1023 && wild.lf_hz == s2_freq_code(9000) && wild.hf_amp == 0 && wild.hf_hz == 0);   // as the console codes them
    s2_haptic nan = s2_haptic_from(NAN, NAN, 0.25f, INFINITY); CHECK(nan.lf_amp == 0 && nan.lf_hz == 0 && nan.hf_amp == 256 && nan.hf_hz == 1023);
    // The console's rounding: truncate, then the nearer of that code and the next, in hertz (amplitude: in 0..1)
    CHECK(s2_amp_code(0.25f) == 256 && s2_amp_code(0.5f) == 511 /* 511.5: a tie keeps the lower */ && s2_amp_code(0.0004f) == 0 && s2_amp_code(0.0006f) == 1 && s2_amp_code(2.f) == 1023 && s2_amp_code(-1.f) == 0);
    CHECK(fabsf(s2_freq_hz(s2_freq_code(482.6f)) - 482.6f) <= fabsf(s2_freq_hz((uint16_t)(s2_freq_code(482.6f) + 1)) - 482.6f));
    { s2_haptic three[3] = {hz, quiet, full}; uint8_t q5[5]; s2_haptic_pack(&quiet, q5);   // three samples, played in turn
      CHECK(s2_rumble_packet3(pk, three, three, 2) == 33 && pk[1] == 0x72 && !memcmp(pk + 2, s5, 5) && !memcmp(pk + 7, q5, 5) && pk[17] == 0x72); }   // 0x40 | 3 samples << 4 | sequence
    { float lo = 0.8f, hi = 0.7f; s2_rumble_cap(&lo, &hi); CHECK(fabsf(lo + hi - 0.98f) < 1e-5f && fabsf(lo / hi - 0.8f / 0.7f) < 1e-4f);   // over 1: scaled to 0.98 together
      lo = 0.5f; hi = 0.1f; s2_rumble_cap(&lo, &hi); CHECK(fabsf(lo - 0.49f) < 1e-6f && fabsf(hi - 0.098f) < 1e-6f); }
    // Which motor each controller plays: a pair's Joy-Cons their own side, a Joy-Con alone the stronger of both in each band, a Pro Controller 2 one per block
    s2_haptic mot[2] = {{100, 500, 200, 10}, {150, 20, 250, 700}}, ra, rb;
    memset(&pad, 0, sizeof pad); pad.left.have = pad.right.have = 1;
    s2_rumble_pick(&pad, &pad.left, mot, &ra, &rb); CHECK(ra.lf_amp == 500 && rb.hf_amp == 10);
    s2_rumble_pick(&pad, &pad.right, mot, &ra, &rb); CHECK(ra.lf_amp == 20 && ra.hf_amp == 700 && rb.hf_hz == 250);
    pad.left.have = 0;
    s2_rumble_pick(&pad, &pad.right, mot, &ra, &rb); CHECK(ra.lf_amp == 500 && ra.lf_hz == 100 && ra.hf_amp == 700 && ra.hf_hz == 250 && rb.lf_amp == 500);
    memset(&pad, 0, sizeof pad); pad.pro.have = 1;
    s2_rumble_pick(&pad, &pad.pro, mot, &ra, &rb); CHECK(ra.lf_amp == 500 && rb.lf_amp == 20);
    CHECK(s2_rumble_motors(&pad, &pad.pro) == 3);
    memset(&pad, 0, sizeof pad); pad.left.have = pad.right.have = 1;
    CHECK(s2_rumble_motors(&pad, &pad.left) == 1 && s2_rumble_motors(&pad, &pad.right) == 2);
    pad.left.have = 0; CHECK(s2_rumble_motors(&pad, &pad.right) == 3);
    // Rumble timing: each value plays once, in the first 5 ms slot ending after it was set
    {   const uint64_t ms = 1000000; s2_rumble_log lg; memset(&lg, 0, sizeof lg); s2_rumble_cursor c; memset(&c, 0, sizeof c);
        s2_haptic on2[2] = {{385, 600, 481, 0}, {385, 0, 481, 0}}, off2[2] = {{385, 0, 481, 0}, {385, 0, 481, 0}}, r2[2] = {{385, 0, 481, 0}, {385, 900, 481, 0}};
        s2_rumble_log_add(&lg, 101 * ms, on2); s2_rumble_log_add(&lg, 106 * ms, off2);   // a one-step pulse: on at 101 ms, off at 106 ms
        s2_rumble_resync(&c, 120 * ms, 5 * ms); CHECK(c.end == 100 * ms);   // three slots up to 115 ms
        unsigned got = 0; for (int k = 0; k < 3; k++) { s2_rumble_slot(&lg, &c, 1); got |= (unsigned)(c.now[0].lf_amp == 600) << k; }
        CHECK(got == 1 && c.end == 115 * ms && c.next == 2);   // played in the first slot only: one sample, not until the packet's end
        s2_rumble_log_add(&lg, 116 * ms, r2);   // only the right motor changes: no slot for a left Joy-Con of a pair
        s2_rumble_slot(&lg, &c, 1); CHECK(c.next == 3 && c.now[1].lf_amp == 0 && c.end == 120 * ms);
        s2_rumble_log_add(&lg, 121 * ms, on2); s2_rumble_log_add(&lg, 122 * ms, off2);   // two in one slot: the second waits for the next
        s2_rumble_slot(&lg, &c, 3); CHECK(c.now[0].lf_amp == 600); s2_rumble_slot(&lg, &c, 3); CHECK(c.now[0].lf_amp == 0 && c.end == 130 * ms);
        s2_rumble_slot(&lg, &c, 3); CHECK(c.now[0].lf_amp == 0 && c.end == 135 * ms);   // nothing new: the value before repeats
        for (int i = 0; i < 10; i++) s2_rumble_log_add(&lg, (136 + i) * ms, i & 1 ? off2 : on2);   // a pile-up: no value waits more than five slots
        for (int k = 0; k < 12; k++) s2_rumble_slot(&lg, &c, 3);
        CHECK(c.next == lg.n);
        s2_rumble_resync(&c, 1000 * ms, 5 * ms); CHECK(c.end == 980 * ms);   // a tick long after: starts again
        s2_rumble_resync(&c, 1010 * ms, 5 * ms); CHECK(c.end == 980 * ms); }   // within a packet: the slots carry on

    // Home, A and B for the Home hold-to-quit prompt: Home from any of the pad's controllers, A and B from the right side
    memset(&pad, 0, sizeof pad); int ha = 0, hb = 0;
    pad.left.have = pad.right.have = 1; pad.left.r.buttons = 0xE0000000u; pad.right.r.buttons = 0xE0000000u | S2_HOME | S2_A;
    s2_pad_ab(&pad, &ha, &hb); CHECK(s2_pad_home(&pad) && ha && !hb);
    pad.right.r.buttons = 0xE0000000u; pad.left.r.buttons = 0xE0000000u | S2_HOME; ha = hb = 0;
    s2_pad_ab(&pad, &ha, &hb); CHECK(s2_pad_home(&pad) && !ha && !hb);
    memset(&pad, 0, sizeof pad); CHECK(!s2_pad_home(&pad));
    printf("t_switch2: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
