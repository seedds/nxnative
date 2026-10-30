// The Mac front end's button assignments (front_mac.m; pure, so tests/t_input_map.c checks it without a keyboard or controller), named as DM_KEYS names them (hle_hid.c).
// DM_KEYMAP=<name>=<key code>,… puts a function on another key: <key code> is the macOS virtual key code (NSEvent.keyCode, decimal), or "none" to leave the function on no key.
// The names are the 16 buttons, the four directions of each stick (LUp … RRight) and Shake; the left stick is on W A S D, the right stick on no key, unless DM_KEYMAP says so.
// One key holds one function: the last assignment of a key wins, and the function that held it before is left on no key, so "A=40,B=38" swaps A and B.
// DM_PAD_MAP=<name>=<name>,… changes what a controller's buttons send: the button that sends <first> by default (by position, as the iPhone maps it) sends <second> instead,
// "none" sends nothing; buttons not named stay as they are, so "A=B,B=A" swaps them. Only the 16 buttons take part (the sticks stay sticks).
// A malformed entry is left out and named in the log; the rest of the list still applies.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dm_input.h"

enum { IM_BUTTONS = 16, IM_LUP = 16, IM_LDOWN, IM_LLEFT, IM_LRIGHT, IM_RUP, IM_RDOWN, IM_RLEFT, IM_RRIGHT, IM_SHAKE, IM_N };   // functions 0..15 are the DM_BTN_* bits
#define IM_KEYS 128   // virtual key codes are below 128
typedef struct { int8_t fn[IM_KEYS]; } key_map;   // the function each key code holds, -1 none
typedef struct { int8_t to[IM_BUTTONS]; } pad_map;   // the button each default button sends, -1 none

static const char *const im_names[IM_N] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down",
                                           "LUp", "LDown", "LLeft", "LRight", "RUp", "RDown", "RLeft", "RRight", "Shake"};
static int im_name(const char *s, size_t n) {
    for (int f = 0; f < IM_N; f++) if (strlen(im_names[f]) == n && !strncasecmp(s, im_names[f], n)) return f;
    return -1;
}
static void key_map_default(key_map *m) {
    memset(m->fn, -1, sizeof m->fn);
    static const struct { uint8_t code; int8_t fn; } d[] = {
        {38, 0}, {40, 1}, {32, 2}, {34, 3},                    // J K U I → A B X Y
        {3, 4}, {5, 5},                                        // F G → press stick
        {12, 6}, {14, 7}, {6, 8}, {8, 9},                      // Q E Z C → L R ZL ZR
        {36, 10}, {48, 11},                                    // Return Tab → + −
        {123, 12}, {126, 13}, {124, 14}, {125, 15},            // ← ↑ → ↓
        {13, IM_LUP}, {1, IM_LDOWN}, {0, IM_LLEFT}, {2, IM_LRIGHT},   // W S A D → left stick
        {49, IM_SHAKE},                                        // Space: shaken while held
    };
    for (size_t i = 0; i < sizeof d / sizeof *d; i++) m->fn[d[i].code] = d[i].fn;
}
static void pad_map_default(pad_map *m) { for (int b = 0; b < IM_BUTTONS; b++) m->to[b] = (int8_t)b; }

// Walks "<name>=<value>,…", calling one(name's function, value text, its length) for each entry; returns the number of malformed entries, each logged here
static int im_parse(const char *spec, int max_fn, int (*one)(void *, int, const char *, size_t), void *ctx) {
    int bad = 0;
    for (const char *p = spec; p && *p; ) {
        const char *end = strchr(p, ','); size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', n);
        int f = eq ? im_name(p, (size_t)(eq - p)) : -1;
        if (f < 0 || f >= max_fn || !one(ctx, f, eq + 1, n - (size_t)(eq + 1 - p))) {
            fprintf(stderr, "[dm] [RED] input map: '%.*s' is not <name>=<value>, left out\n", (int)n, p);   // dm.h LOG's form (this header stays free of dm.h for the test)
            bad++;
        }
        p = end ? end + 1 : NULL;
    }
    return bad;
}
static int im_key_one(void *ctx, int f, const char *v, size_t n) {
    key_map *m = ctx; char buf[8];
    if (n == 4 && !strncasecmp(v, "none", 4)) { for (int c = 0; c < IM_KEYS; c++) if (m->fn[c] == f) m->fn[c] = -1; return 1; }
    if (!n || n >= sizeof buf || strspn(v, "0123456789") < n) return 0;
    memcpy(buf, v, n); buf[n] = 0; int code = atoi(buf);
    if (code >= IM_KEYS) return 0;
    for (int c = 0; c < IM_KEYS; c++) if (m->fn[c] == f) m->fn[c] = -1;   // the function leaves its old key
    m->fn[code] = (int8_t)f;   // the key's old function is left on no key
    return 1;
}
static int im_pad_one(void *ctx, int f, const char *v, size_t n) {
    pad_map *m = ctx;
    if (n == 4 && !strncasecmp(v, "none", 4)) { m->to[f] = -1; return 1; }
    int t = im_name(v, n); if (t < 0 || t >= IM_BUTTONS) return 0;
    m->to[f] = (int8_t)t; return 1;
}
static int key_map_parse(key_map *m, const char *spec) { return im_parse(spec, IM_N, im_key_one, m); }
static int pad_map_parse(pad_map *m, const char *spec) { return im_parse(spec, IM_BUTTONS, im_pad_one, m); }
static int key_map_fn(const key_map *m, unsigned code) { return code < IM_KEYS ? m->fn[code] : -1; }
// The buttons a controller sends for the buttons the default mapping read (DM_BTN_* bits); bits above the 16 buttons (DM_BTN_SHAKE) pass through
static uint64_t pad_map_apply(const pad_map *m, uint64_t btn) {
    uint64_t out = btn & ~0xffffull;
    for (int b = 0; b < IM_BUTTONS; b++) if (btn >> b & 1 && m->to[b] >= 0) out |= 1ull << m->to[b];
    return out;
}
static int pad_map_is_default(const pad_map *m) { for (int b = 0; b < IM_BUTTONS; b++) if (m->to[b] != b) return 0; return 1; }
