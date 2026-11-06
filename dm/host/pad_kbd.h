// The Mac keyboard's map to Switch input (front_mac.m): which macOS virtual key code presses each Switch button and stick direction.
// The actions are dm_input.h's NpadButton bits in order (A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN), then the left stick's four
// directions, the right stick's four and the shake. Pure, so tests/t_pad_kbd.c checks it without a window.
//
// DM_KBD_MAP: comma-separated ACTION=KEY[|KEY...] entries over the default map. A named action's keys are replaced, not added to, and a key
// given to one action is taken away from any other, so the last entry wins; `A=` maps A to no key. An unknown action or key is skipped, as
// pad_hid.h's DM_HID_MAP does. A key is named by its letter or digit, by the character it makes on a US keyboard (so `-`, `=`, `[`, `;`, `'`
// and the like are keys too), or by name: UP DOWN LEFT RIGHT SPACE RETURN TAB ESC DELETE, MINUS EQUAL LBRACKET RBRACKET SEMICOLON QUOTE COMMA
// PERIOD SLASH BACKSLASH GRAVE, KEYPAD_PLUS KEYPAD_MINUS KEYPAD_ENTER, F1..F12.
//
// The default map, Nintendo's layout: WASD left stick, arrows D-pad (with Shift held: the right stick), J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR,
// F=press left stick, Return=+ Tab=-, Space=shake.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    PK_A, PK_B, PK_X, PK_Y, PK_LS, PK_RS, PK_L, PK_R, PK_ZL, PK_ZR, PK_PLUS, PK_MINUS,
    PK_LEFT, PK_UP, PK_RIGHT, PK_DOWN,          // the D-pad; the 16 above and these are dm_input.h's button bits, in its order
    PK_LUP, PK_LDOWN, PK_LLEFT, PK_LRIGHT,      // the left stick's four directions
    PK_RUP, PK_RDOWN, PK_RLEFT, PK_RRIGHT,      // the right stick's four
    PK_SHAKE,                                   // the controller is shaken while held (DM_BTN_SHAKE)
    PK_COUNT
};

// macOS virtual key codes (HIToolbox/Events.h's kVK_*). PK_KEY_NONE: no key
enum {
    PK_KEY_NONE = 0xFFFF,
    PK_KEY_A = 0, PK_KEY_S = 1, PK_KEY_D = 2, PK_KEY_F = 3, PK_KEY_H = 4, PK_KEY_G = 5, PK_KEY_Z = 6, PK_KEY_X = 7,
    PK_KEY_C = 8, PK_KEY_V = 9, PK_KEY_B = 11, PK_KEY_Q = 12, PK_KEY_W = 13, PK_KEY_E = 14, PK_KEY_R = 15,
    PK_KEY_Y = 16, PK_KEY_T = 17, PK_KEY_1 = 18, PK_KEY_2 = 19, PK_KEY_3 = 20, PK_KEY_4 = 21, PK_KEY_6 = 22,
    PK_KEY_5 = 23, PK_KEY_EQUAL = 24, PK_KEY_9 = 25, PK_KEY_7 = 26, PK_KEY_MINUS = 27, PK_KEY_8 = 28, PK_KEY_0 = 29,
    PK_KEY_RBRACKET = 30, PK_KEY_O = 31, PK_KEY_U = 32, PK_KEY_LBRACKET = 33, PK_KEY_I = 34, PK_KEY_P = 35,
    PK_KEY_RETURN = 36, PK_KEY_L = 37, PK_KEY_J = 38, PK_KEY_QUOTE = 39, PK_KEY_K = 40, PK_KEY_SEMICOLON = 41,
    PK_KEY_BACKSLASH = 42, PK_KEY_COMMA = 43, PK_KEY_SLASH = 44, PK_KEY_N = 45, PK_KEY_M = 46, PK_KEY_PERIOD = 47,
    PK_KEY_TAB = 48, PK_KEY_SPACE = 49, PK_KEY_GRAVE = 50, PK_KEY_DELETE = 51, PK_KEY_ESC = 53,
    PK_KEY_F1 = 122, PK_KEY_F2 = 120, PK_KEY_F3 = 99, PK_KEY_F4 = 118, PK_KEY_F5 = 96, PK_KEY_F6 = 97,
    PK_KEY_F7 = 98, PK_KEY_F8 = 100, PK_KEY_F9 = 101, PK_KEY_F10 = 109, PK_KEY_F11 = 103, PK_KEY_F12 = 111,
    PK_KEY_LEFT = 123, PK_KEY_RIGHT = 124, PK_KEY_DOWN = 125, PK_KEY_UP = 126,
    PK_KEY_KEYPAD_PLUS = 69, PK_KEY_KEYPAD_MINUS = 78, PK_KEY_KEYPAD_ENTER = 76
};

#define PK_MAX_KEYS 4   // keys one action can have (an alternate is the keypad's + beside =, say)

typedef struct {
    uint16_t key[PK_COUNT][PK_MAX_KEYS];   // key codes of each action, PK_KEY_NONE past the last
} pad_kbd;

static const char *const pk_names[PK_COUNT] = {
    "A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN",
    "LSTICK_UP", "LSTICK_DOWN", "LSTICK_LEFT", "LSTICK_RIGHT", "RSTICK_UP", "RSTICK_DOWN", "RSTICK_LEFT", "RSTICK_RIGHT", "SHAKE"
};

typedef struct { const char *name; uint16_t code; } pk_keyname;
static const pk_keyname pk_keynames[] = {
    {"A", PK_KEY_A}, {"S", PK_KEY_S}, {"D", PK_KEY_D}, {"F", PK_KEY_F}, {"H", PK_KEY_H}, {"G", PK_KEY_G},
    {"Z", PK_KEY_Z}, {"X", PK_KEY_X}, {"C", PK_KEY_C}, {"V", PK_KEY_V}, {"B", PK_KEY_B}, {"Q", PK_KEY_Q},
    {"W", PK_KEY_W}, {"E", PK_KEY_E}, {"R", PK_KEY_R}, {"Y", PK_KEY_Y}, {"T", PK_KEY_T}, {"O", PK_KEY_O},
    {"U", PK_KEY_U}, {"I", PK_KEY_I}, {"P", PK_KEY_P}, {"L", PK_KEY_L}, {"J", PK_KEY_J}, {"K", PK_KEY_K},
    {"N", PK_KEY_N}, {"M", PK_KEY_M},
    {"1", PK_KEY_1}, {"2", PK_KEY_2}, {"3", PK_KEY_3}, {"4", PK_KEY_4}, {"5", PK_KEY_5}, {"6", PK_KEY_6},
    {"7", PK_KEY_7}, {"8", PK_KEY_8}, {"9", PK_KEY_9}, {"0", PK_KEY_0},
    {"-", PK_KEY_MINUS}, {"MINUS", PK_KEY_MINUS}, {"=", PK_KEY_EQUAL}, {"EQUAL", PK_KEY_EQUAL},
    {"[", PK_KEY_LBRACKET}, {"LBRACKET", PK_KEY_LBRACKET}, {"]", PK_KEY_RBRACKET}, {"RBRACKET", PK_KEY_RBRACKET},
    {";", PK_KEY_SEMICOLON}, {"SEMICOLON", PK_KEY_SEMICOLON}, {"'", PK_KEY_QUOTE}, {"QUOTE", PK_KEY_QUOTE},
    {",", PK_KEY_COMMA}, {"COMMA", PK_KEY_COMMA}, {".", PK_KEY_PERIOD}, {"PERIOD", PK_KEY_PERIOD},
    {"/", PK_KEY_SLASH}, {"SLASH", PK_KEY_SLASH}, {"\\", PK_KEY_BACKSLASH}, {"BACKSLASH", PK_KEY_BACKSLASH},
    {"`", PK_KEY_GRAVE}, {"GRAVE", PK_KEY_GRAVE},
    {"UP", PK_KEY_UP}, {"DOWN", PK_KEY_DOWN}, {"LEFT", PK_KEY_LEFT}, {"RIGHT", PK_KEY_RIGHT},
    {"SPACE", PK_KEY_SPACE}, {"RETURN", PK_KEY_RETURN}, {"ENTER", PK_KEY_RETURN}, {"TAB", PK_KEY_TAB},
    {"ESC", PK_KEY_ESC}, {"ESCAPE", PK_KEY_ESC}, {"DELETE", PK_KEY_DELETE}, {"BACKSPACE", PK_KEY_DELETE},
    {"F1", PK_KEY_F1}, {"F2", PK_KEY_F2}, {"F3", PK_KEY_F3}, {"F4", PK_KEY_F4}, {"F5", PK_KEY_F5}, {"F6", PK_KEY_F6},
    {"F7", PK_KEY_F7}, {"F8", PK_KEY_F8}, {"F9", PK_KEY_F9}, {"F10", PK_KEY_F10}, {"F11", PK_KEY_F11}, {"F12", PK_KEY_F12},
    {"KEYPAD_PLUS", PK_KEY_KEYPAD_PLUS}, {"KEYPAD_MINUS", PK_KEY_KEYPAD_MINUS}, {"KEYPAD_ENTER", PK_KEY_KEYPAD_ENTER}
};

static int pk_name_eq(const char *s, size_t n, const char *name) {
    return strlen(name) == n && !strncasecmp(s, name, n);
}

// An action by name: its own, or an LS_UP / RS_UP shorthand for LSTICK_UP / RSTICK_UP
static int pk_action_of(const char *s, size_t n) {
    for (int a = 0; a < PK_COUNT; a++) if (pk_name_eq(s, n, pk_names[a])) return a;
    if (n > 3 && n < 32) {
        char buf[32];
        if (!strncasecmp(s, "LS_", 3)) snprintf(buf, sizeof buf, "LSTICK_%.*s", (int)(n - 3), s + 3);
        else if (!strncasecmp(s, "RS_", 3)) snprintf(buf, sizeof buf, "RSTICK_%.*s", (int)(n - 3), s + 3);
        else return -1;
        for (int a = 0; a < PK_COUNT; a++) if (!strcasecmp(buf, pk_names[a])) return a;
    }
    return -1;
}

static int pk_key_of(const char *s, size_t n) {
    for (size_t i = 0; i < sizeof pk_keynames / sizeof pk_keynames[0]; i++)
        if (pk_name_eq(s, n, pk_keynames[i].name)) return pk_keynames[i].code;
    return -1;
}

// One action's keys: a key code goes to the action that names it last, so it leaves any other action that had it
static void pk_clear(pad_kbd *k, int a) { for (int i = 0; i < PK_MAX_KEYS; i++) k->key[a][i] = PK_KEY_NONE; }
static void pk_drop_key(pad_kbd *k, uint16_t code) {
    for (int a = 0; a < PK_COUNT; a++)
        for (int i = 0; i < PK_MAX_KEYS; i++)
            if (k->key[a][i] == code) {
                for (int j = i; j + 1 < PK_MAX_KEYS; j++) k->key[a][j] = k->key[a][j + 1];
                k->key[a][PK_MAX_KEYS - 1] = PK_KEY_NONE;
                i--;
            }
}
static void pk_put(pad_kbd *k, int a, uint16_t code) {
    pk_drop_key(k, code);
    for (int i = 0; i < PK_MAX_KEYS; i++) if (k->key[a][i] == PK_KEY_NONE) { k->key[a][i] = code; return; }
}

static void pad_kbd_init(pad_kbd *k, const char *map) {
    memset(k, 0xFF, sizeof *k);   // every key PK_KEY_NONE
    pk_put(k, PK_A, PK_KEY_J);      pk_put(k, PK_B, PK_KEY_K);      pk_put(k, PK_X, PK_KEY_U);      pk_put(k, PK_Y, PK_KEY_I);
    pk_put(k, PK_LS, PK_KEY_F);     pk_put(k, PK_RS, PK_KEY_G);
    pk_put(k, PK_L, PK_KEY_Q);      pk_put(k, PK_R, PK_KEY_E);      pk_put(k, PK_ZL, PK_KEY_Z);     pk_put(k, PK_ZR, PK_KEY_C);
    pk_put(k, PK_PLUS, PK_KEY_RETURN); pk_put(k, PK_MINUS, PK_KEY_TAB);
    pk_put(k, PK_LEFT, PK_KEY_LEFT); pk_put(k, PK_UP, PK_KEY_UP);   pk_put(k, PK_RIGHT, PK_KEY_RIGHT); pk_put(k, PK_DOWN, PK_KEY_DOWN);
    pk_put(k, PK_LUP, PK_KEY_W);    pk_put(k, PK_LDOWN, PK_KEY_S);  pk_put(k, PK_LLEFT, PK_KEY_A);  pk_put(k, PK_LRIGHT, PK_KEY_D);
    pk_put(k, PK_SHAKE, PK_KEY_SPACE);   // the right stick has no key of its own: Shift + an arrow is it (front_mac.m)
    for (const char *e = map; e && *e; ) {
        const char *comma = strchr(e, ',');
        size_t n = comma ? (size_t)(comma - e) : strlen(e);
        const char *eq = memchr(e, '=', n);
        if (eq) {
            int a = pk_action_of(e, (size_t)(eq - e));
            if (a >= 0) {
                pk_clear(k, a);
                const char *v = eq + 1, *end = e + n;
                while (v < end) {
                    const char *bar = memchr(v, '|', (size_t)(end - v));
                    size_t kn = bar ? (size_t)(bar - v) : (size_t)(end - v);
                    int code = pk_key_of(v, kn);
                    if (code >= 0) pk_put(k, a, (uint16_t)code);
                    v = bar ? bar + 1 : end;
                }
            }
        }
        e = comma ? comma + 1 : e + n;
    }
}

// A D-pad action taken as the right stick instead: an arrow key held with Shift is the camera on a keyboard with no right stick of its own (front_mac.m).
// The D-pad's actions are in LEFT UP RIGHT DOWN order and the right stick's in UP DOWN LEFT RIGHT, so they cannot be offset from each other
static int pad_kbd_as_right_stick(int a) {
    switch (a) {
        case PK_LEFT: return PK_RLEFT;  case PK_UP: return PK_RUP;
        case PK_RIGHT: return PK_RRIGHT; case PK_DOWN: return PK_RDOWN;
    }
    return a;
}

// The action a key code presses, -1 for none
static int pad_kbd_action(const pad_kbd *k, unsigned code) {
    for (int a = 0; a < PK_COUNT; a++)
        for (int i = 0; i < PK_MAX_KEYS; i++)
            if (k->key[a][i] == code) return a;
    return -1;
}
