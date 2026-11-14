// Switch 2 controllers over Bluetooth LE (switch2_proto.h): reports and replies recorded from a left and a right Joy-Con 2 (macOS 27), command framing, the advertisement,
// stick calibration, and how one or two controllers make a pad
#include <stdio.h>
#include <stdlib.h>
#include "../switch2_proto.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int same_cal(const s2_cal *a, const s2_cal *b) {   // member by member: floats have no unique object representation for memcmp
    return a->cx == b->cx && a->cy == b->cy && a->maxx == b->maxx && a->maxy == b->maxy && a->minx == b->minx && a->miny == b->miny && a->valid == b->valid;
}
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
    CHECK(r.clock_ms == 0x5248u);   // the first four bytes: the controller's clock in milliseconds

    // The factory gyro offset: a right Joy-Con 2's block (temperature 26.3, offset in rad/s), in counts at the block's own 0.070 degrees per second per count
    {
        uint8_t cal[16]; unhex("8882d24154afec3a4d5abbbce3ccfcbb", cal);
        float off[3] = {0};
        CHECK(s2_gyro_cal_parse(cal, 16, off) && fabsf(off[0] - 1.48f) < 0.05f && fabsf(off[1] + 18.72f) < 0.05f && fabsf(off[2] + 6.31f) < 0.05f);
        CHECK(!s2_gyro_cal_parse(cal, 15, off) && !s2_gyro_cal_parse(NULL, 16, off));
        uint8_t bad[16]; memcpy(bad, cal, 16); memset(bad + 4, 0xff, 4); CHECK(!s2_gyro_cal_parse(bad, 16, off));   // a NaN
        float big[4] = {25.f, 0.1f, 0.f, 0.f}; CHECK(!s2_gyro_cal_parse((const uint8_t *)big, 16, off));       // 5.7 degrees per second: not an offset
        CHECK(s2_gyro_cal_usable(0x2066) && !s2_gyro_cal_usable(0x2067) && !s2_gyro_cal_usable(0x2069));   // the right Joy-Con 2's only, for now
    }
    // The measured gyro scale: a 90 degree tilt at the measured 0.0607 degrees per second per count is 90 degrees, not the 104 the nominal 0.070 gave
    CHECK(fabsf(S2_GYRO_DPS - 0.0607f) < 1e-6f && fabsf(90.f / S2_GYRO_DPS * 0.070f - 103.8f) < 0.2f);

    // Sample times from the controller's clock
    {
        s2_clock c = {0}; unsigned lost = 9; const uint64_t ms = 1000000ull, base = 5000000000ull;
        CHECK(s2_clock_sample(&c, 1000, base, &lost) == base && lost == 0);   // the first report: its arrival
        // a burst: three reports arrive within 1 ms but were sampled 30 ms apart
        uint64_t t1 = s2_clock_sample(&c, 1030, base + 61 * ms, &lost), t2 = s2_clock_sample(&c, 1060, base + 61 * ms + 300000, &lost), t3 = s2_clock_sample(&c, 1090, base + 62 * ms, &lost);
#define NEAR_MS(a, b) ((a) + 200000 > (b) && (b) + 200000 > (a))   // within 0.2 ms: the anchor rises 1 ms a second
        CHECK(NEAR_MS(t1, base + 30 * ms) && NEAR_MS(t2, base + 60 * ms) && t3 == base + 62 * ms);   // 30 ms apart until the arrival caps it (never in the future)
        // a 90 ms step: two reports lost, the gap kept; the second report in a row earlier than the anchor (-28 ms, then -20) moves it, to the later of the two
        uint64_t t4 = s2_clock_sample(&c, 1180, base + 160 * ms, &lost); CHECK(t4 == base + 160 * ms && lost == 2);
        uint64_t same = s2_clock_sample(&c, 1180, base + 161 * ms, &lost); CHECK(same > t4 && same < t4 + ms);   // the same millisecond: still after it
        CHECK(s2_clock_sample(&c, 1210, base + 400 * ms, &lost) == base + 340 * ms);   // arriving late: held to 60 ms behind the arrival
        CHECK(s2_clock_sample(&c, 9000, base + 9000 * ms, &lost) == base + 9000 * ms && lost == 0);   // seconds of silence: starts over at the arrival
        CHECK(s2_clock_sample(&c, 100, base + 9030 * ms, &lost) == base + 9030 * ms);   // the clock went back: starts over too
        // steady reports with jittered arrivals integrate a constant rotation exactly: 34 reports of 30 ms at 100 counts
        s2_clock d = {0}; uint64_t prev = 0; double deg = 0;
        for (int i = 0; i < 34; i++) {
            uint64_t arr = base + (uint64_t)i * 30 * ms + (uint64_t)((i * 7919) % 25) * ms;   // 0..24 ms late
            uint64_t t = s2_clock_sample(&d, 2000 + 30 * (unsigned)i, arr, NULL);
            if (prev) deg += 100 * S2_GYRO_DPS * (double)(t - prev) / 1e9;
            prev = t;
        }
        CHECK(fabs(deg - 100 * S2_GYRO_DPS * 0.99) < 0.06 * 100 * S2_GYRO_DPS);   // 33 intervals of 30 ms: 0.99 s of rotation, the arrival jitter mostly gone
        // the anchor is a report with about the least latency, not the first: a first report 20 ms late, then reports 2..12 ms late, sample at the low end of their latency
        s2_clock e = {0}; uint64_t worst = 0;
        for (int i = 0; i < 100; i++) {
            uint64_t sent = base + (uint64_t)i * 30 * ms, arr = sent + (i ? 2 * ms + (uint64_t)((i * 7919) % 11) * ms : 20 * ms);
            uint64_t t = s2_clock_sample(&e, 3000 + 30 * (unsigned)i, arr, NULL);
            if (i > 10 && t - sent > worst) worst = t - sent;
        }
        CHECK(worst >= 2 * ms && worst < 8 * ms);   // two quick reports in a row set it: under 8 ms of the 2..12, far from the first report's 20
        // a controller clock 500 ppm slow (its 30 ms are 30.015 of the host's) for a minute: the anchor follows, the samples stay near their arrival instead of drifting to 60 ms
        s2_clock g = {0}; uint64_t lag_max = 0;
        for (int i = 0; i < 2000; i++) {
            uint64_t sent = base + (uint64_t)i * 30015000ull, arr = sent + 2 * ms + (uint64_t)((i * 7919) % 6) * ms;
            uint64_t t = s2_clock_sample(&g, 3000 + 30 * (unsigned)i, arr, NULL);
            if (i > 100 && arr - t > lag_max) lag_max = arr - t;
        }
        CHECK(lag_max < 8 * ms);   // the arrival jitter (up to 5 ms) and the anchor's rise, not the drift (30 ms over the minute)
        // one report with an unusually short latency (1 ms, the others 5 ms) does not move the anchor: the samples after it stay 5 ms before their arrival
        s2_clock o = {0}; int moved = 0; uint64_t sp_min = ~0ull, prev_o = 0;
        for (int i = 0; i < 200; i++) {
            uint64_t sent = base + (uint64_t)i * 30 * ms, arr = sent + (i == 100 ? 1 : 5) * ms;
            uint64_t t = s2_clock_sample(&o, 3000 + 30 * (unsigned)i, arr, NULL);
            if (i > 100 && (t > arr || arr - t > 100000)) moved++;   // within 0.1 ms of the arrival (the 1 ms a second rise, re-pinned by each report)
            if (prev_o && t - prev_o < sp_min) sp_min = t - prev_o;
            prev_o = t;
        }
        CHECK(!moved && sp_min >= 26 * ms);   // and the early one itself only takes its arrival (26 ms after the last), no sample microseconds apart
        // a controller clock 1000 ppm fast (its 30 ms are 29.97 of the host's), arrivals 2..7 ms late: the times step evenly, not each one its own arrival
        s2_clock fc = {0}; unsigned uneven = 0; uint64_t prev_f = 0;
        for (int i = 0; i < 2000; i++) {
            uint64_t sent = base + (uint64_t)i * 29970000ull, arr = sent + 2 * ms + (uint64_t)((i * 7919) % 6) * ms;
            uint64_t t = s2_clock_sample(&fc, 3000 + 30 * (unsigned)i, arr, NULL);
            if (i > 100 && (t - prev_f < 27 * ms || t - prev_f > 33 * ms)) uneven++;
            prev_f = t;
        }
        CHECK(uneven < 100);   // under 5% of the steps more than 3 ms off 30
        // connecting: a first burst read with the first report's anchor (three reports at once, 60 ms late), then 2 ms reports: only that burst's samples crowd
        s2_clock cb = {0}; unsigned crowded = 0; uint64_t prev_b = 0;
        for (int i = 0; i < 50; i++) {
            uint64_t sent = base + (uint64_t)i * 30 * ms, arr = i < 3 ? base + 62 * ms : sent + 2 * ms;
            uint64_t t = s2_clock_sample(&cb, 3000 + 30 * (unsigned)i, arr, NULL);
            if (prev_b && t - prev_b < 15 * ms) crowded++;
            prev_b = t;
        }
        CHECK(crowded <= 3);
    }
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

    // Writing a user calibration (DM_SW2_CALIBRATE): the recorded block packs back to its own bytes; the user block is the marker B2 A1 and those 9 bytes
    {
        n = unhex("2ff87c97944c9d9446ffff", b); s2_cal_parse(b, n, &c);
        uint8_t p9[9]; s2_cal_pack(&c, p9); CHECK(!memcmp(p9, b, 9));
        uint8_t blk[S2_USER_CAL_LEN]; s2_user_cal_block(&c, blk);
        CHECK(s2_user_cal_marked(blk) && !memcmp(blk + 2, b, 9));
        s2_cal back; CHECK(s2_cal_parse(blk + 2, 9, &back) && back.cx == c.cx && back.miny == c.miny);   // what readCal reads at 0x1FC042
        CHECK(S2_ADDR_USER_CAL_SECTOR + S2_USER_STICK1_OFF + 2 == S2_ADDR_USER_STICK1);
        // The second stick slot's user calibration is at 0x1FC080 (SDL and Linux read it there), its 9 bytes after the marker at 0x1FC082
        CHECK(S2_ADDR_USER_STICK2 == 0x001FC082u && S2_ADDR_USER_CAL_SECTOR + s2_user_slot_off(1) + 2 == S2_ADDR_USER_STICK2 && s2_user_slot_off(0) == S2_USER_STICK1_OFF);
        // A new centre keeps the end stops: 2095 + 1175 = 3270 and 2095 - 1181 = 914 still read full tilt
        s2_cal rc; CHECK(s2_cal_recentre(&c, 2140.4f, 1980.f, &rc) && rc.cx == 2140 && rc.cy == 1980 && rc.maxx == 1130 && rc.minx == 1226 && rc.maxy == 1244 && rc.miny == 1110);
        s2_cal_apply(&rc, 3270, 1980, 40, &x, &y); CHECK(x == 1.f && y == 0.f);
        s2_cal_apply(&rc, 914, 1980, 40, &x, &y); CHECK(x == -1.f);
        s2_cal_apply(&rc, 2140 + 30, 1980 - 30, 40, &x, &y); CHECK(x == 0.f && y == 0.f);   // the new rest is inside the dead zone
        s2_cal_apply(&c, 2140, 1980, 40, &x, &y); CHECK(x > 0.f);                            // and was past it before: the drift
        CHECK(!s2_cal_recentre(&blank, 2100, 2100, &rc));                                     // no travel of its own: nothing to keep
        CHECK(!s2_cal_recentre(&c, 2095 + 1100, 1999, &rc));                                  // travel above would be under 100
        CHECK(s2_stick_rest_near(&c, 2095 + 300, 1999 - 300) && !s2_stick_rest_near(&c, 2095 + 301, 1999));
        // Rest: two seconds of the controller's clock without a move past the spread, averaged; a touch starts it over
        s2_stick_rest sr; memset(&sr, 0, sizeof sr); float rest[2]; int got = 0; uint32_t t = 1000;
        for (int i = 0; i < 40 && !got; i++, t += 30) got = s2_stick_rest_update(&sr, (uint16_t)(2140 + (i & 1) * 4), 1980, t, rest);
        CHECK(!got);                                                                         // 1.2 s: not yet
        CHECK(!s2_stick_rest_update(&sr, 2140 + 60, 1980, t, rest)); t += 30;                // touched: over again
        for (int i = 0; i < 67 && !got; i++, t += 30) got = s2_stick_rest_update(&sr, 2140, (uint16_t)(1980 - (i & 1) * 2), t, rest);
        CHECK(!got);                                                                         // the release starts it again too: 1.98 s since
        got = s2_stick_rest_update(&sr, 2140, 1980, t, rest);
        CHECK(got && sr.n == 68 && rest[0] == 2140.f && fabsf(rest[1] - (1980.f - 66.f / 68.f)) < 0.01f);   // the touch is not in it; y averages 33 dips of 2
        // Release: the stick goes more than S2_RELEASE_AWAY from the centre, then rests closer than that for 300 ms: one report, then it waits for the next push
        {
            s2_release rl; memset(&rl, 0, sizeof rl); float rr[2]; int reports = 0; uint32_t u = 5000;
            for (int i = 0; i < 34; i++, u += 30) reports += s2_release_update(&rl, &c, 2095, 1999, u, rr);
            CHECK(reports == 0);                                                              // 1 s at rest: nothing was released
            for (int i = 0; i < 7; i++, u += 30) reports += s2_release_update(&rl, &c, 2095 + 900, 1999, u, rr);
            CHECK(reports == 0);                                                              // 200 ms away
            for (int i = 0; i < 11; i++, u += 30) reports += s2_release_update(&rl, &c, 2140, 1980, u, rr);
            CHECK(reports == 1 && fabsf(rr[0] - 2140.f) < 0.01f && fabsf(rr[1] - 1980.f) < 0.01f);   // 330 ms back: reported once
            for (int i = 0; i < 34; i++, u += 30) reports += s2_release_update(&rl, &c, 2140, 1980, u, rr);
            CHECK(reports == 1);                                                              // holding there: no second report
        }
        // Reach: the furthest offset in each direction becomes the travel, trimmed, with the centre kept
        {
            s2_reach rc2; s2_cal nc; memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &c, 2095 + 1200, 1999); s2_reach_update(&rc2, &c, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &c, 2095, 1999 + 1230); s2_reach_update(&rc2, &c, 2095, 1999 - 1130);
            CHECK(s2_reach_travel(&rc2, &c, &c, &nc) == 0 && nc.cx == 2095 && nc.cy == 1999 && nc.valid);
            CHECK(fabsf(nc.maxx - 1164.f) < 0.01f && fabsf(nc.minx - 1144.6f) < 0.01f && fabsf(nc.maxy - 1193.1f) < 0.01f && fabsf(nc.miny - 1096.1f) < 0.01f);
            memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &c, 2095 + 700, 1999); s2_reach_update(&rc2, &c, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &c, 2095, 1999 + 1230); s2_reach_update(&rc2, &c, 2095, 1999 - 1130);
            int sh = s2_reach_travel(&rc2, &c, &c, &nc);
            CHECK((sh & S2_REACH_SHORT_RIGHT) && sh == S2_REACH_SHORT_RIGHT);                 // only the right is short (700 < 822.5)
            memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &c, 2095 + 1500, 1999); s2_reach_update(&rc2, &c, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &c, 2095, 1999 + 1230); s2_reach_update(&rc2, &c, 2095, 1999 - 1130);
            CHECK((s2_reach_travel(&rc2, &c, &c, &nc) & S2_REACH_TOO_FAR));                       // 1500 > 1468.75
            // A stick whose stored calibration has a short travel (900 each way, an old user calibration): the limits are the factory travel, so its real edge
            // (1200, 1180, 1230, 1130) is taken, not too far; the travel is still measured from the stored calibration's centre
            s2_cal shortc = c; shortc.maxx = shortc.minx = shortc.maxy = shortc.miny = 900;
            memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &shortc, 2095 + 1200, 1999); s2_reach_update(&rc2, &shortc, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &shortc, 2095, 1999 + 1230); s2_reach_update(&rc2, &shortc, 2095, 1999 - 1130);
            CHECK(s2_reach_travel(&rc2, &shortc, &c, &nc) == 0 && nc.cx == 2095 && nc.cy == 1999 && fabsf(nc.maxx - 1164.f) < 0.01f && fabsf(nc.miny - 1096.1f) < 0.01f);
            CHECK(s2_reach_short(&rc2, &c) == 0);
            // Short is measured against the factory travel too: 800 right is over 70 % of the stored 900 (630) but under 70 % of the factory 1175 (822.5)
            memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &shortc, 2095 + 800, 1999); s2_reach_update(&rc2, &shortc, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &shortc, 2095, 1999 + 1230); s2_reach_update(&rc2, &shortc, 2095, 1999 - 1130);
            CHECK(s2_reach_travel(&rc2, &shortc, &c, &nc) == S2_REACH_SHORT_RIGHT && s2_reach_short(&rc2, &c) == S2_REACH_SHORT_RIGHT);
            // and too far against the factory travel: 1500 > 1.25 * 1175
            memset(&rc2, 0, sizeof rc2);
            s2_reach_update(&rc2, &shortc, 2095 + 1500, 1999); s2_reach_update(&rc2, &shortc, 2095 - 1180, 1999);
            s2_reach_update(&rc2, &shortc, 2095, 1999 + 1230); s2_reach_update(&rc2, &shortc, 2095, 1999 - 1130);
            s2_cal keep; memset(&keep, 0x5A, sizeof keep); nc = keep;
            CHECK(s2_reach_travel(&rc2, &shortc, &c, &nc) == S2_REACH_TOO_FAR && same_cal(&nc, &keep));   // out untouched when it is refused
            memset(&rc2, 0, sizeof rc2); nc = keep;
            CHECK(s2_reach_travel(&rc2, &shortc, &c, &nc) == (S2_REACH_SHORT_RIGHT | S2_REACH_SHORT_LEFT | S2_REACH_SHORT_UP | S2_REACH_SHORT_DOWN) && same_cal(&nc, &keep));
        }
        // Payloads: the write (length, 0x7E, 0, 0, address, data) and the erase (four zeros, address)
        uint8_t pl[8 + 0x80], f[8 + 255];
        size_t pn = s2_memory_write_payload(pl, sizeof pl, 0x001FC040u, blk, S2_USER_CAL_LEN);
        CHECK(pn == 19 && pl[0] == 11 && pl[1] == 0x7E && pl[2] == 0 && pl[4] == 0x40 && pl[5] == 0xC0 && pl[6] == 0x1F && pl[7] == 0 && pl[8] == 0xB2 && pl[18] == blk[10]);
        CHECK(s2_command(f, sizeof f, S2_CMD_MEMORY, S2_SUB_MEMORY_WRITE, pl, pn) == 27 && f[0] == 0x02 && f[3] == 0x05 && f[5] == 19);
        CHECK(!s2_memory_write_payload(pl, sizeof pl, 0x001FC040u, blk, 0) && !s2_memory_write_payload(pl, 10, 0x001FC040u, blk, 11));
        CHECK(s2_memory_erase_payload(pl, 0x001FC040u) && !pl[0] && !pl[3] && pl[4] == 0x40 && pl[5] == 0xC0 && pl[6] == 0x1F && !pl[7]);
        // Nothing below S2_WRITE_FLOOR is written or erased: the controller refuses it, and the host never sends it
        CHECK(S2_WRITE_FLOOR == 0x001F5000u && s2_memory_write_payload(pl, sizeof pl, S2_WRITE_FLOOR, blk, S2_USER_CAL_LEN) == 19);
        CHECK(!s2_memory_write_payload(pl, sizeof pl, S2_WRITE_FLOOR - 1, blk, S2_USER_CAL_LEN) && !s2_memory_write_payload(pl, sizeof pl, S2_ADDR_INFO, blk, S2_USER_CAL_LEN));
        CHECK(s2_memory_erase_payload(pl, S2_WRITE_FLOOR) && !s2_memory_erase_payload(pl, S2_WRITE_FLOOR - 1) && !s2_memory_erase_payload(pl, S2_ADDR_FACTORY_STICK1));
        uint8_t ff[11]; memset(ff, 0xFF, sizeof ff); CHECK(s2_all_ff(ff, 11) && !s2_all_ff(blk, 11));
        // A read's reply is taken only when it echoes the request: a late reply to the read before is not this one's
        uint8_t rp[S2_REPLY_DATA + 0x40]; memset(rp, 0, sizeof rp); rp[0] = S2_CMD_MEMORY; rp[1] = 1;
        s2_memory_payload(rp + 8, 0x40, 0x001FC040u);
        CHECK(s2_memory_reply_for(rp, sizeof rp, 0x40, 0x001FC040u) && !s2_memory_reply_for(rp, sizeof rp, 0x40, 0x001FC080u) && !s2_memory_reply_for(rp, sizeof rp, 0x20, 0x001FC040u));
        CHECK(!s2_memory_reply_for(rp, sizeof rp - 1, 0x40, 0x001FC040u) && !s2_memory_reply_for(NULL, 0, 0x40, 0x001FC040u));
        // A Pro Controller 2 does not echo the request's 0x7E (nor bytes 10-11): only the length and the address are its own read's
        rp[9] = 0x00; rp[10] = 0x5A; rp[11] = 0xA5;
        CHECK(s2_memory_reply_for(rp, sizeof rp, 0x40, 0x001FC040u) && !s2_memory_reply_for(rp, sizeof rp, 0x40, 0x001FC080u));
    }

    // Pads
    s2_pad_state pad; uint64_t m; int32_t st[4];
    memset(&pad, 0, sizeof pad); CHECK(!s2_pad_input(&pad, S2_DEADZONE, &m, st) && !s2_pad_motion_dev(&pad, 1));
    // A Pro Controller 2: its own buttons, both sticks
    pad.pro.have = 1; pad.pro.pid = S2_PID_PRO; s2_cal_default(&pad.pro.cal[0]); s2_cal_default(&pad.pro.cal[1]);
    pad.pro.r.buttons = S2_A | S2_ZL | S2_SL_L; pad.pro.r.lx = 2048 + 1200; pad.pro.r.ly = 2048; pad.pro.r.rx = 2048; pad.pro.r.ry = 2048 - 600;
    CHECK(s2_pad_input(&pad, S2_DEADZONE, &m, st) && m == (DM_BTN_A | DM_BTN_ZL) && st[0] == DM_STICK_MAX && st[1] == 0 && st[2] == 0 && st[3] == -(int32_t)lrintf(600.f / 1200.f * DM_STICK_MAX));
    CHECK(s2_pad_motion_dev(&pad, 1) == &pad.pro && s2_pad_motion_dev(&pad, 0) == &pad.pro);
    // Two Joy-Cons: the left's d-pad and stick, the right's face buttons and stick
    memset(&pad, 0, sizeof pad);
    pad.left.have = 1; pad.left.pid = S2_PID_JOYCON_L; s2_cal_default(&pad.left.cal[0]); pad.left.r.buttons = S2_UP | S2_L | S2_SR_L; pad.left.r.lx = 2048 - 1200; pad.left.r.ly = 2048;
    pad.right.have = 1; pad.right.pid = S2_PID_JOYCON_R; s2_cal_default(&pad.right.cal[0]); pad.right.r.buttons = S2_B | S2_ZR | S2_PLUS | S2_SL_R; pad.right.r.rx = 2048; pad.right.r.ry = 2048 + 1200;
    pad.left.r.rx = pad.left.r.ry = 4095; pad.right.r.lx = pad.right.r.ly = 4095;   // the other fields of a Joy-Con's report are not its stick
    CHECK(s2_pad_input(&pad, S2_DEADZONE, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS));   // rail buttons are not shoulders in a pair
    CHECK(st[0] == -DM_STICK_MAX && st[1] == 0 && st[2] == 0 && st[3] == DM_STICK_MAX);
    CHECK(s2_pad_motion_dev(&pad, 1) == &pad.right && s2_pad_motion_dev(&pad, 0) == &pad.left);
    // One Joy-Con alone is held sideways: rail buttons are L / R, its stick the left stick
    pad.left.have = 0; CHECK(s2_pad_input(&pad, S2_DEADZONE, &m, st) && m == (DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_L) && st[0] == 0 && st[1] == DM_STICK_MAX && st[2] == 0 && st[3] == 0);
    CHECK(s2_pad_motion_dev(&pad, 0) == &pad.right);
    pad.right.have = 0; pad.left.have = 1; CHECK(s2_pad_input(&pad, S2_DEADZONE, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_R) && st[0] == -DM_STICK_MAX);

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
    uint8_t pk[33];
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

    // A Joy-Con on its own: its side's buttons with the rail buttons as NpadButtons 24..27
    CHECK(s2_joy_buttons(S2_A | S2_SL_R | S2_SR_R | S2_UP, 1) == (DM_BTN_A | DM_BTN_RSL | DM_BTN_RSR));
    CHECK(s2_joy_buttons(S2_UP | S2_SL_L | S2_A, 0) == (DM_BTN_UP | DM_BTN_LSL));
    // The choosing gestures, held three seconds: SL + SR on a Joy-Con of a pair splits it; L (left) or R (right) on one alone joins; a short or broken hold does nothing
    CHECK(s2_gesture(S2_SL_R | S2_SR_R, 1, 1) == S2_GESTURE_SPLIT && s2_gesture(S2_SL_R, 1, 1) == S2_GESTURE_NONE && s2_gesture(S2_SL_L | S2_SR_L, 0, 1) == S2_GESTURE_SPLIT);
    CHECK(s2_gesture(S2_SL_R | S2_SR_R, 1, 0) == S2_GESTURE_NONE && s2_gesture(S2_R, 1, 0) == S2_GESTURE_JOIN && s2_gesture(S2_L, 0, 0) == S2_GESTURE_JOIN);
    CHECK(s2_gesture(S2_R, 1, 1) == S2_GESTURE_NONE && s2_gesture(S2_L | S2_SL_L, 0, 0) == S2_GESTURE_NONE && s2_gesture(S2_R, 0, 0) == S2_GESTURE_NONE);
    s2_hold hold = {0}; uint64_t t0 = 5000000000ull;
    CHECK(!s2_hold_update(&hold, S2_GESTURE_SPLIT, t0) && !s2_hold_update(&hold, S2_GESTURE_SPLIT, t0 + 2900000000ull));
    CHECK(s2_hold_update(&hold, S2_GESTURE_SPLIT, t0 + S2_GESTURE_NS) && hold.kind == S2_GESTURE_SPLIT && !hold.done);
    CHECK(!s2_hold_update(&hold, S2_GESTURE_NONE, t0 + 3100000000ull) && !s2_hold_update(&hold, S2_GESTURE_SPLIT, t0 + 3200000000ull));   // let go: it starts again
    CHECK(!s2_hold_update(&hold, S2_GESTURE_SPLIT, t0 + 6100000000ull) && s2_hold_update(&hold, S2_GESTURE_SPLIT, t0 + 6200000000ull));

    // The mouse sensor's counters wrap at 16 bits; lifted, the sensor sees no surface
    CHECK(s2_mouse_delta(10, 5) == 5 && s2_mouse_delta(-32760, 32760) == 16 && s2_mouse_delta(32760, -32760) == -16);
    { s2_report mr = {0}; CHECK(!s2_mouse_present(&mr)); mr.mouse_s = 150; CHECK(s2_mouse_present(&mr) && s2_mouse_on_surface(&mr)); mr.mouse_s = 3200; CHECK(!s2_mouse_on_surface(&mr)); }   // sensor off: all zero, no sample

    // Home, A and B for the Home hold-to-quit prompt: Home from any of the pad's controllers, A and B from the right side
    memset(&pad, 0, sizeof pad); int ha = 0, hb = 0;
    pad.left.have = pad.right.have = 1; pad.left.r.buttons = 0xE0000000u; pad.right.r.buttons = 0xE0000000u | S2_HOME | S2_A;
    s2_pad_ab(&pad, &ha, &hb); CHECK(s2_pad_home(&pad) && ha && !hb);
    pad.right.r.buttons = 0xE0000000u; pad.left.r.buttons = 0xE0000000u | S2_HOME; ha = hb = 0;
    s2_pad_ab(&pad, &ha, &hb); CHECK(s2_pad_home(&pad) && !ha && !hb);
    memset(&pad, 0, sizeof pad); CHECK(!s2_pad_home(&pad));
    // The dead zone setting: a share of the nominal travel, 40 counts when unset or unusable
    CHECK(s2_dead_counts(NULL) == 40.f && s2_dead_counts("") == 40.f && s2_dead_counts("x") == 40.f && s2_dead_counts("0.3") == 40.f);
    CHECK(s2_dead_counts("0") == 0.f && s2_dead_counts("0.0333") == 40.f && s2_dead_counts("0.03") == 36.f && s2_dead_counts("0.0333") == S2_DEADZONE && s2_dead_counts("0.25") == 300.f);
    {   // a Pro Controller 2 resting 50 counts right of centre: it moves with a 40 dead zone, not with 0.05 (60 counts)
        s2_pad_state rest; memset(&rest, 0, sizeof rest); rest.pro.have = 1; s2_cal_default(&rest.pro.cal[0]); s2_cal_default(&rest.pro.cal[1]);
        rest.pro.r.lx = 2048 + 50; rest.pro.r.ly = 2048; rest.pro.r.rx = 2048; rest.pro.r.ry = 2048;
        CHECK(s2_pad_input(&rest, 40.f, &m, st) && st[0] > 0);
        CHECK(s2_pad_input(&rest, s2_dead_counts("0.05"), &m, st) && st[0] == 0);
    }
    {   // which calibration a stick is read with: the user's, else the factory's, else the defaults; the factory block stands in for itself with a copy of the stick's when it did not read
        s2_cal u, f, c, fa; s2_cal_default(&u); s2_cal_default(&f); u.cx = 2000; u.maxx = u.maxy = u.minx = u.miny = 900; u.valid = 1; f.cx = 2100; f.maxx = f.maxy = f.minx = f.miny = 1200; f.valid = 1;
        CHECK(s2_cal_pick(&u, 1, &f, 1, &c, &fa) && c.cx == 2000 && fa.cx == 2100 && fa.maxx == 1200);
        CHECK(s2_cal_pick(&u, 1, NULL, 0, &c, &fa) && c.cx == 2000 && fa.cx == 2000 && fa.maxx == 900);
        CHECK(s2_cal_pick(NULL, 0, &f, 1, &c, &fa) && c.cx == 2100 && fa.cx == 2100);
        CHECK(!s2_cal_pick(NULL, 0, NULL, 0, &c, &fa) && !c.valid && c.cx == 2048 && !fa.valid && fa.cx == 2048);
    }
    // A share of the travel: each side its own, a travel under one count as one
    CHECK(s2_share(600, 1200, 800) == 0.5f && s2_share(-400, 1200, 800) == -0.5f && s2_share(5, 0, 0) == 5.f);
    {   // The edges step's turns: two full turns at the edge in 10 degree steps make two, at most two; a turn and back again, jitter, or circles inside the share make none
        s2_turns t; memset(&t, 0, sizeof t); float n = 0;
        for (int i = 0; i <= 36 * 3; i++) n = s2_turns_update(&t, 0.9f * cosf(i * 0.17453293f), 0.9f * sinf(i * 0.17453293f));
        CHECK(n == S2_TURNS_WANTED && t.turns == S2_TURNS_WANTED);
        memset(&t, 0, sizeof t);
        for (int i = 0; i <= 18; i++) n = s2_turns_update(&t, cosf(i * 0.17453293f), sinf(i * 0.17453293f));
        CHECK(fabsf(n - 0.5f) < 0.01f);                                                       // half a turn anticlockwise
        for (int i = 18; i >= 0; i--) n = s2_turns_update(&t, cosf(i * 0.17453293f), sinf(i * 0.17453293f));
        CHECK(fabsf(n - 0.5f) < 0.01f && fabsf(t.winding) < 0.01f);                           // back again: the most stays, the angle is undone
        memset(&t, 0, sizeof t);
        for (int i = 0; i < 200; i++) n = s2_turns_update(&t, 0.95f, (i & 1) ? 0.02f : -0.02f);
        CHECK(n < 0.01f);                                                                     // held at the edge, jittering
        memset(&t, 0, sizeof t);
        for (int i = 0; i <= 72; i++) n = s2_turns_update(&t, 0.5f * cosf(i * 0.17453293f), 0.5f * sinf(i * 0.17453293f));
        CHECK(n == 0.f && !t.have);                                                           // two turns inside 0.6 of the travel
        memset(&t, 0, sizeof t);
        for (int i = 0; i <= 9; i++) s2_turns_update(&t, -cosf(i * 0.17453293f), -sinf(i * 0.17453293f));   // a quarter anticlockwise, from the left to the bottom
        s2_turns_update(&t, 0.1f, 0.1f);                                                      // pulled in: the next angle starts over
        for (int i = 0; i <= 9; i++) n = s2_turns_update(&t, cosf(i * 0.17453293f), sinf(i * 0.17453293f));
        CHECK(fabsf(n - 0.5f) < 0.01f && fabsf(t.winding - 3.14159265f) < 0.01f);             // the jump from the bottom to the right is no turn: two quarters, half a turn
    }
    printf("t_switch2: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
