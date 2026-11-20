// Shared Core Haptics output for the Mac and iOS front ends. Configure routes before guest boot.
#pragma once
#import <GameController/GameController.h>
#include "hle_haptics.h"
void dm_haptics_apple_init(void);
void dm_haptics_apple_controller(int player, GCController *controller);
// A controller GameController does not drive (a Switch 2 controller over Bluetooth, pad_switch2_apple.m): the player's vibration goes to the sink instead of Core Haptics, on the
// rumble queue, with Rumble strength and Rumble cutoff applied (DM_RUMBLE_GAIN, DM_RUMBLE_MIN; amplitudes 0..1). A suspension (focus lost) sends silence. NULL gives the player back.
void dm_haptics_apple_set_sink(int player, void (*sink)(const dm_haptic_state *state));
void dm_haptics_apple_shutdown(void);
