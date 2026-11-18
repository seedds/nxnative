// A physical controller's face buttons as Switch buttons, shared by the Mac front end (front_mac.m) and the iOS gamepad (DmGamepad.m). GameController names most pads' face
// buttons by position, as on an Xbox pad (buttonA bottom, buttonB right, buttonX left, buttonY top), but a Nintendo pad's by their printed labels (buttonA is the right
// button, marked A): a Switch Pro Controller (or a pad that presents itself as one) and Joy-Con (productCategory "Nintendo Switch Joy-Con (L)", "(R)", "(L/R)").
// The Switch's layout is right A, bottom B, top X, left Y, so a Nintendo pad maps name for name and any other pad by position. Checked with a Pro Controller on the Mac
// and on iOS (PR #650). Open: a single Joy-Con held sideways is matched too and has not been tried; if GameController keeps its labels there instead of turning them
// with the controller, its face buttons come out a quarter turn off.
// DM_PAD_MAP=label maps the other pads by label instead, as PC games read an Xbox pad: GameController's buttonA (Xbox A, the bottom button; PlayStation cross) is the Switch's A, buttonB
// (right; circle) its B, buttonX (left; square) its X, buttonY (top; triangle) its Y. Confirm moves to the bottom button and the letters the game shows are the pad's; the game's layout
// (A right of B, X above Y) is lost. A Nintendo pad maps name for name either way, so does the keyboard (front_mac.m); raw HID pads keep their own map (pad_hid.h, DM_HID_MAP).
#pragma once
#import <GameController/GameController.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

// DM_PAD_MAP: 0 by position (the default), 1 by label; read once
static inline int dm_pad_by_label(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PAD_MAP"); v = e && !strcmp(e, "label"); }
    return v;
}

static inline int dm_pad_labelled(GCController *c) {
    NSString *k = c.productCategory;
    return [k isEqualToString:@"Switch Pro Controller"] || [k containsString:@"Joy-Con"];
}
// DM_BTN_A / B / X / Y of the pad's pressed face buttons
static inline uint64_t dm_pad_face(GCExtendedGamepad *g) {
    GCControllerButtonInput *a = g.buttonA, *b = g.buttonB, *x = g.buttonX, *y = g.buttonY;
    if (!dm_pad_labelled(g.controller) && !dm_pad_by_label()) { GCControllerButtonInput *t = a; a = b; b = t; t = x; x = y; y = t; }   // by position: right A, bottom B, top X, left Y
    return (a.isPressed ? DM_BTN_A : 0) | (b.isPressed ? DM_BTN_B : 0) | (x.isPressed ? DM_BTN_X : 0) | (y.isPressed ? DM_BTN_Y : 0);
}
