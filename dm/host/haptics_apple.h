// Shared Core Haptics output for the Mac and iOS front ends. Configure routes before guest boot.
#pragma once
#import <GameController/GameController.h>
#include "hle_haptics.h"
void dm_haptics_apple_init(void);
void dm_haptics_apple_controller(int player, GCController *controller);
// Apply the controller cutoff after converting the game's bands and applying gain.
float dm_haptics_apple_controller_intensity(float intensity);
// Native HID alternative. Runs on the adapter's serial queue, refreshed at 40 ms
// while nonzero; receives zero on route changes/suspend/exit. Owner identifies
// an unchanged route. Never calls hardware on a guest thread.
typedef void (^dm_haptics_output)(dm_haptic_state state, float gain);
void dm_haptics_apple_output(int player, id owner, dm_haptics_output output);
void dm_haptics_apple_shutdown(void);

// Mac game-window/modal gate, independent of application activation.
void dm_haptics_apple_focus(int active);
