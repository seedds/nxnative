// The error applet's dialog on the Mac (front.h front_message, shown by front_mac.m), built here alone so tests/t_err_dialog_mac.m can bring it up without a game.
// OK closes it and the game goes on; Quit ends the game, the way the console's Home button closes the software over the applet, so an error a game shows again and again
// cannot hold the player
#pragma once
#import <Cocoa/Cocoa.h>

#define ERR_DIALOG_OK NSAlertFirstButtonReturn
#define ERR_DIALOG_QUIT NSAlertSecondButtonReturn
static NSAlert *err_dialog_alert(const char *title, const char *text, NSString *game) {
    NSAlert *a = [NSAlert new];
    a.alertStyle = NSAlertStyleWarning;
    // stringWithUTF8String: the texts are UTF-8 (hle_err.c checked the game's), which %s would not read as such; nil only for text that is not, shown as nothing
    a.messageText = (title ? [NSString stringWithUTF8String:title] : nil) ?: @"";
    a.informativeText = (text ? [NSString stringWithUTF8String:text] : nil) ?: @"";
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:[@"Quit " stringByAppendingString:game]];
    return a;
}
