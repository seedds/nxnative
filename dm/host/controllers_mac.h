#pragma once
#import <Cocoa/Cocoa.h>
// Main-thread controller routing and a model-specific mapping panel.
void front_controllers_start(void);
NSMenu *front_controllers_menu(void);

#include "front.h"
#include "ctrl_prompt.h"
void front_controllers_mouse(const dm_touch *touch);
void front_controllers_focus(int active);
void front_controllers_modal(int active);
void front_controllers_window(int active);

// Main-thread input for the Home quit prompt. Read physical buttons before remapping.
@class GCController;
GCController *front_controllers_quit_buttons(int *home, int *a, int *b);
void front_controllers_quit_prompt(int active);
void front_home_poll(void);

// Controller applet input uses the same admitted slots, before remapping.
void front_controllers_applet(int active);
int front_controllers_applet_buttons(ctrl_prompt *prompt, int *a, int *b);
int front_controllers_waiting(void);
int front_controller_prompt_poll(void);

// Called on the main thread when mapped input reaches the game.
void front_game_input(void);
