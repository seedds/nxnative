// Xbox 360 (XUSB) pads over USB (pad_xusb.h): the input report layout, the buttons by position, the trigger threshold, the stick dead zone and the device filters
#include <stdio.h>
#include <string.h>
#include "../pad_xusb.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }
static void report(uint8_t r[XUSB_REPORT_LEN], int b1, int b2, int lt, int rt, int lx, int ly, int rx, int ry) {
    memset(r, 0, XUSB_REPORT_LEN); r[1] = 0x14; r[2] = (uint8_t)b1; r[3] = (uint8_t)b2; r[4] = (uint8_t)lt; r[5] = (uint8_t)rt;
    put16(r + 6, lx); put16(r + 8, ly); put16(r + 10, rx); put16(r + 12, ry);
}

static void test_parse(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0};
    report(r, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s));
    CHECK(!pad_xusb_parse(r, XUSB_REPORT_LEN - 1, &s));   // a short report
    const uint8_t state[3] = {0x01, 0x03, 0x00};
    CHECK(!pad_xusb_parse(state, sizeof state, &s));      // the state messages a pad sends at connect (0x01 0x03, 0x02 0x03, 0x03 0x03) are not input
    r[0] = 0x01; CHECK(!pad_xusb_parse(r, sizeof r, &s)); r[0] = 0x00;
    r[1] = 0x13; CHECK(!pad_xusb_parse(r, sizeof r, &s)); r[1] = 0x14;
    // A pad at rest, as an 0e6f:0301 PDP reported it on macOS: both sticks a little off centre, nothing pressed
    report(r, 0, 0, 0, 0, 102, -443, -86, -145);
    CHECK(pad_xusb_parse(r, sizeof r, &s) && s.lx == 102 && s.ly == -443 && s.rx == -86 && s.ry == -145);
    int32_t o[4]; pad_xusb_sticks(&s, o);
    CHECK(o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0 && pad_xusb_buttons(&s) == 0);   // the dead zone absorbs the rest offsets
}

static void test_buttons(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0};
    static const struct { int b1, b2; uint64_t want; } t[] = {
        {0x01, 0, DM_BTN_UP}, {0x02, 0, DM_BTN_DOWN}, {0x04, 0, DM_BTN_LEFT}, {0x08, 0, DM_BTN_RIGHT}, {0x05, 0, DM_BTN_UP | DM_BTN_LEFT},
        {0x10, 0, DM_BTN_PLUS}, {0x20, 0, DM_BTN_MINUS}, {0x40, 0, DM_BTN_LS}, {0x80, 0, DM_BTN_RS},
        {0, 0x01, DM_BTN_L}, {0, 0x02, DM_BTN_R},
        {0, 0x10, DM_BTN_B}, {0, 0x20, DM_BTN_A}, {0, 0x40, DM_BTN_Y}, {0, 0x80, DM_BTN_X},   // by position: Xbox A (bottom) is the Switch's B, Xbox B (right) its A
        {0, 0x04, 0},                                                                       // Guide has no Switch button
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        report(r, t[i].b1, t[i].b2, 0, 0, 0, 0, 0, 0);
        CHECK(pad_xusb_parse(r, sizeof r, &s) && pad_xusb_buttons(&s) == t[i].want);
    }
    report(r, 0, 0, XUSB_TRIGGER_PRESS - 1, XUSB_TRIGGER_PRESS - 1, 0, 0, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s) && !(pad_xusb_buttons(&s) & (DM_BTN_ZL | DM_BTN_ZR)));
    report(r, 0, 0, 255, 0, 0, 0, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s) && pad_xusb_buttons(&s) == DM_BTN_ZL);
    report(r, 0, 0, 0, XUSB_TRIGGER_PRESS, 0, 0, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s) && pad_xusb_buttons(&s) == DM_BTN_ZR);
}

static void test_sticks(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0}; int32_t o[4];
    report(r, 0, 0, 0, 0, 32767, 0, -32768, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s)); pad_xusb_sticks(&s, o);
    CHECK(o[0] == DM_STICK_MAX && o[1] == 0 && o[2] == -DM_STICK_MAX && o[3] == 0);   // full deflection reaches the Switch's range on both sides
    report(r, 0, 0, 0, 0, 0, 32767, 0, -32768);
    CHECK(pad_xusb_parse(r, sizeof r, &s)); pad_xusb_sticks(&s, o);
    CHECK(o[0] == 0 && o[1] == DM_STICK_MAX && o[2] == 0 && o[3] == -DM_STICK_MAX);   // up is positive on the pad and on the Switch
    report(r, 0, 0, 0, 0, 16384, 0, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s)); pad_xusb_sticks(&s, o);
    CHECK(o[0] > 0 && o[0] < DM_STICK_MAX / 2 + 100);   // half deflection scales back past the dead zone, below half
    report(r, 0, 0, 0, 0, -29954, 307, 0, 0);
    CHECK(pad_xusb_parse(r, sizeof r, &s)); pad_xusb_sticks(&s, o);
    CHECK(o[0] < -DM_STICK_MAX * 8 / 10 && o[1] > 0 && o[1] < DM_STICK_MAX / 20);   // a left push seen on a PDP: the dead zone is round, so a small y stays small, not zero
}

static void test_filters(void) {
    CHECK(pad_xusb_device_ok(0xff, 0xff, 0xff));      // a PDP "Gamepad for Xbox 360" (0e6f:0301)
    CHECK(!pad_xusb_device_ok(0xff, 0x47, 0xd0));     // a Turtle Beach REACT-R (10f5:7024) speaks the Xbox One protocol (ff/47/d0): not this reader's
    CHECK(!pad_xusb_device_ok(0x09, 0x00, 0x01));     // a hub
    CHECK(!pad_xusb_device_ok(0x00, 0x00, 0x00));     // a composite device, a keyboard or a disk
    CHECK(pad_xusb_interface_ok(0xff, 0x5d, 0x01));   // the XUSB controller interface
    CHECK(!pad_xusb_interface_ok(0xff, 0x5d, 0x02));  // the same class's audio interface
    CHECK(!pad_xusb_interface_ok(0xff, 0x47, 0xd0));
}

// Xbox One (GIP) reports, as a Turtle Beach REACT-R (10f5:7024) sent them on macOS: 36 bytes, command 0x20, the payload from byte 4
static int gip(const char *hex, pad_xusb *s) {
    uint8_t r[64]; size_t n = 0;
    for (const char *h = hex; h[0] && h[1]; h += 3) { unsigned v; sscanf(h, "%2x", &v); r[n++] = (uint8_t)v; if (!h[2]) break; }
    return pad_gip_parse(r, n, s);
}
static void test_gip(void) {
    pad_xusb s = {0};
    static const char tail[] = " 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00";
    char b[256];
#define REP(fmt) (snprintf(b, sizeof b, "20 00 07 20 " fmt "%s", tail), gip(b, &s))
    CHECK(REP("00 08 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_RIGHT);   // D-pad right
    CHECK(REP("10 01 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == (DM_BTN_B | DM_BTN_UP));   // A (bottom) is the Switch's B
    CHECK(REP("20 00 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_A);   // B (right) is the Switch's A
    CHECK(REP("40 00 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_Y);   // X (left)
    CHECK(REP("80 00 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_X);   // Y (top)
    CHECK(REP("08 00 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_MINUS);   // View
    CHECK(REP("04 00 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_PLUS);    // Menu
    CHECK(REP("00 10 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_L);
    CHECK(REP("00 20 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_R);
    CHECK(REP("00 40 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_LS);
    CHECK(REP("00 80 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_RS);
    CHECK(REP("00 01 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_UP);
    CHECK(REP("00 02 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_DOWN);
    CHECK(REP("00 04 00 00 00 00 e4 05 00 00 52 fe") && pad_xusb_buttons(&s) == DM_BTN_LEFT);
    CHECK(REP("50 00 00 00 00 00 e4 05 00 00 e1 fe") && pad_xusb_buttons(&s) == (DM_BTN_Y | DM_BTN_B));   // X and A together
    // The 10-bit triggers: 0x03ff is full, half is 0x0200, a little is below the press threshold
    CHECK(REP("00 00 ff 03 00 00 e4 05 4f 08 00 00") && s.lt == 255 && s.rt == 0 && pad_xusb_buttons(&s) == DM_BTN_ZL);
    CHECK(REP("00 00 00 00 ff 03 e4 05 00 00 00 00") && s.lt == 0 && s.rt == 255 && pad_xusb_buttons(&s) == DM_BTN_ZR);
    CHECK(REP("00 00 00 02 00 02 e4 05 00 00 00 00") && s.lt == 128 && s.rt == 128 && pad_xusb_buttons(&s) == (DM_BTN_ZL | DM_BTN_ZR));
    CHECK(REP("00 00 3c 00 cc 00 e4 05 00 00 00 00") && !(pad_xusb_buttons(&s) & (DM_BTN_ZL | DM_BTN_ZR)));
    // Sticks: this pad rests off centre (left X 1508, right X -430) and the dead zone takes it in; a long push on left Y is up
    int32_t o[4];
    CHECK(REP("00 00 00 00 00 00 e4 05 00 00 52 fe") && s.lx == 1508 && s.rx == -430 && s.ly == 0 && s.ry == 0);
    pad_xusb_sticks(&s, o); CHECK(o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0);
    CHECK(REP("00 00 00 00 00 00 e4 05 ff 7f 00 00") && s.ly == 32767);
    pad_xusb_sticks(&s, o); CHECK(o[1] == DM_STICK_MAX);
    CHECK(REP("00 00 00 00 00 00 00 80 00 80 00 00") && s.lx == -32768 && s.ly == -32768);
    pad_xusb_sticks(&s, o); CHECK(o[0] < 0 && o[1] < 0);
#undef REP
    // What is not input: the announce and the status message, a short report, and a payload shorter than it declares
    CHECK(!gip("03 20 01 04 80 00 00 00", &s));
    CHECK(!gip("20 00 01 20 00 08", &s));
    CHECK(!gip("20 00 01 20 00 08 00 00 00 00 e4 05 00 00 52 fe 00 00", &s));   // length byte 0x20 but 18 bytes arrived
    CHECK(!gip("02 20 01 08 00 00 00 00 00 00 00 00", &s));
    CHECK(pad_gip_power_on[0] == 0x05 && pad_gip_power_on[1] == 0x20 && pad_gip_power_on[3] == 0x01 && sizeof pad_gip_power_on == 5);
    CHECK(pad_gip_device_ok(0xff, 0x47, 0xd0) && !pad_gip_device_ok(0xff, 0xff, 0xff) && !pad_xusb_device_ok(0xff, 0x47, 0xd0));   // each reader opens only its own devices
    CHECK(pad_gip_interface_ok(0xff, 0x47, 0xd0) && !pad_gip_interface_ok(0xff, 0x5d, 0x01) && !pad_xusb_interface_ok(0xff, 0x47, 0xd0));
}

int main(void) {
    test_parse();
    test_buttons();
    test_sticks();
    test_filters();
    test_gip();
    printf("t_pad_xusb: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
