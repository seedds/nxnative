// A full controller used as one upright Joy-Con. The primary (left) stick drives
// that Joy-Con's stick, shoulders supply SL/SR, and triggers supply its outer shoulder
// and trigger. These are raw Npad bits, not a configurable button-remapping editor.
#pragma once
#include "dm_input.h"

// The game interprets the single Joy-Con's raw controls in its held orientation.
// Express a full pad's normal grip in that raw frame, with the virtual rail at
// the top. Left and right rails start on opposite sides, so the turns differ.
static inline void pad_sideways_input(uint32_t style, uint64_t *buttons, int32_t axes[4]) {
    if (style != DM_STYLE_LEFT && style != DM_STYLE_RIGHT) return;
    static const uint64_t face[] = {DM_BTN_A, DM_BTN_X, DM_BTN_Y, DM_BTN_B};
    static const uint64_t dpad[] = {DM_BTN_RIGHT, DM_BTN_UP, DM_BTN_LEFT, DM_BTN_DOWN};
    int turn = style == DM_STYLE_LEFT ? 3 : 1;
    const uint64_t directions = DM_BTN_A | DM_BTN_X | DM_BTN_Y | DM_BTN_B |
        DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_LEFT | DM_BTN_DOWN;
    uint64_t in = *buttons, out = in & ~directions;
    for (int i = 0; i < 4; i++) {
        if (in & face[i]) out |= face[(i + turn) % 4];
        if (in & dpad[i]) out |= dpad[(i + turn) % 4];
    }
    *buttons = out;
    int32_t x = axes[0];
    axes[0] = style == DM_STYLE_LEFT ? axes[1] : -axes[1];
    axes[1] = style == DM_STYLE_LEFT ? -x : x;
}

static inline void pad_style_input(uint32_t style, uint64_t *buttons, int32_t axes[4]) {
    uint64_t b = *buttons, out;
    if (style == DM_STYLE_LEFT) {
        out = b & (DM_BTN_LEFT | DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN | DM_BTN_ZL | DM_BTN_LEFT_SL | DM_BTN_LEFT_SR);
        if (b & DM_BTN_A) out |= DM_BTN_RIGHT;
        if (b & DM_BTN_B) out |= DM_BTN_DOWN;
        if (b & DM_BTN_X) out |= DM_BTN_UP;
        if (b & DM_BTN_Y) out |= DM_BTN_LEFT;
        if (b & DM_BTN_L) out |= DM_BTN_LEFT_SL;
        if (b & DM_BTN_R) out |= DM_BTN_LEFT_SR;
        if (b & DM_BTN_ZR) out |= DM_BTN_L;
        if (b & (DM_BTN_PLUS | DM_BTN_MINUS)) out |= DM_BTN_MINUS;
        if (b & (DM_BTN_LS | DM_BTN_RS)) out |= DM_BTN_LS;
        axes[2] = axes[3] = 0;
    } else if (style == DM_STYLE_RIGHT) {
        out = b & (DM_BTN_A | DM_BTN_B | DM_BTN_X | DM_BTN_Y | DM_BTN_ZR | DM_BTN_RIGHT_SL | DM_BTN_RIGHT_SR);
        if (b & DM_BTN_RIGHT) out |= DM_BTN_A;
        if (b & DM_BTN_DOWN) out |= DM_BTN_B;
        if (b & DM_BTN_UP) out |= DM_BTN_X;
        if (b & DM_BTN_LEFT) out |= DM_BTN_Y;
        if (b & DM_BTN_L) out |= DM_BTN_RIGHT_SL;
        if (b & DM_BTN_R) out |= DM_BTN_RIGHT_SR;
        if (b & DM_BTN_ZL) out |= DM_BTN_R;
        if (b & (DM_BTN_PLUS | DM_BTN_MINUS)) out |= DM_BTN_PLUS;
        if (b & (DM_BTN_LS | DM_BTN_RS)) out |= DM_BTN_RS;
        axes[2] = axes[0]; axes[3] = axes[1]; axes[0] = axes[1] = 0;
    } else return;
    *buttons = out;
}
