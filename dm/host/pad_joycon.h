// A Joy-Con pair as one controller (front_mac.m). A Mac has each Joy-Con as a controller of its own, and neither half can play alone: the left one has
// the d-pad, the left stick, L / ZL and − but no A / B / X / Y, the right one the face buttons, the right stick, R / ZR and + but no d-pad. Bound into one
// player they make the state a Pro Controller reports: the left half's d-pad, left stick, L / ZL and −, the right half's X / A / B / Y (by their printed
// labels, pad_apple.h), right stick, R / ZR and +. iPadOS hands a pair over as one combined controller ("Joy-Con (L/R)"), so nothing pairs there: the
// framework has already done it. Home and Capture stay system buttons (pad_keep_system_buttons in the front ends): nn::hid has no NpadButton bit for
// them and the host no system-button path, so a pair reports them as little as any other pad.
#pragma once
#import <GameController/GameController.h>
#include <math.h>
#include <stdint.h>
#include "dm_input.h"
#include "pad_apple.h"

// The half a controller is, from its names: macOS calls each Joy-Con by its side ("Joy-Con (L)", "Joy-Con (R)"; GameController's productCategory is
// "Nintendo Switch Joy-Con (L)"). A side is looked for before the combined form, so a pair whose two names disagree (a category naming both) is still
// one half. DM_JOY_WHOLE is a pair that already arrives as one controller; DM_JOY_NONE is anything else, a Joy-Con named without its side included,
// and those are used as they are
enum { DM_JOY_NONE = 0, DM_JOY_L = 1, DM_JOY_R = 2, DM_JOY_WHOLE = 3 };
static int dm_joycon_side(NSString *category, NSString *vendorName) {
    NSString *n = [NSString stringWithFormat:@"%@ %@", category ?: @"", vendorName ?: @""].lowercaseString;
    if ([n rangeOfString:@"joy-con"].location == NSNotFound && [n rangeOfString:@"joycon"].location == NSNotFound) return DM_JOY_NONE;
    if ([n rangeOfString:@"(l)"].location != NSNotFound) return DM_JOY_L;
    if ([n rangeOfString:@"(r)"].location != NSNotFound) return DM_JOY_R;
    if ([n rangeOfString:@"(l/r)"].location != NSNotFound || [n rangeOfString:@"left/right"].location != NSNotFound) return DM_JOY_WHOLE;
    return DM_JOY_NONE;
}
// A Joy-Con has one stick, and GameController reports a controller's own stick on its left stick, which is the one read. A driver that gives a right
// Joy-Con's stick on the right stick instead still plays: whichever of the two is deflected more is taken (the left one on a tie), as hle_hid.c merges
// a player's sources
static void dm_joycon_stick(GCExtendedGamepad *g, float *x, float *y) {
    float lx = g.leftThumbstick.xAxis.value, ly = g.leftThumbstick.yAxis.value;
    float rx = g.rightThumbstick.xAxis.value, ry = g.rightThumbstick.yAxis.value;
    int right = fabsf(rx) > fabsf(lx) || fabsf(ry) > fabsf(ly);
    *x = right ? rx : lx; *y = right ? ry : ly;
}
// A standard controller's state from a pair's halves, either of which may be nil (a half alone reads as what it has). Sticks are -1..1 with y up, as
// GameController has them; the front end scales them as it scales a pad's (pad_axis in front_mac.m, axis in DmGamepad.m), so a pair's sticks move as far
// and with the same feel as any controller's
typedef struct { uint64_t buttons; float lx, ly, rx, ry; } dm_joy_state;
static dm_joy_state dm_joy_pair_state(GCExtendedGamepad *l, GCExtendedGamepad *r) {
    dm_joy_state s = {0};
    if (l) {
        #define K(btn, bit) if (btn.isPressed) s.buttons |= bit
        K(l.dpad.left, DM_BTN_LEFT); K(l.dpad.up, DM_BTN_UP); K(l.dpad.right, DM_BTN_RIGHT); K(l.dpad.down, DM_BTN_DOWN);
        K(l.leftShoulder, DM_BTN_L); K(l.leftTrigger, DM_BTN_ZL); K(l.buttonOptions, DM_BTN_MINUS);
        if (l.leftThumbstickButton) K(l.leftThumbstickButton, DM_BTN_LS);   // nil on pads without a pressed stick: not pressed
        #undef K
        dm_joycon_stick(l, &s.lx, &s.ly);
    }
    if (r) {
        s.buttons |= dm_pad_face(r);   // X A B Y by their labels: right A, bottom B, top X, left Y
        #define K(btn, bit) if (btn.isPressed) s.buttons |= bit
        K(r.rightShoulder, DM_BTN_R); K(r.rightTrigger, DM_BTN_ZR); K(r.buttonMenu, DM_BTN_PLUS);
        if (r.rightThumbstickButton) K(r.rightThumbstickButton, DM_BTN_RS);
        #undef K
        dm_joycon_stick(r, &s.rx, &s.ry);
    }
    return s;
}
