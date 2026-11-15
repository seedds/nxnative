// The Mac keyboard as player 1's controller, through the keyboard map (keymap.h: DM_KEYMAP over the default, the settings window's saved choice) and its in-game panel
// (keymap_panel_mac.h). Included by front_mac.m after the controller applet's prompt state (g_cp): its key monitor hands every key of the game window that is no menu shortcut
// to kb_key; kb_start (after pads_start) reads DM_KEYMAP, adds Window › Keyboard Map… (⌘⇧K) and watches the Shift, Control and Option keys, which macOS reports as
// flagsChanged, not as key downs and ups; keys_release lets go of everything the keyboard holds, wherever front_mac.m lets its input go (focus lost, the prompt, the text box).
#pragma once
#import <Cocoa/Cocoa.h>
#include "keymap.h"

static keymap g_km;      // the keyboard map (DM_KEYMAP over the default, read in kb_start)
static km_state g_ks;    // the keys held and what each pressed (a key goes back up as it went down, whatever Shift does meanwhile)
static void kb_push(void) {
    int32_t lx, ly, rx, ry; km_sticks(&g_ks, &lx, &ly, &rx, &ry);
    dm_input_set(km_buttons(&g_ks), lx, ly, rx, ry);
}
#include "keymap_panel_mac.h"
static void keys_release(void) { kp_forget(); km_release_all(&g_ks); kb_push(); }

// A key went down or up (a modifier key too: flagsChanged); mods: the modifiers held as it went down (KM_MOD_*). With a modifier held a key is its chord's target when
// the chord is bound (Shift + ↑ the right stick by default), until it is released
static void kb_on_key(unsigned short c, int down, int mods) {
    if (!g_input_on) return;
    if (cp_blocks_input(g_cp.phase)) {   // the prompt has the keyboard: Return or the key of A is done, Escape or the key of B closes it, no key reaches the game
        int t = c < KM_KEYS ? g_km.key[c] : KM_NONE;
        if (down && g_cp.phase == CP_OPEN) { if (c == 53 || t == KM_B) g_cp_cancel = 1; else if (c == 36 || t == KM_A) g_cp_confirm = 1; cp_poll(); }
        return;
    }
    if (kp_take_key(c, down, mods)) return;   // the Keyboard Map panel waits for a key
    if (km_key(&g_km, &g_ks, c, down, mods) && down) cursor_hide_for_input();   // a key the game takes
    kb_push();
}
static int kb_modifier_down(NSEvent *e) {   // flagsChanged of a Shift, Control or Option key: whether that very key is down now (the device-dependent bits of NX_DEVICE*KEYMASK)
    NSUInteger f = e.modifierFlags;
    switch (e.keyCode) {
        case 56: return (f & 0x2) != 0;   case 60: return (f & 0x4) != 0;     // left, right Shift
        case 59: return (f & 0x1) != 0;   case 62: return (f & 0x2000) != 0;  // left, right Control
        case 58: return (f & 0x20) != 0;  case 61: return (f & 0x40) != 0;    // left, right Option
    }
    return 0;
}

// front_mac.m's key monitor: a key down or up of the game window that is no menu shortcut
static void kb_key(NSEvent *e) {
    NSEventModifierFlags f = e.modifierFlags;
    if (!e.isARepeat) kb_on_key(e.keyCode, e.type == NSEventTypeKeyDown, (f & NSEventModifierFlagShift ? KM_MOD_SHIFT : 0) | (f & NSEventModifierFlagControl ? KM_MOD_CTRL : 0) |
                                                                          (f & NSEventModifierFlagOption ? KM_MOD_OPT : 0));
}

static void kb_start(NSWindow *win) {
    kp_init(&g_km, keys_release); (void)kp_fixed();   // fixed or not: as the game started
    const char *e = getenv("DM_KEYMAP"); char err[512];
    if (!e || !*e) km_default(&g_km);
    else {
        if (km_parse(&g_km, e, err, sizeof err)) LOG("[RED] front end: DM_KEYMAP: skipped %s", err);
        static char map[KM_FORMAT_MAX]; km_format(&g_km, map, sizeof map);
        LOG("front end: keyboard map (DM_KEYMAP): %s", map);
    }
    NSMenu *wm = [NSApp.mainMenu itemWithTitle:@"Window"].submenu;   // Window › Keyboard Map… (⌘⇧K)
    static DmKeymapMenuTarget *target; target = [DmKeymapMenuTarget new];
    NSMenuItem *it = [wm addItemWithTitle:@"Keyboard Map…" action:@selector(show:) keyEquivalent:@"k"];
    it.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift; it.target = target;
    // Shift, Control and Option: a key of the map only when bound (km_is_modifier); the event goes on either way (menus track modifiers)
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *ev) {
        if (ev.window == win && km_is_modifier(ev.keyCode)) kb_on_key(ev.keyCode, kb_modifier_down(ev), 0);
        return ev;
    }];
}
