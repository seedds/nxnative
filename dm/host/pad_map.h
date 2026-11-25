// Physical button order: east, south, north, west, LS, RS, L, R, ZL, ZR,
// +, -, left, up, right, down, L4, R4, L5, R5, pad clicks L/R, Guide, Quick Access.
// First 16 match the Switch's position layout. Extra buttons start unassigned.
#pragma once
#include "dm_input.h"
#define DM_PAD_BUTTONS 24
static inline void pad_physical_default(int *map, int xbox) {
    for (int i = 0; i < DM_PAD_BUTTONS; i++) map[i] = i < 16 ? i : -1;
    if (xbox) { map[0] = 1; map[1] = 0; map[2] = 3; map[3] = 2; }
}
static inline uint64_t pad_map_buttons(uint32_t physical, const int *map) {
    uint64_t result = 0;
    for (int i = 0; i < DM_PAD_BUTTONS; i++) if ((physical >> i) & 1) {
        int to = map[i]; if ((to >= 0 && to < 16) || to == 63) result |= 1ull << to;
    }
    return result;
}
