// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, report 0x3F's map, raw sticks and Nintendo's descriptor (PR #989), hat directions, axis scaling, the stick dead zone and analog triggers
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
    // The same physical buttons keep their mapping when report 0x3F changes to 0x30, including Home/Capture overrides and the hat.
    pad_hid_init(&h, NULL, 1);
    for (int b = 0; b < 12; b++) CHECK(pad_hid_nin_buttons(&h, 1ull << b, 0) == (1ull << b));
    pad_hid_init(&h, "A=1,B=2,X=13,Y=14,L=0,RIGHT=12", 1);
    h.buttons = 1u << 1 | 1u << 4 | 1u << 11 | 1u << 12 | 1u << 13; h.hat = 0;
    CHECK(pad_hid_buttons(&h) == (DM_BTN_B | DM_BTN_RS | DM_BTN_X | DM_BTN_Y | DM_BTN_RIGHT | DM_BTN_UP));
    CHECK(pad_hid_nin_buttons(&h, DM_BTN_A | DM_BTN_L | DM_BTN_RS | DM_BTN_UP, 0x30) == pad_hid_buttons(&h));
    h.hat = 4; h.has[PH_ACCEL] = 1; h.axis[PH_ACCEL] = 1;
    CHECK(pad_hid_nin_buttons(&h, 0, 0) == 0);   // no stale simple-report hat or analog trigger in full mode
    pad_hid_init(&h, NULL, 1); CHECK(h.raw);                                 // a raw stick, as the NANO's: rests at -0.08, reaches 0.78 either way
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.27f) - 0.5f) < 1e-5f);          // half of the first reach
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.70f) - 1) < 1e-5f && fabsf(pad_hid_raw_axis(&h, PH_X, 0.31f) - 0.5f) < 1e-5f);   // past it the reach grows
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.78f) + 1) < 1e-5f);            // full on both sides, though the rest is off centre
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(pad_hid_raw_axis(&h, PH_Y, 0.9f) == 1 && h.rest[PH_Y] == 0);       // held while connecting: not a rest
    pad_hid_init(&h, NULL, 0); CHECK(!h.raw);
    // Nintendo's descriptor, by its reports: buttons in 0x3F, the replies 0x21, the full report 0x30 and the subcommand output 0x01 make the protocol; 0x10 is the rumble
    { int bits = pad_hid_nin_bit(0, 0x3F, 9) | pad_hid_nin_bit(0, 0x21, 0xFF00) | pad_hid_nin_bit(0, 0x30, 0xFF00) | pad_hid_nin_bit(1, 0x01, 0xFF00);
      CHECK(pad_hid_nin_protocol(bits) && !(bits & PAD_NIN_RUMBLE) && pad_hid_nin_bit(1, 0x10, 0xFF00) == PAD_NIN_RUMBLE);
      CHECK(!pad_hid_nin_protocol(bits & ~PAD_NIN_FULL) && !pad_hid_nin_protocol(bits & ~PAD_NIN_SUBCMD) && !pad_hid_nin_protocol(bits & ~PAD_NIN_SIMPLE));
      CHECK(pad_hid_nin_lights(bits) && pad_hid_nin_lights(bits & ~PAD_NIN_FULL));
      CHECK(!pad_hid_nin_lights(bits & ~PAD_NIN_SUBCMD) && !pad_hid_nin_lights(PAD_NIN_SIMPLE));
      CHECK(pad_hid_nin_bit(0, 0x3F, 1) == 0 && pad_hid_nin_bit(0, 1, 9) == 0 && pad_hid_nin_bit(1, 0x30, 0xFF00) == 0 && pad_hid_nin_bit(0, 0x01, 9) == 0);   // an 8BitDo's buttons in report 1
    }
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
    // identical pads: a model is read here only with two or more pads and more than GameController lists, after the wait, and stops at once
    CHECK(pad_dupes_want(1, 0, 0, 1) == 0); CHECK(pad_dupes_want(2, 2, 1, 0) == 0); CHECK(pad_dupes_want(3, 3, 0, 1) == 0);
    CHECK(pad_dupes_want(2, 1, 0, 0) == -1); CHECK(pad_dupes_want(2, 1, 0, 1) == 1); CHECK(pad_dupes_want(2, 0, 0, 1) == 1);
    CHECK(pad_dupes_want(3, 1, 1, 0) == 1);   // a third pad of a model read already joins it without a wait
    CHECK(pad_dupes_want(1, 1, 1, 0) == 0);   // one unplugged: back to GameController

    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
