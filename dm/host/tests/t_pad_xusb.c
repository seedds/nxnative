// Wired Xbox 360 (XUSB) pads over USB (pad_xusb.h): report validation, buttons by position, 25% trigger threshold, stick dead zone, deduplication and device filters.
#include <stdio.h>
#include <string.h>
#include "../pad_xusb.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }
static void make_report(uint8_t r[XUSB_REPORT_LEN], int b1, int b2, int lt, int rt, int lx, int ly, int rx, int ry) {
    memset(r, 0, XUSB_REPORT_LEN);
    r[1] = XUSB_REPORT_LEN; r[2] = (uint8_t)b1; r[3] = (uint8_t)b2; r[4] = (uint8_t)lt; r[5] = (uint8_t)rt;
    put16(r + 6, lx); put16(r + 8, ly); put16(r + 10, rx); put16(r + 12, ry);
}

static void test_parse_and_dedup(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0};
    make_report(r, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.heard);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 0);   // identical report: no change
    CHECK(!pad_xusb_decode(&s, r, XUSB_REPORT_LEN - 1));
    const uint8_t state[3] = {0x01, 0x03, 0x00};
    CHECK(!pad_xusb_decode(&s, state, sizeof state));   // connect state message, not input
    r[0] = 0x01; CHECK(!pad_xusb_decode(&s, r, sizeof r)); r[0] = 0x00;
    r[1] = 0x13; CHECK(!pad_xusb_decode(&s, r, sizeof r)); r[1] = XUSB_REPORT_LEN;

    // A pad at rest as an 0e6f:0301 PDP reported it on macOS: both sticks off centre inside the dead zone
    pad_xusb_reset(&s);
    CHECK(!s.heard && s.buttons == 0);
    make_report(r, 0, 0, 0, 0, 102, -443, -86, -145);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.s[0] == 0 && s.s[1] == 0 && s.s[2] == 0 && s.s[3] == 0 && s.buttons == 0);
    // 1-LSB stick jitter inside the dead zone does not emit a change (protects DM_PAD_MERGE)
    make_report(r, 0, 0, 0, 0, 103, -442, -85, -144);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 0);
}

static void test_buttons(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0};
    static const struct { int b1, b2; uint64_t want; } t[] = {
        {0x01, 0, DM_BTN_UP}, {0x02, 0, DM_BTN_DOWN}, {0x04, 0, DM_BTN_LEFT}, {0x08, 0, DM_BTN_RIGHT}, {0x05, 0, DM_BTN_UP | DM_BTN_LEFT},
        {0x10, 0, DM_BTN_PLUS}, {0x20, 0, DM_BTN_MINUS}, {0x40, 0, DM_BTN_LS}, {0x80, 0, DM_BTN_RS},
        {0, 0x01, DM_BTN_L}, {0, 0x02, DM_BTN_R},
        {0, 0x10, DM_BTN_B}, {0, 0x20, DM_BTN_A}, {0, 0x40, DM_BTN_Y}, {0, 0x80, DM_BTN_X},   // by position: Xbox A (bottom) -> Switch B, Xbox B (right) -> Switch A
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        pad_xusb_reset(&s);
        make_report(r, t[i].b1, t[i].b2, 0, 0, 0, 0, 0, 0);
        CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.buttons == t[i].want);
    }
    // Guide (byte 3 bit 2) is left unmapped
    pad_xusb_reset(&s);
    make_report(r, 0, 0x04, 0, 0, 0, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.buttons == 0);

    // Triggers actuate past 25% (>= 64 of 255)
    make_report(r, 0, 0, XUSB_TRIGGER_PRESS - 1, XUSB_TRIGGER_PRESS - 1, 0, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 0 && s.buttons == 0);
    make_report(r, 0, 0, 255, 0, 0, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.buttons == DM_BTN_ZL);
    make_report(r, 0, 0, 0, XUSB_TRIGGER_PRESS, 0, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.buttons == DM_BTN_ZR);
}

static void test_sticks(void) {
    uint8_t r[XUSB_REPORT_LEN]; pad_xusb s = {0};
    make_report(r, 0, 0, 0, 0, 32767, 0, -32768, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.s[0] == DM_STICK_MAX && s.s[1] == 0 && s.s[2] == -DM_STICK_MAX && s.s[3] == 0);
    make_report(r, 0, 0, 0, 0, 0, 32767, 0, -32768);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.s[0] == 0 && s.s[1] == DM_STICK_MAX && s.s[2] == 0 && s.s[3] == -DM_STICK_MAX);   // up is positive
    make_report(r, 0, 0, 0, 0, 16384, 0, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.s[0] > 0 && s.s[0] < DM_STICK_MAX / 2 + 100);
    // A left push seen on a PDP: round dead zone preserves small Y
    make_report(r, 0, 0, 0, 0, -29954, 307, 0, 0);
    CHECK(pad_xusb_decode(&s, r, sizeof r) == 1 && s.s[0] < -DM_STICK_MAX * 8 / 10 && s.s[1] > 0 && s.s[1] < DM_STICK_MAX / 20);
}

static void test_filters(void) {
    CHECK(pad_xusb_device_ok(0xff, 0xff, 0xff));
    CHECK(!pad_xusb_device_ok(0xff, 0x47, 0xd0));     // GIP device class
    CHECK(!pad_xusb_device_ok(0x09, 0x00, 0x01));     // USB hub
    CHECK(!pad_xusb_device_ok(0x00, 0x00, 0x00));     // composite device
    CHECK(pad_xusb_interface_ok(0xff, 0x5d, 0x01));   // XUSB controller interface
    CHECK(!pad_xusb_interface_ok(0xff, 0x5d, 0x02));  // XUSB audio interface
    CHECK(!pad_xusb_interface_ok(0xff, 0x5d, 0x81));  // Xbox 360 wireless receiver
    CHECK(!pad_xusb_interface_ok(0xff, 0x47, 0xd0));
}

int main(void) {
    test_parse_and_dedup();
    test_buttons();
    test_sticks();
    test_filters();
    printf("t_pad_xusb: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
