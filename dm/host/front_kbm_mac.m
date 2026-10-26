// Mac front end: keyboard and mouse controls (front_mac.m calls the kbm_* functions in front_kbm_mac.h). Main thread only. The layout is Tears of the Kingdom's, the only game
// with them so far: it switches on only for a game whose patch turns the camera by the mouse (hle.h dm_mouse_camera; the rewriter emits that patch for that game's build alone), so
// no other game sees it. It stays in this one file, apart from the general front end, until a second game takes keyboard and mouse controls; that game's needs then show which
// parts are general (capture, contexts, scroll, shake, aim toggles) and which become per-game bindings in the profiles
// The controls (a game whose stubs turn the camera by the mouse, hle.h dm_mouse_camera) are opt in: the game starts with the classic keys, as every other game (most
// players use a controller), and ` switches to the controls and back; DM_KBM=1 starts with them. A click in the
// window captures the mouse (cursor hidden and held still); leaving the window (⌘Tab) or ` lets it go. The keyboard takes a PC layout (kbm_bit) and the captured mouse turns the camera by angle (raw counts from GCMouse to dm_mouse_move): WASD left stick, left click Y,
// right click ZL, middle click R3, F bow mode (ZR held; left click fires, F or right click leaves), R D-pad up,
// G throw mode (R held; left click throws, G or right click leaves), Space X, Shift B, E or Return A, Q L (while held, the mouse points the right stick, which picks in the ability wheel, and right click is B), Ctrl or C L3, 1-4 D-pad right / left / up / down (Tears of the Kingdom's quick menus:
// weapons, shields, abilities, whistle; while one is held WASD is the right stick and the scroll wheel flicks it, which pick in them, left click is X, right click B, E Y), Tab or M −, Esc + (with an object held in Ultrahand E is R, Q ZL, the scroll wheel D-pad down / up and left / right, and a shake of the mouse or R shakes the right stick; in the pause menu and the Purah Pad E is R, left click A, right click B; while aiming or using an ability right click is B), arrows D-pad.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "dm_input.h"
#include "hle.h"
#include "front_kbm_mac.h"
#include <math.h>

static uint64_t g_mbtn;   // buttons held on the captured mouse
static int g_kbm;                           // the controls on (kbm_set)
static double g_shake_t0, g_shake_until;   // a right stick shake played into the game (shake_start)
static int32_t g_flick_rx;                  // a right stick flick in a quick menu (flick_add)
static uint64_t g_aim, g_aimbits;            // aim mode (ZR the bow, R a throw; 0 none) and the buttons it holds (aim_set)
static float g_wx, g_wy;                    // the mouse's sum while the ability wheel is open (wheel_open)
static int g_mouse_cap;                     // the mouse captured (mouse_capture)
static int menu_open(void);
#define DPAD (DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN | DM_BTN_LEFT)
// The ability wheel (L held by the keyboard and mouse layout, outside the menus): the mouse points the right stick, which picks in it, instead of turning the camera
static int wheel_open(void) { return g_kbm && g_mouse_cap && (front_btn & DM_BTN_L) && !menu_open(); }
// A quick menu (weapons, shields, abilities: a D-pad key held, outside the menus and not with an object held, where the D-pad moves it): left click is X, right click B
// and E is Y, the menu's own buttons (B closes it)
static int quick_open(void) { return g_kbm && (front_btn & DPAD) && !menu_open() && dm_mouse_camera_kind() != 5; }
// The controls' own buttons and right stick, added to the keys' (front_push)
void kbm_input(uint64_t *btn, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    double now = CACurrentMediaTime();
    float wl = sqrtf(g_wx * g_wx + g_wy * g_wy);
    *btn |= g_mbtn | g_aimbits;
    if (now < g_shake_until) { *rx = ((int)((now - g_shake_t0) / 0.05) & 1) ? -32767 : 32767; *ry = 0; }   // full swings, 0.05 s each
    else if (wheel_open() && wl >= 60) { *rx = (int32_t)(g_wx / wl * 32767); *ry = (int32_t)(g_wy / wl * 32767); }   // full tilt toward the mouse's direction
    else if (g_kbm && (front_btn & DPAD)) { *rx = g_flick_rx ? g_flick_rx : *lx; *ry = *ly; *lx = *ly = 0; }   // a held D-pad opens a quick menu, picked with the right stick
}
// Ultrahand pulls apart objects stuck together when the right stick is shaken, which the mouse (camera by angle, not the stick) never does: a shake of the mouse or R
// with an object held plays 0.6 s of full right stick swings instead (at most one a second)
static void shake_tick(void) {
    front_push();
    if (CACurrentMediaTime() < g_shake_until) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 25 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ shake_tick(); });
}
// Aim modes, toggled by a key: F the bow (ZR held draws it and aims), G a throw (R held winds it up). Left click lets the button go, which fires or throws; the bow is
// drawn again after 0.15 s for the next arrow, a throw ends the mode (the weapon is gone). The same key again or right click leaves without firing: the game's own
// button + B (B while it is held puts the bow or weapon away; the button let go alone would fire), held 0.1 s, then both let go. The other key leaves, then enters its mode
static void aim_set(uint64_t bit) {
    if (bit == g_aim) return;
    if (g_aim) {
        g_aim = 0; g_aimbits |= DM_BTN_B; front_push();   // B added while the button is still held (between shots it is up and B alone puts the bow away)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (!g_aim) { g_aimbits = 0; front_push(); } });
        if (bit) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (!g_aim && g_kbm && front_input_on) aim_set(bit); });
        return;
    }
    if (bit) { g_aim = bit; g_aimbits = bit; front_push(); }
}
static void aim_fire(void) {
    uint64_t bit = g_aim;
    if (!(g_aimbits & bit)) return;   // the next arrow is not drawn yet
    g_aimbits &= ~bit; front_push();
    if (bit != DM_BTN_ZR) { g_aim = 0; return; }   // thrown: the mode ends
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (g_aim == DM_BTN_ZR) { g_aimbits |= DM_BTN_ZR; front_push(); } });
}
static void shake_start(void) {
    double now = CACurrentMediaTime();
    if (now < g_shake_until + 0.4) return;
    g_shake_t0 = now; g_shake_until = now + 0.6; shake_tick();
}
// Keyboard and mouse controls (main thread), available when the game's patch reads mouse counts (dm_mouse_camera), on from the start with DM_KBM=1; ` switches between them and the classic keys.
// A click in the window captures the mouse: the cursor is hidden and detached from it, and it reaches the game only as camera movement and its buttons; leaving the window
// (input_release) or ` gives the cursor back
static float g_mrx, g_mry;   // fractions of a count carried to the next movement
static void mouse_capture(int on) {
    if (on == g_mouse_cap || (on && !dm_mouse_camera())) return;
    g_mouse_cap = on; g_mrx = g_mry = 0;
    g_mbtn = 0; front_push();   // no mouse button stays pressed once the mouse is let go
    CGAssociateMouseAndMouseCursorPosition(!on);
    if (on) [NSCursor hide]; else [NSCursor unhide];
    LOG("front end: mouse %s", on ? "captured: it turns the camera (⌘Tab or ` gives the cursor back)" : "released (a click in the window captures it again)");
}
static void kbm_set(int on) {
    if (on && !dm_mouse_camera()) return;
    g_kbm = on; if (!on) mouse_capture(0);
    front_btn = 0; g_aim = 0; g_aimbits = 0; front_push();   // the two layouts map keys differently: nothing held stays pressed across the switch
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
    if (!g_mouse_cap || !front_input_on) return;
    if (wheel_open()) {   // the mouse points the right stick (dy up); the sum is kept within 400 counts, so turning toward another ability takes a short move
        g_wx += dx; g_wy += dy; float l = sqrtf(g_wx * g_wx + g_wy * g_wy);
        if (l > 400) { g_wx *= 400 / l; g_wy *= 400 / l; }
        front_push(); return;
    }
    g_mrx += dx; g_mry += dy;
    int32_t ix = (int32_t)g_mrx, iy = (int32_t)g_mry;
    g_mrx -= ix; g_mry -= iy;
    if (ix || iy) dm_mouse_move(ix, iy);
    if (dm_mouse_camera_kind() == 5) shake_detect(dx);
}
// Mouse button n (0 left, 1 right, 2 middle): Y, ZL, R3; in the menus left is A and right B. While aiming (the game's cameras 2 to 4: Tears of the Kingdom's bow and
// throw, Ultrahand, the other abilities, 5 an object held) right is B, which puts the weapon away or cancels the ability, and with an ability (3 to 5) left is A, which
// picks or confirms. Its release lets go of what its press pressed
static void mouse_button(int n, int down) {
    static uint64_t held[3];
    if (!g_mouse_cap || !front_input_on) return;
    if (g_aim && n < 2 && down) { if (n == 0) aim_fire(); else aim_set(0); held[n] = 0; return; }   // an aim mode: left fires or throws, right leaves
    if (down) {
        int m = menu_open(), k = dm_mouse_camera_kind();
        int q = quick_open();
        held[n] = n == 0 ? (q ? DM_BTN_X : m || k >= 3 ? DM_BTN_A : DM_BTN_Y) : n == 1 ? (m || q || k >= 2 || wheel_open() ? DM_BTN_B : DM_BTN_ZL) : DM_BTN_RS;   // right: B also in the ability wheel and the quick menus (closes them)
    }
    g_mbtn = down ? (g_mbtn | held[n]) : (g_mbtn & ~held[n]); front_push();
}
// The scroll wheel in a quick menu (a D-pad key held): one item per wheel event (macOS accelerates a fast turn into events of several lines) or per 1.5 notches of a
// trackpad (15 points), away or left the next, toward or right the previous (tested in game): a right stick flick of 0.06 s, centred again for 0.045 s (two frames and
// one at 30 fps). Scrolling during a flick is
// dropped, not queued, so a fast turn moves a few items, not a burst
static int g_flick_busy; static double g_flick_acc;
static void flick_add(double notches, int precise) {   // > 0: the next item
    if (g_flick_busy || notches == 0) return;
    if (precise) {
        if ((notches > 0) != (g_flick_acc > 0)) g_flick_acc = 0;
        g_flick_acc += notches;
        if (fabs(g_flick_acc) < 1.5) return;
        notches = g_flick_acc; g_flick_acc = 0;
    }
    g_flick_busy = 1; g_flick_rx = notches > 0 ? 32767 : -32767; front_push();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        g_flick_rx = 0; front_push();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ g_flick_busy = 0; }); });
}
// The scroll wheel with an object held (Ultrahand, the game's camera 5): vertically D-pad down (away) or up (toward), which pushes the object away or pulls it in (tested
// in game), horizontally D-pad left or right. Each notch holds the button for 0.1 s more (at most 0.5 s ahead); a turn the other way switches at once
static uint64_t g_scroll_bit[2]; static double g_scroll_until[2]; static int g_scroll_gen[2];
static void mouse_scroll(int axis, double notches) {   // axis 0: away > 0, 1: right > 0
    if (!g_mouse_cap || !front_input_on || notches == 0 || dm_mouse_camera_kind() != 5) return;
    uint64_t bit = axis ? (notches > 0 ? DM_BTN_RIGHT : DM_BTN_LEFT) : (notches > 0 ? DM_BTN_DOWN : DM_BTN_UP); double now = CACurrentMediaTime();
    if (bit != g_scroll_bit[axis]) { g_mbtn &= ~g_scroll_bit[axis]; g_scroll_bit[axis] = bit; g_scroll_until[axis] = now; }
    g_scroll_until[axis] = fmin(fmax(g_scroll_until[axis], now) + 0.1 * fabs(notches), now + 0.5);
    g_mbtn |= bit; front_push();
    int gen = ++g_scroll_gen[axis];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((g_scroll_until[axis] - now) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (gen == g_scroll_gen[axis]) { g_mbtn &= ~g_scroll_bit[axis]; g_scroll_bit[axis] = 0; front_push(); } });
}
// Keyboard and mouse layout; Shift and Ctrl come as modifier changes (kbm_flags)
static int kbm_bit(unsigned short code) {
    switch (code) {
        case 14: case 36: return 0; case 49: return 2; case 15: return 13;                   // E Return → A, Space → X, R → D-pad up (F: bow mode, on_key)
        case 12: return 6;  case 8: return 4;                                                 // Q → L, C → press left stick (G: throw mode, on_key)
        case 18: return 14; case 19: return 12; case 20: return 13; case 21: return 15;      // 1 2 3 4 → D-pad right left up down (weapons, shields, abilities, whistle)
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
void kbm_flags(NSEvent *e) {
    if (!front_input_on || !g_kbm) return;
    NSEventModifierFlags f = e.modifierFlags;
    front_btn = (front_btn & ~(uint64_t)(DM_BTN_B | DM_BTN_LS)) | (f & NSEventModifierFlagShift ? DM_BTN_B : 0) | (f & NSEventModifierFlagControl ? DM_BTN_LS : 0);   // Shift → B (run), Ctrl → L3 (crouch)
    front_push();
}
int kbm_key(unsigned short c, int down) {
    if (c == 50 || c == 10) { if (down && dm_mouse_camera()) kbm_set(!g_kbm); return 1; }   // ` (§ beside it on ISO keyboards) switches between the two layouts
    if (!g_kbm) return 0;
    if (c == 15 && down && dm_mouse_camera_kind() == 5) { shake_start(); g_kmask[c] = 0; return 1; }   // R with an object held: shake the right stick (Ultrahand: pull apart)
    if (c == 3 || c == 5) {                                                                // F: bow mode, G: throw mode, on / off
        uint64_t bit = c == 3 ? DM_BTN_ZR : DM_BTN_R;
        if (down && !menu_open()) aim_set(g_aim == bit ? 0 : bit);
        return 1;
    }
    if (down && (c == 53 || c == 48 || c == 46)) {
        int m = c == 53 ? DM_BTN_PLUS : DM_BTN_MINUS;
        if (menu_open() == m) g_menu = 0; else { g_menu = m; g_menu_t = CACurrentMediaTime(); }
    }
    int held = down && dm_mouse_camera_kind() == 5;
    int kb = c == 14 && down && (menu_open() || held) ? 7 : c == 14 && down && quick_open() ? 3 : c == 12 && held ? 8 : kbm_bit(c);   // E: R in the menus and with an object held (Ultrahand: rotate), Y in a quick menu; Q: ZL with an object held
    if (c >= 128) return 1;
    if (down) g_kmask[c] = kb >= 0 ? 1ull << kb : 0;
    front_btn = down ? (front_btn | g_kmask[c]) : (front_btn & ~g_kmask[c]);
    if (g_kmask[c] & DM_BTN_L) g_wx = g_wy = 0;   // the ability wheel opens or closes: the mouse's aim starts again from the centre
    front_push(); return 1;
}
int kbm_on(void) { return g_kbm; }
// A mouse button in the game view (0 left, 1 right, 2 middle); 1: taken by the controls, not a touch. The click that captures the mouse is not an attack
int kbm_mouse(int n, int down) {
    if (g_mouse_cap) { mouse_button(n, down); return 1; }
    if (n == 0 && down && g_kbm) { if (front_input_on) mouse_capture(1); return 1; }
    return 0;
}
void kbm_scroll(NSEvent *e) {
    if (e.momentumPhase != NSEventPhaseNone) return;   // a trackpad's glide after the fingers lift
    double k = (e.hasPreciseScrollingDeltas ? 0.1 : 1) * (e.isDirectionInvertedFromDevice ? -1 : 1);   // points on a trackpad, lines on a wheel; by the device's own direction, whatever the natural scrolling setting
    double y = e.scrollingDeltaY * k, x = -e.scrollingDeltaX * k;
    if (e.hasPreciseScrollingDeltas) { if (fabs(x) < fabs(y) / 2) x = 0; else if (fabs(y) < fabs(x) / 2) y = 0; }   // a trackpad's stray other axis
    if (g_mouse_cap && front_input_on && g_kbm && (front_btn & DPAD) && dm_mouse_camera_kind() != 5) { flick_add(y - x, e.hasPreciseScrollingDeltas); return; }   // a quick menu: away or left is the next item
    mouse_scroll(0, y); mouse_scroll(1, x);
}
void kbm_release(void) { mouse_capture(0); g_aim = 0; g_aimbits = 0; }
// Mouse camera: GCMouse reports raw counts (no pointer acceleration), dy up; handlers run on the main queue
void kbm_start(void) {
    if (!dm_mouse_camera()) return;
    void (^hook)(GCMouse *) = ^(GCMouse *m) { m.mouseInput.mouseMovedHandler = ^(GCMouseInput *mi, float dx, float dy) { (void)mi; mouse_moved(dx, dy); }; };
    for (GCMouse *m in GCMouse.mice) hook(m);
    [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { hook(n.object); }];
    const char *e = getenv("DM_KBM");
    if (e && atoi(e) > 0) kbm_set(1);
    else LOG("front end: classic keys (` for keyboard and mouse controls, DM_KBM=1 starts with them)");
}
