// The mouse as a stick (Mac front end, front_mac.m). DM_MOUSE_STICK=left or right: a click on the game picture captures the pointer, Escape (or the window losing
// the focus) lets it go. While captured, mouse movement pushes the stick and the mouse buttons are Switch buttons; the pointer is not the touchscreen then.
// Unset or off: the mouse is the touchscreen, as before.
//   left   a reticle the left stick aims (Pokemon: Let's Go's Poke Ball throw in handheld mode: aim, then A); buttons A, B by default
//   right  the camera (mouse look); buttons ZR, ZL by default (fire, aim)
// The stick is a position, -1..1 per axis inside the unit circle: movement adds to it (DM_MOUSE_SENS: 1.0 = 100 points of movement for full deflection) and it
// goes back to the centre with a half-life of DM_MOUSE_RECENTER milliseconds (default 80; 0 = it stays where the mouse put it, a virtual stick held in place).
// DM_MOUSE_BUTTONS=<left>,<right>: the Switch buttons of the left and right mouse buttons, by DM_KEYS name (A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down).
// Pure, so tests/t_mouse_stick.c checks it without a mouse or a window.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum { MS_OFF, MS_LEFT, MS_RIGHT } ms_target;

typedef struct {
    ms_target target;
    double sens;          // stick units per point of mouse movement
    double half_life;     // seconds; 0: no recentering
    uint64_t btn_left, btn_right;   // NpadButton bits of the left and right mouse buttons
    double x, y;          // the stick, x right, y up (the Switch's), inside the unit circle
    double last;          // the time of the last ms_tick (seconds, a clock that only goes forward); < 0 before the first
} mouse_stick;

#define MS_SENS_BASE 0.01   // sensitivity 1.0: 100 points of movement are a full deflection
#define MS_HALF_LIFE_MS 80.0

// The bit of a DM_KEYS button name (the NpadButton order of dm_input.h), 0 when unknown
static inline uint64_t ms_key_bit(const char *k, size_t n) {
    static const char *const names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
    for (int b = 0; b < 16; b++) if (strlen(names[b]) == n && !strncasecmp(k, names[b], n)) return 1ull << b;
    return 0;
}

// A number, or def when unset or not a number
static inline double ms_num(const char *s, double def) {
    if (!s) return def;
    char *end; double v = strtod(s, &end);
    return end != s && !*end ? v : def;
}

// The settings from the environment's values (NULL when unset). Returns the target; an unknown value is off
static inline ms_target ms_config(mouse_stick *m, const char *target, const char *sens, const char *recenter, const char *buttons) {
    memset(m, 0, sizeof *m); m->last = -1;
    m->target = !target ? MS_OFF : !strcasecmp(target, "left") ? MS_LEFT : !strcasecmp(target, "right") ? MS_RIGHT : MS_OFF;
    double s = ms_num(sens, 1.0); if (!(s > 0) || s > 20) s = 1.0;
    m->sens = s * MS_SENS_BASE;
    double h = ms_num(recenter, MS_HALF_LIFE_MS); if (!(h >= 0) || h > 10000) h = MS_HALF_LIFE_MS;
    m->half_life = h / 1000.0;
    if (m->target == MS_LEFT) { m->btn_left = 1ull << 0; m->btn_right = 1ull << 1; }        // A, B
    else if (m->target == MS_RIGHT) { m->btn_left = 1ull << 9; m->btn_right = 1ull << 8; }  // ZR, ZL
    if (buttons) {
        const char *c = strchr(buttons, ',');
        size_t n = c ? (size_t)(c - buttons) : strlen(buttons);
        uint64_t l = ms_key_bit(buttons, n); if (l || !n) m->btn_left = l;
        if (c) { uint64_t r = ms_key_bit(c + 1, strlen(c + 1)); if (r || !c[1]) m->btn_right = r; }
    }
    return m->target;
}

// Mouse movement in points, as AppKit reports it (deltaX right, deltaY down)
static inline void ms_move(mouse_stick *m, double dx, double dy) {
    m->x += dx * m->sens; m->y -= dy * m->sens;
    double r = hypot(m->x, m->y);
    if (r > 1) { m->x /= r; m->y /= r; }
}

// The stick goes back toward the centre for the time since the last tick; returns 1 while it is deflected (the caller keeps publishing)
static inline int ms_tick(mouse_stick *m, double now) {
    if (m->last >= 0 && m->half_life > 0 && now > m->last) {
        double k = exp2(-(now - m->last) / m->half_life);
        m->x *= k; m->y *= k;
        if (hypot(m->x, m->y) < 1e-3) m->x = m->y = 0;
    }
    m->last = now;
    return m->x != 0 || m->y != 0;
}

static inline void ms_center(mouse_stick *m) { m->x = m->y = 0; m->last = -1; }

// The stick in Switch units (±32767)
static inline int32_t ms_axis(double v) { return (int32_t)lrint(fmax(-1, fmin(1, v)) * 32767); }

// One axis of two sources: whichever is deflected more (as dm_input.h merges the controller paths)
static inline int32_t ms_merge(int32_t a, int32_t b) { return abs(b) > abs(a) ? b : a; }
