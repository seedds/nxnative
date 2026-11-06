// Exercise the actual AppKit sheet, menu target and guest wait bridge without a game or hardware.
#import "../controller_panel_mac.h"
#import <QuartzCore/QuartzCore.h>
#include <stdatomic.h>
#include <assert.h>

static void settleAt(BOOL (^ready)(void), int line) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:5];
    while (!ready() && end.timeIntervalSinceNow > 0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    if (!ready()) fprintf(stderr, "controller sheet wait timed out at line %d\n", line);
    assert(ready());
}
#define settle(condition) settleAt(condition, __LINE__)
static DmControllerPopup *popup(NSView *view, NSString *identifier) {
    if ([view isKindOfClass:DmControllerPopup.class] && [view.identifier isEqual:identifier]) return (id)view;
    for (NSView *child in view.subviews) { DmControllerPopup *found = popup(child, identifier); if (found) return found; }
    return nil;
}
static NSButton *identifyButton(NSView *view, NSString *key) {
    if ([view isKindOfClass:NSButton.class] && [view.identifier isEqual:key]) return (id)view;
    for (NSView *child in view.subviews) {
        NSButton *button = identifyButton(child, key);
        if (button) return button;
    }
    return nil;
}
static void selectPlayer(DmControllerPanel *panel, NSString *key, int player) {
    DmControllerPopup *choice = popup(panel.window.contentView, [@"player." stringByAppendingString:key]); assert(choice);
    [choice selectItemWithTag:player]; [choice sendAction:choice.action to:choice.target];
}
static void selectStyle(DmControllerPanel *panel, NSString *key, NSInteger index) {
    DmControllerPopup *choice = popup(panel.window.contentView, [@"style." stringByAppendingString:key]); assert(choice);
    [choice selectItemAtIndex:index + 1]; [choice sendAction:choice.action to:choice.target];
}
static void capture(NSWindow *window, NSString *path) {
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    window.contentView.wantsLayer = YES;
    [window.appearance performAsCurrentDrawingAppearance:^{ window.contentView.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor; }];
    [window orderBack:nil];
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:.3];
    while (end.timeIntervalSinceNow > 0) { [window.contentView layoutSubtreeIfNeeded]; [NSApp updateWindows]; [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]]; }
    [window displayIfNeeded];
    NSBitmapImageRep *bitmap = [window.contentView bitmapImageRepForCachingDisplayInRect:window.contentView.bounds];
    [window.contentView cacheDisplayInRect:window.contentView.bounds toBitmapImageRep:bitmap];
    [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}
int main(int argc, char **argv) { @autoreleasepool {
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited]; [NSApp finishLaunching];
    NSWindow *parent = [[NSWindow alloc] initWithContentRect:NSMakeRect(-10000, -10000, 800, 700) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    parent.releasedWhenClosed = NO; [parent orderBack:nil];
    __block NSArray *devices = @[@{@"id": @"a", @"name": @"DualSense"}, @{@"id": @"b", @"name": @"DualSense"}];
    __block NSArray *active = @[@{@"id": @"a", @"player": @1, @"style": @"pro"}, @{@"id": @"b", @"player": @2, @"style": @"pro"}];
    __block int opened = 0, closed = 0, applied = 0; __block BOOL reject = NO;
    DmControllerUI *ui = [DmControllerUI new]; ui.parent = parent;
    __block NSString *signaled = nil;
    ui.identify = ^BOOL(NSString *key) { signaled = key; return YES; };
    ui.snapshot = ^{ return @{@"controllers": devices, @"choices": active}; };
    __block dm_controller_requirements game = {0x1f, 0xff, NO};
    ui.requirements = ^{ return game; };
    ui.applyChoices = ^NSString *(NSArray *choices, int minimum, int maximum) {
        int count = controller_ready_players(game, choices, NULL);
        if (reject || count < minimum || count > maximum) return @"Connect and assign the controllers this game requires.";
        active = [choices copy]; applied++; return nil;
    };
    ui.didOpen = ^{ opened++; }; ui.didClose = ^{ closed++; };
    NSMenu *bar = [NSMenu new]; NSMenuItem *item = dm_controller_menu_add(bar, ui);
    assert([bar.itemArray.lastObject.title isEqual:@"Controllers"] && [item.title isEqual:@"Configure Controllers…"]);
    [item.menu performActionForItemAtIndex:0];
    assert(ui.panel && parent.attachedSheet == ui.panel.window && opened == 1);
    DmControllerPopup *automatic = popup(ui.panel.window.contentView, @"player.a");
    assert([[automatic.menu itemWithTag:-1].title hasSuffix:@" • Automatic"]);
    DmControllerPopup *types = popup(ui.panel.window.contentView, @"style.a");
    assert([types.itemArray[0].title hasSuffix:@" • Automatic"]);
    assert([types.itemArray[1].title isEqual:@"Pro Controller • Recommended"]);
    NSButton *identify = identifyButton(ui.panel.window.contentView, @"b");
    assert(identify && [identify.title isEqual:@"Identify"]);
    [identify sendAction:identify.action to:identify.target];
    assert([signaled isEqual:@"b"] && applied == 0);
    assert([identify.title isEqual:@"Identify • signal sent"]);
    selectPlayer(ui.panel, @"a", 2);
    assert([ui.panel.drafts[@"a"][@"player"] intValue] == 2 && [ui.panel.drafts[@"b"][@"player"] intValue] == 1);
    selectStyle(ui.panel, @"a", 2);
    [ui.panel cancel:nil]; settle(^BOOL { return ui.panel == nil; });
    assert(applied == 0 && closed == 1 && [active[0][@"style"] isEqual:@"pro"]);

    // The engine's guest request bridge must leave the main queue free for editing and Apply.
    __block _Atomic int finished = 0; __block int answer = -2;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ answer = [ui requestMinimum:2 maximum:2]; atomic_store(&finished, 1); });
    settle(^BOOL { return ui.panel != nil; });
    assert(!atomic_load(&finished) && ui.panel.minimum == 2 && ui.panel.maximum == 2);
    DmControllerPanel *same = ui.panel;
    [item.menu performActionForItemAtIndex:0]; assert(ui.panel == same && opened == 2);
    selectPlayer(ui.panel, @"b", 0); assert(!ui.panel.applyButton.enabled);
    selectPlayer(ui.panel, @"b", 2); assert(ui.panel.applyButton.enabled);
    // The live game can change its accepted styles and slots while the sheet is open.
    game.styles = DM_STYLE_LEFT | DM_STYLE_RIGHT;
    game.ids = 3;
    settle(^BOOL { return ui.panel.applyButton.enabled && [ui.panel.drafts[@"a"][@"style"] isEqual:@"left"]; });
    assert([ui.panel.drafts[@"b"][@"style"] isEqual:@"right"]);
    selectStyle(ui.panel, @"a", 0); selectStyle(ui.panel, @"b", 0);
    assert(!ui.panel.applyButton.enabled);
    assert([ui.panel.requirementDetails.stringValue containsString:@"Joy-Con (L) or Joy-Con (R)"]);
    assert([ui.panel.requirementDetails.stringValue containsString:@"Player slots          1, 2"]);
    selectStyle(ui.panel, @"a", 2);
    assert(!ui.panel.applyButton.enabled && [ui.panel.message.stringValue containsString:@"1 of 2"]);
    if (argc > 1) {
        [[NSFileManager defaultManager] createDirectoryAtPath:@(argv[1]) withIntermediateDirectories:YES attributes:nil error:NULL];
        capture(ui.panel.window, [@(argv[1]) stringByAppendingPathComponent:@"controller-requirements-blocked.png"]);
    }
    selectStyle(ui.panel, @"b", 3);
    assert(ui.panel.applyButton.enabled && [ui.panel.message.stringValue containsString:@"2 players ready"]);
    reject = YES; [ui.panel apply:nil];
    assert(ui.panel && applied == 0 && !atomic_load(&finished) && [ui.panel.operationError.stringValue containsString:@"Connect"]);
    reject = NO;
    // A newly connected device cannot take a player reserved by an existing draft.
    NSArray *previousDevices = devices, *previousActive = active;
    devices = [devices arrayByAddingObject:@{@"id": @"c", @"name": @"DualSense"}];
    active = [active arrayByAddingObject:@{@"id": @"c", @"player": @2, @"style": @"pro"}];
    [ui refresh]; assert([ui.panel.drafts[@"c"][@"player"] intValue] == 0 && [ui.panel.drafts[@"b"][@"player"] intValue] == 2);
    devices = previousDevices; active = previousActive;
    [ui.panel refresh];
    if (argc > 1) { [[NSFileManager defaultManager] createDirectoryAtPath:@(argv[1]) withIntermediateDirectories:YES attributes:nil error:NULL]; capture(ui.panel.window, [@(argv[1]) stringByAppendingPathComponent:@"controller-requirements-ready.png"]); }
    [ui.panel apply:nil]; settle(^BOOL { return atomic_load(&finished) != 0; });
    assert(answer == 1 && applied == 1 && closed == 2 && [active[0][@"style"] isEqual:@"left"] && [active[1][@"style"] isEqual:@"right"]);

    game = (dm_controller_requirements){0x1f, 0xff, NO};
    atomic_store(&finished, 0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ answer = [ui requestMinimum:1 maximum:4]; atomic_store(&finished, 1); });
    settle(^BOOL { return ui.panel != nil; });
    devices = @[devices[1]]; [ui refresh];
    assert(ui.panel.rows.count == 1 && !ui.panel.drafts[@"a"]);
    [ui.panel cancel:nil]; settle(^BOOL { return atomic_load(&finished) != 0; });
    assert(answer == 0 && applied == 1 && opened == closed);
    [item.menu performActionForItemAtIndex:0];
    devices = @[]; [ui refresh]; assert(!ui.panel.rows.count && !ui.panel.choices.count);
    if (argc > 1) capture(ui.panel.window, [@(argv[1]) stringByAppendingPathComponent:@"in-game-empty.png"]);
    NSMutableArray *many = [NSMutableArray new], *assignments = [NSMutableArray new];
    for (int i = 0; i < 9; i++) {
        NSString *key = [NSString stringWithFormat:@"device-%d", i];
        [many addObject:@{@"id": key, @"name": @"DualSense"}];
        [assignments addObject:@{@"id": key, @"player": @(i < 8 ? i + 1 : 0), @"style": @"pro"}];
    }
    devices = many; active = assignments; [ui refresh];
    settle(^BOOL { [ui.panel.window.contentView layoutSubtreeIfNeeded]; return ui.panel.fields.frame.size.height > ui.panel.fields.superview.frame.size.height; });
    NSView *first = ui.panel.fields.arrangedSubviews.firstObject;
    NSRect firstFrame = [first convertRect:first.bounds toView:ui.panel.window.contentView];
    NSView *clip = ui.panel.fields.superview; NSRect viewport = [clip convertRect:clip.bounds toView:ui.panel.window.contentView];
    assert(NSMaxY(viewport) - NSMaxY(firstFrame) < 40);   // long lists open at Controller 1, not their last row
    if (argc > 1) capture(ui.panel.window, [@(argv[1]) stringByAppendingPathComponent:@"in-game-many.png"]);
    [ui.panel.window performClose:nil]; settle(^BOOL { return ui.panel == nil; });
    assert(opened == closed);
    atomic_store(&finished, 0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ answer = [ui requestMinimum:1 maximum:8]; atomic_store(&finished, 1); });
    settle(^BOOL { return ui.panel != nil; }); [parent close];
    settle(^BOOL { return atomic_load(&finished) != 0; }); assert(answer == 0 && opened == closed);
    ui.parent = nil;
    atomic_store(&finished, 0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ answer = [ui requestMinimum:1 maximum:4]; atomic_store(&finished, 1); });
    settle(^BOOL { return atomic_load(&finished) != 0; }); assert(answer == -1);
    [parent orderOut:nil]; parent.contentView = nil;
    puts("controller panel: menu, applet wait, Apply, validation, cancellation and hotplug OK");
    return 0;
} }
