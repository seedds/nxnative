// Add the game's File, Game, Audio and View menus without replacing existing menus.
#pragma once
#import <Cocoa/Cocoa.h>
void mac_menus_install(NSWindow *window, NSMenu *bar);
void mac_menus_focus(int active);
