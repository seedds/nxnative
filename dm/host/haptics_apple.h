// Shared Core Haptics output for the Mac and iOS front ends. Configure routes before guest boot.
#pragma once
#import <GameController/GameController.h>
#include "hle_haptics.h"
void dm_haptics_apple_init(void);
void dm_haptics_apple_controller(int player, GCController *controller);
void dm_haptics_apple_shutdown(void);
// A controller the front end drives itself (the Xbox 360 receiver, pad_x360_mac.h): its route is two rotating motors, set by motors(player, strong, weak) with levels
// 0..255 on the haptics queue, each call a change. NULL removes the route. A GameController controller that takes the player replaces it (dm_haptics_apple_controller)
typedef void (*dm_haptics_motors_fn)(int player, uint8_t strong, uint8_t weak);
void dm_haptics_apple_motors(int player, dm_haptics_motors_fn motors);
