// Controller applet (nn::hid::ShowControllerSupport), the decision only: pure, so tests/t_hid.c checks it without a front end (hle_hid.c asks the front end when it says so).
// ControllerSupportArg starts { s8 playerCountMin, s8 playerCountMax, u8 enableTakeOverConnection, u8 enableLeftJustify, u8 enablePermitJoyDual, u8 enableSingleMode, ... }
// (switchbrew "Controller Applet"); the SDK's constructor defaults are min 0, max 4. On a Switch the applet shows its screen and the calling thread waits until the player confirms
// (Result 0) or closes it (the SDK's ShowControllerSupportCore turns that into ResultControllerSupportCanceled, 2202-3101: RES_CTRL_CANCELED in hle_hid.c).
// The host shows the screen only when the game cannot go on without it: fewer players than the minimum. The real applet always opens; a game that asks for no controller
// (min 0), or is already satisfied, is answered at once, as before, so a game that calls it on every menu does not interrupt the player. Player 1 always counts as connected
// (the keyboard, the touch gamepad, DM_KEYS: dm_pad_players), so a minimum of 1 is always met on the host.
#pragma once
#include <stdint.h>

// The numbers the applet works with. Single mode (enableSingleMode) picks one controller: one player, whatever the game's counts say. A maximum below the minimum is the minimum;
// a maximum of 0 with a minimum of 0 is "no limit given" and caps nothing
static inline void ca_normalize(int *min, int *max, int single) {
    if (single) { *min = 1; *max = 1; return; }
    if (*min < 0) *min = 0;
    if (*max > 0 && *max < *min) *max = *min;
}
// The player count the game gets: the connected players, capped at the game's maximum
static inline int ca_count(int connected, int max) { return max > 0 && connected > max ? max : connected; }
// 1 = show the screen and wait until `connected` reaches `min` (or the player cancels); 0 = answer now with ca_count. `capacity` is the most players this front end can ever report (the
// Mac: its slots; the iPhone: 1; also bounded by the npads the game supports): a minimum above it could only be cancelled, so hle_hid.c answers it as that
// (ResultControllerSupportCanceled) without showing a sheet
static inline int ca_ask(int min, int max, int single, int connected, int capacity) {
    ca_normalize(&min, &max, single);
    return min > 0 && connected < min && min <= capacity;
}
