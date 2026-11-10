// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, hat directions, axis scaling, the stick dead zone and analog triggers
#include <stdio.h>
#include "../pad_hid.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    pad_hid h; int32_t s[4];
    pad_hid_init(&h, NULL);
    CHECK(pad_hid_buttons(&h) == 0);
    h.buttons = 1u << 1; CHECK(pad_hid_buttons(&h) == DM_BTN_A);             // usage 2 (right) is A
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_B);             // usage 1 (bottom) is B
    h.buttons = 1u << 11 | 1u << 9; CHECK(pad_hid_buttons(&h) == (DM_BTN_PLUS | DM_BTN_ZR));
    pad_hid_init(&h, "a=1,B=2,bogus=3,ZR=99,X=,MINUS=13");
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);
    h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_MINUS);
    h.buttons = 1u << 9; CHECK(pad_hid_buttons(&h) == DM_BTN_ZR);           // 99 skipped: the default stays
    h.buttons = 1u << 4; CHECK(pad_hid_buttons(&h) == 0);                    // X= maps X to nothing
    pad_hid_init(&h, NULL);
    CHECK(pad_hid_hat(1, 1, 8) == 0 && pad_hid_hat(0, 1, 8) == -1 && pad_hid_hat(8, 0, 7) == -1 && pad_hid_hat(15, 0, 15) == -1);
    h.hat = pad_hid_hat(4, 1, 8); CHECK(pad_hid_buttons(&h) == (DM_BTN_DOWN | DM_BTN_RIGHT));
    CHECK(pad_hid_axis_of(1, 0x30) == PH_X && pad_hid_axis_of(1, 0x35) == PH_RZ && pad_hid_axis_of(2, 0xC5) == PH_BRAKE && pad_hid_axis_of(1, 0x39) == -1);
    CHECK(pad_hid_norm(0, 0, 65535) == -1 && pad_hid_norm(65535, 0, 65535) == 1 && pad_hid_norm(5, 3, 3) == 0);
    h.has[PH_X] = h.has[PH_Y] = h.has[PH_Z] = h.has[PH_RZ] = 1;
    // a pad whose sticks reach their whole range: once pushed to both ends the reach is 1 and the dead zone checks below read as without it
    for (int a = PH_X; a <= PH_RZ; a++) { pad_hid_set_axis(&h, a, 1); pad_hid_set_axis(&h, a, -1); h.axis[a] = 0; }   // back at rest
    CHECK(h.reach[PH_X][0] == 1 && h.reach[PH_X][1] == 1 && h.reach[PH_BRAKE][1] == PAD_HID_REACH0);   // triggers keep their range
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

    // stick reach: the PowerA for Switch 2 ("Lic3 Pro Controller") reaches only X 8,400..56,464 and Y 3,823..58,943 of 0..65,535 (Smash Q115, 27.10.)
    pad_hid_init(&h, NULL); h.has[PH_X] = h.has[PH_Y] = 1;
    float xr = pad_hid_norm(56464, 0, 65535), yu = pad_hid_norm(3823, 0, 65535);   // full right 0.72, full up -0.88
    { pad_hid raw = h; raw.reach[PH_X][1] = raw.reach[PH_Y][0] = 1; raw.axis[PH_X] = xr; raw.axis[PH_Y] = 0; pad_hid_sticks(&raw, s);
      CHECK(s[0] < DM_STICK_MAX * 3 / 4); }                                   // without the reach a full push stays under 3/4: the walk, not the run (the bug)
    pad_hid_set_axis(&h, PH_X, xr); pad_hid_set_axis(&h, PH_Y, 0); pad_hid_sticks(&h, s);
    CHECK(s[0] == DM_STICK_MAX && s[1] == 0);                                // with it: a full push is full
    pad_hid_set_axis(&h, PH_X, 0); pad_hid_set_axis(&h, PH_Y, yu); pad_hid_sticks(&h, s);
    CHECK(s[0] == 0 && s[1] == DM_STICK_MAX);                                // up, its own side's reach (Y up -0.88)
    CHECK(h.reach[PH_X][0] == PAD_HID_REACH0 && h.reach[PH_Y][1] == PAD_HID_REACH0);   // the sides not pushed yet keep the start value
    pad_hid_set_axis(&h, PH_X, xr / 2); pad_hid_set_axis(&h, PH_Y, 0); pad_hid_sticks(&h, s);
    CHECK(s[0] > DM_STICK_MAX / 3 && s[0] < DM_STICK_MAX * 2 / 3);          // half way is about half, not full
    pad_hid_set_axis(&h, PH_X, 0); pad_hid_sticks(&h, s); CHECK(s[0] == 0 && s[1] == 0);   // at rest: 0
    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
