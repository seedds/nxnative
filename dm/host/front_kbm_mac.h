// The Mac front end's keyboard and mouse controls (front_kbm_mac.m), called by front_mac.m; main thread only. Tears of the Kingdom's layout, kept apart like this until a second
// game takes keyboard and mouse controls and warrants a better abstraction (front_kbm_mac.m). Each function does nothing for a game whose patch does not turn the
// camera by the mouse (hle.h dm_mouse_camera): the controls never switch on there.
#pragma once
#import <Cocoa/Cocoa.h>
#include <stdint.h>
// front_mac.m: the buttons the keys hold (DM_BTN_*), whether the game window takes input, and front_push, which sends the keys with kbm_input's additions to the game
extern uint64_t front_btn;
extern int front_input_on;
void front_push(void);
// front_kbm_mac.m
void kbm_start(void);                                                              // at start: hooks the mice; switches the controls on with DM_KBM=1
int kbm_on(void);                                                                  // the controls are on (not the classic keys)
void kbm_input(uint64_t *btn, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);  // the mouse's buttons, aim modes, shakes, wheel and quick menu sticks
int kbm_key(unsigned short code, int down);                                        // a key; 1: taken (` or the controls on)
void kbm_flags(NSEvent *e);                                                        // Shift and Ctrl
int kbm_mouse(int n, int down);                                                    // a button in the game view (0 left, 1 right, 2 middle); 1: taken, not a touch
void kbm_scroll(NSEvent *e);
void kbm_release(void);                                                            // the window loses focus: lets go of the mouse and the aim mode
