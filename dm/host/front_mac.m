// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard and controllers → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// The window is fullscreen-capable (the green button and ⌃⌘F); DM_FULLSCREEN=1 starts it that way. ⌘Q quits; the left mouse button is the touchscreen.
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−, Space=shake the controller.
// Controllers: buttons by position as on the iPhone (right A, bottom B, top X, left Y), shoulders L R, triggers ZL ZR, Menu + Options −, both sticks and their presses.
// Each controller is a player of its own (local co-op): the first one connected is player 1, together with the keyboard, the next player 2, and so on up to 8 (pad_slots.h);
// DM_PAD_MERGE=1 merges every controller into player 1 instead, DM_KBD_PLAYER=own makes the keyboard player 1 on its own, with the controllers from player 2 on.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"
#include "pad_slots.h"
#include "haptics_apple.h"
#include "motion_apple.h"
#include "motion_pro_mac.h"
#include <signal.h>
#include <math.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;
static int g_input_on = 1;   // main thread: the game window is key, so its keyboard, mouse and controllers reach the game (input_release / input_resume)

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;   // the window's screen's maximumFramesPerSecond, kept up to date when the window moves to another screen
int front_screen_max_fps(void) { return g_screen_fps; }
// The screen's refresh rate matched to the game's frame rate in fullscreen (front_screen_match, from PR #79; DM_SCREEN_MATCH=0 turns it off). 60 fps on a 144 Hz screen cannot be paced
// evenly: the display link counts a tick on the refresh nearest each 1/60 s slot (nvn_mtl.m link_range), so frames stay up 2 and 3 refreshes in turn (13.9 / 20.8 ms) and Apple's HUD shows
// the frame interval jumping between them. In fullscreen, a screen whose rate is not a multiple of the game's switches to the mode of the same size with the highest rate that is (144 → 120 Hz
// for 60 and 30 fps), for this app only: macOS switches it back when the host quits, crashes included, and leaving fullscreen or that screen switches it back at once. A window keeps the
// screen's rate, because the switch changes the whole screen for every app and most external screens go black for a second or more. A screen with no such mode keeps the virtual ticks
static NSWindow *g_win; static _Atomic int g_match_want;   // the game's window; the frame rate to match (0 = not known yet), written by the present thread only
static CGDirectDisplayID g_match_id; static CGDisplayModeRef g_match_orig;   // main thread: the screen this host switched and its own mode (NULL = none switched)
static double mode_hz(CGDisplayModeRef m) { double hz = CGDisplayModeGetRefreshRate(m); return hz > 0 ? hz : 0; }   // 0 = not known (older built-in screens report none)
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }   // 59.94 Hz counts as 60
static CGError screen_set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {   // for this app only (kCGConfigureForAppOnly)
    CGDisplayConfigRef cfg; CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    return CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
}
static void screen_match_restore(int on_it) {   // main thread: the screen this host switched gets its own mode again; on_it: the window is on that screen
    double hz = mode_hz(g_match_orig); CGError r = screen_set_mode(g_match_id, g_match_orig);
    LOG("front end: screen %s %.0f Hz", r == kCGErrorSuccess ? "back at" : "could not be switched back to", hz);
    if (r == kCGErrorSuccess && on_it && hz > 0) g_screen_fps = (int)round(hz);
    CGDisplayModeRelease(g_match_orig); g_match_orig = NULL;
}
static void screen_match_apply(void) {   // main thread: on a new frame rate, on entering and leaving fullscreen and on a move to another screen
    static int off = -1; if (off < 0) { const char *e = getenv("DM_SCREEN_MATCH"); off = e && !strcmp(e, "0"); if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)"); }
    NSScreen *scr = g_win.screen; int want = g_match_want, full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_match_orig && (!full || id != g_match_id)) screen_match_restore(id == g_match_id);
    if (off || !full || want <= 0 || !scr) return;
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    double hz = mode_hz(cur);
    if (hz <= 0 || hz_multiple(hz, want)) {   // already a multiple, or not known
        if (g_match_orig && hz > 0) g_screen_fps = (int)round(hz);   // the screen this host switched: its rate from the mode, which maximumFramesPerSecond (the screen observer) may not show yet
        CGDisplayModeRelease(cur); return;
    }
    size_t w = CGDisplayModeGetWidth(cur), h = CGDisplayModeGetHeight(cur), pw = CGDisplayModeGetPixelWidth(cur), ph = CGDisplayModeGetPixelHeight(cur);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES});
    CGDisplayModeRef pick = NULL; double pick_hz = 0;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i); double r = mode_hz(m);
        if (CGDisplayModeGetWidth(m) != w || CGDisplayModeGetHeight(m) != h || CGDisplayModeGetPixelWidth(m) != pw || CGDisplayModeGetPixelHeight(m) != ph || !CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        if (hz_multiple(r, want) && r > pick_hz) { pick = m; pick_hz = r; }
    }
    if (!pick) LOG("front end: screen at %.0f Hz has no mode of this size at a multiple of %d Hz: frames are paced on the nearest refreshes", hz, want);
    else {
        CGError r = screen_set_mode(id, pick);
        LOG("front end: screen at %.0f Hz %s %.0f Hz for %d fps in fullscreen (DM_SCREEN_MATCH=0 keeps it)", hz, r == kCGErrorSuccess ? "switched to" : "could not be switched to", pick_hz, want);
        if (r == kCGErrorSuccess) { if (!g_match_orig) { g_match_orig = CGDisplayModeRetain(cur); g_match_id = id; } g_screen_fps = (int)round(pick_hz); }
    }
    if (modes) CFRelease(modes);
    CGDisplayModeRelease(cur);
}
void front_screen_match(int fps) {
    if (!g_layer || g_match_want == fps) return;   // no window (a windowless run's main thread runs the guest), or the rate as before
    g_match_want = fps; dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); });
}
static uint64_t g_mbtn;   // buttons held on the captured mouse
static int g_kbm;   // keyboard and mouse controls (below)
static double g_shake_t0, g_shake_until;   // a right stick shake played into the game (shake_start)
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    double now = CACurrentMediaTime();
    if (now < g_shake_until) dm_input_set(g_btn | g_mbtn, lx, ly, ((int)((now - g_shake_t0) / 0.05) & 1) ? -32767 : 32767, 0);   // full swings, 0.05 s each
    else if (g_kbm && (g_btn & (DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN | DM_BTN_LEFT))) dm_input_set(g_btn | g_mbtn, 0, 0, lx, ly);   // a held D-pad opens a quick menu, picked with the right stick
    else dm_input_set(g_btn | g_mbtn, lx, ly, 0, 0);
}
// Ultrahand pulls apart objects stuck together when the right stick is shaken, which the mouse (camera by angle, not the stick) never does: a shake of the mouse or R
// with an object held plays 0.6 s of full right stick swings instead (at most one a second)
static void shake_tick(void) {
    push();
    if (CACurrentMediaTime() < g_shake_until) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ shake_tick(); });
}
static void shake_start(void) {
    double now = CACurrentMediaTime();
    if (now < g_shake_until + 0.4) return;
    g_shake_t0 = now; g_shake_until = now + 0.6; shake_tick();
}
// Keyboard and mouse controls (main thread), on from the start when the game's patch reads mouse counts (dm_mouse_camera); ` switches between them and the classic keys.
// A click in the window captures the mouse: the cursor is hidden and detached from it, and it reaches the game only as camera movement and its buttons; leaving the window
// (input_release) or ` gives the cursor back
static int g_mouse_cap;
static float g_mrx, g_mry;   // fractions of a count carried to the next movement
static void mouse_capture(int on) {
    if (on == g_mouse_cap || (on && !dm_mouse_camera())) return;
    g_mouse_cap = on; g_mrx = g_mry = 0;
    g_mbtn = 0; push();   // no mouse button stays pressed once the mouse is let go
    CGAssociateMouseAndMouseCursorPosition(!on);
    if (on) [NSCursor hide]; else [NSCursor unhide];
    LOG("front end: mouse %s", on ? "captured: it turns the camera (⌘Tab or ` gives the cursor back)" : "released (a click in the window captures it again)");
}
static void kbm_set(int on) {
    if (on && !dm_mouse_camera()) return;
    g_kbm = on; if (!on) mouse_capture(0);
    g_btn = 0; push();   // the two layouts map keys differently: nothing held stays pressed across the switch
    LOG("front end: %s", on ? "keyboard and mouse controls (click in the window to capture the mouse, ` for the classic keys)" : "classic keys (` for keyboard and mouse controls)");
}
// A mouse shake: 4 turns left / right within 0.7 s, each after at least 150 counts the other way (a few centimetres at common resolutions; aiming turns slower)
static void shake_detect(float dx) {
    static int dir, n; static float run; static double t[4];
    int d = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    if (!d) return;
    if (d == dir) { run += fabsf(dx); return; }
    double now = CACurrentMediaTime();
    if (run >= 150) { t[n++ & 3] = now; if (n >= 4 && now - t[n & 3] < 0.7) { n = 0; shake_start(); } }
    else n = 0;
    dir = d; run = fabsf(dx);
}
static void mouse_moved(float dx, float dy) {
    if (!g_mouse_cap || !g_input_on) return;
    g_mrx += dx; g_mry += dy;
    int32_t ix = (int32_t)g_mrx, iy = (int32_t)g_mry;
    g_mrx -= ix; g_mry -= iy;
    if (ix || iy) dm_mouse_move(ix, iy);
    if (dm_mouse_camera_kind() == 5) shake_detect(dx);
}
static int menu_open(void);
// Mouse button n (0 left, 1 right, 2 middle): Y, ZL, R3; in the menus left is A and right B. While aiming (the game's cameras 2 to 4: Tears of the Kingdom's bow and
// throw, Ultrahand, the other abilities, 5 an object held) right is B, which puts the weapon away or cancels the ability, and with an ability (3 to 5) left is A, which
// picks or confirms. Its release lets go of what its press pressed
static void mouse_button(int n, int down) {
    static uint64_t held[3];
    if (!g_mouse_cap || !g_input_on) return;
    if (down) {
        int m = menu_open(), k = dm_mouse_camera_kind();
        held[n] = n == 0 ? (m || k >= 3 ? DM_BTN_A : DM_BTN_Y) : n == 1 ? (m || k >= 2 ? DM_BTN_B : DM_BTN_ZL) : DM_BTN_RS;
    }
    g_mbtn = down ? (g_mbtn | held[n]) : (g_mbtn & ~held[n]); push();
}
// The scroll wheel with an object held (Ultrahand, the game's camera 5): vertically D-pad down (away) or up (toward), which pushes the object away or pulls it in (tested
// in game), horizontally D-pad left or right. Each notch holds the button for 0.1 s more (at most 0.5 s ahead); a turn the other way switches at once
static uint64_t g_scroll_bit[2]; static double g_scroll_until[2]; static int g_scroll_gen[2];
static void mouse_scroll(int axis, double notches) {   // axis 0: away > 0, 1: right > 0
    if (!g_mouse_cap || !g_input_on || notches == 0 || dm_mouse_camera_kind() != 5) return;
    uint64_t bit = axis ? (notches > 0 ? DM_BTN_RIGHT : DM_BTN_LEFT) : (notches > 0 ? DM_BTN_DOWN : DM_BTN_UP); double now = CACurrentMediaTime();
    if (bit != g_scroll_bit[axis]) { g_mbtn &= ~g_scroll_bit[axis]; g_scroll_bit[axis] = bit; g_scroll_until[axis] = now; }
    g_scroll_until[axis] = fmin(fmax(g_scroll_until[axis], now) + 0.1 * fabs(notches), now + 0.5);
    g_mbtn |= bit; push();
    int gen = ++g_scroll_gen[axis];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((g_scroll_until[axis] - now) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (gen == g_scroll_gen[axis]) { g_mbtn &= ~g_scroll_bit[axis]; g_scroll_bit[axis] = 0; push(); } });
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
// Keyboard and mouse layout; Shift and Ctrl come as modifier changes (on_flags)
static int kbm_bit(unsigned short code) {
    switch (code) {
        case 14: case 36: return 0; case 49: return 2; case 3: return 9;                     // E Return → A, Space → X, F → ZR
        case 12: return 6;  case 5: return 7;   case 8: return 4;                             // Q → L, G → R, C → press left stick
        case 18: return 13; case 19: return 14; case 20: return 15; case 21: return 12;      // 1 2 3 4 → D-pad up right down left
        case 53: return 10; case 48: case 46: return 11;                                      // Esc → +, Tab M → −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15;  // ← ↑ → ↓
    }
    return -1;
}
// The menus (Esc → + the pause menu, Tab or M → − the Purah Pad): while one is open E is R, so Q and E switch its tabs (L / R). The host does not see the game's menus: the key
// that opened one closes it, the other key switches to the other menu (it stays open), and it counts as closed once the camera runs again (dm_mouse_camera_idle: the
// game's cameras stand still in the menus), which also catches a menu closed with B
static int g_menu; static double g_menu_t;   // g_menu: the button of the open menu (DM_BTN_PLUS or DM_BTN_MINUS), 0 when none
static int menu_open(void) {
    if (g_menu && CACurrentMediaTime() - g_menu_t > 0.5 && dm_mouse_camera_idle() < 0.1) g_menu = 0;
    return g_menu;
}
static uint64_t g_kmask[128];   // the button each key pressed, released by its key-up even if the layout changed in between
static void on_flags(NSEvent *e) {
    if (!g_input_on || !g_kbm) return;
    NSEventModifierFlags f = e.modifierFlags;
    g_btn = (g_btn & ~(uint64_t)(DM_BTN_B | DM_BTN_LS)) | (f & NSEventModifierFlagShift ? DM_BTN_B : 0) | (f & NSEventModifierFlagControl ? DM_BTN_LS : 0);   // Shift → B (run), Ctrl → L3 (crouch)
    push();
}
static void on_key(NSEvent *e, int down) {
    if (!g_input_on) return;
    unsigned short c = e.keyCode;
    if (c == 50 || c == 10) { if (down && dm_mouse_camera()) kbm_set(!g_kbm); return; }   // ` (§ beside it on ISO keyboards) switches between the two layouts
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    if (g_kbm) {
        if (c == 15) { if (down && dm_mouse_camera_kind() == 5) shake_start(); return; }   // R: shake the right stick (Ultrahand: pull apart)
        if (down && (c == 53 || c == 48 || c == 46)) {
            int m = c == 53 ? DM_BTN_PLUS : DM_BTN_MINUS;
            if (menu_open() == m) g_menu = 0; else { g_menu = m; g_menu_t = CACurrentMediaTime(); }
        }
        int held = down && dm_mouse_camera_kind() == 5;
        int kb = c == 14 && down && (menu_open() || held) ? 7 : c == 12 && held ? 8 : kbm_bit(c);   // E: R in the menus and with an object held (Ultrahand: rotate); Q: ZL with an object held
        if (c >= 128) return;
        if (down) g_kmask[c] = kb >= 0 ? 1ull << kb : 0;
        g_btn = down ? (g_btn | g_kmask[c]) : (g_btn & ~g_kmask[c]); push(); return;
    }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    push();
}

// Physical controllers (GameController) go to dm_pad_set_player, mapped as the iPhone's (DmGamepad.m read_pad, same layout); player 1's is ORed with the keyboard, a stick takes
// whichever is deflected more (hle_hid.c). DM_KEYS uses player 1's path, so player 1's controller touched during a script takes it over.
// Players: every controller with an extended gamepad profile holds a slot (pad_slots.h: player 1 first, a reconnected controller gets its player back), and hle_hid.c reports
// player n on its own npad while the slot is held (dm_pad_players). A one-player game that supports only No1 / Handheld still sees every controller as player 1 (hle_hid.c).
// With DM_PAD_MERGE=1 every controller is player 1, as before (the last one moved wins; when the last one goes away its buttons and sticks are released)
static pad_slots g_slots;
static GCController *g_held[DM_PLAYERS];   // the controller of each slot, kept alive while it holds it (the slot table keys on its pointer)
static int g_merge, g_kbd_own;
static int32_t pad_axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX); }
static void pad_read(GCExtendedGamepad *g) {
    if (!g_input_on) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)g.controller); if (p < 0) return;
    dm_haptics_apple_controller(p, g.controller);
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.buttonB, DM_BTN_A); K(g.buttonA, DM_BTN_B); K(g.buttonY, DM_BTN_X); K(g.buttonX, DM_BTN_Y);   // by position: right A, bottom B, top X, left Y
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);   // nil on pads without them: not pressed
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    dm_pad_set_player(p, m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value), pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}
static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
static void pro_players_changed(GCController *gone) {
    int count = 0, player = -1;
    for (GCController *c in GCController.controllers) if (c != gone && c.extendedGamepad && [c.productCategory isEqualToString:@"Switch Pro Controller"]) {
        count++; player = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)c);
    }
    dm_motion_pro_players(count, player);
}
static void pro_devices_changed(void) { pro_players_changed(nil); }
// Called at start and on every connect / disconnect (gone: the controller that was just removed, nil on a connect)
static void pads_changed(GCController *gone) {
    if (g_merge) {
        int any = 0;
        for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) {
            any = 1; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            dm_motion_attach(c, 0);   // every controller is player 1: the last one moved drives its six-axis sensor
            pad_read(c.extendedGamepad);
        }
        if (gone) dm_motion_detach(gone, 0);
        if (!any) { dm_pad_set(0, 0, 0, 0, 0); dm_haptics_apple_controller(0, nil); }
        pro_players_changed(gone);
        return;
    }
    NSArray<GCController *> *now = GCController.controllers; char nb[16];
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p] && (g_held[p] == gone || ![now containsObject:g_held[p]])) {
        npad_name(p, nb, sizeof nb); LOG("controller '%s' disconnected: player %d (npad %s) is free", pad_name(g_held[p]), p + 1, nb);
        g_held[p].extendedGamepad.valueChangedHandler = nil;
        dm_motion_detach(g_held[p], p);
        dm_haptics_apple_controller(p, nil);
        pad_slot_free(&g_slots, (__bridge const void *)g_held[p]); g_held[p] = nil;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    }
    for (GCController *c in now) if (c.extendedGamepad && c != gone && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) {
        int p = pad_slot_take(&g_slots, (__bridge const void *)c, pad_name(c));
        if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", pad_name(c), DM_PLAYERS); continue; }
        g_held[p] = c;
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
        dm_motion_attach(c, p);
        npad_name(p, nb, sizeof nb); LOG("controller '%s' connected: player %d (npad %s)%s", pad_name(c), p + 1, nb, c.motion.hasRotationRate ? ", with motion (gyro)" : "");
        pad_read(c.extendedGamepad);
    }
    dm_pad_players(pad_slot_mask(&g_slots));
    pro_players_changed(gone);
}
// Focus: a window that is no longer key sees no key-up, mouse-up or controller release, so whatever is held when it loses focus (another app, the text box, minimized) would
// stay pressed in the game. The front end releases only what it pressed itself: scripted buttons and sticks have their own path, and the touch is also DM_KEYS / DM_CTL's (hle_hid.c).
// Out of focus a controller's changes are ignored, so back in focus each controller is read as it is now (a button still held, a stick still deflected)
static int g_mouse_down;   // the mouse holds the touch (DmGameView)
static void input_release(void) {
    mouse_capture(0);
    g_input_on = 0;
    dm_motion_pro_active(0);
    g_btn = 0; g_w = g_a = g_s = g_d = 0; push();
    if (g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); }
    if (g_merge) { for (GCController *c in GCController.controllers) if (c.extendedGamepad) { dm_pad_set(0, 0, 0, 0, 0); break; } }
    else for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
}
static void input_resume(void) {
    g_input_on = 1;
    dm_motion_pro_active(1);
    if (g_merge) { for (GCController *c in GCController.controllers) if (c.extendedGamepad) pad_read(c.extendedGamepad); }
    else for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p].extendedGamepad) pad_read(g_held[p].extendedGamepad);
}
static void pads_start(void) {
    const char *e = getenv("DM_PAD_MERGE"); g_merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); g_kbd_own = !g_merge && e && !strcmp(e, "own");
    static const char kbd = 0;
    if (g_kbd_own) pad_slot_take(&g_slots, &kbd, "keyboard");   // holds player 1 for good
    LOG("front end: %s", g_merge ? "every controller is player 1 (DM_PAD_MERGE)" : g_kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)"
                                                                           : "each controller is a player of its own, the keyboard plays with player 1");
    pads_changed(nil);
    dm_motion_pro_start(pro_devices_changed);
}

// Mouse as a single-point touchscreen (front.h dm_touch_set): left button down / drag = finger 0, release = lifted. The content view takes the clicks,
// not a window-wide event monitor, so clicks on the title bar, the window buttons and the resize edges still reach AppKit.
@interface DmGameView : NSView @end
@implementation DmGameView
- (void)mouseDown:(NSEvent *)e {
    if (g_mouse_cap) mouse_button(0, 1);
    else if (g_kbm) { if (g_input_on) mouse_capture(1); }   // the click that captures the mouse is not an attack
    else [self mouseDragged:e];
}
- (void)rightMouseDown:(NSEvent *)e { (void)e; mouse_button(1, 1); }
- (void)rightMouseUp:(NSEvent *)e { (void)e; mouse_button(1, 0); }
- (void)otherMouseDown:(NSEvent *)e { if (e.buttonNumber == 2) mouse_button(2, 1); }
- (void)otherMouseUp:(NSEvent *)e { if (e.buttonNumber == 2) mouse_button(2, 0); }
- (void)scrollWheel:(NSEvent *)e {
    if (e.momentumPhase != NSEventPhaseNone) return;   // a trackpad's glide after the fingers lift
    double k = (e.hasPreciseScrollingDeltas ? 0.1 : 1) * (e.isDirectionInvertedFromDevice ? -1 : 1);   // points on a trackpad, lines on a wheel; by the device's own direction, whatever the natural scrolling setting
    double y = e.scrollingDeltaY * k, x = -e.scrollingDeltaX * k;
    if (e.hasPreciseScrollingDeltas) { if (fabs(x) < fabs(y) / 2) x = 0; else if (fabs(y) < fabs(x) / 2) y = 0; }   // a trackpad's stray other axis
    mouse_scroll(0, y); mouse_scroll(1, x);
}
- (void)mouseDragged:(NSEvent *)e {
    if (!g_input_on || g_kbm) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSSize b = self.bounds.size;
    double w = b.width, h = b.height;
    if (w <= 0 || h <= 0) return;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;   // the centered 16:9 picture the present fits into the view, as on the iPhone (DmGamepad.m)
    double x0 = (b.width - w) / 2, y0 = (b.height - h) / 2;
    dm_touch t = {0, (float)fmin(fmax((p.x - x0) / w, 0), 0.999), (float)fmin(fmax(1 - (p.y - y0) / h, 0), 0.999)};   // The AppKit view origin is at bottom left, the touchscreen origin is at top left
    dm_touch_set(&t, 1); g_mouse_down = 1;
}
- (void)mouseUp:(NSEvent *)e { (void)e; mouse_button(0, 0); if (g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); } }
// The drawable follows the view in pixels: a layer assigned to a layer-backed view kept the drawable at the first window's 1280x720 in fullscreen too, so the game picture was
// scaled down to it and up again by the compositor
- (void)fitDrawable {
    CAMetalLayer *l = (CAMetalLayer *)self.layer; if (![l isKindOfClass:CAMetalLayer.class] || !self.window) return;
    l.contentsScale = self.window.backingScaleFactor;
    NSSize px = [self convertSizeToBacking:self.bounds.size]; if (px.width < 1 || px.height < 1 || CGSizeEqualToSize(l.drawableSize, px)) return;
    l.drawableSize = px; LOG("front end: drawable %.0fx%.0f", px.width, px.height);
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self fitDrawable]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self fitDrawable]; }
@end

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

// Metal reads MTL_HUD_ENABLED when the process makes its first device, which can be before the window: set from DM_METAL_HUD before main (set in front_run, the HUD did not show)
__attribute__((constructor)) static void hud_env(void) { front_hud_env(); }
void front_run(void (*guest)(void)) {
    // A game: tell macOS its work is latency-critical, so the system does not trade its frame time for power while the frame rate is capped
    static id activity; activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @DM_GAME_NAME;   // the game profile's name (game name label)
        // Fullscreen: without FullScreenPrimary the green button only zooms the window. The present path already scales
        // the game picture to fit the drawable with black bars around it, so fullscreen keeps the game's aspect ratio.
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        NSView *v = win.contentView = [[DmGameView alloc] initWithFrame:r];
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) { g_layer.developerHUDProperties = @{@"mode": @"default"}; LOG("front end: Metal performance HUD on (DM_METAL_HUD)"); }   // Apple's HUD over the game: frame rate, frame and GPU time, memory
        v.layer = g_layer; [(DmGameView *)v fitDrawable];
        [win center];
        [win makeKeyAndOrderFront:nil];
        g_screen_fps = (int)(win.screen ?: NSScreen.mainScreen).maximumFramesPerSecond;
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];
        // Main menu: AppKit runs ⌘ shortcuts only through menu items, so ⌘Q quits and ⌃⌘F toggles fullscreen from here (AppKit shows the item as Globe-F and keeps ⌃⌘F as a hidden twin)
        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
        [app addItemWithTitle:@"Quit " DM_GAME_NAME action:@selector(terminate:) keyEquivalent:@"q"];
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];
        [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        const char *fs = getenv("DM_FULLSCREEN");   // 1 (or any value but 0): start in fullscreen; 0, the settings window's Window, or unset: a window
        if (fs && strcmp(fs, "0")) dispatch_async(dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
        for (NSNotificationName nm in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [NSNotificationCenter.defaultCenter addObserverForName:nm object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                GCController *c = n.object; int conn = n.name == GCControllerDidConnectNotification;
                if (!c.extendedGamepad || g_merge) LOG("controller %s: %s%s", conn ? "connected" : "disconnected", pad_name(c), c.extendedGamepad ? "" : " (no extended gamepad profile: not used)");
                pads_changed(conn ? nil : c); }];
        dm_haptics_apple_init();
        pads_start();
        // Mouse camera: GCMouse reports raw counts (no pointer acceleration), dy up; handlers run on the main queue
        if (dm_mouse_camera()) {
            void (^hook)(GCMouse *) = ^(GCMouse *m) { m.mouseInput.mouseMovedHandler = ^(GCMouseInput *mi, float dx, float dy) { (void)mi; mouse_moved(dx, dy); }; };
            for (GCMouse *m in GCMouse.mice) hook(m);
            [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { hook(n.object); }];
            kbm_set(1);
        }
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if (e.type == NSEventTypeFlagsChanged) { on_flags(e); return e; }
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;   // ⌘ shortcuts go to the menu; a key released while ⌘ is held still reaches the game, or it would stay pressed
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1).
        // Input is released when the window stops being key (and when it is minimized) and taken up again when it is key again
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (win.screen) { g_screen_fps = (int)win.screen.maximumFramesPerSecond; g_layer.contentsScale = win.backingScaleFactor; } screen_match_apply(); }];
        // Entering and leaving fullscreen: the layer is layer-backed, so its frame (and with it the drawable) follows the
        // content view by itself; only the scale factor has to be carried over to the screen the window ended up on. The screen's rate is matched in fullscreen only (screen_match_apply)
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); if (!win.miniaturized) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_resume(); dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Progress screen of the prepare step (front.h front_progress): a dark view over the game's, the title, a bar and a count, centered; AppKit views, so it draws while the game's thread waits
static NSView *g_prog; static NSTextField *g_prog_title, *g_prog_count; static NSProgressIndicator *g_prog_bar;
static NSTextField *prog_label(NSRect r, CGFloat size, NSFontWeight w, NSColor *c) {
    NSTextField *t = [NSTextField labelWithString:@""]; t.frame = r; t.alignment = NSTextAlignmentCenter; t.textColor = c;
    t.font = [NSFont monospacedDigitSystemFontOfSize:size weight:w]; return t;
}
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!tt) { [g_prog removeFromSuperview]; g_prog = nil; return; }
        if (!g_prog) {
            NSView *cv = g_win.contentView;
            g_prog = [[NSView alloc] initWithFrame:cv.bounds]; g_prog.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            g_prog.wantsLayer = YES; g_prog.layer.backgroundColor = [NSColor colorWithWhite:0.06 alpha:0.94].CGColor;
            NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(floor((cv.bounds.size.width - 400) / 2), floor((cv.bounds.size.height - 96) / 2), 400, 96)];
            box.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;
            g_prog_title = prog_label(NSMakeRect(0, 62, 400, 30), 20, NSFontWeightSemibold, NSColor.whiteColor);
            g_prog_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 36, 360, 20)];
            g_prog_bar.style = NSProgressIndicatorStyleBar; g_prog_bar.indeterminate = NO; g_prog_bar.minValue = 0;
            g_prog_count = prog_label(NSMakeRect(0, 4, 400, 22), 13, NSFontWeightRegular, [NSColor colorWithWhite:0.72 alpha:1]);
            [box addSubview:g_prog_title]; [box addSubview:g_prog_bar]; [box addSubview:g_prog_count]; [g_prog addSubview:box];
            [cv addSubview:g_prog];
        }
        g_prog_title.stringValue = tt; g_prog_bar.maxValue = total > 0 ? (double)total : 1; g_prog_bar.doubleValue = (double)done;
        g_prog_count.stringValue = [NSString stringWithFormat:@"%@ of %@", [NSNumberFormatter localizedStringFromNumber:@(done) numberStyle:NSNumberFormatterDecimalStyle],
                                    [NSNumberFormatter localizedStringFromNumber:@(total) numberStyle:NSNumberFormatterDecimalStyle]];
    });
}

// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
extern int g_swkbd_min_chars;   // hle_swkbd.c: this input's minimum length from ShowKeyboardArg; the applet keeps OK disabled until the text is long enough
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        // The box takes the keyboard and the mouse, so the game window never sees the key-up of the button or the touch-up of the click that opened it: nothing may stay pressed
        // while it is up, or the game reads the stuck button / touch the moment the box closes (Stardew Valley's character creation re-opened the keyboard endlessly)
        g_btn = 0; g_w = g_a = g_s = g_d = 0; push();
        dm_touch_set(NULL, 0);
        int min_chars = g_swkbd_min_chars > 0 ? g_swkbd_min_chars : 0;
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        for (;;) {
            NSAlert *a = [NSAlert new];
            a.messageText = title && *title ? @(title) : @"Input";
            a.informativeText = info;
            [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
            NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
            f.stringValue = text ?: (initial ? @(initial) : @"");
            if (guide && *guide) f.placeholderString = @(guide);
            a.accessoryView = f; a.window.initialFirstResponder = f;
            r = [a runModal] == NSAlertFirstButtonReturn;
            text = f.stringValue;
            if (!r || (int)text.length >= min_chars) break;
            // On hardware the applet would not have accepted the shorter text (OK stayed disabled); a game that rejects it re-opens the keyboard for ever, so the box comes back with the rule instead
            info = [info stringByAppendingFormat:@"\nAt least %d characters are needed here.", min_chars];
        }
    });
    if (!r) return 0;
    (void)max_chars;                              // Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
