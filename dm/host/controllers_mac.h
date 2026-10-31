#pragma once
#import <Cocoa/Cocoa.h>
// Main-thread controller routing and a model-specific mapping panel.
void front_controllers_start(void);
NSMenu *front_controllers_menu(void);

#include "front.h"
void front_controllers_mouse(const dm_touch *touch);
void front_controllers_focus(int active);
void front_controllers_modal(int active);
void front_controllers_window(int active);
