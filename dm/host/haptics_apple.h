// Shared Core Haptics output for the Mac and iOS front ends. Configure routes before guest boot.
#pragma once
#import <GameController/GameController.h>
#include "hle_haptics.h"
void dm_haptics_apple_init(void);
void dm_haptics_apple_controller(int player, GCController *controller);
void dm_haptics_apple_shutdown(void);
// A controller that is not a GameController device (the iOS Switch 2 BLE path) plays its player's game vibration instead. The sink runs on the
// rumble queue and must only enqueue/store. NULL restores the GameController route, or the phone's engine for player 1.
void dm_haptics_apple_set_ble(int player, void (*sink)(const dm_haptic_state *));
