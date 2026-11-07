// Mouse or trackpad as the right stick (DM_MOUSE=1, the launcher's Mouse: Camera), shared by the Mac front end (front_mac.m) and the iPad's (DmGamepad.m).
// While the pointer is captured (locked to the game: a click on the picture takes it, Esc gives it back) its movement is the right stick, which turns the camera in a 3D
// game, and its buttons press the profile's mouse_buttons (left, right, middle; Breath of the Wild: Y attack, ZL lock on, R3). A Switch game reads a stick position once
// a frame and turns the camera by it, so the movement becomes a deflection: each report's speed (its movement over the time since the one before, so a 125 Hz mouse,
// a 1000 Hz one and a trackpad give the same speed for the same hand), smoothed over ML_TAU ms on ML_TICK ms ticks, full tilt at ML_FULL points a second times
// DM_MOUSE_SENS, past a small offset (DM_MOUSE_DEAD, 0.12) that clears the game's own dead zone, so slow aiming still turns. Once reports stop for more than two and a half
// report intervals (at least 12 ms) the speed falls to 0 the same way. Main thread only (AppKit events, GameController's handlers on the main queue)
#pragma once
#include <dispatch/dispatch.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

enum { ML_TICK = 2 };   // ms per tick
#define ML_TAU 4.0
#define ML_FULL 1000.0
#define ML_SPAN 0.004   // seconds: the least time a speed is measured over (a 1000 Hz mouse's reports in fours)

typedef void (*ml_out_fn)(int32_t rx, int32_t ry);
static struct {
    ml_out_fn out;
    dispatch_source_t timer; int running;
    uint64_t last_ns;                    // the last report
    uint64_t span_ns; double ax, ay;     // the movement since span_ns not yet turned into a speed (reports less than ML_SPAN apart are taken together)
    double dt_est;                       // seconds between reports, a running estimate
    double tx, ty;                       // the last report's speed (points a second, y up)
    double vx, vy;                       // the smoothed speed
    double sens, dead;
    int32_t rx, ry;
} g_ml = { .dt_est = 0.008 };

static uint64_t ml_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static double ml_env(const char *k, double def, double lo, double hi) {
    const char *e = getenv(k); double v = e && *e ? atof(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}
static void ml_settings(void) { g_ml.sens = ml_env("DM_MOUSE_SENS", 1.0, 0.05, 10); g_ml.dead = ml_env("DM_MOUSE_DEAD", 0.12, 0, 0.5); }
static void ml_emit(int32_t rx, int32_t ry) { if (rx == g_ml.rx && ry == g_ml.ry) return; g_ml.rx = rx; g_ml.ry = ry; if (g_ml.out) g_ml.out(rx, ry); }
static void ml_sleep(void) { if (g_ml.running) { dispatch_suspend(g_ml.timer); g_ml.running = 0; } }
static void ml_tick(void) {
    double gap = (ml_now() - g_ml.last_ns) / 1e9, hold = fmax(2.5 * g_ml.dt_est, 0.012);
    if (gap > hold) g_ml.tx = g_ml.ty = 0;   // the reports stopped
    double a = 1 - exp(-ML_TICK / ML_TAU);
    g_ml.vx += (g_ml.tx - g_ml.vx) * a; g_ml.vy += (g_ml.ty - g_ml.vy) * a;
    double v = hypot(g_ml.vx, g_ml.vy);
    if (!g_ml.tx && !g_ml.ty && v * g_ml.sens < ML_FULL * 0.02) { g_ml.vx = g_ml.vy = 0; ml_emit(0, 0); ml_sleep(); return; }   // at rest: the timer sleeps until the next report
    double d = v * g_ml.sens / ML_FULL; d = g_ml.dead + (1 - g_ml.dead) * (d > 1 ? 1 : d);
    ml_emit((int32_t)lrint(g_ml.vx / v * d * DM_STICK_MAX), (int32_t)lrint(g_ml.vy / v * d * DM_STICK_MAX));
}
static void ml_init(ml_out_fn out) {
    g_ml.out = out; ml_settings();
    g_ml.timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(g_ml.timer, DISPATCH_TIME_NOW, ML_TICK * NSEC_PER_MSEC, NSEC_PER_MSEC / 4);
    dispatch_source_set_event_handler(g_ml.timer, ^{ ml_tick(); });   // created suspended
}
static void ml_move(double dx, double dy_up) {   // one report's movement in points, y up
    if (!g_ml.timer) return;
    uint64_t now = ml_now(); double dt = (now - g_ml.last_ns) / 1e9;
    if (!g_ml.last_ns || dt > 0.05) { g_ml.span_ns = now - (uint64_t)(fmax(g_ml.dt_est, ML_SPAN) * 1e9); g_ml.ax = g_ml.ay = 0; }   // the first report after a pause: over the usual interval
    else g_ml.dt_est += (fmin(dt, 0.033) - g_ml.dt_est) * 0.2;
    g_ml.last_ns = now; g_ml.ax += dx; g_ml.ay += dy_up;
    double span = (now - g_ml.span_ns) / 1e9;
    if (span >= ML_SPAN) { g_ml.tx = g_ml.ax / span; g_ml.ty = g_ml.ay / span; g_ml.ax = g_ml.ay = 0; g_ml.span_ns = now; }
    if (!g_ml.running) { g_ml.running = 1; dispatch_resume(g_ml.timer); }
}
static void ml_stop(void) {   // capture lost, focus lost: the stick at rest
    if (!g_ml.timer) return;
    g_ml.tx = g_ml.ty = g_ml.vx = g_ml.vy = g_ml.ax = g_ml.ay = 0; g_ml.last_ns = 0; ml_sleep(); ml_emit(0, 0);
}
// The mouse's left, right and middle buttons as DM_BTN_* bits: DM_MOUSE_BUTTONS, else the profile's mouse_buttons (game.py DM_GAME_MOUSE_BUTTONS), names as on a Switch
// pad (A B X Y L R ZL ZR + - L3 R3 LEFT UP RIGHT DOWN, or - for none), separated by spaces
static void ml_buttons(const char *def, uint64_t out[3]) {
    static const struct { const char *n; uint64_t b; } T[] = {
        {"A", DM_BTN_A}, {"B", DM_BTN_B}, {"X", DM_BTN_X}, {"Y", DM_BTN_Y}, {"L3", DM_BTN_LS}, {"R3", DM_BTN_RS}, {"L", DM_BTN_L}, {"R", DM_BTN_R}, {"ZL", DM_BTN_ZL},
        {"ZR", DM_BTN_ZR}, {"+", DM_BTN_PLUS}, {"-", 0}, {"MINUS", DM_BTN_MINUS}, {"LEFT", DM_BTN_LEFT}, {"UP", DM_BTN_UP}, {"RIGHT", DM_BTN_RIGHT}, {"DOWN", DM_BTN_DOWN}};
    const char *e = getenv("DM_MOUSE_BUTTONS"); char s[64]; snprintf(s, sizeof s, "%s", e && *e ? e : def);
    out[0] = out[1] = out[2] = 0; int i = 0;
    for (char *save = NULL, *w = strtok_r(s, " ,", &save); w && i < 3; w = strtok_r(NULL, " ,", &save), i++)
        for (size_t k = 0; k < sizeof T / sizeof *T; k++) if (!strcasecmp(w, T[k].n)) { out[i] = T[k].b; break; }
}
