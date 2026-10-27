// Player slots of the physical controllers (front_mac.m; pure, so tests/t_hid.c checks it without controllers): each controller holds one slot, slot p is player p + 1
// (hle_hid.c dm_player_npad gives its npad). A connecting controller takes, in this order: a free slot whose last holder had its name (the same controller reconnected gets its
// player back; GameController gives a controller no identity across a reconnect, so two controllers of one model may trade places), player 1 when it is free, the lowest slot
// no controller has held yet (so a slot stays kept for a controller that went away while others are in use), then the lowest free slot. With every slot held it gets none (-1).
// A disconnected controller frees its slot and leaves its name on it. The owner is any pointer that stays the same while the controller is connected (the front end keeps the
// GCController); a holder that never disconnects (the keyboard with DM_KBD_PLAYER=own) takes slot 0 first and keeps it.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "dm_input.h"

typedef struct { const void *owner[DM_PLAYERS]; char last[DM_PLAYERS][64]; } pad_slots;

// The stick dead zone (DM_PAD_STICK_DZ=<inner>[,<full>], front_mac.m): a stick within <inner> of the centre (a fraction of full tilt) is 0; past it, its length is clamped to the
// unit circle (GameController gives each axis up to ±1 on its own, so a diagonal came out longer than full tilt) and scaled so a tilt of <full> already counts as full. A worn stick
// that rests a little off centre stops drifting, and one whose full tilt falls short of 1.0 (GameController's full tilt is not always 1.0) still reaches the game's full speed
static void pad_stick_shape(int32_t *x, int32_t *y, double inner, double full) {
    double m = sqrt((double)*x * *x + (double)*y * *y) / DM_STICK_MAX;
    if (m <= inner) { *x = *y = 0; return; }
    double k = 1 / full < 1 / m ? 1 / full : 1 / m;
    double fx = *x * k, fy = *y * k;
    *x = (int32_t)lround(fx > DM_STICK_MAX ? DM_STICK_MAX : fx < -DM_STICK_MAX ? -DM_STICK_MAX : fx);
    *y = (int32_t)lround(fy > DM_STICK_MAX ? DM_STICK_MAX : fy < -DM_STICK_MAX ? -DM_STICK_MAX : fy);
}
// A Nintendo controller's face buttons by their printed letters: GameController names its buttonA / B / X / Y by position (buttonA bottom, buttonB right), but a real Pro Controller
// came through with its own letters in those places (buttonA = its A, on the right), so mapped by position its A was the game's B. letters[i] is the letter GameController gives
// buttonA / B / X / Y (from the element's localized name); with all four distinct the buttons go by letter, otherwise (0) by position. → the NpadButton bit for button i
static uint64_t pad_face_bit(const char letters[4], int i) {
    static const uint64_t by_pos[4] = {DM_BTN_B, DM_BTN_A, DM_BTN_Y, DM_BTN_X};   // buttonA bottom = B, buttonB right = A, buttonX left = Y, buttonY top = X
    if (!letters || !letters[0]) return by_pos[i];
    switch (letters[i]) { case 'A': return DM_BTN_A; case 'B': return DM_BTN_B; case 'X': return DM_BTN_X; case 'Y': return DM_BTN_Y; }
    return by_pos[i];
}

static int pad_slot_of(const pad_slots *s, const void *owner) {
    for (int i = 0; owner && i < DM_PLAYERS; i++) if (s->owner[i] == owner) return i;
    return -1;
}
static int pad_slot_take(pad_slots *s, const void *owner, const char *name) {
    int p = pad_slot_of(s, owner); if (p >= 0 || !owner) return p;
    if (!name) name = "";
    for (int i = 0; p < 0 && *name && i < DM_PLAYERS; i++) if (!s->owner[i] && !strcmp(s->last[i], name)) p = i;
    if (p < 0 && !s->owner[0]) p = 0;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i] && !s->last[i][0]) p = i;
    for (int i = 0; p < 0 && i < DM_PLAYERS; i++) if (!s->owner[i]) p = i;
    if (p < 0) return -1;
    s->owner[p] = owner; snprintf(s->last[p], sizeof s->last[p], "%s", name);
    return p;
}
static int pad_slot_free(pad_slots *s, const void *owner) {
    int p = pad_slot_of(s, owner); if (p >= 0) s->owner[p] = NULL;
    return p;
}
static uint32_t pad_slot_mask(const pad_slots *s) {   // bit p: slot p is held
    uint32_t m = 0; for (int i = 0; i < DM_PLAYERS; i++) if (s->owner[i]) m |= 1u << i;
    return m;
}
