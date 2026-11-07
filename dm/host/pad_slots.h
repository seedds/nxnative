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
#include "dm_input.h"

typedef struct { const void *owner[DM_PLAYERS]; char last[DM_PLAYERS][64]; } pad_slots;

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
// DM_PAD_PRESS_TO_JOIN=1 (front_mac.m): a controller takes the lowest free slot at its first button press, whatever slot its name held before
static int pad_slot_take_lowest(pad_slots *s, const void *owner, const char *name) {
    int p = pad_slot_of(s, owner); if (p >= 0 || !owner) return p;
    for (int i = 0; i < DM_PLAYERS; i++) if (!s->owner[i]) { s->owner[i] = owner; snprintf(s->last[i], sizeof s->last[i], "%s", name ? name : ""); return i; }
    return -1;
}
static int pad_slot_free(pad_slots *s, const void *owner) {
    int p = pad_slot_of(s, owner); if (p >= 0) s->owner[p] = NULL;
    return p;
}
// After a disconnect: player 1 free and exactly one controller left, in another slot → it moves to slot 0, its name with it, and the old slot is returned (-1 when nothing
// moved). A Bluetooth pad that reconnects can show up again under another name (an Xbox Wireless Controller as "Controller") before its old connection is gone, so it took
// player 2 while player 1 was still held, and then player 1 went free: a game that reads player 1 only (Star Fox) saw no input until a restart. With two or more controllers
// left nobody moves, so the players of a multiplayer game keep their places
static int pad_slot_settle(pad_slots *s) {
    if (s->owner[0]) return -1;
    int held = -1;
    for (int i = 1; i < DM_PLAYERS; i++) if (s->owner[i]) { if (held >= 0) return -1; held = i; }
    if (held < 0) return -1;
    s->owner[0] = s->owner[held]; s->owner[held] = NULL;
    memcpy(s->last[0], s->last[held], sizeof s->last[0]); s->last[held][0] = 0;
    return held;
}
static uint32_t pad_slot_mask(const pad_slots *s) {   // bit p: slot p is held
    uint32_t m = 0; for (int i = 0; i < DM_PLAYERS; i++) if (s->owner[i]) m |= 1u << i;
    return m;
}
