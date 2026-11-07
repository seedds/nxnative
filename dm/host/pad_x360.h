// The Xbox 360 Wireless Gaming Receiver's controller reports, turned into a raw pad's state (pad_hid.h). Pure, so tests/t_pad_x360.c checks it without a receiver;
// pad_x360_mac.h reads the receiver. Wire format as Linux's xpad.c (and x360w, MIT): byte 0 bit 3 is a presence report, with byte 1 bit 7 the controller present (bit 6 a headset);
// byte 1 == 0x01 with byte 4 == 0x00 is an input report whose bytes from 4 on are the wired controller's: 6 / 7 the buttons (6: D-pad up, down, left, right, Start, Back, left and
// right stick clicks; 7: LB, RB, Guide, -, A, B, X, Y), 8 / 9 the triggers (0..255), then the left and right sticks' x and y as signed 16-bit (y grows upward).
// Buttons by position, as front_mac.m maps GameController (Switch A on the right = 360 B, B at the bottom = 360 A, X on top = 360 Y, Y on the left = 360 X); the triggers
// press ZL / ZR past half (pad_hid_buttons). The sticks' dead zones are XInput's own for this controller (XINPUT_GAMEPAD_LEFT / RIGHT_THUMB_DEADZONE, 7849 and 8689 of 32767):
// an old controller's sticks rest past the raw pads' 12%. A dead zone given as an argument (DM_X360_DEAD) sets both.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pad_hid.h"

enum { X360_A = 1, X360_B, X360_X, X360_Y, X360_LB, X360_RB, X360_BACK, X360_START, X360_L3, X360_R3 };   // the button usages this reader reports (pad_hid bit u - 1)
enum { X360_PRESENT = 1, X360_ABSENT = 2, X360_INPUT = 4 };   // what a report says (pad_x360_report)

static void pad_x360_init(pad_hid *h, const char *dead) {
    pad_hid_init(h, NULL);
    static const uint8_t map[16] = {X360_B, X360_A, X360_Y, X360_X, X360_L3, X360_R3, X360_LB, X360_RB, 0, 0, X360_START, X360_BACK};
    memcpy(h->map, map, sizeof map);
    h->has[PH_X] = h->has[PH_Y] = h->has[PH_RX] = h->has[PH_RY] = h->has[PH_BRAKE] = h->has[PH_ACCEL] = 1;
    h->dead[0] = 7849 / 32767.f; h->dead[1] = 8689 / 32767.f;
    float v = dead && *dead ? strtof(dead, NULL) : -1;
    if (v >= 0 && v < 0.9f) h->dead[0] = h->dead[1] = v;
}
static float pad_x360_axis(const uint8_t *p) { int v = (int16_t)(p[0] | p[1] << 8); return v < -32767 ? -1.f : v / 32767.f; }
// One report from a controller slot's interrupt endpoint: X360_PRESENT / X360_ABSENT for a presence report, X360_INPUT (and the state in h) for an input report, 0 for anything else
static int pad_x360_report(pad_hid *h, const uint8_t *d, size_t n) {
    int r = 0;
    if (n >= 2 && d[0] & 0x08) r |= d[1] & 0x80 ? X360_PRESENT : X360_ABSENT;
    if (n < 18 || d[1] != 0x01 || d[4] != 0x00) return r;
    const uint8_t *p = d + 4; uint8_t b0 = p[2], b1 = p[3];
    static const uint8_t bits0[8] = {0, 0, 0, 0, X360_START, X360_BACK, X360_L3, X360_R3}, bits1[8] = {X360_LB, X360_RB, 0, 0, X360_A, X360_B, X360_X, X360_Y};
    uint32_t b = 0;
    for (int i = 0; i < 8; i++) {
        if (b0 >> i & 1 && bits0[i]) b |= 1u << (bits0[i] - 1);
        if (b1 >> i & 1 && bits1[i]) b |= 1u << (bits1[i] - 1);
    }
    h->buttons = b;
    int x = !!(b0 & 0x08) - !!(b0 & 0x04), y = !!(b0 & 0x02) - !!(b0 & 0x01);   // right - left, down - up
    static const int hat[3][3] = {{7, 0, 1}, {6, -1, 2}, {5, 4, 3}};
    h->hat = hat[y + 1][x + 1];
    h->axis[PH_BRAKE] = p[4] / 127.5f - 1; h->axis[PH_ACCEL] = p[5] / 127.5f - 1;
    h->axis[PH_X] = pad_x360_axis(p + 6); h->axis[PH_Y] = -pad_x360_axis(p + 8);     // HID's y grows downward (pad_hid_sticks turns it back), the 360's upward
    h->axis[PH_RX] = pad_x360_axis(p + 10); h->axis[PH_RY] = -pad_x360_axis(p + 12);
    return r | X360_INPUT;
}
