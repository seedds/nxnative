// The Xbox 360 Wireless Gaming Receiver's reports (pad_x360.h): presence, buttons by position, the D-pad, triggers, sticks with XInput's dead zones and DM_X360_DEAD
#include <stdio.h>
#include "../pad_x360.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

// An input report as the receiver sends it: 00 01 00 f0 then the wired controller's 20 bytes (00 14, buttons, triggers, sticks)
static size_t input(uint8_t r[29], uint8_t b0, uint8_t b1, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    memset(r, 0, 29);
    r[1] = 0x01; r[3] = 0xf0; r[5] = 0x14; r[6] = b0; r[7] = b1; r[8] = lt; r[9] = rt;
    int16_t s[4] = {lx, ly, rx, ry};
    for (int i = 0; i < 4; i++) { r[10 + 2 * i] = (uint8_t)s[i]; r[11 + 2 * i] = (uint8_t)((uint16_t)s[i] >> 8); }
    return 29;
}

int main(void) {
    pad_hid h; uint8_t r[29]; int32_t s[4];
    pad_x360_init(&h, NULL);
    CHECK(h.dead[0] > 0.23f && h.dead[0] < 0.25f && h.dead[1] > 0.26f && h.dead[1] < 0.27f);   // XInput's 7849 and 8689 of 32767
    uint8_t on[2] = {0x08, 0x80}, off[2] = {0x08, 0x00}, head[2] = {0x08, 0xc0};
    CHECK(pad_x360_report(&h, on, 2) == X360_PRESENT);
    CHECK(pad_x360_report(&h, head, 2) == X360_PRESENT);                    // a headset beside the controller
    CHECK(pad_x360_report(&h, off, 2) == X360_ABSENT);
    uint8_t other[4] = {0x00, 0x0f, 0x00, 0xf0}; CHECK(pad_x360_report(&h, other, 4) == 0);   // a status report
    input(r, 0, 0x10, 0, 0, 0, 0, 0, 0); CHECK(pad_x360_report(&h, r, 17) == 0);               // too short: no sticks
    CHECK(pad_x360_report(&h, r, input(r, 0, 0x10, 0, 0, 0, 0, 0, 0)) == X360_INPUT && pad_hid_buttons(&h) == DM_BTN_B);   // A (bottom) is B
    pad_x360_report(&h, r, input(r, 0, 0x20, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == DM_BTN_A);    // B (right) is A
    pad_x360_report(&h, r, input(r, 0, 0x40, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == DM_BTN_Y);    // X (left) is Y
    pad_x360_report(&h, r, input(r, 0, 0x80, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == DM_BTN_X);    // Y (top) is X
    pad_x360_report(&h, r, input(r, 0, 0x03, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == (DM_BTN_L | DM_BTN_R));
    pad_x360_report(&h, r, input(r, 0, 0x04, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == 0);           // Guide: nothing
    pad_x360_report(&h, r, input(r, 0xf0, 0, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == (DM_BTN_PLUS | DM_BTN_MINUS | DM_BTN_LS | DM_BTN_RS));
    pad_x360_report(&h, r, input(r, 0x01, 0, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == DM_BTN_UP);
    pad_x360_report(&h, r, input(r, 0x0a, 0, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == (DM_BTN_DOWN | DM_BTN_RIGHT));
    pad_x360_report(&h, r, input(r, 0x05, 0, 0, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == (DM_BTN_UP | DM_BTN_LEFT));
    pad_x360_report(&h, r, input(r, 0, 0, 100, 0, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == 0);           // a trigger below half
    pad_x360_report(&h, r, input(r, 0, 0, 200, 255, 0, 0, 0, 0)); CHECK(pad_hid_buttons(&h) == (DM_BTN_ZL | DM_BTN_ZR));
    pad_x360_report(&h, r, input(r, 0, 0, 0, 0, 32767, 32767, -32768, -32768)); pad_hid_sticks(&h, s);
    CHECK(s[0] == DM_STICK_MAX && s[1] == DM_STICK_MAX && s[2] == -DM_STICK_MAX && s[3] == -DM_STICK_MAX);   // y grows upward on both
    pad_x360_report(&h, r, input(r, 0, 0, 0, 0, 6000, -3000, 8000, 1000)); pad_hid_sticks(&h, s);
    CHECK(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0);                // a drifting stick at rest: inside XInput's dead zones
    pad_x360_report(&h, r, input(r, 0, 0, 0, 0, 16000, 0, 0, 0)); pad_hid_sticks(&h, s);
    CHECK(s[0] > 0 && s[0] < DM_STICK_MAX / 2 && s[1] == 0);                // past it: scaled back from the edge
    pad_x360_init(&h, "0.1");
    CHECK(h.dead[0] == 0.1f && h.dead[1] == 0.1f);
    pad_x360_report(&h, r, input(r, 0, 0, 0, 0, 6000, 0, 0, 0)); pad_hid_sticks(&h, s); CHECK(s[0] > 0);
    pad_x360_init(&h, "2"); CHECK(h.dead[0] > 0.23f);                       // out of range: XInput's
    printf("t_pad_x360: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
