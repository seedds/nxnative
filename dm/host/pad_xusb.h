// Xbox 360 (XUSB) pads over USB, read by pad_xusb_mac.h: one 20-byte input report turned into Switch buttons and sticks. Pure, so tests/t_pad_xusb.c checks it without a pad.
// Report: byte 0 is 0x00 (input), byte 1 its length 0x14; byte 2 holds the D-pad (bits 0..3: up, down, left, right), Start (4), Back (5) and the stick clicks (6, 7); byte 3 holds
// LB (0), RB (1), Guide (2), A (4), B (5), X (6), Y (7); bytes 4 and 5 are the left and right triggers (0..255); bytes 6..13 are the left X and Y and the right X and Y, each a
// little-endian int16 with up positive. Other reports (a 3-byte state message at connect, 0x01 0x03 ...) are not input and are skipped.
// Buttons by position, as front_mac.m maps GameController (right A, bottom B, top X, left Y): Xbox B is the Switch's A, Xbox A its B, Xbox Y its X and Xbox X its Y.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pad_hid.h"   // pad_hid_stick: the round dead zone shared with the other HID pads

#define XUSB_REPORT_LEN 20
#define XUSB_TRIGGER_PRESS 128   // a trigger past half presses ZL / ZR, as pad_hid_buttons does for a HID trigger

typedef struct { uint8_t b1, b2, lt, rt; int16_t lx, ly, rx, ry; } pad_xusb;

// 1 when r is an input report and s was filled, 0 for anything else (a short report, a state message)
static int pad_xusb_parse(const uint8_t *r, size_t n, pad_xusb *s) {
    if (n < XUSB_REPORT_LEN || r[0] != 0x00 || r[1] != 0x14) return 0;
    s->b1 = r[2]; s->b2 = r[3]; s->lt = r[4]; s->rt = r[5];
    s->lx = (int16_t)(r[6] | r[7] << 8); s->ly = (int16_t)(r[8] | r[9] << 8);
    s->rx = (int16_t)(r[10] | r[11] << 8); s->ry = (int16_t)(r[12] | r[13] << 8);
    return 1;
}

static uint64_t pad_xusb_buttons(const pad_xusb *s) {
    uint64_t m = 0;
    if (s->b1 & 0x01) m |= DM_BTN_UP;
    if (s->b1 & 0x02) m |= DM_BTN_DOWN;
    if (s->b1 & 0x04) m |= DM_BTN_LEFT;
    if (s->b1 & 0x08) m |= DM_BTN_RIGHT;
    if (s->b1 & 0x10) m |= DM_BTN_PLUS;
    if (s->b1 & 0x20) m |= DM_BTN_MINUS;
    if (s->b1 & 0x40) m |= DM_BTN_LS;
    if (s->b1 & 0x80) m |= DM_BTN_RS;
    if (s->b2 & 0x01) m |= DM_BTN_L;
    if (s->b2 & 0x02) m |= DM_BTN_R;
    if (s->b2 & 0x10) m |= DM_BTN_B;
    if (s->b2 & 0x20) m |= DM_BTN_A;
    if (s->b2 & 0x40) m |= DM_BTN_Y;
    if (s->b2 & 0x80) m |= DM_BTN_X;
    if (s->lt >= XUSB_TRIGGER_PRESS) m |= DM_BTN_ZL;
    if (s->rt >= XUSB_TRIGGER_PRESS) m |= DM_BTN_ZR;
    return m;
}

static float pad_xusb_norm(int16_t v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }
static void pad_xusb_sticks(const pad_xusb *s, int32_t o[4]) {   // lx, ly, rx, ry; both pads and the Switch have up positive
    pad_hid_stick(pad_xusb_norm(s->lx), pad_xusb_norm(s->ly), &o[0], &o[1]);
    pad_hid_stick(pad_xusb_norm(s->rx), pad_xusb_norm(s->ry), &o[2], &o[3]);
}

// Whether a USB device may hold an XUSB interface: the vendor-specific class at the device level (Microsoft's pads, the receiver and most third-party pads). Only such a device is
// opened, so a hub, a disk or a keyboard is never touched
static int pad_xusb_device_ok(int dev_class, int dev_subclass, int dev_protocol) { return dev_class == 0xff && dev_subclass == 0xff && dev_protocol == 0xff; }
static int pad_xusb_interface_ok(int cls, int sub, int pro) { return cls == 0xff && sub == 0x5d && pro == 0x01; }

// Xbox One pads over USB (GIP, the Gaming Input Protocol), read by the same reader: a vendor-specific device of class ff/47/d0 whose control interface (ff/47/d0 with an interrupt IN and an
// interrupt OUT endpoint) must first be sent the power-on command, after which it sends input reports. An input report is command 0x20, a flags byte, a sequence number and the payload
// length, then 14 bytes or more: byte 0 holds Menu (bit 2), View (3), A (4), B (5), X (6), Y (7); byte 1 the D-pad (bits 0..3: up, down, left, right), LB (4), RB (5) and the stick clicks (6, 7);
// the triggers are two little-endian 10-bit values (0..1023) and the sticks four int16 with up positive: left X, left Y, right X, right Y. A report is decoded into the XUSB form above, so
// the buttons, the dead zone and the trigger threshold are the same for both. Other commands (the announce, the status message, the Guide button's 0x07) are not input and are skipped.
// Checked on a Turtle Beach REACT-R (10f5:7024), which sends 36-byte input reports with 32 bytes of payload and needs no acknowledgement.
#define GIP_HEADER_LEN 4
#define GIP_INPUT_MIN 18   // the header and the 14 bytes of an input payload
static const uint8_t pad_gip_power_on[5] = { 0x05, 0x20, 0x00, 0x01, 0x00 };   // command 0x05 (power), flags 0x20 (internal), sequence 0, length 1, payload 0 (on)

static int pad_gip_parse(const uint8_t *r, size_t n, pad_xusb *s) {
    if (n < GIP_INPUT_MIN || r[0] != 0x20 || r[3] < 14 || n < (size_t)GIP_HEADER_LEN + r[3]) return 0;
    const uint8_t *p = r + GIP_HEADER_LEN;
    s->b1 = (uint8_t)((p[0] & 0x04 ? 0x10 : 0) | (p[0] & 0x08 ? 0x20 : 0) | (p[1] & 0x0f) | (p[1] & 0x40) | (p[1] & 0x80));   // Menu is Start, View is Back
    s->b2 = (uint8_t)((p[1] & 0x10 ? 0x01 : 0) | (p[1] & 0x20 ? 0x02 : 0) | (p[0] & 0xf0));                                       // LB, RB, then A, B, X, Y in the XUSB positions
    int lt = (p[2] | p[3] << 8) & 0x3ff, rt = (p[4] | p[5] << 8) & 0x3ff;
    s->lt = (uint8_t)(lt >> 2); s->rt = (uint8_t)(rt >> 2);
    s->lx = (int16_t)(p[6] | p[7] << 8); s->ly = (int16_t)(p[8] | p[9] << 8);
    s->rx = (int16_t)(p[10] | p[11] << 8); s->ry = (int16_t)(p[12] | p[13] << 8);
    return 1;
}
static int pad_gip_device_ok(int dev_class, int dev_subclass, int dev_protocol) { return dev_class == 0xff && dev_subclass == 0x47 && dev_protocol == 0xd0; }
static int pad_gip_interface_ok(int cls, int sub, int pro) { return cls == 0xff && sub == 0x47 && pro == 0xd0; }
