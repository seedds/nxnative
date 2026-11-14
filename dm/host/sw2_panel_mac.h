// The game window's Controller menu and panel (Mac): the Switch 2 stick tester on the controller session the game already holds (pad_switch2_apple.h, the tester's side).
// sw2_panel_mac.m has the whole story; front_mac.m builds the menu bar, blocks the Switch 2 pads while the panel is open and makes a quit wait for a save. Main thread only.
#pragma once
#import <Cocoa/Cocoa.h>

// Once, before sw2_panel_menu: game is the game window (the panel's first place is beside it); release sets every Switch 2 player's buttons, sticks and sensors to neutral,
// called each time the panel opens (the game then gets nothing from those pads until it closes)
void sw2_panel_setup(NSWindow *game, void (*release)(void));
NSMenu *sw2_panel_menu(void);   // the Controller menu: filled now, so ⌘⇧P works before it is first opened, and rebuilt by its delegate on every open
void sw2_panel_show(void);      // Show Controller Window: the panel is created on first use and kept, ordered front without taking the key window from the game
int sw2_panel_open(void);       // the panel is open (shown and not closed; hidden with the app too): Switch 2 pads give the game nothing
