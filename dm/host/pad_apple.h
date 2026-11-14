// A physical controller's face buttons as Switch buttons, shared by the Mac front end (front_mac.m) and the iOS gamepad (DmGamepad.m). GameController names most pads' face
// buttons by position, as on an Xbox pad (buttonA bottom, buttonB right, buttonX left, buttonY top), but a Nintendo pad's by their printed labels (buttonA is the right
// button, marked A): a Switch Pro Controller (or a pad that presents itself as one) and Joy-Con (productCategory "Nintendo Switch Joy-Con (L)", "(R)", "(L/R)").
// The Switch's layout is right A, bottom B, top X, left Y, so a Nintendo pad maps name for name and any other pad by position. Checked with a Pro Controller on the Mac
// and on iOS (PR #650). Open: a single Joy-Con held sideways is matched too and has not been tried; if GameController keeps its labels there instead of turning them
// with the controller, its face buttons come out a quarter turn off.
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
// DM_PAD_LAYOUT=xbox (the launcher's Button layout, "Xbox / PS labels"): any other pad by its printed labels instead, bottom A, right B, left X, top Y, so the letters
// the game shows are the ones printed on an Xbox pad; a PS pad takes the same places (Cross A, Circle B, Square X, Triangle Y). The default, position, keeps the Switch's
// places. From the FC 26 120 fps patch by @imnotsharingmyname (MIT)
static inline int dm_pad_label_layout(void) { const char *e = getenv("DM_PAD_LAYOUT"); return e && !strcmp(e, "xbox"); }
// DM_BTN_A / B / X / Y of the pad's pressed face buttons
static inline uint64_t dm_pad_face(GCExtendedGamepad *g) {
    GCControllerButtonInput *a = g.buttonA, *b = g.buttonB, *x = g.buttonX, *y = g.buttonY;
    if (!dm_pad_labelled(g.controller) && !dm_pad_label_layout()) { GCControllerButtonInput *t = a; a = b; b = t; t = x; x = y; y = t; }   // by position: right A, bottom B, top X, left Y
    return (a.isPressed ? DM_BTN_A : 0) | (b.isPressed ? DM_BTN_B : 0) | (x.isPressed ? DM_BTN_X : 0) | (y.isPressed ? DM_BTN_Y : 0);
}
