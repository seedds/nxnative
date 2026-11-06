// The Switch 2 BLE protocol (dm_switch2_proto.h): reports and adverts recorded from a left and a right Joy-Con 2, command framing, stick
// calibration, the pad a player holds (a full pad member, two halves as one pair, one half alone sideways) and the per-form motion maps.
// Compiles on any machine: this file plus the header only, no platform headers under test.
//
// The recorded reports and the pad/motion expectations follow JakePre's hardware-verified seedds/nxnative PR 1346 (dm/host/tests/t_switch2.c);
// the GameCube's analog-trigger reading and the motor-lane rule come from switch2mac's published protocol notes, which are not hardware-verified
// on our side yet.

#include <stdio.h>
#include <string.h>
#include "dm_switch2_proto.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static size_t unhex(const char *h, uint8_t *out) { size_t n = 0; for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); out[n++] = (uint8_t)v; } return n; }

static void pad_default(ds2_dev *dev, uint16_t pid) {
    memset(dev, 0, sizeof *dev);
    dev->have = 1; dev->pid = pid;
    for (int i = 0; i < 2; i++) ds2_cal_default(&dev->cal[i]);
}

static void pad_clear(ds2_pad_state *p) { memset(p, 0, sizeof *p); }

int main(void) {
    uint8_t b[128]; size_t n; ds2_report r = {0};
    // The first report of the left Joy-Con 2 (stick at rest; nothing pressed; the top three button bits always set)
    n = unhex("b3240000000000e0ff0f14d882fff77f000000000000000000edff12ffd6fda70d0015080000000000012ee30200fdff000000000000000000000000000000", b);
    CHECK(n == 63 && ds2_report_parse(b, n, &r));
    CHECK(r.buttons == 0xE0000000u && r.lx == 2068 && r.ly == 2093 && r.rx == 2047 && r.ry == 2047 && r.battery_mv > 3000 && r.battery_mv < 4400);
    CHECK(!ds2_report_parse(b, 0x3B, &r) && !ds2_report_parse(b, 0x60, &r) && !ds2_report_parse(NULL, 63, &r));
    // The left Joy-Con with the d-pad's Down pressed, the right Joy-Con with A pressed (its stick data is in the right field)
    n = unhex("48520000000001e0ff0f13e882fff77f000000000000000000b1ff5bfef1fda70d00110800000000000116a9b300fdff8d005d0aef0bf0ffefffe7ff000000", b);
    CHECK(ds2_report_parse(b, n, &r) && (r.buttons & DS2_BTN_DOWN) && !(r.buttons & DS2_BTN_UP));
    CHECK(r.accel[0] == ds2_s16(b + 0x30) && r.gyro[2] == ds2_s16(b + 0x3A));
    n = unhex("722e0000080000e0ff0ffff77ff2a77d000000000000000000a000160002ff880d002608000000000001af628a00fdff000000000000000000000000000000", b);
    CHECK(ds2_report_parse(b, n, &r) && r.buttons == (0xE0000000u | DS2_BTN_A) && r.rx == 2034 && r.ry == 2010 && r.lx == 2047 && r.ly == 2047);

    // Command frames and the memory-read payload
    uint8_t f[64], p[8];
    ds2_memory_payload(p, 0x40, DS2_ADDR_INFO);
    CHECK(p[0] == 0x40 && p[1] == 0x7E && p[4] == 0x00 && p[5] == 0x30 && p[6] == 0x01 && p[7] == 0);
    CHECK(ds2_command(f, sizeof f, DS2_CMD_MEMORY, DS2_SUB_MEMORY_READ, p, 8) == 16 && f[0] == 0x02 && f[1] == 0x91 && f[2] == 0x01 && f[3] == 0x04 && f[5] == 8 && f[8] == 0x40);
    CHECK(ds2_command(f, 7, 1, 1, NULL, 0) == 0 && ds2_command(f, 8, 1, 1, NULL, 0) == 8);

    // Advertisements as captured: after the company id 0x0553; the four models, and pairing mode (no host)
    ds2_advert a = {0};
    n = unhex("0100037e0566200001000000000000000f00000000000000", b);
    CHECK(ds2_advert_parse(b, n, &a) && a.vid == 0x057E && a.pid == DS2_PID_JOYCON2_R && a.host == 0);
    n = unhex("0100037e0567200001000000000000000f00000000000000", b);
    CHECK(ds2_advert_parse(b, n, &a) && a.pid == DS2_PID_JOYCON2_L);
    n = unhex("0100037e0569200001000000000000000f00000000000000", b);
    CHECK(ds2_advert_parse(b, n, &a) && a.pid == DS2_PID_PRO_2);
    n = unhex("0100037e0573200001000000000000000f00000000000000", b);
    CHECK(ds2_advert_parse(b, n, &a) && a.pid == DS2_PID_NSO_GAMECUBE);
    CHECK(!ds2_advert_parse(b, 15, &a));
    n = unhex("0100037e0599200001000000000000000f00000000000000", b);
    CHECK(!ds2_advert_parse(b, n, &a));   // an unknown Nintendo product is not accepted

    // Calibration of the right Joy-Con 2 (factory, stick slot 1): centre 2095, 1999; travel 1175, 1225 up and 1181, 1129 down
    ds2_cal c;
    n = unhex("2ff87c97944c9d9446ffff", b);
    CHECK(!ds2_cal_blank(b, n) && ds2_cal_parse(b, n, &c) && c.cx == 2095 && c.cy == 1999 && c.maxx == 1175 && c.maxy == 1225 && c.minx == 1181 && c.miny == 1129);
    float x, y;
    ds2_cal_apply(&c, 3270, 1999, 40, &x, &y); CHECK(x == 1.f && y == 0.f);
    ds2_cal_apply(&c, 2095 + 20, 1999 - 20, 40, &x, &y); CHECK(x == 0.f && y == 0.f);        // inside the dead zone
    ds2_cal_apply(&c, 2095 - 1181, 1999 + 2000, 40, &x, &y); CHECK(x == -1.f && y == 1.f);    // past the travel clamps
    ds2_cal_default(&c);
    n = unhex("ffffffffffffffff", b); CHECK(!ds2_cal_parse(b, n, &c) && !c.valid);            // an unset user slot (0xFFFFFF) leaves it default

    // The motor packet: one 16-byte block per actuator (a Joy-Con's one grip), two for the Pro Controller 2
    ds2_vibration v = {0x0E1, 0x2FF, 0x1E1, 0x2FF};
    CHECK(ds2_motor_packet(b, &v, 3, 1) == 17);
    CHECK(ds2_motor_packet(f, &v, 3, 2) == 33 && f[0] == 0 && f[1] == 0x53 && f[17] == 0x53);

    // The report's environment fields, built by hand into the recorded shape: battery current +320 (charging), charge state, die temperature,
    // the GameCube pad's analog triggers, and the buttons
    memset(b, 0, sizeof b);
    b[4] = 0x08;   // A
    b[0x1F] = 0x10; b[0x20] = 0x0F;          // 3856 mV
    b[0x21] = 0x01;                          // charge state
    b[0x22] = 0x40; b[0x23] = 0x01;          // current +320 (little-endian, signed): charging
    b[0x2E] = 127; b[0x2F] = 0;              // temperature raw 127: 26 degrees C
    b[0x3C] = 63; b[0x3D] = 64;              // triggers: left at 63, right just past it
    CHECK(ds2_report_parse(b, 0x40, &r) && r.buttons == DS2_BTN_A && r.battery_mv == 3856 && r.battery_current == 320 && r.charge_state == 1 && r.temperature_raw == 127);
    CHECK(ds2_gamecube_buttons(r.buttons, r.trigger_l, r.trigger_r, 64) == (DM_BTN_A | DM_BTN_R));
    CHECK(ds2_gamecube_buttons(r.buttons, r.trigger_l, r.trigger_r, 63) == (DM_BTN_A | DM_BTN_L | DM_BTN_R));   // the threshold is the setting's
    CHECK(ds2_report_parse(b, 0x3F, &r) && r.battery_current == 320 && r.trigger_r == 64);   // a 63-byte report carries the same fields

    // Pads
    ds2_pad_state pad; uint64_t m; int32_t st[4];
    pad_clear(&pad);
    CHECK(!ds2_pad_input(&pad, 40.f, 64, 0, 0, &m, st) && !ds2_pad_motion_dev(&pad));
    // A Pro Controller 2: its own buttons (GL / GR remapped here to B / A for the check), both sticks
    pad_clear(&pad);
    pad_default(&pad.pro, DS2_PID_PRO_2);
    pad.pro.r.buttons = DS2_BTN_A | DS2_BTN_ZL | DS2_BTN_GL; pad.pro.r.lx = 3248; pad.pro.r.ly = 2048; pad.pro.r.rx = 2048; pad.pro.r.ry = 1448;
    CHECK(ds2_pad_input(&pad, 40.f, 64, DM_BTN_B, DM_BTN_A, &m, st) && m == (DM_BTN_A | DM_BTN_ZL | DM_BTN_B));
    CHECK(st[0] == 32767 && st[1] == 0 && st[2] == 0 && st[3] == -(int32_t)lrintf(600.f / 1200.f * 32767.f));
    CHECK(ds2_pad_motion_dev(&pad) == &pad.pro);
    // An NSO GameCube pad: its analog triggers act as digital L / R past a quarter travel
    pad_clear(&pad);
    pad_default(&pad.pro, DS2_PID_NSO_GAMECUBE);
    pad.pro.r.buttons = DS2_BTN_A; pad.pro.r.trigger_l = 63; pad.pro.r.trigger_r = 64;
    CHECK(ds2_pad_input(&pad, 40.f, 64, 0, 0, &m, st) && m == (DM_BTN_A | DM_BTN_R));
    // Two Joy-Con halves: the left's d-pad and stick, the right's face buttons and stick; a rail button is not a shoulder in a pair
    pad_clear(&pad);
    pad_default(&pad.left, DS2_PID_JOYCON2_L);
    pad_default(&pad.right, DS2_PID_JOYCON2_R);
    pad.left.r.buttons = DS2_BTN_UP | DS2_BTN_L | DS2_BTN_SL_L; pad.left.r.lx = 848; pad.left.r.ly = 2048;
    pad.right.r.buttons = DS2_BTN_B | DS2_BTN_ZR | DS2_BTN_PLUS; pad.right.r.rx = 2048; pad.right.r.ry = 3248;
    pad.left.r.rx = pad.left.r.ry = 4095; pad.right.r.lx = pad.right.r.ly = 4095;   // the other field of a Joy-Con's report is not its stick
    CHECK(ds2_pad_input(&pad, 40.f, 64, 0, 0, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS));
    CHECK(st[0] == -32767 && st[1] == 0 && st[2] == 0 && st[3] == 32767);
    CHECK(ds2_pad_motion_dev(&pad) == &pad.right);
    // One right half alone is held sideways: its rail buttons are the shoulders, its stick (the right field, the stick-1 calibration) the left stick
    pad_clear(&pad);
    pad_default(&pad.right, DS2_PID_JOYCON2_R);
    pad.right.r.buttons = DS2_BTN_B | DS2_BTN_ZR | DS2_BTN_PLUS | DS2_BTN_SL_R; pad.right.r.rx = 2048; pad.right.r.ry = 3248;
    CHECK(ds2_pad_input(&pad, 40.f, 64, 0, 0, &m, st) && m == (DM_BTN_B | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_L));
    CHECK(st[0] == 0 && st[1] == 32767 && st[2] == 0 && st[3] == 0);
    CHECK(ds2_pad_motion_dev(&pad) == &pad.right);
    // One left half alone, sideways: d-pad and L / ZL, stick left, SL and SR both shoulders
    pad_clear(&pad);
    pad_default(&pad.left, DS2_PID_JOYCON2_L);
    pad.left.r.buttons = DS2_BTN_UP | DS2_BTN_L | DS2_BTN_SL_L | DS2_BTN_SR_L; pad.left.r.lx = 848; pad.left.r.ly = 2048;
    CHECK(ds2_pad_input(&pad, 40.f, 64, 0, 0, &m, st) && m == (DM_BTN_UP | DM_BTN_L | DM_BTN_R) && st[0] == -32767 && st[2] == 0 && st[3] == 0);

    // Motion, the per-form maps. A right Joy-Con 2 held upright, still: gravity +1 G on raw z, which is the Switch frame's (0, 0, -1)
    int16_t raw[6] = {100, 200, 4096, 10, 20, 30}; float acc[3], rot[3];
    ds2_motion(raw, ds2_map_pair, NULL, acc, rot);
    CHECK(fabsf(acc[0] + 100.f / 4096.f) < 1e-6f && fabsf(acc[1] + 200.f / 4096.f) < 1e-6f && fabsf(acc[2] + 1.f) < 1e-6f);
    CHECK(fabsf(rot[0] - 10 * DS2_GYRO_RAD) < 1e-6f && fabsf(rot[1] - 20 * DS2_GYRO_RAD) < 1e-6f && fabsf(rot[2] - 30 * DS2_GYRO_RAD) < 1e-6f);   // identity for rotation
    // Right alone takes the Pro Controller 1's map: its raw x (the top, held upright) reads gravity along -y
    int16_t sw[6] = {4096, 0, 0, 0, 0, 0};
    ds2_motion(sw, ds2_map_right_alone, NULL, acc, rot); CHECK(fabsf(acc[1] + 1.f) < 1e-6f && fabsf(acc[0]) < 1e-6f);
    sw[0] = 0; sw[2] = 4096;
    ds2_motion(sw, ds2_map_right_alone, NULL, acc, rot); CHECK(fabsf(acc[2] + 1.f) < 1e-6f);
    // The pad's map follows its shape
    pad_clear(&pad);
    pad_default(&pad.right, DS2_PID_JOYCON2_R);
    CHECK(ds2_pad_map(&pad, &pad.right) == ds2_map_right_alone);
    pad_default(&pad.left, DS2_PID_JOYCON2_L);
    CHECK(ds2_pad_map(&pad, &pad.left) == ds2_map_pair && ds2_pad_map(&pad, &pad.right) == ds2_map_pair);
    pad_clear(&pad);
    pad_default(&pad.pro, DS2_PID_PRO_2);
    CHECK(ds2_pad_map(&pad, &pad.pro) == ds2_map_pro);

    // The gyro rest offset: about a second of nearly constant samples is learned; movement restarts the window; a large offset is a turn, not rest
    ds2_rest rs; memset(&rs, 0, sizeof rs); int16_t q[6] = {0, 0, 4096, 18, -20, -4};
    for (int i = 0; i < DS2_REST_N - 1; i++) { q[3] = 18 + (i & 1); ds2_rest_update(&rs, q); }
    CHECK(!rs.have); q[3] = 18; ds2_rest_update(&rs, q);
    CHECK(rs.have && fabsf(rs.bias[0] - 18.5f) < 1.f && fabsf(rs.bias[1] + 20.f) < 0.01f && fabsf(rs.bias[2] + 4.f) < 0.01f);
    for (int i = 0; i < DS2_REST_N * 2; i++) { q[3] = i % 2 ? 500 : -500; ds2_rest_update(&rs, q); }   // shaking: never learned
    CHECK(fabsf(rs.bias[0] - 18.5f) < 1.f);
    ds2_rest big; memset(&big, 0, sizeof big); int16_t z[6] = {0, 0, 4096, 200, 0, 0};
    for (int i = 0; i < DS2_REST_N; i++) ds2_rest_update(&big, z);
    CHECK(!big.have);   // 200 counts (14 degrees per second) is a turn, not rest

    printf("switch2_ios: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
