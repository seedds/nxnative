#pragma once
#import <Foundation/Foundation.h>
#include "front.h"
#include "dm_input.h"

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    int touch_count;
    dm_touch touches[16];
    uint64_t touch_update_count;
    int last_touch_count;
    dm_touch last_touches[16];
} DuoHarnessInput;

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
} DuoHarnessPlayerInput;

typedef struct {
    float acceleration[3];
    float rotation[3];
} DuoHarnessMotion;

typedef struct {
    DuoHarnessInput input;
    DuoHarnessPlayerInput players[DM_PLAYERS];
    uint32_t connected_players;
    DuoHarnessMotion controller_motion[DM_PLAYERS];
    uint32_t controller_motion_mask;
} DuoHarnessState;

DuoHarnessInput duo_harness_input(void);
int duo_harness_rumble(void);   // the in-game menu's rumble switch (dm_haptics_set_enabled)
void duo_harness_upscale(int present, int game);   // the synthetic MetalFX choices: -1 = not offered, else 1 MetalFX / 0 bilinear (present) or the game's own (game)
int duo_harness_present_upscale(void);   // the present's upscaling in force (dm_present_upscale_set)
int duo_harness_game_upscaler(void);   // the game's upscaler in force (dm_upscaler_set)
void dm_set_opmode(int mode);
int dm_opmode(void);
DuoHarnessState duo_harness_state(void);
void duo_harness_clear(void);

FOUNDATION_EXPORT NSString * const DuoHarnessInputDidChangeNotification;
