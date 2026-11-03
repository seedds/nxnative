// The Mac front end's button assignments (front_mac.m; pure, so tests/t_input_map.c checks it without a keyboard or controller), named as DM_KEYS names them (hle_hid.c).
// DM_KEYMAP=<name>=<key code>,… puts a function on another key: <key code> is the macOS virtual key code (NSEvent.keyCode, decimal), or "none" to leave the function on no key.
// The names are the 16 buttons, the four directions of each stick (LUp … RRight) and Shake; the left stick is on W A S D, the right stick on no key, unless DM_KEYMAP says so.
// One key holds one function: the last assignment of a key wins, and the function that held it before is left on no key, so "A=40,B=38" swaps A and B.
// DM_PAD_MAP=<name>=<name>,… changes what a controller's buttons send: the button that sends <first> by default (as front_mac.m maps the pad: face buttons by position, a Nintendo pad's by its labels, pad_apple.h) sends <second> instead,
// "none" sends nothing; buttons not named stay as they are, so "A=B,B=A" swaps them. Only the 16 buttons take part (the sticks stay sticks).
// A malformed entry is left out and named in the log; the rest of the list still applies.
// input_map_valid() is the same parser without a map or a log line: the Mac launcher (dm/mac/launcher.c) checks a saved DM_KEYMAP / DM_PAD_MAP with it, so a saved value is used only when the host takes every entry of it.
#pragma once
#include <math.h>
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
static int im_parse(const char *spec, int max_fn, int (*one)(void *, int, const char *, size_t), void *ctx, int log) {
    int bad = 0;
    for (const char *p = spec; p && *p; ) {
        const char *end = strchr(p, ','); size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', n);
        int f = eq ? im_name(p, (size_t)(eq - p)) : -1;
        if (f < 0 || f >= max_fn || !one(ctx, f, eq + 1, n - (size_t)(eq + 1 - p))) {
            if (log) fprintf(stderr, "[dm] [RED] input map: '%.*s' is not <name>=<value>, left out\n", (int)n, p);   // dm.h LOG's form (this header stays free of dm.h for the test)
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
static int key_map_parse(key_map *m, const char *spec) { return im_parse(spec, IM_N, im_key_one, m, 1); }
static int pad_map_parse(pad_map *m, const char *spec) { return im_parse(spec, IM_BUTTONS, im_pad_one, m, 1); }
// 1 when spec is a DM_KEYMAP (pad = 0) or DM_PAD_MAP (pad = 1) value the host takes whole: not empty, every entry well-formed. Pure: nothing is logged, no map is kept
static int input_map_valid(const char *spec, int pad) {
    if (!spec || !*spec) return 0;
    key_map k; pad_map m; key_map_default(&k); pad_map_default(&m);
    return pad ? !im_parse(spec, IM_BUTTONS, im_pad_one, &m, 0) : !im_parse(spec, IM_N, im_key_one, &k, 0);
}
static int key_map_fn(const key_map *m, unsigned code) { return code < IM_KEYS ? m->fn[code] : -1; }
// The buttons a controller sends for the buttons the default mapping read (DM_BTN_* bits); bits above the 16 buttons (DM_BTN_SHAKE) pass through
static uint64_t pad_map_apply(const pad_map *m, uint64_t btn) {
    uint64_t out = btn & ~0xffffull;
    for (int b = 0; b < IM_BUTTONS; b++) if (btn >> b & 1 && m->to[b] >= 0) out |= 1ull << m->to[b];
    return out;
}
static int pad_map_is_default(const pad_map *m) { for (int b = 0; b < IM_BUTTONS; b++) if (m->to[b] != b) return 0; return 1; }

// Face buttons: DM_PAD_ABXY=auto|label|position says how a controller's A, B, X and Y are read. GameController names most pads' face buttons by position (Xbox style) and a
// Nintendo pad's by their labels (pad_apple.h). auto (the default) decides that per controller; label takes every pad's names as its printed letters (an Xbox pad's bottom button,
// marked A, sends A), position takes them as Xbox positions (the bottom button sends B, as on a Switch). Raw HID pads (pad_hid.h) are read by position; label swaps them too
enum { IM_ABXY_AUTO, IM_ABXY_LABEL, IM_ABXY_POSITION };
static int pad_abxy_parse(const char *v) {   // -1: not a value the host takes; unset or empty is auto
    if (!v || !*v || !strcasecmp(v, "auto")) return IM_ABXY_AUTO;
    if (!strcasecmp(v, "label")) return IM_ABXY_LABEL;
    if (!strcasecmp(v, "position")) return IM_ABXY_POSITION;
    return -1;
}
static int pad_abxy_valid(const char *v) { return v && *v && pad_abxy_parse(v) >= 0; }
// A and B, X and Y trade places (from GameController's position names to labels, or back); other bits pass
static uint64_t pad_abxy_swap(uint64_t m) {
    const uint64_t ab = DM_BTN_A | DM_BTN_B, xy = DM_BTN_X | DM_BTN_Y;
    uint64_t out = m & ~(ab | xy);
    if (m & DM_BTN_A) out |= DM_BTN_B; if (m & DM_BTN_B) out |= DM_BTN_A;
    if (m & DM_BTN_X) out |= DM_BTN_Y; if (m & DM_BTN_Y) out |= DM_BTN_X;
    return out;
}

// Stick shape for controllers: DM_PAD_STICK_DZ=<0..0.5> is a radial dead zone (a stick resting a little off centre stays at rest), DM_PAD_STICK_SENS=<0.25..3> scales the
// deflection past it (above 1 a smaller tilt reaches full). The length past the dead zone is rescaled to start at 0 and clamped to the unit circle, so the direction is kept.
// Both unset (0 and 1): the axes go to the game as GameController gives them, each on its own, as before
#define IM_DZ_MAX 0.5
#define IM_SENS_MIN 0.25
#define IM_SENS_MAX 3.0
typedef struct { float dz, sens; } stick_shape;
static void stick_shape_default(stick_shape *s) { s->dz = 0; s->sens = 1; }
static int stick_shape_is_default(const stick_shape *s) { return s->dz == 0 && s->sens == 1; }
// 1 when v is a plain decimal number in [lo, hi] (no sign or space before it, no hex, nothing after it), as the launcher's other range checks; *out gets it
static int im_number(const char *v, double lo, double hi, double *out) {
    if (!v || !*v || strpbrk(v, "xX") || !(*v == '.' || (*v >= '0' && *v <= '9'))) return 0;
    char *end; double x = strtod(v, &end);
    if (*end || x != x || x < lo || x > hi) return 0;
    if (out) *out = x;
    return 1;
}
static int stick_dz_valid(const char *v) { return im_number(v, 0, IM_DZ_MAX, NULL); }
static int stick_sens_valid(const char *v) { return im_number(v, IM_SENS_MIN, IM_SENS_MAX, NULL); }
// Reads DM_PAD_STICK_DZ / DM_PAD_STICK_SENS values (NULL: unset); returns the number of values left out as malformed or out of range
static int stick_shape_parse(stick_shape *s, const char *dz, const char *sens) {
    int bad = 0; double x;
    if (dz && *dz) { if (im_number(dz, 0, IM_DZ_MAX, &x)) s->dz = (float)x; else bad++; }
    if (sens && *sens) { if (im_number(sens, IM_SENS_MIN, IM_SENS_MAX, &x)) s->sens = (float)x; else bad++; }
    return bad;
}
static void stick_shape_apply(const stick_shape *s, float *x, float *y) {
    if (stick_shape_is_default(s)) return;
    float r = sqrtf(*x * *x + *y * *y);
    if (r <= s->dz) { *x = *y = 0; return; }
    float n = (r - s->dz) / (1 - s->dz) * s->sens;
    if (n > 1) n = 1;
    *x = *x / r * n; *y = *y / r * n;
}

