// A Switch Pro Controller's input report 0x30 as Switch buttons and sticks, for a pad GameController does not list (two identical pads over USB: pad_hid_mac.h's twins).
// Pure, so tests/t_pad_pro.c checks it without a controller. Layout: dekuNukem/Nintendo_Switch_Reverse_Engineering, bluetooth_hid_notes.md: byte 3 right buttons, 4 shared, 5 left,
// 6-8 left stick, 9-11 right stick, two 12-bit values in three bytes, and y grows upwards as in dm_input.h. Buttons by their labels, which on this pad are the Switch's positions.
// Sticks: the pad's own calibration, read from its SPI flash with subcommand 0x10 (spi_flash_notes.md: the user's at 0x8010 when its magic is there, else the factory's
// at 0x603D), gives each axis its rest and its reach below and above. Until it answers, or if it never does, each axis measures itself: its rest is the first report's value
// when that is near the middle (a stick held at that moment is off until the pad is plugged in again), and each reach starts low and grows with what the stick shows.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dm_input.h"

enum { PAD_PRO_MIDDLE = 2048, PAD_PRO_REST_MAX = 500, PAD_PRO_REACH_MIN = 1200, PAD_PRO_DEAD_PCT = 7 };
typedef struct { int seen, cal[2], rest[4], reach[4][2]; } pad_pro;   // cal: the stick's calibration came from the pad (1 factory, 2 user); reach: below, above the rest
enum { PAD_PRO_SPI_USER = 0x8010, PAD_PRO_SPI_USER_LEN = 22, PAD_PRO_SPI_FACTORY = 0x603D, PAD_PRO_SPI_FACTORY_LEN = 18 };

// Output report 0x01 with a neutral rumble and subcommand 0x10, reading len bytes of SPI flash at addr. Returns its length
static inline size_t pad_pro_spi_read(uint8_t out[16], uint8_t counter, uint32_t addr, uint8_t len) {
    static const uint8_t rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    out[0] = 0x01; out[1] = counter & 0x0f;
    for (int i = 0; i < 8; i++) out[2 + i] = rumble[i];
    out[10] = 0x10; out[11] = addr & 0xff; out[12] = addr >> 8 & 0xff; out[13] = addr >> 16 & 0xff; out[14] = addr >> 24 & 0xff; out[15] = len;
    return 16;
}
// One stick's 9 calibration bytes, as x and y of 12 bits each: the left stick's order is above, center, below; the right stick's center, below, above
static inline void pad_pro_cal_decode(const uint8_t *d, int right, int c[6]) {   // c: x and y rest, below, above
    int v[6];
    for (int i = 0; i < 3; i++) { v[2 * i] = (d[3 * i + 1] << 8 & 0xf00) | d[3 * i]; v[2 * i + 1] = d[3 * i + 2] << 4 | d[3 * i + 1] >> 4; }
    static const int left[3] = {1, 2, 0}, rightorder[3] = {0, 1, 2};   // where rest, below and above sit
    const int *o = right ? rightorder : left;
    for (int k = 0; k < 3; k++) { c[2 * k] = v[2 * o[k]]; c[2 * k + 1] = v[2 * o[k] + 1]; }
}
static inline int pad_pro_cal_sane(const int c[6]) {
    for (int a = 0; a < 2; a++) {
        if (abs(c[a] - PAD_PRO_MIDDLE) > PAD_PRO_REST_MAX) return 0;
        for (int k = 1; k < 3; k++) if (c[2 * k + a] < 400 || c[2 * k + a] > PAD_PRO_MIDDLE) return 0;
    }
    return 1;
}
// A reply to pad_pro_spi_read (report 0x21: ack at 13, subcommand 0x10 at 14, address at 15, length at 19, data from 20) sets the sticks it holds a sane calibration for.
// The user's wins over the factory's. Returns the sticks it set as bits (1 left, 2 right), 0 for any other report
static inline int pad_pro_spi_reply(pad_pro *s, const uint8_t *r, size_t len) {
    if (len < 20 || r[0] != 0x21 || !(r[13] & 0x80) || r[14] != 0x10) return 0;
    uint32_t addr = r[15] | r[16] << 8 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 24; size_t n = r[19]; const uint8_t *d = r + 20;
    if (len < 20 + n) return 0;
    int set = 0;
    for (int stick = 0; stick < 2; stick++) {
        const uint8_t *at = NULL; int kind = 0;
        if (addr == PAD_PRO_SPI_USER && n >= PAD_PRO_SPI_USER_LEN && d[11 * stick] == 0xb2 && d[11 * stick + 1] == 0xa1) { at = d + 11 * stick + 2; kind = 2; }
        if (addr == PAD_PRO_SPI_FACTORY && n >= PAD_PRO_SPI_FACTORY_LEN && s->cal[stick] < 2) { at = d + 9 * stick; kind = 1; }
        if (!at) continue;
        int c[6]; pad_pro_cal_decode(at, stick, c); if (!pad_pro_cal_sane(c)) continue;
        for (int a = 0; a < 2; a++) { s->rest[2 * stick + a] = c[a]; s->reach[2 * stick + a][0] = c[2 + a]; s->reach[2 * stick + a][1] = c[4 + a]; }
        s->cal[stick] = kind; set |= 1 << stick;
    }
    if (set) s->seen = 1;   // no first-report rest over it
    return set;
}

static inline uint64_t pad_pro_buttons(const uint8_t *r) {
    static const struct { uint8_t at, bit; uint32_t btn; } map[] = {
        {3, 0x01, DM_BTN_Y}, {3, 0x02, DM_BTN_X}, {3, 0x04, DM_BTN_B}, {3, 0x08, DM_BTN_A}, {3, 0x40, DM_BTN_R}, {3, 0x80, DM_BTN_ZR},
        {4, 0x01, DM_BTN_MINUS}, {4, 0x02, DM_BTN_PLUS}, {4, 0x04, DM_BTN_RS}, {4, 0x08, DM_BTN_LS},
        {5, 0x01, DM_BTN_DOWN}, {5, 0x02, DM_BTN_UP}, {5, 0x04, DM_BTN_RIGHT}, {5, 0x08, DM_BTN_LEFT}, {5, 0x40, DM_BTN_L}, {5, 0x80, DM_BTN_ZL},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (r[map[i].at] & map[i].bit) m |= map[i].btn;
    return m;
}
static inline int32_t pad_pro_axis(pad_pro *s, int a, int raw) {
    int v = raw - s->rest[a], mag = abs(v), *reach = &s->reach[a][v > 0];
    if (mag > *reach && !s->cal[a / 2]) *reach = mag;   // a calibrated reach stays, and past it reads full
    int dead = *reach * PAD_PRO_DEAD_PCT / 100;
    if (mag <= dead) return 0;
    int64_t out = (int64_t)(mag - dead) * DM_STICK_MAX / (*reach - dead); if (out > DM_STICK_MAX) out = DM_STICK_MAX;
    return v < 0 ? -(int32_t)out : (int32_t)out;
}
// A report of the right kind fills *m and st (left x, y, right x, y) and returns 1; anything else leaves them and returns 0
static inline int pad_pro_decode(pad_pro *s, const uint8_t *r, size_t len, uint64_t *m, int32_t st[4]) {
    if (len < 12 || r[0] != 0x30) return 0;
    const int raw[4] = {r[6] | (r[7] & 0x0f) << 8, r[7] >> 4 | r[8] << 4, r[9] | (r[10] & 0x0f) << 8, r[10] >> 4 | r[11] << 4};
    if (!s->seen) {
        s->seen = 1;
        for (int a = 0; a < 4; a++) { s->rest[a] = abs(raw[a] - PAD_PRO_MIDDLE) <= PAD_PRO_REST_MAX ? raw[a] : PAD_PRO_MIDDLE; s->reach[a][0] = s->reach[a][1] = PAD_PRO_REACH_MIN; }
    }
    *m = pad_pro_buttons(r);
    for (int a = 0; a < 4; a++) st[a] = pad_pro_axis(s, a, raw[a]);
    return 1;
}
