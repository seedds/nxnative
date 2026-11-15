// The in-game Keyboard Map panel (Window › Keyboard Map…, ⌘⇧K): the keyboard map (keymap.h) while the game runs, each button and stick direction with its key; click one, then
// press a key, or hold Shift, Control or Option and press a key for a chord (Esc stops). A change applies at once and is saved as the saved choice launcher.DM_KEYMAP (the settings window's, which launcher.c makes DM_KEYMAP at the next start),
// unless an env file or the launch environment set DM_KEYMAP: then it lasts until the game quits. Like the Cheats panel it never takes the key window for a click, so the game keeps the
// keyboard: keyboard_mac.h's kb_on_key hands the panel the next key while it waits for one (kp_take_key), and that key never reaches the game.
#pragma once
#import <Cocoa/Cocoa.h>
#include "keymap.h"

@interface DmKeymapPanel : NSObject <NSWindowDelegate, NSMenuDelegate>
@end

static keymap *g_kp_map;              // keyboard_mac.h's map, changed in place (main thread, as kb_on_key)
static void (*g_kp_changed)(void);    // called after a change: the keys held are let go, so none stays down under its old target
static DmKeymapPanel *g_kp;
static int g_kp_add;                 // the key awaited is added to the target's (Add Key…), not in place of them
static int g_kp_capture = -1;         // the target waiting for a key, -1 none

static km_preset g_kp_presets[KM_MAX_PRESETS]; static int g_kp_npresets;   // the Presets menu's, loaded each time it opens

static int kp_active(void) {   // the presets read again; the one the map is (its index), else -1: Custom
    char err[2048]; g_kp_npresets = km_presets_load(g_kp_presets, KM_MAX_PRESETS, err, sizeof err);
    static int logged; if (*err && !logged++) LOG("[RED] front end: keymap files: lines skipped: %s", err);   // once: this runs at every refresh
    static keymap p;
    for (int i = 0; i < g_kp_npresets; i++) if (!km_parse(&p, g_kp_presets[i].map, NULL, 0) && km_same(&p, g_kp_map)) return i;
    return -1;
}

static NSString *kp_keys(int t) {   // the target's keys as the panel shows them
    NSMutableArray *a = [NSMutableArray array];
    for (int c = 0; c < KM_SLOTS; c++) if (g_kp_map->key[c] == t) { char l[64]; [a addObject:@(km_slot_label(c, l, sizeof l))]; }
    return a.count ? [a componentsJoinedByString:@", "] : @"None";
}
// The map is not the saved choice's (as the game started), so changes are not saved: DM_KEYMAP from an env file or the launch environment (1), or saved choices off for this
// start (DM_SAVED_CHOICES=0, 2: the launcher did not apply the saved map, so the panel leaves it alone too)
static int kp_fixed(void) {
    static int fixed = -1;
    if (fixed < 0) {
        const char *files = getenv("DM_ENV_FILE_KEYS"), *e = getenv("DM_KEYMAP"), *sc = getenv("DM_SAVED_CHOICES");
        NSString *saved = [NSUserDefaults.standardUserDefaults stringForKey:@"launcher.DM_KEYMAP"];
        fixed = (files && strstr(files, "DM_KEYMAP")) || (e && *e && !(saved && !strcmp(saved.UTF8String, e))) ? 1 : sc && !strcmp(sc, "0") ? 2 : 0;
    }
    return fixed;
}

@implementation DmKeymapPanel {
    NSPanel *_panel;
    NSMutableDictionary<NSNumber *, NSButton *> *_buttons;   // by target
    NSTextField *_status, *_note;
    NSPopUpButton *_presets;
}

- (NSTextField *)label:(NSString *)s bold:(BOOL)bold {
    NSTextField *l = [NSTextField labelWithString:s];
    if (bold) l.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return l;
}

- (void)make {
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 360, 620)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
                                          backing:NSBackingStoreBuffered defer:NO];
    _panel.title = @"Keyboard Map";
    _panel.releasedWhenClosed = NO;
    _panel.floatingPanel = YES;
    _panel.becomesKeyOnlyIfNeeded = YES;   // a button click never takes the key window: the game keeps the keyboard
    _panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    _panel.minSize = NSMakeSize(300, 300);
    _panel.delegate = self;
    _buttons = [NSMutableDictionary dictionary];

    NSStackView *rows = [NSStackView stackViewWithViews:@[]];
    rows.orientation = NSUserInterfaceLayoutOrientationVertical; rows.alignment = NSLayoutAttributeLeading; rows.spacing = 6;
    rows.edgeInsets = NSEdgeInsetsMake(10, 14, 10, 14);
    for (int g = 0; g < KM_NGROUPS; g++) {
        NSTextField *h = [self label:@(km_groups[g].title) bold:YES];
        [rows addView:h inGravity:NSStackViewGravityTop];
        if (g) [rows setCustomSpacing:14 afterView:rows.views[rows.views.count - 2]];
        for (int i = 0; i < km_groups[g].n; i++) {
            int t = km_groups[g].targets[i];
            NSButton *b = [NSButton buttonWithTitle:@"" target:self action:@selector(pick:)];
            b.tag = t; b.bezelStyle = NSBezelStyleRounded;
            [b.widthAnchor constraintEqualToConstant:150].active = YES;
            NSMenu *m = [NSMenu new]; NSMenuItem *none = [m addItemWithTitle:@"No Key" action:@selector(clearKey:) keyEquivalent:@""]; none.target = self; none.tag = t;
            NSMenuItem *add = [m addItemWithTitle:@"Add Key\u2026" action:@selector(addKey:) keyEquivalent:@""]; add.target = self; add.tag = t;   // another key for the same button
            b.menu = m;   // right-click: no key
            _buttons[@(t)] = b;
            NSTextField *name = [self label:@(km_targets[t].label) bold:NO];
            NSView *spacer = [NSView new]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
            NSStackView *row = [NSStackView stackViewWithViews:@[name, spacer, b]];
            row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
            [rows addView:row inGravity:NSStackViewGravityTop];
            [row.widthAnchor constraintEqualToAnchor:rows.widthAnchor constant:-28].active = YES;
        }
    }
    NSScrollView *scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES; scroll.drawsBackground = NO;
    NSClipView *clip = scroll.contentView;
    rows.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = rows;
    [rows.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor].active = YES;
    [rows.trailingAnchor constraintEqualToAnchor:clip.trailingAnchor].active = YES;
    [rows.topAnchor constraintEqualToAnchor:clip.topAnchor].active = YES;

    _presets = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
    [_presets addItemWithTitle:@"Presets"];
    _presets.menu.delegate = self;   // the items: menuNeedsUpdate, from the folder at every open
    _status = [self label:@"Click a button, then press its key (Shift + a key: a chord)." bold:NO];
    _status.lineBreakMode = NSLineBreakByWordWrapping; _status.maximumNumberOfLines = 3;
    [_status setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _note = [self label:@"" bold:NO];
    _note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; _note.textColor = NSColor.secondaryLabelColor;
    _note.lineBreakMode = NSLineBreakByWordWrapping; _note.maximumNumberOfLines = 3;
    [_note setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSView *content = [NSView new];
    for (NSView *v in @[_presets, scroll, _status, _note]) { v.translatesAutoresizingMaskIntoConstraints = NO; [content addSubview:v]; }
    [NSLayoutConstraint activateConstraints:@[
        [_presets.topAnchor constraintEqualToAnchor:content.topAnchor constant:10], [_presets.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
        [scroll.topAnchor constraintEqualToAnchor:_presets.bottomAnchor constant:8], [scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_status.topAnchor constraintEqualToAnchor:scroll.bottomAnchor constant:8], [_status.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
        [_status.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14],
        [_note.topAnchor constraintEqualToAnchor:_status.bottomAnchor constant:4], [_note.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:14],
        [_note.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-14], [_note.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-10],
    ]];
    _panel.contentView = content;
    _panel.frameAutosaveName = @"DmKeymapPanel";
}

- (void)refresh {
    for (NSNumber *k in _buttons) { int t = k.intValue; _buttons[k].title = g_kp_capture == t ? @"Press a key…" : kp_keys(t); }
    int active = kp_active();
    _presets.itemArray[0].title = [NSString stringWithFormat:@"Preset: %s", active >= 0 ? g_kp_presets[active].name : "Custom"];
    _note.stringValue = kp_fixed() == 1 ? @"DM_KEYMAP is set by an env file: changes last until the game quits." : kp_fixed() ? @"Saved choices are off (DM_SAVED_CHOICES=0): changes last until the game quits." :
                        @"Changes are saved for the next start.";
}

- (void)show {
    if (!_panel) [self make];
    [_panel orderFront:nil];
    [self refresh];
}

- (void)changed:(NSString *)status {
    if (g_kp_changed) g_kp_changed();
    if (!kp_fixed()) {
        keymap d; km_default(&d); NSUserDefaults *u = NSUserDefaults.standardUserDefaults;
        if (km_same(&d, g_kp_map)) [u removeObjectForKey:@"launcher.DM_KEYMAP"];
        else {
            static char s[KM_FORMAT_MAX];
            if (km_format(g_kp_map, s, sizeof s) < (int)sizeof s) [u setObject:@(s) forKey:@"launcher.DM_KEYMAP"];
            else { LOG("[RED] front end: keyboard map too long to save (%d bytes at most): not saved", KM_FORMAT_MAX - 1); status = @"The map is too long to save; it lasts until the game quits."; }
        }
    }
    static char s[KM_FORMAT_MAX]; km_format(g_kp_map, s, sizeof s); LOG("front end: keyboard map changed in the panel: %s", s);
    _status.stringValue = status;
    [self refresh];
}

- (void)pick:(NSButton *)b {
    int t = (int)b.tag;
    g_kp_capture = g_kp_capture == t ? -1 : t; g_kp_add = 0;
    _status.stringValue = g_kp_capture < 0 ? @"" : [NSString stringWithFormat:@"Press a key for %s (Esc stops).", km_targets[t].label];
    [self refresh];
}

- (void)addKey:(NSMenuItem *)it {
    int t = (int)it.tag; g_kp_capture = t; g_kp_add = 1;
    _status.stringValue = [NSString stringWithFormat:@"Press another key for %s (Esc stops).", km_targets[t].label];
    [self refresh];
}

- (void)clearKey:(NSMenuItem *)it {
    g_kp_capture = -1; g_kp_add = 0;
    km_bind(g_kp_map, (int)it.tag, -1);
    [self changed:[NSString stringWithFormat:@"%s has no key.", km_targets[it.tag].label]];
}

- (void)menuNeedsUpdate:(NSMenu *)menu {   // the presets: Default and the keymap files, then saving and the folder
    int active = kp_active();
    [menu removeAllItems];
    [menu addItemWithTitle:[NSString stringWithFormat:@"Preset: %s", active >= 0 ? g_kp_presets[active].name : "Custom"] action:nil keyEquivalent:@""];   // a pull-down's first item is its title
    for (int i = 0; i < g_kp_npresets; i++) {
        NSMenuItem *it = [menu addItemWithTitle:@(g_kp_presets[i].name) action:@selector(preset:) keyEquivalent:@""]; it.target = self; it.tag = i;
        if (i == active) it.state = NSControlStateValueOn;   // the preset the map is, as the settings window's Keyboard row names it
    }
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"Save as Preset\u2026" action:@selector(savePreset:) keyEquivalent:@""].target = self;
    [menu addItemWithTitle:@"Show Presets Folder" action:@selector(showFolder:) keyEquivalent:@""].target = self;
}

- (void)preset:(NSMenuItem *)it {
    if (it.tag < 0 || it.tag >= g_kp_npresets) return;
    g_kp_capture = -1;
    km_parse(g_kp_map, g_kp_presets[it.tag].map, NULL, 0);
    [self changed:[NSString stringWithFormat:@"%s map.", g_kp_presets[it.tag].name]];
}

// The map as a keymap file, under a name the player types (a file of that name is replaced). The question takes the keyboard from the game until it is answered
- (void)savePreset:(id)sender {
    (void)sender; g_kp_capture = -1; [self refresh];
    NSAlert *a = [NSAlert new];
    a.messageText = @"Save as Preset"; a.informativeText = @"A keymap file in the presets folder, for every game. A preset of the same name is replaced.";
    NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)]; f.placeholderString = @"Name";
    a.accessoryView = f; [a addButtonWithTitle:@"Save"]; [a addButtonWithTitle:@"Cancel"];
    a.window.initialFirstResponder = f;
    NSModalResponse r = [a runModal];
    NSString *name = [f.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (r != NSAlertFirstButtonReturn || !name.length) return;
    char path[1024] = "";
    if (km_preset_save(g_kp_map, name.UTF8String, path, sizeof path)) { _status.stringValue = [NSString stringWithFormat:@"Could not save the preset%s%s: %s", *path ? " " : "", path, strerror(errno)]; return; }
    LOG("front end: keyboard map saved as preset '%s': %s", name.UTF8String, path);
    _status.stringValue = [NSString stringWithFormat:@"Saved as %@ (%@).", name, @(path).lastPathComponent];
}

- (void)showFolder:(id)sender {
    (void)sender; char dir[1024]; if (!km_preset_dir(dir, sizeof dir)) return;
    [NSFileManager.defaultManager createDirectoryAtPath:@(dir) withIntermediateDirectories:YES attributes:nil error:nil];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:@(dir)]];
}

- (void)bindKey:(int)slot {   // a key or a chord (keymap.h: layer * KM_KEYS + key code)
    int t = g_kp_capture, was = g_kp_map->key[slot], add = g_kp_add; g_kp_capture = -1; g_kp_add = 0;
    if (!add) km_bind(g_kp_map, t, -1);
    km_bind(g_kp_map, t, slot);
    char k[64]; NSString *key = @(km_slot_label(slot, k, sizeof k));
    NSString *s = [NSString stringWithFormat:add ? @"%@ is %s too." : @"%@ is %s.", key, km_targets[t].label];
    if (was != KM_NONE && was != t) {
        int left = 0; for (int c = 0; c < KM_SLOTS; c++) left |= g_kp_map->key[c] == was;
        s = [NSString stringWithFormat:@"%@ is %s now; %s %s.", key, km_targets[t].label, km_targets[was].label, left ? "keeps its other keys" : "has no key"];
    }
    [self changed:s];
}

- (void)cancelCapture { g_kp_capture = -1; g_kp_add = 0; _status.stringValue = @""; [self refresh]; }

- (BOOL)windowShouldClose:(NSWindow *)w { g_kp_capture = -1; [_panel orderOut:nil]; return NO; }
@end

// kb_on_key: while the panel waits for a key, the keys pressed are the panel's, and so are their releases. A key with a modifier held binds the chord (mods, KM_MOD_*);
// a modifier pressed and let go with no key between binds the modifier itself. Returns whether the key was taken
static uint8_t g_kp_taken[KM_KEYS];   // the keys the panel took, whose releases the game must not see either
static int g_kp_pending = -1;         // a modifier pressed while waiting: bound alone if let go first
static void kp_forget(void) { memset(g_kp_taken, 0, sizeof g_kp_taken); g_kp_pending = -1; }   // input let go (focus lost: a release may never come): no release is swallowed later
static int kp_take_key(int code, int down, int mods) {
    if (code < 0 || code >= KM_KEYS) return 0;
    if (!down) {
        if (code == g_kp_pending && g_kp_capture >= 0) [g_kp bindKey:code];
        if (code == g_kp_pending) g_kp_pending = -1;
        if (g_kp_taken[code]) { g_kp_taken[code] = 0; return 1; }
        return 0;
    }
    if (g_kp_capture < 0) return 0;
    g_kp_taken[code] = 1;
    if (km_is_modifier(code)) { g_kp_pending = code; return 1; }
    g_kp_pending = -1;
    int slot = km_layer(mods) * KM_KEYS + code;
    if (code == 53 && !mods) [g_kp cancelCapture];
    else if (km_slot_bindable(slot)) [g_kp bindKey:slot];
    else NSBeep();
    return 1;
}

static void kp_init(keymap *m, void (*changed)(void)) { g_kp_map = m; g_kp_changed = changed; }
static void kp_show(void) { if (!g_kp) g_kp = [DmKeymapPanel new]; [g_kp show]; }

@interface DmKeymapMenuTarget : NSObject @end
@implementation DmKeymapMenuTarget
- (void)show:(id)sender { (void)sender; kp_show(); }
@end
