// The Mac keyboard map (DM_KEYMAP): which key presses which button or stick direction of player 1's controller (keyboard_mac.h kb_on_key), and the names the settings window
// (dm/mac/Launcher.swift KeyboardSection, through launcher.c) shows and saves. Keys are macOS virtual key codes, so a binding is a key's position (the US layout's names: on
// another layout the key in the same place). A binding is a key, or a chord of Shift, Control or Option (either side) and a key: shift+up, ctrl+j, opt+1. One map from binding
// to target: a binding presses one thing, a target may have several bindings. A key pressed with a modifier held is the chord's when the chord is bound, else the key's own.
// DM_KEYMAP is a list of <TARGET>=<key>[,<key>...] separated by spaces or ';', applied over the default map (KM_DEFAULT) in order: an entry takes its keys from whatever had
// them and replaces the target's own keys; <TARGET>= leaves the target with no key. Targets: A B X Y LS RS (stick presses) L R ZL ZR PLUS MINUS DLEFT DUP DRIGHT DDOWN SHAKE
// LLEFT LUP LRIGHT LDOWN (left stick) RLEFT RUP RRIGHT RDOWN (right stick), any case. Keys: km_keys' names, or #<code>, each with shift+, ctrl+ or opt+ before it for a chord
// (control+, option+, alt+ read too; a modifier key itself takes none); none is no key. ⌘, Caps Lock and fn are never bound
// (⌘ is the menus'). The form of PR #386 (and NX2Apple's patch of it) reads too: <name>=<key code>,<name>=<key code>,… with its names (Plus, Minus, Left Up Right Down for
// the D-pad, LUp … RRight, Shake): a token of several entries separated by ',' takes its numbers as key codes; one alone is ours (A=1: the 1 key; #1: key code 1).
// The default map has the arrows as the D-pad and Shift + the arrows as the right stick (Donkey Kong Bananza's camera), as before the map.
// Mouse buttons are keys too (codes 128..132: click rclick mclick mouse4 mouse5). Mouse movement (mouselook_mac.h), once a click in the game captures the pointer, is a
// source of each analog output as keys are: MOUSERSTICK, MOUSELSTICK, MOUSEMOTION (the motion sensor, turned as a controller in the hands) each = none (the default), mouse
// (the movement goes there while no other output's key is held), or <key>+mouse (while that key is held; shift+mouse, ctrl+mouse, opt+mouse: either side). One output
// takes the movement at a time: a held key's first, else the one set to mouse; with none set a click is the touch screen. Each output keeps its own settings:
// MOUSERSTICKSPEED, MOUSELSTICKSPEED (default 2), MOUSEMOTIONSPEED (default 1; 1 the normal speed) and their ...INVERT (the vertical turned round).
// Pure, so tests/t_keymap.c checks it without a window.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {   // targets: 0..15 are the NpadButton bits (dm_input.h DM_BTN_*), then the shake and the sticks' directions, each stick left, up, right, down
    KM_A, KM_B, KM_X, KM_Y, KM_LS, KM_RS, KM_L, KM_R, KM_ZL, KM_ZR, KM_PLUS, KM_MINUS, KM_DLEFT, KM_DUP, KM_DRIGHT, KM_DDOWN,
    KM_SHAKE, KM_LLEFT, KM_LUP, KM_LRIGHT, KM_LDOWN, KM_RLEFT, KM_RUP, KM_RRIGHT, KM_RDOWN, KM_N
};
#define KM_NONE 0xff
#define KM_KEYS 136   // virtual key codes 0..127, then the mouse buttons
#define KM_MOUSE 128  // the first mouse button's code (left click); KM_MOUSE_N of them
#define KM_MOUSE_N 5
enum { KM_PLAIN, KM_SHIFT, KM_CTRL, KM_OPT, KM_LAYERS };   // a binding's modifier
#define KM_SLOTS (KM_LAYERS * KM_KEYS)   // a binding: layer * KM_KEYS + key code
#define KM_MOD_SHIFT 1   // km_key's mods: the event's modifier flags
#define KM_MOD_CTRL 2
#define KM_MOD_OPT 4

enum { KM_OUT_RSTICK, KM_OUT_LSTICK, KM_OUT_MOTION, KM_OUTS };   // the outputs mouse movement can go to
#define KM_SRC_OFF (-1)     // mouse_src: the output takes no mouse movement
#define KM_SRC_MOUSE (-2)   // it takes the movement while no other output's key is held; a key code: while that key is held (a Shift, Control or Option: either side)
typedef struct { uint8_t key[KM_SLOTS]; int mouse_src[KM_OUTS], mouse_invert[KM_OUTS]; float mouse_speed[KM_OUTS]; } keymap;   // key[binding]: its target or KM_NONE
typedef struct { uint8_t pressed[KM_KEYS]; uint8_t held[KM_N]; uint8_t down[KM_KEYS]; int32_t mrx, mry, mlx, mly; } km_state;   // mrx, mry, mlx, mly: the sticks the mouse holds (mouselook_mac.h)   // pressed[code]: 1 + the target the key pressed (released as it went down), held: keys down per target, down: every key down, bound or not (the modifiers)

static const struct { const char *name, *label, *ini; } km_targets[KM_N] = {   // name: DM_KEYMAP's; ini: a keymap file's (either is read in both)
    {"A", "A", "A"}, {"B", "B", "B"}, {"X", "X", "X"}, {"Y", "Y", "Y"}, {"LS", "Left stick press", "LeftStickPress"}, {"RS", "Right stick press", "RightStickPress"},
    {"L", "L", "L"}, {"R", "R", "R"}, {"ZL", "ZL", "ZL"}, {"ZR", "ZR", "ZR"}, {"PLUS", "+", "Plus"}, {"MINUS", "\xE2\x88\x92", "Minus"},
    {"DLEFT", "D-pad left", "DpadLeft"}, {"DUP", "D-pad up", "DpadUp"}, {"DRIGHT", "D-pad right", "DpadRight"}, {"DDOWN", "D-pad down", "DpadDown"}, {"SHAKE", "Shake", "Shake"},
    {"LLEFT", "Left stick left", "LeftStickLeft"}, {"LUP", "Left stick up", "LeftStickUp"}, {"LRIGHT", "Left stick right", "LeftStickRight"}, {"LDOWN", "Left stick down", "LeftStickDown"},
    {"RLEFT", "Right stick left", "RightStickLeft"}, {"RUP", "Right stick up", "RightStickUp"}, {"RRIGHT", "Right stick right", "RightStickRight"}, {"RDOWN", "Right stick down", "RightStickDown"},
};
static const struct { const char *title; int n, targets[12]; } km_groups[] = {   // the panels' and the keymap files' order
    {"Left stick", 4, {KM_LUP, KM_LDOWN, KM_LLEFT, KM_LRIGHT}},
    {"Right stick", 4, {KM_RUP, KM_RDOWN, KM_RLEFT, KM_RRIGHT}},
    {"Buttons", 12, {KM_A, KM_B, KM_X, KM_Y, KM_L, KM_R, KM_ZL, KM_ZR, KM_PLUS, KM_MINUS, KM_LS, KM_RS}},
    {"D-pad", 4, {KM_DUP, KM_DDOWN, KM_DLEFT, KM_DRIGHT}},
    {"Motion", 1, {KM_SHAKE}},
};
#define KM_NGROUPS (int)(sizeof km_groups / sizeof km_groups[0])

static const struct { const char *name, *label; uint8_t code; } km_keys[] = {   // ANSI positions (Carbon kVK_*)
    {"a", "A", 0}, {"b", "B", 11}, {"c", "C", 8}, {"d", "D", 2}, {"e", "E", 14}, {"f", "F", 3}, {"g", "G", 5}, {"h", "H", 4}, {"i", "I", 34}, {"j", "J", 38},
    {"k", "K", 40}, {"l", "L", 37}, {"m", "M", 46}, {"n", "N", 45}, {"o", "O", 31}, {"p", "P", 35}, {"q", "Q", 12}, {"r", "R", 15}, {"s", "S", 1}, {"t", "T", 17},
    {"u", "U", 32}, {"v", "V", 9}, {"w", "W", 13}, {"x", "X", 7}, {"y", "Y", 16}, {"z", "Z", 6},
    {"1", "1", 18}, {"2", "2", 19}, {"3", "3", 20}, {"4", "4", 21}, {"5", "5", 23}, {"6", "6", 22}, {"7", "7", 26}, {"8", "8", 28}, {"9", "9", 25}, {"0", "0", 29},
    {"minus", "-", 27}, {"equal", "=", 24}, {"lbracket", "[", 33}, {"rbracket", "]", 30}, {"semicolon", ";", 41}, {"quote", "'", 39}, {"backslash", "\\", 42},
    {"comma", ",", 43}, {"period", ".", 47}, {"slash", "/", 44}, {"grave", "`", 50}, {"section", "\xC2\xA7", 10},
    {"space", "Space", 49}, {"return", "Return", 36}, {"tab", "Tab", 48}, {"delete", "Delete", 51}, {"escape", "Esc", 53},
    {"left", "\xE2\x86\x90", 123}, {"right", "\xE2\x86\x92", 124}, {"down", "\xE2\x86\x93", 125}, {"up", "\xE2\x86\x91", 126},
    {"lshift", "Left Shift", 56}, {"rshift", "Right Shift", 60}, {"lctrl", "Left Control", 59}, {"rctrl", "Right Control", 62}, {"lopt", "Left Option", 58}, {"ropt", "Right Option", 61},
    {"home", "Home", 115}, {"end", "End", 119}, {"pageup", "Page Up", 116}, {"pagedown", "Page Down", 121}, {"fdelete", "Forward Delete", 117}, {"help", "Help", 114},
    {"f1", "F1", 122}, {"f2", "F2", 120}, {"f3", "F3", 99}, {"f4", "F4", 118}, {"f5", "F5", 96}, {"f6", "F6", 97}, {"f7", "F7", 98}, {"f8", "F8", 100},
    {"f9", "F9", 101}, {"f10", "F10", 109}, {"f11", "F11", 103}, {"f12", "F12", 111},
    {"kp0", "Keypad 0", 82}, {"kp1", "Keypad 1", 83}, {"kp2", "Keypad 2", 84}, {"kp3", "Keypad 3", 85}, {"kp4", "Keypad 4", 86}, {"kp5", "Keypad 5", 87},
    {"kp6", "Keypad 6", 88}, {"kp7", "Keypad 7", 89}, {"kp8", "Keypad 8", 91}, {"kp9", "Keypad 9", 92}, {"kpdot", "Keypad .", 65}, {"kpstar", "Keypad *", 67},
    {"kpplus", "Keypad +", 69}, {"kpclear", "Keypad Clear", 71}, {"kpslash", "Keypad /", 75}, {"kpenter", "Keypad Enter", 76}, {"kpminus", "Keypad -", 78}, {"kpequal", "Keypad =", 81},
    {"click", "Click", 128}, {"rclick", "Right Click", 129}, {"mclick", "Middle Click", 130}, {"mouse4", "Mouse 4", 131}, {"mouse5", "Mouse 5", 132},
};
#define KM_NKEYS (sizeof km_keys / sizeof km_keys[0])

// WASD left stick, arrow keys D-pad, Shift + arrow keys right stick, J=A K=B U=X I=Y, F G stick presses, Q=L E=R, Z=ZL C=ZR, Return=+ Tab=−, Space=shake
#define KM_DEFAULT "LUP=w LLEFT=a LDOWN=s LRIGHT=d RUP=shift+up RLEFT=shift+left RDOWN=shift+down RRIGHT=shift+right A=j B=k X=u Y=i LS=f RS=g L=q R=e ZL=z ZR=c " \
                   "PLUS=return MINUS=tab DLEFT=left DUP=up DRIGHT=right DDOWN=down SHAKE=space"

static inline int km_bindable(int code) { return code >= 0 && (code < KM_MOUSE ? code != 54 && code != 55 && code != 57 && code != 63 : code < KM_MOUSE + KM_MOUSE_N); }   // not ⌘ (54 right, 55 left), Caps Lock, fn
static inline int km_is_modifier(int code) { return code >= 56 && code <= 62 && code != 57; }   // Shift, Option, Control (flagsChanged, not keyDown)

static inline int km_slot_bindable(int slot) {   // a binding: a key, or a modifier with a key that is no modifier
    return slot >= 0 && slot < KM_SLOTS && km_bindable(slot % KM_KEYS) && (slot < KM_KEYS || !km_is_modifier(slot % KM_KEYS));
}
static const struct { const char *name, *label; int layer; } km_mods[] = {   // a chord's modifier: name+key (the first name is written), label then key (the menus' symbols)
    {"shift", "\xE2\x87\xA7", KM_SHIFT}, {"ctrl", "\xE2\x8C\x83", KM_CTRL}, {"control", NULL, KM_CTRL}, {"opt", "\xE2\x8C\xA5", KM_OPT}, {"option", NULL, KM_OPT}, {"alt", NULL, KM_OPT},
};
static inline int km_key_code(const char *name) {   // a binding (a key or a chord: shift+up); -1: no such key
    int layer = KM_PLAIN;
    for (size_t i = 0; i < sizeof km_mods / sizeof km_mods[0]; i++) {
        size_t l = strlen(km_mods[i].name);
        if (!strncasecmp(name, km_mods[i].name, l) && name[l] == '+' && name[l + 1]) { layer = km_mods[i].layer; name += l + 1; break; }
    }
    int c = -1;
    if (name[0] == '#' && name[1]) { char *end; long v = strtol(name + 1, &end, 10); c = !*end && km_bindable((int)v) ? (int)v : -1; }
    else for (size_t i = 0; i < KM_NKEYS; i++) if (!strcasecmp(km_keys[i].name, name)) { c = km_keys[i].code; break; }
    return c >= 0 && km_slot_bindable(layer * KM_KEYS + c) ? layer * KM_KEYS + c : -1;
}
static inline const char *km_key_label(int code) { for (size_t i = 0; i < KM_NKEYS; i++) if (km_keys[i].code == code) return km_keys[i].label; return NULL; }   // a key's; NULL: no name
static inline const char *km_mod_of(int slot, int label) {   // a binding's modifier: its name or label, "" for none
    for (size_t i = 0; i < sizeof km_mods / sizeof km_mods[0]; i++) if (km_mods[i].layer == slot / KM_KEYS) return label ? km_mods[i].label : km_mods[i].name;
    return "";
}
static inline const char *km_slot_name(int slot, char *buf, size_t n) {   // a binding as DM_KEYMAP writes it: shift+up, #<code> for a key with no name
    int c = slot % KM_KEYS; const char *kn = NULL, *mod = km_mod_of(slot, 0);
    for (size_t i = 0; i < KM_NKEYS; i++) if (km_keys[i].code == c) { kn = km_keys[i].name; break; }
    if (kn) snprintf(buf, n, "%s%s%s", mod, *mod ? "+" : "", kn); else snprintf(buf, n, "%s%s#%d", mod, *mod ? "+" : "", c);
    return buf;
}
static inline const char *km_slot_label(int slot, char *buf, size_t n) {   // a binding as the panels show it: ⇧↑, Key <code> for a key with no label
    const char *kl = km_key_label(slot % KM_KEYS), *mod = km_mod_of(slot, 1);
    if (kl) snprintf(buf, n, "%s%s", mod, kl); else snprintf(buf, n, "%sKey %d", mod, slot % KM_KEYS);
    return buf;
}
// The mouse's outputs, by DM_KEYMAP name (MOUSE<name>, MOUSE<name>SPEED, MOUSE<name>INVERT) and keymap file name (Mouse<ini>, Mouse<ini>Speed, Mouse<ini>InvertY)
static const char *const km_out_names[KM_OUTS] = {"RSTICK", "LSTICK", "MOTION"}, *const km_out_ini[KM_OUTS] = {"RightStick", "LeftStick", "Motion"};
static const char *const km_out_labels[KM_OUTS] = {"Right stick", "Left stick", "Motion controls"};
enum { KMS_SRC, KMS_SPEED, KMS_INVERT };
static inline int km_setting(const char *name, int *out) {   // a mouse setting's kind (KMS_*) and output; -1: none
    static const char *const sfx[3] = {"", "SPEED", "INVERT"}, *const isfx[3] = {"", "Speed", "InvertY"};
    for (int o = 0; o < KM_OUTS; o++) for (int k = 0; k < 3; k++) {
        char a[48], b[48]; snprintf(a, sizeof a, "MOUSE%s%s", km_out_names[o], sfx[k]); snprintf(b, sizeof b, "Mouse%s%s", km_out_ini[o], isfx[k]);
        if (!strcasecmp(name, a) || !strcasecmp(name, b)) { *out = o; return k; }
    }
    return -1;
}
static inline int km_mod_pair(int code) { return code == 56 ? 60 : code == 60 ? 56 : code == 59 ? 62 : code == 62 ? 59 : code == 58 ? 61 : code == 61 ? 58 : code; }   // the other side
static inline int km_src(const char *v) {   // a source's value: none or "" off, mouse, <key>+mouse (shift, ctrl, opt: the left key); -3: neither
    if (!*v || !strcasecmp(v, "none")) return KM_SRC_OFF;
    if (!strcasecmp(v, "mouse")) return KM_SRC_MOUSE;
    size_t l = strlen(v); if (l < 7 || strcasecmp(v + l - 6, "+mouse")) return -3;
    char k[32]; snprintf(k, sizeof k, "%.*s", (int)(l - 6), v);
    if (!strcasecmp(k, "shift")) return 56;
    if (!strcasecmp(k, "ctrl") || !strcasecmp(k, "control")) return 59;
    if (!strcasecmp(k, "opt") || !strcasecmp(k, "option") || !strcasecmp(k, "alt")) return 58;
    int c = km_key_code(k); return c >= 0 && c < KM_KEYS ? c : -3;
}
static inline const char *km_src_name(int src, char *buf, size_t n) {   // as DM_KEYMAP writes it: none, mouse, space+mouse, shift+mouse
    if (src == KM_SRC_OFF) return "none";
    if (src == KM_SRC_MOUSE) return "mouse";
    int c = km_mod_pair(src) < src ? km_mod_pair(src) : src; char kn[32];
    snprintf(buf, n, "%s+mouse", c == 56 ? "shift" : c == 59 ? "ctrl" : c == 58 ? "opt" : km_slot_name(c, kn, sizeof kn)); return buf;
}
static inline const char *km_src_label(int src, char *buf, size_t n) {   // as the panels show it: None, Mouse, Space + Mouse, Shift + Mouse
    if (src == KM_SRC_OFF) return "None";
    if (src == KM_SRC_MOUSE) return "Mouse";
    int c = km_mod_pair(src) < src ? km_mod_pair(src) : src; char kl[64];
    snprintf(buf, n, "%s + Mouse", c == 56 ? "Shift" : c == 59 ? "Control" : c == 58 ? "Option" : km_slot_label(c, kl, sizeof kl)); return buf;
}
// An output's source; one output has the plain mouse, and one each key (either side of a modifier): another that had it gets none
static inline void km_mouse_set(keymap *m, int out, int src) {
    if (out < 0 || out >= KM_OUTS) return;
    for (int o = 0; o < KM_OUTS; o++) if (o != out && src != KM_SRC_OFF && (m->mouse_src[o] == src || (src >= 0 && m->mouse_src[o] == km_mod_pair(src)))) m->mouse_src[o] = KM_SRC_OFF;
    m->mouse_src[out] = src;
}
static inline int km_yes(const char *v) { return !strcmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true"); }
static inline int km_key_number(const char *v) {   // a key code in decimal (PR #386's form); -1: none, or a key never bound
    if (!*v || strspn(v, "0123456789") != strlen(v)) return -1;
    long c = strtol(v, NULL, 10); return km_bindable((int)c) ? (int)c : -1;
}
static inline int km_target(const char *name) {
    for (int t = 0; t < KM_N; t++) if (!strcasecmp(km_targets[t].name, name) || !strcasecmp(km_targets[t].ini, name)) return t;
    static const char *const dpad[4] = {"Left", "Up", "Right", "Down"};   // PR #386's names of the D-pad
    for (int i = 0; i < 4; i++) if (!strcasecmp(dpad[i], name)) return KM_DLEFT + i;
    return -1;
}

static inline void km_bind(keymap *m, int target, int slot) {   // the binding presses target (taken from whatever had it); slot -1: the target loses all its bindings
    if (slot < 0) { for (int c = 0; c < KM_SLOTS; c++) if (m->key[c] == target) m->key[c] = KM_NONE; }
    else if (km_slot_bindable(slot)) m->key[slot] = (uint8_t)target;
}

// Apply a map's entries over m; returns the number of entries or keys it skipped, each named in err (may be NULL)
static inline int km_apply(keymap *m, const char *s, char *err, size_t errn) {
    int bad = 0; size_t used = 0; if (err && errn) *err = 0;
    #define KM_ERR(...) do { bad++; if (err && used < errn) { int k_ = snprintf(err + used, errn - used, "%s", used ? "; " : ""); if (k_ > 0) used += (size_t)k_; \
                             if (used < errn) { k_ = snprintf(err + used, errn - used, __VA_ARGS__); if (k_ > 0) used += (size_t)k_; } } } while (0)
    while (s && *s) {
        while (*s == ' ' || *s == '\t' || *s == ';' || *s == '\n') s++;
        if (!*s) break;
        const char *e = s; while (*e && *e != ' ' && *e != '\t' && *e != ';' && *e != '\n') e++;
        char tok[256]; size_t n = (size_t)(e - s); if (n >= sizeof tok) n = sizeof tok - 1;
        memcpy(tok, s, n); tok[n] = 0; s = e;
        int legacy = strchr(tok, '=') != strrchr(tok, '=');   // PR #386's form: entries separated by ',', a key by its code
        for (char *ent = tok, *nx; ent; ent = nx) {
            nx = legacy ? strchr(ent, ',') : NULL; if (nx) *nx++ = 0;
            char *eq = strchr(ent, '=');
            if (!eq) { KM_ERR("'%s' is not TARGET=key", ent); continue; }
            *eq = 0; int t = km_target(ent), out = 0, set = km_setting(ent, &out);
            if (set == KMS_SPEED) { char *end; double v = strtod(eq + 1, &end);
                if (*end || v <= 0 || v > 10) { KM_ERR("%s=%s is not a speed above 0, up to 10", ent, eq + 1); continue; }
                m->mouse_speed[out] = (float)v; continue; }
            if (set == KMS_INVERT) { m->mouse_invert[out] = km_yes(eq + 1); continue; }
            if (set == KMS_SRC) { int src = km_src(eq + 1);
                if (src == -3) { KM_ERR("%s=%s is not none, mouse or <key>+mouse", ent, eq + 1); continue; }
                km_mouse_set(m, out, src); continue; }
            if (t < 0) { KM_ERR("no target '%s'", ent); continue; }
            int codes[KM_SLOTS], nc = 0;
            for (char *k = eq + 1, *next; k && *k; k = next) {
                next = strchr(k, ','); if (next) *next++ = 0;
                if (!*k) continue;
                if (!strcasecmp(k, "none")) continue;
                int c = legacy ? km_key_number(k) : km_key_code(k);
                if (c < 0) KM_ERR("no key '%s' for %s", k, km_targets[t].name); else if (nc < KM_SLOTS) codes[nc++] = c;
            }
            km_bind(m, t, -1);
            for (int i = 0; i < nc; i++) km_bind(m, t, codes[i]);
        }
    }
    #undef KM_ERR
    return bad;
}
static inline void km_default(keymap *m) {
    memset(m->key, KM_NONE, sizeof m->key);
    for (int o = 0; o < KM_OUTS; o++) { m->mouse_src[o] = KM_SRC_OFF; m->mouse_speed[o] = o == KM_OUT_MOTION ? 1 : 2; m->mouse_invert[o] = 0; }
    km_apply(m, KM_DEFAULT, NULL, 0);
}
static inline int km_mouse_on(const keymap *m) { for (int o = 0; o < KM_OUTS; o++) if (m->mouse_src[o] != KM_SRC_OFF) return 1; return 0; }   // mouse movement is a game input
static inline int km_parse(keymap *m, const char *s, char *err, size_t errn) { km_default(m); return km_apply(m, s, err, errn); }   // DM_KEYMAP over the default map

static inline int km_same(const keymap *a, const keymap *b) {
    for (int o = 0; o < KM_OUTS; o++) if (a->mouse_src[o] != b->mouse_src[o] || a->mouse_speed[o] != b->mouse_speed[o] || a->mouse_invert[o] != b->mouse_invert[o]) return 0;
    return !memcmp(a->key, b->key, sizeof a->key);
}

// Every target with its keys, in target order ("A=space B=j ... DUP="): the map in full, as the settings window saves it. Returns the length (snprintf's: >= n when cut)
static inline int km_format(const keymap *m, char *out, size_t n) {
    size_t len = 0; if (n) *out = 0;
    for (int t = 0; t < KM_N; t++) {
        int k = snprintf(len < n ? out + len : NULL, len < n ? n - len : 0, "%s%s=", t ? " " : "", km_targets[t].name); len += (size_t)(k > 0 ? k : 0);
        int first = 1;
        for (int c = 0; c < KM_SLOTS; c++) {
            if (m->key[c] != t) continue;
            char name[32]; km_slot_name(c, name, sizeof name);
            k = snprintf(len < n ? out + len : NULL, len < n ? n - len : 0, "%s%s", first ? "" : ",", name); len += (size_t)(k > 0 ? k : 0); first = 0;
        }
    }
    for (int o = 0; o < KM_OUTS; o++) {
        char sn[48]; const char *nm = km_out_names[o];
        int k = snprintf(len < n ? out + len : NULL, len < n ? n - len : 0, " MOUSE%s=%s MOUSE%sSPEED=%g MOUSE%sINVERT=%d", nm, km_src_name(m->mouse_src[o], sn, sizeof sn), nm,
                         m->mouse_speed[o], nm, m->mouse_invert[o]);
        len += (size_t)(k > 0 ? k : 0);
    }
    return (int)len;
}

static inline int km_mods_held(const km_state *st, int mods) {   // the modifiers held: the event's flags (mods, KM_MOD_*; a modifier pressed before the window had the keyboard) and the keys seen down
    return mods | (st->down[56] || st->down[60] ? KM_MOD_SHIFT : 0) | (st->down[59] || st->down[62] ? KM_MOD_CTRL : 0) | (st->down[58] || st->down[61] ? KM_MOD_OPT : 0);
}
static inline int km_layer(int mods) { return mods & KM_MOD_SHIFT ? KM_SHIFT : mods & KM_MOD_CTRL ? KM_CTRL : mods & KM_MOD_OPT ? KM_OPT : KM_PLAIN; }   // a chord's layer, as the panels bind it
static inline int km_held_key(const km_state *st, int code) { return code >= 0 && code < KM_KEYS && (st->down[code] || st->down[km_mod_pair(code)]); }   // either side of a modifier
// The output mouse movement goes to now (KM_OUT_*): the first whose key is held, else the one set to the plain mouse; -1 none
static inline int km_mouse_out(const keymap *m, const km_state *st) {
    for (int o = 0; o < KM_OUTS; o++) if (m->mouse_src[o] >= 0 && km_held_key(st, m->mouse_src[o])) return o;
    for (int o = 0; o < KM_OUTS; o++) if (m->mouse_src[o] == KM_SRC_MOUSE) return o;
    return -1;
}
// The binding a key pressed with mods held is: a bound chord of a modifier held (Shift's, then Control's, then Option's), else the key's own
static inline int km_resolve(const keymap *m, int code, int mods) {
    if (!km_is_modifier(code)) {
        static const int layer[3] = {KM_SHIFT, KM_CTRL, KM_OPT};
        for (int i = 0; i < 3; i++) if (mods & 1 << i && m->key[layer[i] * KM_KEYS + code] != KM_NONE) return layer[i] * KM_KEYS + code;
    }
    return code;
}
// A key went down or up; mods: the event's modifier flags (KM_MOD_*). Returns whether the key is one the game takes (a key repeat changes nothing)
static inline int km_key(const keymap *m, km_state *st, int code, int down, int mods) {
    if (code < 0 || code >= KM_KEYS) return 0;
    st->down[code] = (uint8_t)!!down;
    if (!down) {
        if (!st->pressed[code]) return m->key[code] != KM_NONE;
        int t = st->pressed[code] - 1; st->pressed[code] = 0;
        if (st->held[t]) st->held[t]--;
        return 1;
    }
    if (st->pressed[code]) return 1;
    int t = m->key[km_resolve(m, code, km_mods_held(st, mods))]; if (t == KM_NONE) return 0;
    st->pressed[code] = (uint8_t)(t + 1); st->held[t]++;
    return 1;
}
static inline int km_held(const km_state *st, int t) { return st->held[t] != 0; }
static inline uint64_t km_buttons(const km_state *st) {   // NpadButton bits, and bit 63 (DM_BTN_SHAKE) for the shake
    uint64_t b = 0; for (int t = 0; t <= KM_DDOWN; t++) if (st->held[t]) b |= 1ull << t;
    return st->held[KM_SHAKE] ? b | 1ull << 63 : b;
}
static inline void km_sticks(const km_state *st, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {   // each direction full over, opposite keys cancel; the mouse's sticks where they are over further
    *lx = (km_held(st, KM_LRIGHT) - km_held(st, KM_LLEFT)) * 32767; *ly = (km_held(st, KM_LUP) - km_held(st, KM_LDOWN)) * 32767;
    *rx = (km_held(st, KM_RRIGHT) - km_held(st, KM_RLEFT)) * 32767; *ry = (km_held(st, KM_RUP) - km_held(st, KM_RDOWN)) * 32767;
    if (abs(st->mrx) > abs(*rx)) *rx = st->mrx;
    if (abs(st->mry) > abs(*ry)) *ry = st->mry;
    if (abs(st->mlx) > abs(*lx)) *lx = st->mlx;
    if (abs(st->mly) > abs(*ly)) *ly = st->mly;
}
static inline void km_release_all(km_state *st) { memset(st, 0, sizeof *st); }

// Keymap files: the presets, one INI file each in ~/Library/Application Support/nxnative/keymaps/ (every game's), listed after the built-in Default by their names:
//   [Keymap]
//   Name = WASD + IJKL          (the name shown; the file's name without .ini when missing)
//   A = l, space                (a target, by its ini or DM_KEYMAP name: its keys, comma-separated; nothing after = is no key)
//   RightStickUp = shift+up     (a chord: shift+, ctrl+ or opt+ and a key)
//   MouseRightStick = mouse     (mouse movement: each output's source, speed and InvertY, as km_ini_write's comments say)
// ; or # starts a comment; a section line is skipped. A target not listed keeps its default key, so a hand-written file may be short; km_ini_write lists every one.
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#define KM_MAX_PRESETS 64
#define KM_FORMAT_MAX 12288   // the full form of any map fits (every binding of every target named), with its NUL
#define KM_INI_MAX 24576      // a keymap file's text, likewise
typedef struct { char name[128], path[1024], map[KM_FORMAT_MAX]; } km_preset;   // map: the full form (km_format), for km_parse; path "" for the built-in Default

static inline char *km_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
// A keymap file's text: its name (if it has one) and its map over the default; returns the lines skipped, named in err
static inline int km_ini_read(const char *text, char *name, size_t namen, keymap *m, char *err, size_t errn) {
    km_default(m); if (namen) *name = 0; if (errn) *err = 0;
    int bad = 0; size_t used = 0; char line[1024];
    for (const char *s = text; s && *s; ) {
        const char *e = strchr(s, '\n'); size_t n = e ? (size_t)(e - s) : strlen(s); if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, s, n); line[n] = 0; s = e ? e + 1 : s + strlen(s);
        char *l = km_trim(line);
        if (!*l || *l == ';' || *l == '#' || *l == '[') continue;
        char *eq = strchr(l, '='); int t = -1;
        if (eq) { *eq = 0; char *k = km_trim(l), *v = km_trim(eq + 1);
            if (!strcasecmp(k, "Name")) { snprintf(name, namen, "%s", v); continue; }
            t = km_target(k); int out = 0, set = km_setting(k, &out);
            if (t >= 0 || set >= 0) {
                char entry[1100], *o = entry + snprintf(entry, sizeof entry, "%s=", t >= 0 ? km_targets[t].name : k);
                for (char *c = v; *c && o < entry + sizeof entry - 1; c++) if (*c != ' ' && *c != '\t') *o++ = *c;
                *o = 0;
                char e1[256]; int b = km_apply(m, entry, e1, sizeof e1);
                if (b) { bad += b; if (used < errn) { int w = snprintf(err + used, errn - used, "%s%s", used ? "; " : "", e1); if (w > 0) used += (size_t)w; } }
                continue;
            }
            *eq = '=';
        }
        bad++;
        if (used < errn) { int w = snprintf(err + used, errn - used, "%s'%s' is not Button = key", used ? "; " : "", l); if (w > 0) used += (size_t)w; }
    }
    return bad;
}
// A keymap file of m, named name, with every target (in km_groups' order); returns the length (snprintf's)
static inline int km_ini_write(const keymap *m, const char *name, char *out, size_t n) {
    size_t len = 0;
    #define KM_PUT(...) do { int k_ = snprintf(len < n ? out + len : NULL, len < n ? n - len : 0, __VA_ARGS__); if (k_ > 0) len += (size_t)k_; } while (0)
    KM_PUT("; nxnative keyboard map: Button = key[, key] (no key after = : none; shift+, ctrl+, opt+ before a key: a chord). Key names as in dm/host/keymap.h;\n"
           "; a button not listed keeps its default key.\n[Keymap]\nName = %s\n", name);
    for (int g = 0; g < KM_NGROUPS; g++) {
        KM_PUT("\n; %s\n", km_groups[g].title);
        for (int i = 0; i < km_groups[g].n; i++) {
            int t = km_groups[g].targets[i], first = 1;
            KM_PUT("%s =", km_targets[t].ini);
            for (int c = 0; c < KM_SLOTS; c++) if (m->key[c] == t) {
                char kn[32]; km_slot_name(c, kn, sizeof kn);
                KM_PUT("%s%s", first ? " " : ", ", kn); first = 0;
            }
            KM_PUT("\n");
        }
    }
    KM_PUT("\n; Mouse movement: a click in the game captures the pointer; Esc lets it go. Each output's source: none, Mouse (while no other output's key is held) or\n"
           "; <key>+Mouse (while that key is held: Space+Mouse; Shift, Ctrl, Opt for either side); its speed (1 the normal speed) and whether its vertical is turned round\n");
    for (int o = 0; o < KM_OUTS; o++) {
        char sn[48]; const char *src = km_src_name(m->mouse_src[o], sn, sizeof sn);
        KM_PUT("Mouse%s =%s%s\nMouse%sSpeed = %g\nMouse%sInvertY = %s\n", km_out_ini[o], m->mouse_src[o] == KM_SRC_OFF ? "" : " ", m->mouse_src[o] == KM_SRC_OFF ? "" : src,
               km_out_ini[o], m->mouse_speed[o], km_out_ini[o], m->mouse_invert[o] ? "yes" : "no");
    }
    #undef KM_PUT
    return (int)len;
}
static inline int km_preset_dir(char *out, size_t n) {   // 0: no HOME
    const char *h = getenv("HOME"); if (!h || !*h) return 0;
    return snprintf(out, n, "%s/Library/Application Support/nxnative/keymaps", h) < (int)n;
}
static inline int km_preset_cmp(const void *a, const void *b) { return strcasecmp(((const km_preset *)a)->name, ((const km_preset *)b)->name); }
// Default, then the folder's files by name; returns the count. A file with wrong lines is listed without them, and each is named in err ("<file>: ..." separated by newlines)
static inline int km_presets_load(km_preset *p, int max, char *err, size_t errn) {
    if (errn) *err = 0;
    if (max < 1) return 0;
    keymap m; km_default(&m); snprintf(p[0].name, sizeof p[0].name, "Default"); p[0].path[0] = 0; km_format(&m, p[0].map, sizeof p[0].map);
    int n = 1; size_t used = 0; char dir[1024];
    if (!km_preset_dir(dir, sizeof dir)) return n;
    DIR *d = opendir(dir); if (!d) return n;
    for (struct dirent *e; (e = readdir(d)) && n < max; ) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcasecmp(e->d_name + l - 4, ".ini")) continue;
        km_preset *q = &p[n]; snprintf(q->path, sizeof q->path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(q->path, "r"); if (!f) continue;
        char text[KM_INI_MAX]; size_t r = fread(text, 1, sizeof text - 1, f); fclose(f); text[r] = 0;
        char e1[512]; int bad = km_ini_read(text, q->name, sizeof q->name, &m, e1, sizeof e1);
        if (!q->name[0]) snprintf(q->name, sizeof q->name, "%.*s", (int)(l - 4), e->d_name);
        if (bad && used < errn) { int w = snprintf(err + used, errn - used, "%s%s: %s", used ? "\n" : "", e->d_name, e1); if (w > 0) used += (size_t)w; }
        km_format(&m, q->map, sizeof q->map); n++;
    }
    closedir(d);
    qsort(p + 1, (size_t)(n - 1), sizeof *p, km_preset_cmp);
    return n;
}
// Save m as a preset named name: <folder>/<name, letters and digits, others '-'>.ini, replacing a file of that name. Returns 0, or -1 (errno), with the path in path ("" before one is known)
static inline int km_preset_save(const keymap *m, const char *name, char *path, size_t pathn) {
    char dir[1024], slug[128]; size_t k = 0;
    if (pathn) *path = 0;
    if (!km_preset_dir(dir, sizeof dir)) return -1;
    for (const char *c = name; *c && k < sizeof slug - 1; c++) {
        char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) slug[k++] = ch; else if (k && slug[k - 1] != '-') slug[k++] = '-';
    }
    while (k && slug[k - 1] == '-') k--;
    slug[k] = 0; if (!k) snprintf(slug, sizeof slug, "keymap");
    for (char *s = dir + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(dir, 0755); *s = '/'; }   // mkdir -p
    mkdir(dir, 0755);
    snprintf(path, pathn, "%s/%s.ini", dir, slug);
    static char text[KM_INI_MAX];   // main thread only, as the panels
    if (km_ini_write(m, name, text, sizeof text) >= (int)sizeof text) { errno = E2BIG; return -1; }
    FILE *f = fopen(path, "w"); if (!f) return -1;
    int ok = fputs(text, f) >= 0; ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}
