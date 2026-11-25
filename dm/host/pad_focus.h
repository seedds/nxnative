#pragma once
// DM_PAD_BACKGROUND (Mac): whether the controllers stay with the game when its window stops being key (front_mac.m). By default the window's
// keyboard, mouse and controllers all stop reaching the game then. With the switch on, another app in front or another window key releases only
// the keyboard and mouse, and the controllers keep playing; GameController is told to keep sending a background app their input
// (shouldMonitorBackgroundEvents). Minimizing the window and the software keyboard's input box still release the controllers.
static inline int pad_focus_keep(int background, int miniaturized, int text_box) { return background && !miniaturized && !text_box; }
