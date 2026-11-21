// Mouse look and mouse buttons for the Mac keyboard map (keymap.h MOUSERSTICK, MOUSELSTICK, MOUSEMOTION; mouse buttons as keys 128..132), through keyboard_mac.h's event monitor
// (ml_event, ML_EVENTS). With an output's source set, a left click on the game captures the pointer: it is hidden and held in place, and its movement (a mouse's or the trackpad's)
// goes to one output at a time (keymap.h km_mouse_out: the output whose key is held, else the one set to the plain mouse). A stick (km_state mrx / mry, mlx / mly, which km_sticks
// takes where the keys push it less): A game reads a stick as a rate (a camera turning), not a position, so the speed of the movement is the
// stick's deflection: it is measured 120 times a second, smoothed, and lifted past a typical dead zone so slow movement still turns. While captured every mouse button is
// a key of the map (click, rclick, ...). Esc, the window losing focus and the Keyboard Map panel let the pointer go.
// The motion controls (MOUSEMOTION): the movement turns player 1's motion sensor (at MOUSEMOTIONSPEED), as a controller turned in the hands: horizontal is a turn about the world's vertical, vertical
// tilts the controller up and down (held within ±70° of its rest), and its gravity follows the tilt. At rest it is held as the game's mode expects: in handheld mode
// (DM_OPMODE=0) the console stands upright facing the player, so turning is about its y axis (a game reads aim from that axis: lying flat, OoT's turned barely at all); in
// console mode a controller lies flat, turning about z. It goes in as the Switch-frame stream the iPhone's own sensor uses
// (dm_motion_set_switch), which the host reads for player 1 while no controller sends motion; it goes stale 0.3 s after the pointer is let go. With MOUSEMOTION set the sensor
// keeps reporting (still) while the movement goes to a stick (Space+Mouse: the motion controls only while Space is held), so the game never loses it. Without capture a mouse button that is bound is the
// game's when it goes down on the picture; the others stay the touch screen's (DmGameView). Included by keyboard_mac.h (the map, the state, kb_push, kb_on_key; the panel's kp_click declared) after front_mac.m (the window).
#pragma once
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include "keymap.h"
#include "dm_input.h"

#define ML_EVENTS (NSEventMaskMouseMoved | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown | \
                   NSEventMaskOtherMouseUp | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged)
#define ML_FULL 600.0   // points per second of pointer movement that hold the stick full over at speed 1
#define ML_DEAD 0.18    // the least deflection while the pointer moves: past the dead zone most games give their sticks
#define ML_GYRO 0.0025  // radians the motion sensor turns per point of pointer movement at speed 1

static void kb_on_key(unsigned short c, int down, int mods);   // keyboard_mac.h
static int g_ml_on;                       // the pointer is captured
static double g_ml_dx, g_ml_dy;           // movement since the last tick, in points (y down)
static double g_ml_sx, g_ml_sy, g_ml_last;   // the smoothed stick (-1..1 and more), the last tick's time
static double g_ml_wp, g_ml_wy;           // motion aiming: the smoothed tilt and turn rates, rad/s
static double g_ml_pitch, g_ml_rest;      // motion aiming: the virtual controller's tilt from its rest, radians (up positive); the rest's tilt from flat (upright: π/2)
static NSTimer *g_ml_timer;
static unsigned g_ml_taken;               // buttons whose press went to the game without capture: their drags and release are the game's too
static int g_ml_esc;                      // the Esc that let the pointer go: its release is swallowed too

static void ml_tick(void) {
    double now = CACurrentMediaTime(), dt = now - g_ml_last; g_ml_last = now;
    if (dt <= 0 || dt > 0.1) dt = 1.0 / 120;
    int out = km_mouse_out(&g_km, &g_ks);   // where the movement goes now: a held key's output, else the plain mouse's
    int sensor = g_km.mouse_src[KM_OUT_MOTION] != KM_SRC_OFF;   // the sensor reports, still while the movement goes to a stick
    double gx = out == KM_OUT_MOTION ? g_ml_dx : 0, gy = out == KM_OUT_MOTION ? g_ml_dy : 0;
    if (sensor) {   // rad/s in the Switch frame (x right, y forward, z out of the face): the tilt about x, the turn about the world's vertical, which tilted is (0, sin, cos)
        // The pointer reports at its own uneven rate (a trackpad about 60-90 Hz), so a tick's raw rate jumps between none and double: smoothed (a low-pass of gain 1, so the angle
        // turned in all stays the movement's), and the tilt follows the smoothed rate, the gyro and the gravity agreeing
        double kg = g_km.mouse_speed[KM_OUT_MOTION] * ML_GYRO, was = g_ml_pitch, inv = g_km.mouse_invert[KM_OUT_MOTION] ? -1 : 1;
        g_ml_wp += (-gy * kg * inv / dt - g_ml_wp) * 0.3; g_ml_wy += (-gx * kg / dt - g_ml_wy) * 0.3;
        if (fabs(g_ml_wp) < 1e-4) g_ml_wp = 0;
        if (fabs(g_ml_wy) < 1e-4) g_ml_wy = 0;
        g_ml_pitch = fmax(fmin(g_ml_pitch + g_ml_wp * dt, 1.2), -1.2);
        double wp = (g_ml_pitch - was) / dt, wy = g_ml_wy, sp = sin(g_ml_rest + g_ml_pitch), cp = cos(g_ml_rest + g_ml_pitch);
        float acc[3] = {0, (float)-sp, (float)-cp}, rot[3] = {(float)wp, (float)(sp * wy), (float)(cp * wy)};
        dm_motion_set_switch(0, acc, rot);
    }
    static int stick_was = -1;   // the stick of the last tick (KM_OUT_RSTICK, LSTICK, -1 none): a change lets it go and starts the smoothing again
    int stick = out == KM_OUT_RSTICK || out == KM_OUT_LSTICK ? out : -1;
    if (stick != stick_was) { g_ml_sx = g_ml_sy = 0; g_ks.mrx = g_ks.mry = g_ks.mlx = g_ks.mly = 0; kb_push(); stick_was = stick; }
    double k = stick >= 0 ? g_km.mouse_speed[stick] / ML_FULL : 0, tx = g_ml_dx / dt * k, ty = -g_ml_dy / dt * k * (stick >= 0 && g_km.mouse_invert[stick] ? -1 : 1);
    g_ml_dx = g_ml_dy = 0;
    g_ml_sx += (tx - g_ml_sx) * 0.4; g_ml_sy += (ty - g_ml_sy) * 0.4;
    double m = hypot(g_ml_sx, g_ml_sy); int32_t x = 0, y = 0;
    if (m > 0.02) { double o = (ML_DEAD + (1 - ML_DEAD) * fmin(m, 1)) / m * 32767; x = (int32_t)lrint(fmax(fmin(g_ml_sx * o, 32767), -32767)); y = (int32_t)lrint(fmax(fmin(g_ml_sy * o, 32767), -32767)); }
    else g_ml_sx = g_ml_sy = 0;
    int32_t *sx = stick == KM_OUT_LSTICK ? &g_ks.mlx : &g_ks.mrx, *sy = stick == KM_OUT_LSTICK ? &g_ks.mly : &g_ks.mry;
    if (x != *sx || y != *sy) { *sx = x; *sy = y; kb_push(); }
}

static void ml_release(void) {
    if (!g_ml_on) return;
    g_ml_on = 0; [g_ml_timer invalidate]; g_ml_timer = nil;
    CGAssociateMouseAndMouseCursorPosition(true); [NSCursor unhide];
    for (int b = 0; b < KM_MOUSE_N; b++) kb_on_key((unsigned short)(KM_MOUSE + b), 0, 0);   // a button held while captured goes up
    g_ml_sx = g_ml_sy = 0; g_ks.mrx = g_ks.mry = g_ks.mlx = g_ks.mly = 0; kb_push();
    LOG("front end: mouse look: the pointer is free again");
}

static void ml_capture(void) {
    if (g_ml_on) return;
    static int once;
    if (!once) {
        once = 1; NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidResignKeyNotification object:g_win queue:nil usingBlock:^(NSNotification *n) { (void)n; ml_release(); }];
        [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; ml_release(); }];
    }
    g_ml_on = 1; g_ml_taken = 0;
    g_win.acceptsMouseMovedEvents = YES;
    [NSCursor hide]; CGAssociateMouseAndMouseCursorPosition(false);
    g_ml_dx = g_ml_dy = g_ml_sx = g_ml_sy = g_ml_pitch = g_ml_wp = g_ml_wy = 0; g_ml_last = CACurrentMediaTime();
    const char *op = getenv("DM_OPMODE"); g_ml_rest = op && !strcmp(op, "0") ? M_PI_2 : 0;   // handheld: upright
    g_ml_timer = [NSTimer timerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *t) { (void)t; ml_tick(); }];
    [NSRunLoop.mainRunLoop addTimer:g_ml_timer forMode:NSRunLoopCommonModes];
    char srcs[256]; size_t u = 0; srcs[0] = 0;
    for (int o = 0; o < KM_OUTS; o++) if (g_km.mouse_src[o] != KM_SRC_OFF && u < sizeof srcs) {
        char l[64]; int w = snprintf(srcs + u, sizeof srcs - u, "%s%s: %s", u ? ", " : "", km_out_labels[o], km_src_label(g_km.mouse_src[o], l, sizeof l)); if (w > 0) u += (size_t)w;
    }
    LOG("front end: mouse look: the pointer is captured: %s (Esc lets it go)", srcs);
}

// The event monitor's (front_mac.m): 0 not ours (the monitor goes on), 1 swallow it, 2 let it through untouched
static int ml_event(NSEvent *e) {
    NSEventType t = e.type;
    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) {
        if (e.keyCode != 53) return 0;
        if (t == NSEventTypeKeyDown && g_ml_on) { ml_release(); g_ml_esc = 1; return 1; }
        if (t == NSEventTypeKeyUp && g_ml_esc) { g_ml_esc = 0; return 1; }
        return 0;
    }
    if (t == NSEventTypeFlagsChanged) return 0;
    int moved = t == NSEventTypeMouseMoved || t == NSEventTypeLeftMouseDragged || t == NSEventTypeRightMouseDragged || t == NSEventTypeOtherMouseDragged;
    int down = t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown || t == NSEventTypeOtherMouseDown;
    int b = t == NSEventTypeMouseMoved ? -1 : (int)e.buttonNumber;
    if (g_ml_on) {
        if (moved) { g_ml_dx += e.deltaX; g_ml_dy += e.deltaY; }
        else if (b >= 0 && b < KM_MOUSE_N) kb_on_key((unsigned short)(KM_MOUSE + b), down, 0);
        return 1;
    }
    unsigned bit = b >= 0 && b < 32 ? 1u << b : 0;
    if (!down) {   // a move, a drag or a release: the game's when it had the press
        if (!(g_ml_taken & bit)) return 2;
        if (!moved) { g_ml_taken &= ~bit; if (b < KM_MOUSE_N) kb_on_key((unsigned short)(KM_MOUSE + b), 0, 0); }
        return 1;
    }
    // the Keyboard Map panel waits for a key: a press of any mouse button in the app's windows (the panel's own too) is that input, as a key is, Shift + a click a chord;
    // a window's frame (title bar, close button, edges) stays AppKit's
    int kc = kp_click(e);
    if (kc == -1) return 2;
    if (kc == 1) {
        if (b < KM_MOUSE_N) { g_ml_taken |= bit; kb_on_key((unsigned short)(KM_MOUSE + b), 1, kb_mods(e.modifierFlags)); } else NSBeep();
        return 1;
    }
    if (e.window != g_win || !g_input_on) return 2;
    NSView *v = g_win.contentView;
    if (!NSPointInRect([v convertPoint:e.locationInWindow fromView:nil], v.bounds)) return 2;   // the title bar and the window's edges stay AppKit's
    if (b == 0 && km_mouse_on(&g_km)) { ml_capture(); return 1; }
    int mods = kb_mods(e.modifierFlags);   // a bound button, or a bound chord of it (Shift + click): the game's; else the touch screen's
    if (b < KM_MOUSE_N && g_km.key[km_resolve(&g_km, KM_MOUSE + b, km_mods_held(&g_ks, mods))] != KM_NONE) { g_ml_taken |= bit; kb_on_key((unsigned short)(KM_MOUSE + b), 1, mods); return 1; }
    return 2;
}
