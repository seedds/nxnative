// Raw HID gamepads (pad_hid.h, GitHub issue #637): the default map and DM_HID_MAP, the Xbox 360 layout of virtual pads (issue #2465), the Switch wired HID layout, report 0x3F's map, raw sticks and Nintendo's descriptor (PR #989), hat directions, axis scaling, the stick dead zone and analog triggers
#include <stdio.h>
#include "../pad_hid.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    pad_hid h; int32_t s[4];
    pad_hid_init(&h, NULL, PAD_HID_DEFAULT);
    CHECK(pad_hid_buttons(&h) == 0);
    h.buttons = 1u << 1; CHECK(pad_hid_buttons(&h) == DM_BTN_A);             // usage 2 (right) is A
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_B);             // usage 1 (bottom) is B
    h.buttons = 1u << 11 | 1u << 9; CHECK(pad_hid_buttons(&h) == (DM_BTN_PLUS | DM_BTN_ZR));
    pad_hid_init(&h, "a=1,B=2,bogus=3,ZR=99,X=,MINUS=13", PAD_HID_DEFAULT);
    h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);
    h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_MINUS);
    h.buttons = 1u << 9; CHECK(pad_hid_buttons(&h) == DM_BTN_ZR);           // 99 skipped: the default stays
    h.buttons = 1u << 4; CHECK(pad_hid_buttons(&h) == 0);                    // X= maps X to nothing
    pad_hid_init(&h, NULL, PAD_HID_NINTENDO);                               // Nintendo's report 0x3F: B A Y X L R ZL ZR - + LS RS
    { static const uint64_t want[12] = {DM_BTN_B, DM_BTN_A, DM_BTN_Y, DM_BTN_X, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_MINUS, DM_BTN_PLUS, DM_BTN_LS, DM_BTN_RS};
      for (int b = 0; b < 12; b++) { h.buttons = 1u << b; CHECK(pad_hid_buttons(&h) == want[b]); } }
    h.buttons = 1u << 12 | 1u << 13; CHECK(pad_hid_buttons(&h) == 0);        // Home and Capture are not Npad buttons here
    pad_hid_init(&h, "X=13", PAD_HID_NINTENDO); h.buttons = 1u << 12; CHECK(pad_hid_buttons(&h) == DM_BTN_X);   // DM_HID_MAP still replaces entries
    // The same physical buttons keep their mapping when report 0x3F changes to 0x30, including Home/Capture overrides and the hat.
    pad_hid_init(&h, NULL, PAD_HID_NINTENDO);
    for (int b = 0; b < 12; b++) CHECK(pad_hid_nin_buttons(&h, 1ull << b, 0) == (1ull << b));
    pad_hid_init(&h, "A=1,B=2,X=13,Y=14,L=0,RIGHT=12", PAD_HID_NINTENDO);
    h.buttons = 1u << 1 | 1u << 4 | 1u << 11 | 1u << 12 | 1u << 13; h.hat = 0;
    CHECK(pad_hid_buttons(&h) == (DM_BTN_B | DM_BTN_RS | DM_BTN_X | DM_BTN_Y | DM_BTN_RIGHT | DM_BTN_UP));
    CHECK(pad_hid_nin_buttons(&h, DM_BTN_A | DM_BTN_L | DM_BTN_RS | DM_BTN_UP, 0x30) == pad_hid_buttons(&h));
    h.hat = 4; h.has[PH_ACCEL] = 1; h.axis[PH_ACCEL] = 1;
    CHECK(pad_hid_nin_buttons(&h, 0, 0) == 0);   // no stale simple-report hat or analog trigger in full mode
    pad_hid_init(&h, NULL, PAD_HID_NINTENDO); CHECK(h.layout == PAD_HID_NINTENDO);   // a raw stick, as the NANO's: rests at -0.08, reaches 0.78 either way
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.27f) - 0.5f) < 1e-5f);          // half of the first reach
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, 0.70f) - 1) < 1e-5f && fabsf(pad_hid_raw_axis(&h, PH_X, 0.31f) - 0.5f) < 1e-5f);   // past it the reach grows
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.78f) + 1) < 1e-5f);            // full on both sides, though the rest is off centre
    CHECK(fabsf(pad_hid_raw_axis(&h, PH_X, -0.08f)) < 1e-6f);
    CHECK(pad_hid_raw_axis(&h, PH_Y, 0.9f) == 1 && h.rest[PH_Y] == 0);       // held while connecting: not a rest
    pad_hid_init(&h, NULL, PAD_HID_DEFAULT); CHECK(h.layout == PAD_HID_DEFAULT);
    // Nintendo's descriptor, by its reports: buttons in 0x3F, the replies 0x21, the full report 0x30 and the subcommand output 0x01 make the protocol; 0x10 is the rumble
    { int bits = pad_hid_nin_bit(0, 0x3F, 9) | pad_hid_nin_bit(0, 0x21, 0xFF00) | pad_hid_nin_bit(0, 0x30, 0xFF00) | pad_hid_nin_bit(1, 0x01, 0xFF00);
      CHECK(pad_hid_nin_protocol(bits) && !(bits & PAD_NIN_RUMBLE) && pad_hid_nin_bit(1, 0x10, 0xFF00) == PAD_NIN_RUMBLE);
      CHECK(!pad_hid_nin_protocol(bits & ~PAD_NIN_FULL) && !pad_hid_nin_protocol(bits & ~PAD_NIN_SUBCMD) && !pad_hid_nin_protocol(bits & ~PAD_NIN_SIMPLE));
      CHECK(pad_hid_nin_lights(bits) && pad_hid_nin_lights(bits & ~PAD_NIN_FULL));
      CHECK(!pad_hid_nin_lights(bits & ~PAD_NIN_SUBCMD) && !pad_hid_nin_lights(PAD_NIN_SIMPLE));
      CHECK(pad_hid_nin_bit(0, 0x3F, 1) == 0 && pad_hid_nin_bit(0, 1, 9) == 0 && pad_hid_nin_bit(1, 0x30, 0xFF00) == 0 && pad_hid_nin_bit(0, 0x01, 9) == 0);   // an 8BitDo's buttons in report 1
    }
    pad_hid_init(&h, NULL, PAD_HID_DEFAULT);
    CHECK(pad_hid_hat(1, 1, 8) == 0 && pad_hid_hat(0, 1, 8) == -1 && pad_hid_hat(8, 0, 7) == -1 && pad_hid_hat(15, 0, 15) == -1);
    { static const uint64_t dir[8] = {DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT, DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN,   // the hat's eight values, clockwise from up
                                      DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP};
      for (int v = 0; v < 8; v++) { h.hat = v; CHECK(pad_hid_buttons(&h) == dir[v]); } }
    h.hat = pad_hid_hat(4, 1, 8); CHECK(pad_hid_buttons(&h) == (DM_BTN_DOWN | DM_BTN_RIGHT));
    CHECK(pad_hid_axis_of(1, 0x30) == PH_X && pad_hid_axis_of(1, 0x35) == PH_RZ && pad_hid_axis_of(2, 0xC5) == PH_BRAKE && pad_hid_axis_of(1, 0x39) == -1);
    CHECK(pad_hid_norm(0, 0, 65535) == -1 && pad_hid_norm(65535, 0, 65535) == 1 && pad_hid_norm(5, 3, 3) == 0);
    // Wrapped unsigned ranges (Logical Max encoded as signed -1, exposed as -1..0 or 0..-1) recover their unsigned bits by report width.
    CHECK(fabsf(pad_hid_norm_bits(0x7fff, -1, 0, 16)) < 0.001f);
    CHECK(pad_hid_norm_bits(-32768, -1, 0, 16) > 0 && pad_hid_norm_bits(-32768, -1, 0, 16) < 0.001f);
    CHECK(pad_hid_norm_bits(0, -1, 0, 16) == -1 && pad_hid_norm_bits(-1, -1, 0, 16) == 1 && pad_hid_norm_bits(65535, -1, 0, 16) == 1);
    CHECK(pad_hid_norm_bits(0, 0, -1, 16) == -1 && pad_hid_norm_bits(65535, 0, -1, 16) == 1);
    CHECK(pad_hid_norm_bits(0, -1, 0, 8) == -1 && fabsf(pad_hid_norm_bits(127, -1, 0, 8)) < 0.01f && pad_hid_norm_bits(-1, -1, 0, 8) == 1);
    CHECK(pad_hid_norm_bits(0, -1, 0, 1) == 1 && pad_hid_norm_bits(0, -1, 0, 0) == 1 && pad_hid_norm_bits(0, -1, 0, 32) == 1);
    CHECK(pad_hid_norm_bits(127, 0, 254, 16) == 0 && pad_hid_norm_bits(0, -32768, 32767, 16) > 0);
    // Replay the measured SHIELD 2015 16-bit trigger sweep and stick extremes through pad_hid_norm_bits.
    { static const long sweep[] = {0, 5305, 16397, 26314, 44624, 65535, 20445, 0};
      pad_hid sh; int32_t ss[4];
      pad_hid_init(&sh, NULL, PAD_HID_DEFAULT);
      static const int axes[4] = {PH_X, PH_Y, PH_Z, PH_RZ};
      for (int i = 0; i < 4; i++) { sh.has[axes[i]] = 1; sh.axis[axes[i]] = pad_hid_norm_bits(0x7fff, -1, 0, 16); }
      pad_hid_sticks(&sh, ss);
      CHECK(ss[0] == 0 && ss[1] == 0 && ss[2] == 0 && ss[3] == 0);
      for (int i = 0; i < 4; i++) {
          int a = axes[i];
          sh.axis[a] = pad_hid_norm_bits(-1, -1, 0, 16); pad_hid_sticks(&sh, ss);
          CHECK(ss[i] == (i % 2 ? -DM_STICK_MAX : DM_STICK_MAX));
          sh.axis[a] = pad_hid_norm_bits(0, -1, 0, 16); pad_hid_sticks(&sh, ss);
          CHECK(ss[i] == (i % 2 ? DM_STICK_MAX : -DM_STICK_MAX));
          sh.axis[a] = pad_hid_norm_bits(0x7fff, -1, 0, 16);
      }
      for (int a = PH_BRAKE; a <= PH_ACCEL; a++) {
          sh.has[a] = 1;
          for (unsigned i = 0; i < sizeof sweep / sizeof *sweep; i++) {
              sh.axis[a] = pad_hid_norm_bits(sweep[i], -1, 0, 16);
              uint64_t bit = a == PH_BRAKE ? DM_BTN_ZL : DM_BTN_ZR;
              CHECK(!!(pad_hid_buttons(&sh) & bit) == (sweep[i] > 32767));
          }
      }
      sh.ac_back = 1; CHECK(pad_hid_buttons(&sh) == DM_BTN_MINUS);
      sh.buttons = 1u << 11; CHECK(pad_hid_buttons(&sh) == (DM_BTN_PLUS | DM_BTN_MINUS));
      sh.ac_back = 0; CHECK(pad_hid_buttons(&sh) == DM_BTN_PLUS);
      sh.buttons = 0; sh.ac_back = 1; CHECK(pad_hid_nin_buttons(&sh, 0, 0) == 0);   // full report 0x30 ignores simple-mode ac_back
    }
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
    // The Xbox 360 layout (a virtual 045e:028e pad read as HID, Steam Link's GamePad-N, issue #2465): one-at-a-time presses of a GameSir G8 through Steam Link as macOS
    // delivered them: bottom, right, top, left, back, start, LB, RB, then the d-pad up, right, down, left
    uint8_t has[PH_AXES] = {[PH_X] = 1, [PH_Y] = 1, [PH_Z] = 1, [PH_RX] = 1, [PH_RY] = 1, [PH_RZ] = 1};
    CHECK(pad_hid_layout_for(0, 0x045e, 0x028e, has) == PAD_HID_X360);
    CHECK(pad_hid_layout_for(0, 0x045e, 0x02ea, has) == PAD_HID_DEFAULT);
    CHECK(pad_hid_layout_for(0, 0x054c, 0x028e, has) == PAD_HID_DEFAULT);
    CHECK(pad_hid_layout_for(PAD_NIN_SIMPLE, 0x045e, 0x028e, has) == PAD_HID_NINTENDO);
    has[PH_RX] = 0; CHECK(pad_hid_layout_for(0, 0x045e, 0x028e, has) == PAD_HID_DEFAULT);
    has[PH_RX] = 1; has[PH_ACCEL] = 1; CHECK(pad_hid_layout_for(0, 0x045e, 0x028e, has) == PAD_HID_DEFAULT);
    pad_hid_init(&h, NULL, PAD_HID_X360);
    { static const unsigned usage[] = {1, 2, 4, 3, 10, 9, 5, 6, 12, 15, 13, 14};
      static const uint64_t want[] = {DM_BTN_B, DM_BTN_A, DM_BTN_X, DM_BTN_Y, DM_BTN_MINUS, DM_BTN_PLUS, DM_BTN_L, DM_BTN_R, DM_BTN_UP, DM_BTN_RIGHT, DM_BTN_DOWN, DM_BTN_LEFT};
      for (unsigned i = 0; i < sizeof usage / sizeof *usage; i++) { h.buttons = 1u << (usage[i] - 1); CHECK(pad_hid_buttons(&h) == want[i]); } }
    h.buttons = 1u << 6 | 1u << 7; CHECK(pad_hid_buttons(&h) == (DM_BTN_LS | DM_BTN_RS));
    h.buttons = 0;
    CHECK(pad_hid_axis_for(PAD_HID_X360, PH_Z) == PH_BRAKE && pad_hid_axis_for(PAD_HID_X360, PH_RZ) == PH_ACCEL);
    CHECK(pad_hid_axis_for(PAD_HID_X360, PH_RX) == PH_RX && pad_hid_axis_for(PAD_HID_X360, PH_X) == PH_X);
    CHECK(pad_hid_axis_for(PAD_HID_DEFAULT, PH_Z) == PH_Z && pad_hid_axis_for(PAD_HID_DEFAULT, PH_RZ) == PH_RZ);
    h.has[PH_X] = h.has[PH_Y] = h.has[PH_RX] = h.has[PH_RY] = h.has[PH_BRAKE] = h.has[PH_ACCEL] = 1;   // the axes hidpad_found sees on it: no Z
    h.axis[PH_X] = pad_hid_norm(16384, -32768, 32767); h.axis[PH_Y] = pad_hid_norm(-32768, -32768, 32767);
    h.axis[PH_RX] = pad_hid_norm(-32768, -32768, 32767); h.axis[PH_RY] = pad_hid_norm(0, -32768, 32767);
    h.axis[PH_BRAKE] = pad_hid_norm(255, 0, 255); h.axis[PH_ACCEL] = pad_hid_norm(0, 0, 255);
    pad_hid_sticks(&h, s);
    CHECK(s[0] > DM_STICK_MAX / 3 && s[0] < DM_STICK_MAX * 2 / 3 && s[1] == DM_STICK_MAX);   // a half push stays a half push
    CHECK(s[2] == -DM_STICK_MAX && abs(s[3]) < 2);                                          // the right stick from Rx / Ry
    CHECK(pad_hid_buttons(&h) == DM_BTN_ZL);                                                // left trigger down, right one up
    pad_hid_init(&h, "A=1,B=2", PAD_HID_X360); h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);   // DM_HID_MAP still replaces entries
    // The Switch wired HID map needs both halves of each verified USB ID. Nintendo report 0x3F takes precedence.
    uint8_t wired[PH_AXES] = {[PH_X] = 1, [PH_Y] = 1, [PH_Z] = 1, [PH_RZ] = 1};
    CHECK(pad_hid_layout_for(0, 0x0f0d, 0x00f0, wired) == PAD_HID_SWITCH_WIRED);
    CHECK(pad_hid_layout_for(0, 0x0f0d, 0x011c, wired) == PAD_HID_DEFAULT);
    CHECK(pad_hid_layout_for(0, 0x2dc8, 0x00f0, wired) == PAD_HID_DEFAULT);
    CHECK(pad_hid_layout_for(PAD_NIN_SIMPLE, 0x0f0d, 0x00f0, wired) == PAD_HID_NINTENDO);
    pad_hid_init(&h, NULL, PAD_HID_SWITCH_WIRED);
    { static const uint64_t want[] = {DM_BTN_Y, DM_BTN_B, DM_BTN_A, DM_BTN_X, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR, DM_BTN_MINUS, DM_BTN_PLUS, DM_BTN_LS, DM_BTN_RS, 0, 0};
      for (unsigned i = 0; i < sizeof want / sizeof *want; i++) { h.buttons = 1u << i; CHECK(pad_hid_buttons(&h) == want[i]); } }
    pad_hid_init(&h, "A=1,Y=3", PAD_HID_SWITCH_WIRED); h.buttons = 1u << 0; CHECK(pad_hid_buttons(&h) == DM_BTN_A);   // DM_HID_MAP still replaces entries
    // PDP's Wired Fight Pad Pro for Switch (0e6f:0185, GitHub issue #3290) uses the same layout: its reporter measured usages 1 Y, 2 B, 3 A, 4 X, 5 L, 6 R. Non-Switch PDP IDs keep the default.
    CHECK(pad_hid_layout_for(0, 0x0e6f, 0x0185, wired) == PAD_HID_SWITCH_WIRED);
    CHECK(pad_hid_layout_for(0, 0x0e6f, 0x02a8, wired) == PAD_HID_DEFAULT);
    CHECK(pad_hid_layout_for(PAD_NIN_SIMPLE, 0x0e6f, 0x0185, wired) == PAD_HID_NINTENDO);
    pad_hid_init(&h, NULL, pad_hid_layout_for(0, 0x0e6f, 0x0185, wired));
    { static const uint64_t want[] = {DM_BTN_Y, DM_BTN_B, DM_BTN_A, DM_BTN_X, DM_BTN_L, DM_BTN_R};
      for (unsigned i = 0; i < sizeof want / sizeof *want; i++) { h.buttons = 1u << i; CHECK(pad_hid_buttons(&h) == want[i]); } }
    // identical pads: a model is read here only with two or more pads and more than GameController lists, after the wait, and stops at once
    CHECK(pad_dupes_want(1, 0, 0, 1) == 0); CHECK(pad_dupes_want(2, 2, 1, 0) == 0); CHECK(pad_dupes_want(3, 3, 0, 1) == 0);
    CHECK(pad_dupes_want(2, 1, 0, 0) == -1); CHECK(pad_dupes_want(2, 1, 0, 1) == 1); CHECK(pad_dupes_want(2, 0, 0, 1) == 1);
    CHECK(pad_dupes_want(3, 1, 1, 0) == 1);   // a third pad of a model read already joins it without a wait
    CHECK(pad_dupes_want(1, 1, 1, 0) == 0);   // one unplugged: back to GameController

    printf("t_pad_hid: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
