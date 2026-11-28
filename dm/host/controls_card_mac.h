// The controls card (Window → Show Controls, ⌘/): what the keyboard and the mouse do in this game, read from the player's controls as the host loaded them (controls.h:
// the built-in map with the controls files' changes), drawn by front_mac.m in the top-right corner of the game's view. The views exist only while the card is shown: hiding
// it removes and releases them, so a hidden card costs nothing. Shown, it is a static layer-backed view that AppKit draws once and the window server composites over the
// game's layer; it takes no clicks (they reach the touch screen beneath) and no keys (the game keeps the keyboard). Built here alone so tests/t_controls_card_mac.m can
// build it without a window or a game.
#pragma once
#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>
#include "controls.h"
#include "home_quit_mac.h"   // hq_color, hq_scale, hq_label: the same look as the Home prompt

// A macOS key code's name: the special keys by name, the others as the current keyboard layout prints them (an AZERTY keyboard's A key is "Q")
static NSString *cc_key_name(int code) {
    switch (code) {
    case 36: return @"Return"; case 48: return @"Tab"; case 49: return @"Space"; case 51: return @"Delete"; case 53: return @"Esc"; case 76: return @"Enter";
    case 117: return @"⌦"; case 115: return @"Home"; case 119: return @"End"; case 116: return @"Page Up"; case 121: return @"Page Down";
    case 123: return @"←"; case 124: return @"→"; case 125: return @"↓"; case 126: return @"↑";
    case 122: return @"F1"; case 120: return @"F2"; case 99: return @"F3"; case 118: return @"F4"; case 96: return @"F5"; case 97: return @"F6";
    case 98: return @"F7"; case 100: return @"F8"; case 101: return @"F9"; case 109: return @"F10"; case 103: return @"F11"; case 111: return @"F12";
    case 105: return @"F13"; case 107: return @"F14"; case 113: return @"F15"; case 106: return @"F16"; case 64: return @"F17"; case 79: return @"F18"; case 80: return @"F19";
    }
    int keypad = code == 65 || code == 67 || code == 69 || code == 71 || code == 75 || code == 78 || code == 81 || (code >= 82 && code <= 92 && code != 90);
    NSString *s = nil;
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    if (src) {
        CFDataRef d = TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData);
        if (d) {
            UInt32 dead = 0; UniChar ch[4]; UniCharCount n = 0;
            if (UCKeyTranslate((const UCKeyboardLayout *)CFDataGetBytePtr(d), (UInt16)code, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysMask,
                               &dead, 4, &n, ch) == noErr && n > 0 && ch[0] > ' ')
                s = [[NSString stringWithCharacters:ch length:n] uppercaseString];
        }
        CFRelease(src);
    }
    if (!s) return [NSString stringWithFormat:@"Key %d", code];
    return keypad ? [@"Num " stringByAppendingString:s] : s;
}
// An action's name as the controls file writes it: "A", "A+ZR", "LUp"
static NSString *cc_action_name(ctl_action a) {
    NSMutableArray *parts = [NSMutableArray array];
    for (int i = 0; i < CTL_NNAMES; i++)
        if ((CTL_NAMES[i].btn && (a.btn & CTL_NAMES[i].btn)) || (CTL_NAMES[i].dirs && (a.dirs & CTL_NAMES[i].dirs))) [parts addObject:@(CTL_NAMES[i].name)];
    return [parts componentsJoinedByString:@"+"];
}
static int cc_single(ctl_action a) { return __builtin_popcountll(a.btn) + __builtin_popcount(a.dirs) == 1; }

// The keys that do exactly this one action, joined with " / " (nil for none). Shift with an arrow that keeps its built-in action is the right stick (front_mac.m on_key)
static NSString *cc_keys_for(const controls *c, uint64_t btn, uint8_t dirs) {
    NSMutableArray *keys = [NSMutableArray array];
    for (int k = 0; k < CTL_KEYS; k++) if (cc_single(c->key[k]) && c->key[k].btn == btn && c->key[k].dirs == dirs) [keys addObject:cc_key_name(k)];
    static const struct { int code; uint8_t dir; } shift_arrows[] = {{126, CTL_RUP}, {123, CTL_RLEFT}, {125, CTL_RDOWN}, {124, CTL_RRIGHT}};
    for (int i = 0; i < 4; i++) if (!btn && dirs == shift_arrows[i].dir && ctl_key_is_default(c, shift_arrows[i].code)) [keys addObject:[@"⇧" stringByAppendingString:cc_key_name(shift_arrows[i].code)]];
    static NSString *const clicks[3] = {@"Left click", @"Right click", @"Middle click"};   // the mouse's buttons, while it is a stick (captured)
    if (c->mouse != CTL_MOUSE_OFF)
        for (int i = 0; i < 3; i++) if (cc_single(c->mouse_btn[i]) && c->mouse_btn[i].btn == btn && c->mouse_btn[i].dirs == dirs) [keys addObject:clicks[i]];
    return keys.count ? [keys componentsJoinedByString:@" / "] : nil;
}
// Four directions (up, left, down, right) on one line: "W A S D", a missing one as "·"; nil when none has a key
static NSString *cc_four(NSString *up, NSString *left, NSString *down, NSString *right) {
    if (!up && !left && !down && !right) return nil;
    return [NSString stringWithFormat:@"%@  %@  %@  %@", up ?: @"·", left ?: @"·", down ?: @"·", right ?: @"·"];
}

// The card's rows, label and keys (keys nil: nothing does it, drawn dimmed). Exposed for the test
static NSArray<NSArray *> *cc_rows(const controls *c) {
    NSMutableArray *rows = [NSMutableArray array];
    void (^row)(NSString *, NSString *) = ^(NSString *label, NSString *keys) { [rows addObject:@[label, keys ?: NSNull.null]]; };
    NSString *ls = cc_four(cc_keys_for(c, 0, CTL_LUP), cc_keys_for(c, 0, CTL_LLEFT), cc_keys_for(c, 0, CTL_LDOWN), cc_keys_for(c, 0, CTL_LRIGHT));
    NSString *rs = cc_four(cc_keys_for(c, 0, CTL_RUP), cc_keys_for(c, 0, CTL_RLEFT), cc_keys_for(c, 0, CTL_RDOWN), cc_keys_for(c, 0, CTL_RRIGHT));
    if (c->mouse == CTL_MOUSE_LEFT) ls = ls ? [ls stringByAppendingString:@" · Mouse"] : @"Mouse";
    if (c->mouse == CTL_MOUSE_RIGHT) rs = rs ? [rs stringByAppendingString:@" · Mouse"] : @"Mouse";
    row(@"Left stick", ls); row(@"Right stick", rs);
    row(@"D-pad", cc_four(cc_keys_for(c, DM_BTN_UP, 0), cc_keys_for(c, DM_BTN_LEFT, 0), cc_keys_for(c, DM_BTN_DOWN, 0), cc_keys_for(c, DM_BTN_RIGHT, 0)));
    static const struct { const char *label; uint64_t btn; int always; } buttons[] = {
        {"A", DM_BTN_A, 1}, {"B", DM_BTN_B, 1}, {"X", DM_BTN_X, 1}, {"Y", DM_BTN_Y, 1}, {"L", DM_BTN_L, 1}, {"R", DM_BTN_R, 1}, {"ZL", DM_BTN_ZL, 1}, {"ZR", DM_BTN_ZR, 1},
        {"Left stick press", DM_BTN_LS, 1}, {"Right stick press", DM_BTN_RS, 1}, {"+", DM_BTN_PLUS, 1}, {"−", DM_BTN_MINUS, 1}, {"Shake", DM_BTN_SHAKE, 0},
        {"SL (left)", DM_BTN_LSL, 0}, {"SR (left)", DM_BTN_LSR, 0}, {"SL (right)", DM_BTN_RSL, 0}, {"SR (right)", DM_BTN_RSR, 0},
    };
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
        NSString *k = cc_keys_for(c, buttons[i].btn, 0);
        if (k || buttons[i].always) row(@(buttons[i].label), k);
    }
    for (int k = 0; k < CTL_KEYS; k++)   // a key that does several things at once
        if ((c->key[k].btn || c->key[k].dirs) && !cc_single(c->key[k])) row(cc_action_name(c->key[k]), cc_key_name(k));
    if (c->mouse == CTL_MOUSE_OFF) row(@"Touch screen", @"Left click");
    else {
        static const char *mb[3] = {"Left click", "Right click", "Middle click"};   // a click doing several things at once (the single ones are in their buttons' rows)
        for (int i = 0; i < 3; i++) if ((c->mouse_btn[i].btn || c->mouse_btn[i].dirs) && !cc_single(c->mouse_btn[i])) row(cc_action_name(c->mouse_btn[i]), @(mb[i]));
        row(@"Capture the mouse", @"Click the game · Esc releases");
    }
    return rows;
}

// The overlay over the whole view: lets every click through to the game's view, so the card never takes the touch screen
@interface CcPassthrough : NSView @end
@implementation CcPassthrough
- (NSView *)hitTest:(NSPoint)p { (void)p; return nil; }
@end

// The card in the view's top-right corner: a title and the rows in two columns of label and keys
static NSView *cc_card_view(NSRect bounds, const controls *c) {
    CGFloat k = hq_scale(bounds.size) * 0.85, fs = 14 * k;
    NSArray<NSArray *> *rows = cc_rows(c);
    NSUInteger half = (rows.count + 1) / 2;
    NSGridView *grid = [NSGridView gridViewWithNumberOfColumns:5 rows:0];
    grid.rowSpacing = 5 * k; grid.columnSpacing = 10 * k;
    for (NSUInteger i = 0; i < half; i++) {
        NSMutableArray *cells = [NSMutableArray array];
        for (int col = 0; col < 2; col++) {
            NSUInteger j = i + col * half;
            if (col) [cells addObject:[NSView new]];   // the gap between the two columns
            if (j >= rows.count) { [cells addObject:NSGridCell.emptyContentView]; [cells addObject:NSGridCell.emptyContentView]; continue; }
            id keys = rows[j][1]; int none = keys == NSNull.null;
            NSTextField *l = hq_label(rows[j][0], fs, NSFontWeightRegular, [NSColor colorWithWhite:1 alpha:0.6], 0);
            NSTextField *v = hq_label(none ? @"—" : keys, fs, NSFontWeightSemibold, none ? [NSColor colorWithWhite:1 alpha:0.3] : HQ_CREAM, 0);
            l.alignment = NSTextAlignmentRight; v.alignment = NSTextAlignmentLeft;
            [cells addObject:l]; [cells addObject:v];
        }
        [grid addRowWithViews:cells];
    }
    [grid columnAtIndex:2].width = 14 * k;
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *title = hq_label(@"Controls", 18 * k, NSFontWeightBold, NSColor.whiteColor, 0);
    NSTextField *note = hq_label(@"⌘/ hides this · keyboard and mouse, player 1", 12 * k, NSFontWeightRegular, [NSColor colorWithWhite:1 alpha:0.45], 0);
    NSStackView *card = [NSStackView stackViewWithViews:@[title, grid, note]];
    card.orientation = NSUserInterfaceLayoutOrientationVertical; card.alignment = NSLayoutAttributeLeading;
    card.spacing = 10 * k; card.edgeInsets = NSEdgeInsetsMake(16 * k, 20 * k, 14 * k, 20 * k); card.translatesAutoresizingMaskIntoConstraints = NO;
    card.wantsLayer = YES; card.layer.backgroundColor = [HQ_SURFACE colorWithAlphaComponent:0.88].CGColor; card.layer.cornerRadius = 16 * k;
    card.layer.borderWidth = 1; card.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.10].CGColor;
    NSView *host = [[CcPassthrough alloc] initWithFrame:bounds]; host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [host addSubview:card];
    [NSLayoutConstraint activateConstraints:@[[card.trailingAnchor constraintEqualToAnchor:host.trailingAnchor constant:-16 * k],
        [card.topAnchor constraintEqualToAnchor:host.topAnchor constant:16 * k],
        [card.trailingAnchor constraintGreaterThanOrEqualToAnchor:grid.trailingAnchor constant:20 * k],   // a leading-aligned stack keeps no trailing inset of its own
        [card.trailingAnchor constraintGreaterThanOrEqualToAnchor:note.trailingAnchor constant:20 * k]]];
    host.accessibilityElement = YES; host.accessibilityRole = NSAccessibilityGroupRole; host.accessibilityLabel = @"Controls";
    return host;
}
