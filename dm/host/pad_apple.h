// A physical controller's face buttons as Switch buttons, shared by the Mac front end (front_mac.m) and the iOS gamepad (DmGamepad.m). GameController names most pads' face
// buttons by position, as on an Xbox pad (buttonA bottom, buttonB right, buttonX left, buttonY top), but a Nintendo pad's by their printed labels (buttonA is the right
// button, marked A): a Switch Pro Controller (or a pad that presents itself as one) and Joy-Con (productCategory "Nintendo Switch Joy-Con (L)", "(R)", "(L/R)").
// The Switch's layout is right A, bottom B, top X, left Y, so a Nintendo pad maps name for name and any other pad by position. Checked with a Pro Controller on the Mac
// and on iOS (PR #650). Open: a single Joy-Con held sideways is matched too and has not been tried; if GameController keeps its labels there instead of turning them
// with the controller, its face buttons come out a quarter turn off.
// DM_PAD_LAYOUT=name maps every pad name for name instead, for a player used to an Xbox pad's letters (issue #2170); the Home hold prompt reads GameController's names
// either way.
#pragma once
#import <GameController/GameController.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dm_input.h"

static inline int dm_pad_labelled(GCController *c) {
    NSString *k = c.productCategory;
    return [k isEqualToString:@"Switch Pro Controller"] || [k containsString:@"Joy-Con"];
}
// DM_BTN_A / B / X / Y of the pad's pressed face buttons; by_name maps every pad name for name, as a Nintendo pad
static inline uint64_t dm_pad_face_as(GCExtendedGamepad *g, int by_name) {
    GCControllerButtonInput *a = g.buttonA, *b = g.buttonB, *x = g.buttonX, *y = g.buttonY;
    if (!by_name && !dm_pad_labelled(g.controller)) { GCControllerButtonInput *t = a; a = b; b = t; t = x; x = y; y = t; }   // by position: right A, bottom B, top X, left Y
    return (a.isPressed ? DM_BTN_A : 0) | (b.isPressed ? DM_BTN_B : 0) | (x.isPressed ? DM_BTN_X : 0) | (y.isPressed ? DM_BTN_Y : 0);
}
// DM_PAD_LAYOUT=name: whether every pad maps name for name (the button marked A on an Xbox pad, the bottom one, is the game's A); read once. Anything else is by position
static inline int dm_pad_by_name(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PAD_LAYOUT"); v = e && !strcmp(e, "name"); }
    return v;
}
static inline uint64_t dm_pad_face(GCExtendedGamepad *g) { return dm_pad_face_as(g, dm_pad_by_name()); }
