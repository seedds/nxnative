// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, hat directions, axis scaling, the stick dead zone and analog triggers
#include <stdio.h>
#include "../pad_hid.h"
#include "../pad_joycon.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static void pack_stick(uint8_t *p, int x, int y) {
    p[0] = (uint8_t)(x & 0xff);
    p[1] = (uint8_t)(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    p[2] = (uint8_t)((y >> 4) & 0xff);
}

static void test_joycon_split(void) {
    CHECK(joycon_is_pid(0x057e, 0x2006) && joycon_is_pid(0x057e, 0x2007));
    CHECK(!joycon_is_pid(0x057e, 0x2009) && !joycon_is_pid(0x045e, 0x2006));

    joycon_state jl, jr;
    joycon_init(&jl, JOYCON_PID_L);
    joycon_init(&jr, JOYCON_PID_R);
    uint8_t rep[49];
    memset(rep, 0, sizeof rep);
    CHECK(!joycon_decode_0x30(&jl, rep, sizeof rep));   // report id 0x00 rejected
    rep[0] = 0x30;
    CHECK(!joycon_decode_0x30(&jl, rep, 11));           // truncated report rejected

    // Real Joy-Con (L) 0x30 sample captured on macOS (center 1958, 2117, battery 0x4e = not charging, level 2; 0x5e = charging, level 2)
    rep[2] = 0x4e;
    pack_stick(rep + 6, 1958, 2117);
    CHECK(joycon_decode_0x30(&jl, rep, sizeof rep));
    CHECK(jl.calibrated && jl.cx == 1958 && jl.cy == 2117);
    CHECK(jl.charging == 0 && jl.battery == 2);
    rep[2] = 0x5e;
    CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.charging == 1 && jl.battery == 2);
    CHECK(jl.buttons == 0 && jl.sx == 0.0f && jl.sy == 0.0f);

    // Sideways Left Joy-Con buttons: Down->A, Left->B, Right->X, Up->Y, SL->L, SR->R, L->ZL, ZL->ZR, Minus->PLUS, Capture->MINUS, LS->LS
    rep[5] = 0x01; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == DM_BTN_A);
    rep[5] = 0x08; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == DM_BTN_B);
    rep[5] = 0x04; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == DM_BTN_X);
    rep[5] = 0x02; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == DM_BTN_Y);
    rep[5] = 0x20 | 0x10; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == (DM_BTN_L | DM_BTN_R));
    rep[5] = 0x40 | 0x80; CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == (DM_BTN_ZL | DM_BTN_ZR));
    rep[5] = 0; rep[4] = 0x01 | 0x20 | 0x08;
    CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.buttons == (DM_BTN_PLUS | DM_BTN_MINUS | DM_BTN_LS));
    rep[4] = 0;

    // Sideways Left Joy-Con stick rotation (90 deg CCW): physical right (+X) is sideways up (+Y), physical down (-Y) is sideways right (+X)
    pack_stick(rep + 6, 1958 + 1400, 2117);
    CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.sx == 0.0f && jl.sy > 0.99f);
    pack_stick(rep + 6, 1958, 2117 - 1400);
    CHECK(joycon_decode_0x30(&jl, rep, sizeof rep) && jl.sx > 0.99f && jl.sy == 0.0f);

    // Real Joy-Con (R) 0x30 sample captured on macOS (center 2067, 2415, battery 0x80 = full, not charging)
    memset(rep, 0, sizeof rep);
    rep[0] = 0x30; rep[2] = 0x80;
    pack_stick(rep + 9, 2067, 2415);
    CHECK(joycon_decode_0x30(&jr, rep, sizeof rep));
    CHECK(jr.calibrated && jr.cx == 2067 && jr.cy == 2415);
    CHECK(jr.charging == 0 && jr.battery == 4);

    // Sideways Right Joy-Con buttons: X->A, A->B, Y->X, B->Y, SL->L, SR->R, R->ZL, ZR->ZR, Plus->PLUS, RS->LS, Home->home
    rep[3] = 0x02; CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == DM_BTN_A);
    rep[3] = 0x08; CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == DM_BTN_B);
    rep[3] = 0x01; CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == DM_BTN_X);
    rep[3] = 0x04; CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == DM_BTN_Y);
    rep[3] = 0x20 | 0x10; CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == (DM_BTN_L | DM_BTN_R));
    rep[3] = 0; rep[4] = 0x02 | 0x04 | 0x10;
    CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.buttons == (DM_BTN_PLUS | DM_BTN_LS) && jr.home == 1);
    rep[4] = 0;

    // Sideways Right Joy-Con stick rotation (90 deg CW): physical left (-X) is sideways up (+Y), physical up (+Y) is sideways right (+X)
    pack_stick(rep + 9, 2067 - 1400, 2415);
    CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.sx == 0.0f && jr.sy > 0.99f);
    pack_stick(rep + 9, 2067, 2415 + 1400);
    CHECK(joycon_decode_0x30(&jr, rep, sizeof rep) && jr.sx > 0.99f && jr.sy == 0.0f);

    // Sideways 6-axis IMU motion rotation for Left (90 deg CCW) and Right (raw Y/Z inverted + 90 deg CW)
    memset(rep + 13, 0, 36);
    rep[1] = 7;
    // Sample 0: raw_ax = 4096 (1g toward upright top), raw_az = 4096 (1g face-up support on Left, inverted on Right)
    int16_t raw_ax = 4096, raw_az_l = 4096, raw_az_r = -4096;
    memcpy(rep + 13, &raw_ax, 2); memcpy(rep + 17, &raw_az_l, 2);
    pro_motion ml = {0}, mr = {0}; pro_motion_sample msl[3], msr[3];
    CHECK(joycon_motion_decode(&ml, JOYCON_PID_L, rep, sizeof rep, 50000000ULL, msl) == 3);
    // Upright (+raw_ax) gives sy = -1; rotated 90 deg CCW gives sideways acc_x = -sy = +1, acc_z = -1
    CHECK(fabsf(msl[0].acc[0] - 1.0f) < 1e-4f && fabsf(msl[0].acc[1]) < 1e-4f && fabsf(msl[0].acc[2] + 1.0f) < 1e-4f);
    memcpy(rep + 17, &raw_az_r, 2);
    CHECK(joycon_motion_decode(&mr, JOYCON_PID_R, rep, sizeof rep, 50000000ULL, msr) == 3);
    // Right Joy-Con rotated 90 deg CW with inverted raw Z: sideways acc_x = +sy = -1, acc_z = -1
    CHECK(fabsf(msr[0].acc[0] + 1.0f) < 1e-4f && fabsf(msr[0].acc[1]) < 1e-4f && fabsf(msr[0].acc[2] + 1.0f) < 1e-4f);
}

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
    test_joycon_split();
    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
