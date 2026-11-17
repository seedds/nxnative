// The Mac front end's keyboard and mouse controls (front_kbm_mac.m), called by front_mac.m; main thread only. Tears of the Kingdom's layout, kept apart like this until a second
// game takes keyboard and mouse controls and warrants a better abstraction (front_kbm_mac.m). Each function does nothing for a game whose patch does not turn the camera by the
// mouse, or without DM_KBM=1: the controls never switch on there. front_mac.m only calls these four, so the feature stays in its own files
#pragma once
#import <Cocoa/Cocoa.h>
#include <stdint.h>
void kbm_start(NSWindow *win, uint64_t *btn, const int *input_on, void (*push)(void));   // at start: front_mac.m's keys (the buttons they hold, the window takes input, its push) and window
void kbm_input(uint64_t *btn, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);       // in push: adds the mouse's buttons, aim modes, shakes, wheel and quick menu sticks
int kbm_key(unsigned short code, int down);                                             // a key; 1: taken (the controls on)
void kbm_release(void);                                                                 // the window loses focus: lets go of the mouse and the aim mode
