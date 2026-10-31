// A physical controller's face buttons as Switch buttons, shared by the Mac front end (front_mac.m) and the iOS gamepad (DmGamepad.m). GameController names most pads' face
// buttons by position, as on an Xbox pad (buttonA bottom, buttonB right, buttonX left, buttonY top), but a Nintendo pad's by their printed labels (buttonA is the right
// button, marked A): a Switch Pro Controller (or a pad that presents itself as one) and Joy-Con (productCategory "Nintendo Switch Joy-Con (L)", "(R)", "(L/R)").
// The Switch's layout is right A, bottom B, top X, left Y, so a Nintendo pad maps name for name and any other pad by position. Checked with a Pro Controller on the Mac
// and on iOS (PR #650). Open: a single Joy-Con held sideways is matched too and has not been tried; if GameController keeps its labels there instead of turning them
// with the controller, its face buttons come out a quarter turn off.
#pragma once
#import <GameController/GameController.h>
#include <stdint.h>
#include "dm_input.h"

static inline int dm_pad_labelled(GCController *c) {
    NSString *k = c.productCategory;
    return [k isEqualToString:@"Switch Pro Controller"] || [k containsString:@"Joy-Con"];
}
// DM_BTN_A / B / X / Y of the pad's pressed face buttons
// DM_PAD_AB=xbox (ab_xbox) puts A at the bottom and B on the right (Xbox / PlayStation), overriding Nintendo's A right / B bottom
static inline uint64_t dm_pad_face_ab(GCExtendedGamepad *g, int ab_xbox) {
    GCControllerButtonInput *a = g.buttonA, *b = g.buttonB, *x = g.buttonX, *y = g.buttonY;
    int labelled = dm_pad_labelled(g.controller);
    if (!labelled) { GCControllerButtonInput *t = x; x = y; y = t; }   // by position: top X, left Y
    int swap_ab = ab_xbox ? labelled : !labelled;
    if (swap_ab) { GCControllerButtonInput *t = a; a = b; b = t; }   // Nintendo default: right A, bottom B; xbox: bottom A, right B
    return (a.isPressed ? DM_BTN_A : 0) | (b.isPressed ? DM_BTN_B : 0) | (x.isPressed ? DM_BTN_X : 0) | (y.isPressed ? DM_BTN_Y : 0);
}
static inline uint64_t dm_pad_face(GCExtendedGamepad *g) {
    return dm_pad_face_ab(g, 0);
}
