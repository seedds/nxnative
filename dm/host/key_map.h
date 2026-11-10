// The Mac keyboard's map to the game's controls (front_mac.m): DM_KEY_MAP="<key>:<control>,..." changes what a key does in the front end's layout, per game through its
// env.txt (GitHub issue #1216: Space drifts better than E on a MacBook's keyboard). A key the map names loses its layout meaning and takes the control named; every other
// key keeps its layout. Plain C, so tests/t_key_map.c checks the names, the parsing and the table without a window.
// Keys are named by macOS's virtual key codes (an ANSI layout: a letter's code is the same on any layout, the letter printed on the key is not): letters and digits as they are,
// then space, return (enter), tab, escape (esc), delete, grave, minus, equal, lbracket, rbracket, backslash, semicolon, quote, comma, period, slash, f1 to f12, home, end,
// pageup, pagedown, left, right, up, down, and the modifiers lshift, rshift, lctrl, rctrl, lalt, ralt, lcmd, rcmd, capslock, fn.
// Controls: a, b, x, y, l, r, zl, zr, ls, rs (the sticks pressed), plus, minus, left, up, right, down (the D-pad), lup, lleft, ldown, lright (the left stick),
// rup, rleft, rdown, rright (the right stick), shake (the controller shaken while held), none (the key does nothing).
#pragma once
#include <stdio.h>
#include <string.h>

enum { KM_KEYS = 128 };   // virtual key codes 0..127
enum { KM_NONE = -2, KM_DEFAULT = -1,   // the key does nothing; the key keeps the layout's meaning (the map says nothing about it)
       KM_LS_UP = 16, KM_LS_LEFT, KM_LS_DOWN, KM_LS_RIGHT, KM_RS_UP, KM_RS_LEFT, KM_RS_DOWN, KM_RS_RIGHT, KM_SHAKE = 24 };   // 0..15: the button bits of dm_input.h
typedef struct { signed char act[KM_KEYS]; int n; } key_map;   // act[code]: what the key does; n: keys the map names

static const struct { const char *name; unsigned char code; } KM_KEY_NAMES[] = {
    {"a", 0}, {"s", 1}, {"d", 2}, {"f", 3}, {"h", 4}, {"g", 5}, {"z", 6}, {"x", 7}, {"c", 8}, {"v", 9}, {"b", 11}, {"q", 12}, {"w", 13}, {"e", 14}, {"r", 15}, {"y", 16},
    {"t", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22}, {"5", 23}, {"equal", 24}, {"9", 25}, {"7", 26}, {"minus", 27}, {"8", 28}, {"0", 29}, {"rbracket", 30},
    {"o", 31}, {"u", 32}, {"lbracket", 33}, {"i", 34}, {"p", 35}, {"return", 36}, {"enter", 36}, {"l", 37}, {"j", 38}, {"quote", 39}, {"k", 40}, {"semicolon", 41},
    {"backslash", 42}, {"comma", 43}, {"slash", 44}, {"n", 45}, {"m", 46}, {"period", 47}, {"tab", 48}, {"space", 49}, {"grave", 50}, {"delete", 51}, {"escape", 53},
    {"esc", 53}, {"rcmd", 54}, {"lcmd", 55}, {"lshift", 56}, {"capslock", 57}, {"lalt", 58}, {"lctrl", 59}, {"rshift", 60}, {"ralt", 61}, {"rctrl", 62}, {"fn", 63},
    {"f5", 96}, {"f6", 97}, {"f7", 98}, {"f3", 99}, {"f8", 100}, {"f9", 101}, {"f11", 103}, {"f10", 109}, {"f12", 111}, {"home", 115}, {"pageup", 116}, {"f4", 118},
    {"end", 119}, {"f2", 120}, {"pagedown", 121}, {"f1", 122}, {"left", 123}, {"right", 124}, {"down", 125}, {"up", 126},
};
static const struct { const char *name; signed char act; } KM_CONTROL_NAMES[] = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"ls", 4}, {"rs", 5}, {"l", 6}, {"r", 7}, {"zl", 8}, {"zr", 9}, {"plus", 10}, {"+", 10}, {"minus", 11}, {"-", 11},
    {"left", 12}, {"up", 13}, {"right", 14}, {"down", 15}, {"lup", KM_LS_UP}, {"lleft", KM_LS_LEFT}, {"ldown", KM_LS_DOWN}, {"lright", KM_LS_RIGHT},
    {"rup", KM_RS_UP}, {"rleft", KM_RS_LEFT}, {"rdown", KM_RS_DOWN}, {"rright", KM_RS_RIGHT}, {"shake", KM_SHAKE}, {"none", KM_NONE},
};

static inline int km_same(const char *s, size_t n, const char *name) {   // s[0..n) is name, whatever its letter case
    if (strlen(name) != n) return 0;
    for (size_t i = 0; i < n; i++) { char a = s[i], b = name[i]; if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a'); if (a != b) return 0; }
    return 1;
}
static inline int km_key(const char *s, size_t n) {   // the code of a key name, -1 for none
    for (size_t i = 0; i < sizeof KM_KEY_NAMES / sizeof *KM_KEY_NAMES; i++) if (km_same(s, n, KM_KEY_NAMES[i].name)) return KM_KEY_NAMES[i].code;
    return -1;
}
static inline int km_control(const char *s, size_t n) {   // the action of a control name, KM_DEFAULT for none
    for (size_t i = 0; i < sizeof KM_CONTROL_NAMES / sizeof *KM_CONTROL_NAMES; i++) if (km_same(s, n, KM_CONTROL_NAMES[i].name)) return KM_CONTROL_NAMES[i].act;
    return KM_DEFAULT;
}
static inline void key_map_clear(key_map *m) { memset(m->act, KM_DEFAULT, sizeof m->act); m->n = 0; }
// The map of spec, "<key>:<control>" entries separated by commas or spaces, names in any letter case; the last entry for a key wins. Returns the keys mapped, or -1 with the
// reason in err (the entry that is wrong), the map then cleared: a wrong map is refused whole, so a typo never leaves half a layout
static inline int key_map_parse(key_map *m, const char *spec, char *err, size_t cap) {
    key_map_clear(m);
    if (cap) err[0] = 0;
    const char *p = spec ? spec : "";
    for (;;) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *e = p; while (*e && *e != ',' && *e != ' ' && *e != '\t') e++;
        const char *colon = memchr(p, ':', (size_t)(e - p));
        int code = colon ? km_key(p, (size_t)(colon - p)) : -1, act = colon ? km_control(colon + 1, (size_t)(e - colon - 1)) : KM_DEFAULT;
        if (!colon || code < 0 || act == KM_DEFAULT) {
            if (cap) snprintf(err, cap, "%s \"%.*s\" (entries are <key>:<control>, such as space:r)", !colon ? "no colon in" : code < 0 ? "no such key in" : "no such control in", (int)(e - p), p);
            key_map_clear(m); return -1;
        }
        if (m->act[code] == KM_DEFAULT) m->n++;
        m->act[code] = (signed char)act;
        p = e;
    }
    return m->n;
}
static inline int key_map_act(const key_map *m, unsigned code) { return code < KM_KEYS ? m->act[code] : KM_DEFAULT; }
