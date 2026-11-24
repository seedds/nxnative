// Native account choice, called on the main thread while the applet owns input.
// Kept separate from game-window setup so its actual dialog can be exercised without a game.
#pragma once
#import <Cocoa/Cocoa.h>
#include "front.h"
static inline int user_select_mac(const front_user *users, int count, int allow_create, int allow_skip) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = count ? @"Choose a user" : @"Add a user";
    alert.informativeText = @"Choose who will play. Each user keeps separate progress.";
    [alert addButtonWithTitle:count ? @"Select" : @"Add user"];
    [alert addButtonWithTitle:@"Cancel"];
    if (allow_skip) [alert addButtonWithTitle:@"Skip"];
    NSPopUpButton *list = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 28) pullsDown:NO];
    // A slot label distinguishes equal nicknames; an open user remains selectable
    // unless the game excluded it. The player, not the controller count, chooses.
    for (int i = 0; i < count; i++) {
        [list addItemWithTitle:[NSString stringWithFormat:@"%s (user %d)%s", users[i].nickname, users[i].slot + 1, users[i].open ? " - in use" : ""]];
        list.lastItem.tag = users[i].slot;
    }
    if (allow_create) { [list addItemWithTitle:@"Add user"]; list.lastItem.tag = FRONT_USER_CREATE; }
    alert.buttons.firstObject.enabled = list.numberOfItems > 0;
    alert.accessoryView = list; alert.window.initialFirstResponder = list;
    NSModalResponse r = [alert runModal];
    if (r == NSAlertFirstButtonReturn && list.selectedItem) return (int)list.selectedItem.tag;
    return r == NSAlertThirdButtonReturn && allow_skip ? FRONT_USER_SKIP : FRONT_USER_CANCEL;
}
