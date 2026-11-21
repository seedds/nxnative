// The player's controls (docs/frontends.md, Controls): a controller's buttons sent as other buttons, the keyboard's keys and the mouse as a stick. Plain C, no host state, so a
// test can link it (tests/t_controls.c); hle_hid.c loads it once (dm_controls) and applies the controller map to every controller path, front_mac.m reads the keyboard and the mouse.
//
// The file is text, one setting per line; '#' starts a comment; later lines win, so a game's own file read after the global one changes only what it names:
//   pad <button>=<buttons>    a controller's button in that position (by position: a Nintendo pad's labels, another pad's right / bottom / top / left face button is A / B / X / Y)
//                             sends these instead; <buttons> is names joined with '+', or "none". Every controller of every player, the Joy-Cons' rail buttons included (LSL LSR RSL RSR)
//   pad swap_sticks=1         the left and the right stick trade places
//   key <code>=<actions>      the macOS key code <code> (kVK_*, 0..127) does these; "none" unbinds it. Actions: the buttons, LLeft LUp LRight LDown / RLeft RUp RRight RDown (a stick
//                             pushed all the way) and Shake
//   key clear                 forget the built-in keyboard map (the lines after it give the whole map)
//   mouse=<off|right|left>    while the pointer is captured (a click in the picture; Escape gives it back), its movement is that stick; off (the default) keeps the mouse the touch screen
//   mouse_speed=<n>           how far a movement tilts the stick, 0.1 to 10 (1 the default)
//   mouse_invert_y=1          moving the mouse up tilts the stick down
//   mouse_button <left|right|middle>=<actions>   what the buttons do while the pointer is captured (defaults: left ZR, right ZL, middle RS)
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "dm_input.h"

enum { CTL_LLEFT = 1, CTL_LUP = 2, CTL_LRIGHT = 4, CTL_LDOWN = 8, CTL_RLEFT = 16, CTL_RUP = 32, CTL_RRIGHT = 64, CTL_RDOWN = 128 };
typedef struct { uint64_t btn; uint8_t dirs; } ctl_action;   // buttons (DM_BTN_*, DM_BTN_SHAKE) and stick directions (CTL_*)
#define CTL_KEYS 128
#define CTL_PAD_BITS 28   // NpadButton 0..15 and the rail buttons 24..27
enum { CTL_MOUSE_OFF, CTL_MOUSE_RIGHT, CTL_MOUSE_LEFT };
typedef struct controls_s {
    uint64_t pad[CTL_PAD_BITS];   // what bit i of a controller's buttons sends
    int pad_mapped;               // any pad line changed it (the identity is skipped)
    int swap_sticks;
    ctl_action key[CTL_KEYS];
    int mouse, mouse_invert_y;
    float mouse_speed;
    ctl_action mouse_btn[3];      // left, right, middle
} controls;
// The host's, loaded at the first call from DM_CONTROLS or <DM_ROOT>/controls.txt (hle_hid.c); dm_pad_set_player and dm_joy_set apply its controller map themselves. Any thread
const controls *dm_controls(void);

static const struct { const char *name; uint64_t btn; uint8_t dirs; } CTL_NAMES[] = {
    {"A", DM_BTN_A}, {"B", DM_BTN_B}, {"X", DM_BTN_X}, {"Y", DM_BTN_Y}, {"LS", DM_BTN_LS}, {"RS", DM_BTN_RS}, {"L", DM_BTN_L}, {"R", DM_BTN_R},
    {"ZL", DM_BTN_ZL}, {"ZR", DM_BTN_ZR}, {"Plus", DM_BTN_PLUS}, {"Minus", DM_BTN_MINUS}, {"Left", DM_BTN_LEFT}, {"Up", DM_BTN_UP}, {"Right", DM_BTN_RIGHT}, {"Down", DM_BTN_DOWN},
    {"LSL", DM_BTN_LSL}, {"LSR", DM_BTN_LSR}, {"RSL", DM_BTN_RSL}, {"RSR", DM_BTN_RSR}, {"Shake", DM_BTN_SHAKE},
    {"LLeft", 0, CTL_LLEFT}, {"LUp", 0, CTL_LUP}, {"LRight", 0, CTL_LRIGHT}, {"LDown", 0, CTL_LDOWN},
    {"RLeft", 0, CTL_RLEFT}, {"RUp", 0, CTL_RUP}, {"RRight", 0, CTL_RRIGHT}, {"RDown", 0, CTL_RDOWN},
};
#define CTL_NNAMES ((int)(sizeof CTL_NAMES / sizeof CTL_NAMES[0]))

// The built-in keyboard map (front_mac.m's before this file): WASD the left stick, arrows the D-pad, J K U I = A B X Y, F G = stick presses, Q E Z C = L R ZL ZR,
// Return = Plus, Tab = Minus, Space = Shake. Shift with an arrow that still has its built-in action is the right stick (front_mac.m)
static const struct { uint8_t code; uint64_t btn; uint8_t dirs; } CTL_KEY_DEFAULTS[] = {
    {13, 0, CTL_LUP}, {0, 0, CTL_LLEFT}, {1, 0, CTL_LDOWN}, {2, 0, CTL_LRIGHT},
    {38, DM_BTN_A}, {40, DM_BTN_B}, {32, DM_BTN_X}, {34, DM_BTN_Y}, {3, DM_BTN_LS}, {5, DM_BTN_RS},
    {12, DM_BTN_L}, {14, DM_BTN_R}, {6, DM_BTN_ZL}, {8, DM_BTN_ZR}, {36, DM_BTN_PLUS}, {48, DM_BTN_MINUS},
    {123, DM_BTN_LEFT}, {126, DM_BTN_UP}, {124, DM_BTN_RIGHT}, {125, DM_BTN_DOWN}, {49, DM_BTN_SHAKE},
};

static inline void ctl_defaults(controls *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < CTL_PAD_BITS; i++) c->pad[i] = 1ull << i;
    for (size_t i = 0; i < sizeof CTL_KEY_DEFAULTS / sizeof CTL_KEY_DEFAULTS[0]; i++) c->key[CTL_KEY_DEFAULTS[i].code] = (ctl_action){CTL_KEY_DEFAULTS[i].btn, CTL_KEY_DEFAULTS[i].dirs};
    c->mouse_speed = 1;
    c->mouse_btn[0] = (ctl_action){DM_BTN_ZR}; c->mouse_btn[1] = (ctl_action){DM_BTN_ZL}; c->mouse_btn[2] = (ctl_action){DM_BTN_RS};
}
// The key's action as built in (front_mac.m's Shift + arrow test)
static inline int ctl_key_is_default(const controls *c, int code) {
    if (code < 0 || code >= CTL_KEYS) return 0;
    for (size_t i = 0; i < sizeof CTL_KEY_DEFAULTS / sizeof CTL_KEY_DEFAULTS[0]; i++)
        if (CTL_KEY_DEFAULTS[i].code == code) return c->key[code].btn == CTL_KEY_DEFAULTS[i].btn && c->key[code].dirs == CTL_KEY_DEFAULTS[i].dirs;
    return 0;
}
static inline int ctl_name(const char *s, size_t n, uint64_t *btn, uint8_t *dirs) {
    for (int i = 0; i < CTL_NNAMES; i++) if (strlen(CTL_NAMES[i].name) == n && !strncasecmp(CTL_NAMES[i].name, s, n)) { *btn |= CTL_NAMES[i].btn; *dirs |= CTL_NAMES[i].dirs; return 1; }
    return 0;
}
// "A+ZR", "LUp", "none": 1 parsed, 0 not
static inline int ctl_parse_action(const char *s, ctl_action *out) {
    ctl_action a = {0, 0};
    if (!strcasecmp(s, "none")) { *out = a; return 1; }
    while (*s) {
        const char *e = strchr(s, '+'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (!n || !ctl_name(s, n, &a.btn, &a.dirs)) return 0;
        s += n + (e != NULL);
        if (e && !*s) return 0;
    }
    if (!a.btn && !a.dirs) return 0;
    *out = a; return 1;
}
static inline char *ctl_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
// One line of the file, changed in place: 1 applied (or empty / a comment), 0 not understood
static inline int ctl_line(controls *c, char *line) {
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *s = ctl_trim(line); if (!*s) return 1;
    char *sp = s; while (*sp && !isspace((unsigned char)*sp) && *sp != '=') sp++;
    char word[32]; size_t wn = (size_t)(sp - s); if (wn >= sizeof word) return 0;
    memcpy(word, s, wn); word[wn] = 0;
    char *rest = ctl_trim(sp);
    if (!strcasecmp(word, "key") && !strcasecmp(rest, "clear")) { memset(c->key, 0, sizeof c->key); return 1; }
    char *eq = strchr(rest, '='), *lhs, *rhs;
    if (*rest == '=') { lhs = NULL; rhs = ctl_trim(rest + 1); }   // "mouse=right"
    else if (eq) { *eq = 0; lhs = ctl_trim(rest); rhs = ctl_trim(eq + 1); }
    else return 0;
    if (!strcasecmp(word, "pad") && lhs) {
        if (!strcasecmp(lhs, "swap_sticks")) { c->swap_sticks = atoi(rhs) != 0; return 1; }
        uint64_t b = 0; uint8_t d = 0; ctl_action a;
        if (!ctl_name(lhs, strlen(lhs), &b, &d) || d || !b || (b & DM_BTN_SHAKE) || !ctl_parse_action(rhs, &a) || a.dirs) return 0;
        int i = __builtin_ctzll(b); if (i >= CTL_PAD_BITS) return 0;
        c->pad[i] = a.btn & ~DM_BTN_SHAKE; c->pad_mapped = 1;
        for (int k = 0; k < CTL_PAD_BITS; k++) if (c->pad[k] != 1ull << k) return 1;
        c->pad_mapped = 0; return 1;   // back to the identity
    }
    if (!strcasecmp(word, "key") && lhs) {
        char *end; long code = strtol(lhs, &end, 10);
        if (*end || code < 0 || code >= CTL_KEYS) return 0;
        ctl_action a; if (!ctl_parse_action(rhs, &a)) return 0;
        c->key[code] = a; return 1;
    }
    if (!strcasecmp(word, "mouse") && !lhs) {
        if (!strcasecmp(rhs, "off")) c->mouse = CTL_MOUSE_OFF; else if (!strcasecmp(rhs, "right")) c->mouse = CTL_MOUSE_RIGHT; else if (!strcasecmp(rhs, "left")) c->mouse = CTL_MOUSE_LEFT; else return 0;
        return 1;
    }
    if (!strcasecmp(word, "mouse_speed") && !lhs) { float v = strtof(rhs, NULL); if (!(v >= 0.1f && v <= 10.f)) return 0; c->mouse_speed = v; return 1; }
    if (!strcasecmp(word, "mouse_invert_y") && !lhs) { c->mouse_invert_y = atoi(rhs) != 0; return 1; }
    if (!strcasecmp(word, "mouse_button") && lhs) {
        int i = !strcasecmp(lhs, "left") ? 0 : !strcasecmp(lhs, "right") ? 1 : !strcasecmp(lhs, "middle") ? 2 : -1;
        ctl_action a; if (i < 0 || !ctl_parse_action(rhs, &a)) return 0;
        c->mouse_btn[i] = a; return 1;
    }
    return 0;
}
// Reads a file into c (on top of what it holds). Returns the number of lines not understood (each reported through bad, which may be NULL), or -1 when the file cannot be opened
static inline int ctl_load(controls *c, const char *path, void (*bad)(const char *path, int line, const char *text)) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char buf[512], copy[512]; int n = 0, errs = 0;
    while (fgets(buf, sizeof buf, f)) {
        n++; snprintf(copy, sizeof copy, "%s", buf); copy[strcspn(copy, "\r\n")] = 0;
        if (!ctl_line(c, buf)) { errs++; if (bad) bad(path, n, copy); }
    }
    fclose(f); return errs;
}
// A controller's buttons as the player mapped them: each of bits 0..15 and 24..27 sends its mapped buttons; the other bits (Shake) pass
static inline uint64_t ctl_pad_map(const controls *c, uint64_t b) {
    if (!c->pad_mapped) return b;
    uint64_t keep = b & ~(0xffffull | 0xf000000ull), out = keep;
    for (int i = 0; i < CTL_PAD_BITS; i++) if (b >> i & 1) out |= c->pad[i];
    return out;
}
// The stick values of a set of directions: x from left / right, y from down / up, full tilt (±DM_STICK_MAX)
static inline void ctl_dirs_sticks(uint8_t d, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    *lx = (!!(d & CTL_LRIGHT) - !!(d & CTL_LLEFT)) * DM_STICK_MAX; *ly = (!!(d & CTL_LUP) - !!(d & CTL_LDOWN)) * DM_STICK_MAX;
    *rx = (!!(d & CTL_RRIGHT) - !!(d & CTL_RLEFT)) * DM_STICK_MAX; *ry = (!!(d & CTL_RUP) - !!(d & CTL_RDOWN)) * DM_STICK_MAX;
}
