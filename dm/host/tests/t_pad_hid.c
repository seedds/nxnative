// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, hat directions, axis scaling, the stick dead zone and analog triggers
#include <stdio.h>
#include "../pad_hid.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    pad_hid h; int32_t s[4];
    pad_hid_init(&h, NULL, 0);
    CHECK(pad_hid_buttons(&h) == 0);
    h.buttons = 1u << 1; CHECK(pad_hid_buttons(&h) == DM_BTN_A);             // usage 2 (right) is A
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_B);             // usage 1 (bottom) is B
    h.buttons = 1u << 11 | 1u << 9; CHECK(pad_hid_buttons(&h) == (DM_BTN_PLUS | DM_BTN_ZR));
    pad_hid_init(&h, "a=1,B=2,bogus=3,ZR=99,X=,MINUS=13", 0);
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);
    h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_MINUS);
    h.buttons = 1u << 9; CHECK(pad_hid_buttons(&h) == DM_BTN_ZR);           // 99 skipped: the default stays
    h.buttons = 1u << 4; CHECK(pad_hid_buttons(&h) == 0);                    // X= maps X to nothing
    pad_hid_init(&h, NULL, 1);                                               // Nintendo's report 0x3F: B A Y X L R ZL ZR - + LS RS
    { static const uint64_t want[12] = {DM_BTN_B, DM_BTN_A, DM_BTN_Y, DM_BTN_X, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_MINUS, DM_BTN_PLUS, DM_BTN_LS, DM_BTN_RS};
      for (int b = 0; b < 12; b++) { h.buttons = 1u << b; CHECK(pad_hid_buttons(&h) == want[b]); } }
    h.buttons = 1u << 12 | 1u << 13; CHECK(pad_hid_buttons(&h) == 0);        // Home and Capture are not Npad buttons here
    pad_hid_init(&h, "X=13", 1); h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_X);   // DM_HID_MAP still replaces entries
    pad_hid_init(&h, NULL, 1);                                               // a raw stick, as the NANO's: rests at -0.08, reaches 0.78 either way
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.27f) - 0.5f) < 1e-5f);          // half of the first reach
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.70f) - 1) < 1e-5f && fabsf(pad_hid_raw_axis(&h, PH_X, 0.31f) - 0.5f) < 1e-5f);   // past it the reach grows
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.78f) + 1) < 1e-5f);            // full on both sides, though the rest is off centre
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(pad_hid_raw_axis(&h, PH_Y, 0.9f) == 1 && h.rest[PH_Y] == 0);       // held while connecting: not a rest
    pad_hid_init(&h, NULL, 0); CHECK(!h.raw);
    { uint8_t r[PAD_HID_LIGHTS_SIZE]; pad_hid_lights(r, 0); CHECK(r[0] == 0x01 && r[2] == 0x00 && r[3] == 0x01 && r[9] == 0x40 && r[10] == 0x30 && r[11] == 0x1 && r[12] == 0 && r[48] == 0);
      pad_hid_lights(r, 3); CHECK(r[11] == 0xF); pad_hid_lights(r, 7); CHECK(r[11] == 0x6); }   // players 1, 4 and 8, as the console lights them
    pad_hid_init(&h, NULL, 0);
    CHECK(pad_hid_hat(1, 1, 8) == 0 && pad_hid_hat(0, 1, 8) == -1 && pad_hid_hat(8, 0, 7) == -1 && pad_hid_hat(15, 0, 15) == -1);
    h.hat = pad_hid_hat(4, 1, 8); CHECK(pad_hid_buttons(&h) == (DM_BTN_DOWN | DM_BTN_RIGHT));
    CHECK(pad_hid_axis_of(1, 0x30) == PH_X && pad_hid_axis_of(1, 0x35) == PH_RZ && pad_hid_axis_of(2, 0xC5) == PH_BRAKE && pad_hid_axis_of(1, 0x39) == -1);
    CHECK(pad_hid_norm(0, 0, 65535) == -1 && pad_hid_norm(65535, 0, 65535) == 1 && pad_hid_norm(5, 3, 3) == 0);
    h.has[PH_X] = h.has[PH_Y] = h.has[PH_Z] = h.has[PH_RZ] = 1;
    h.axis[PH_X] = 1; h.axis[PH_Y] = -1; h.axis[PH_Z] = 0.05f; h.axis[PH_RZ] = 0.04f;
    pad_hid_sticks(&h, s);
    CHECK(s[0] == DM_STICK_MAX && s[1] == DM_STICK_MAX);                     // Y up; a square-gated corner reaches full x and y
    CHECK(s[2] == 0 && s[3] == 0);                                           // inside the round dead zone
    h.axis[PH_RZ] = 0.5f; pad_hid_sticks(&h, s);
    CHECK(s[2] > 0 && s[3] < 0 && s[3] > -DM_STICK_MAX / 2);                 // past it: the small x stays, the deflection is scaled back
    h.axis[PH_X] = 0.13f; h.axis[PH_Y] = 0; pad_hid_sticks(&h, s);
    CHECK(s[0] > 0 && s[0] < DM_STICK_MAX / 50 && s[1] == 0);                // just past the edge: near 0, no jump
    h.has[PH_Z] = 0; h.has[PH_RX] = 1; h.axis[PH_RX] = -1; pad_hid_sticks(&h, s); CHECK(s[2] == -DM_STICK_MAX && s[3] == 0);   // Rx / Ry without Z
    h.has[PH_ACCEL] = 1; h.axis[PH_ACCEL] = -1; CHECK(!(pad_hid_buttons(&h) & DM_BTN_ZR));
    h.axis[PH_ACCEL] = pad_hid_norm(200, 0, 255); CHECK(pad_hid_buttons(&h) & DM_BTN_ZR);
    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
