// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard and controllers → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// The window is fullscreen-capable (the green button and ⌃⌘F); DM_FULLSCREEN starts it that way. ⌘Q quits; the left mouse button is the touchscreen.
// Key map: Fully configurable with presets (Nintendo, PC Action, Custom); supports Left & Right analog sticks, face buttons, shoulders, triggers, clicks, and motion shake.
// Controllers: Supports Nintendo position layout and Xbox label layout, configurable deadzones, and independent local multiplayer mapping for Players 1–4.
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"
#include "pad_slots.h"
#include "haptics_apple.h"
#include <signal.h>
#include <math.h>
#include <unistd.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_lx_pos, g_lx_neg, g_ly_pos, g_ly_neg;
static int g_rx_pos, g_rx_neg, g_ry_pos, g_ry_neg;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;
int front_screen_max_fps(void) { return g_screen_fps; }

static NSWindow *g_win; static _Atomic int g_match_want;
static CGDirectDisplayID g_match_id; static CGDisplayModeRef g_match_orig;
static double mode_hz(CGDisplayModeRef m) { double hz = CGDisplayModeGetRefreshRate(m); return hz > 0 ? hz : 0; }
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }
static CGError screen_set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {
    CGDisplayConfigRef cfg; CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    return CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
}
static void screen_match_restore(int on_it) {
    double hz = mode_hz(g_match_orig); CGError r = screen_set_mode(g_match_id, g_match_orig);
    LOG("front end: screen %s %.0f Hz", r == kCGErrorSuccess ? "back at" : "could not be switched back to", hz);
    if (r == kCGErrorSuccess && on_it && hz > 0) g_screen_fps = (int)round(hz);
    CGDisplayModeRelease(g_match_orig); g_match_orig = NULL;
}
static void screen_match_apply(void) {
    static int off = -1; if (off < 0) { const char *e = getenv("DM_SCREEN_MATCH"); off = e && !strcmp(e, "0"); if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)"); }
    NSScreen *scr = g_win.screen; int want = g_match_want, full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_match_orig && (!full || id != g_match_id)) screen_match_restore(id == g_match_id);
    if (off || !full || want <= 0 || !scr) return;
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    double hz = mode_hz(cur);
    if (hz <= 0 || hz_multiple(hz, want)) {
        if (g_match_orig && hz > 0) g_screen_fps = (int)round(hz);
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
    if (!g_layer || g_match_want == fps) return;
    g_match_want = fps; dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); });
}

typedef enum {
    PLAYER_DEV_DISCONNECTED = 0,
    PLAYER_DEV_KEYBOARD,
    PLAYER_DEV_CONTROLLER
} PlayerDeviceType;

typedef struct {
    PlayerDeviceType type;
    __strong GCController *controller;
} PlayerSlot;

static PlayerSlot g_players[DM_PLAYERS];
static int g_pad_xbox_layout;
static float g_pad_deadzone = 0.08f;

static void push(void) {
    int32_t lx = (g_lx_pos - g_lx_neg) * 32767;
    int32_t ly = (g_ly_pos - g_ly_neg) * 32767;
    int32_t rx = (g_rx_pos - g_rx_neg) * 32767;
    int32_t ry = (g_ry_pos - g_ry_neg) * 32767;

    int kbd_p = -1;
    for (int i = 0; i < DM_PLAYERS; i++) {
        if (g_players[i].type == PLAYER_DEV_KEYBOARD) {
            kbd_p = i;
            break;
        }
    }

    if (kbd_p == 0) {
        dm_input_set(g_btn, lx, ly, rx, ry);
    } else {
        dm_input_set(0, 0, 0, 0, 0);
        if (kbd_p > 0) {
            dm_pad_set_player(kbd_p, g_btn, lx, ly, rx, ry);
        }
    }
}

typedef NS_ENUM(NSInteger, DmKeyAction) {
    DmKeyActionMoveUp = 0,
    DmKeyActionMoveDown,
    DmKeyActionMoveLeft,
    DmKeyActionMoveRight,
    DmKeyActionStickRUp,
    DmKeyActionStickRDown,
    DmKeyActionStickRLeft,
    DmKeyActionStickRRight,
    DmKeyActionBtnA,
    DmKeyActionBtnB,
    DmKeyActionBtnX,
    DmKeyActionBtnY,
    DmKeyActionBtnL,
    DmKeyActionBtnR,
    DmKeyActionBtnZL,
    DmKeyActionBtnZR,
    DmKeyActionBtnLS,
    DmKeyActionBtnRS,
    DmKeyActionBtnPlus,
    DmKeyActionBtnMinus,
    DmKeyActionDpadUp,
    DmKeyActionDpadDown,
    DmKeyActionDpadLeft,
    DmKeyActionDpadRight,
    DmKeyActionShake,
    DmKeyActionCount
};

static const char *g_action_names[DmKeyActionCount] = {
    "Move Up", "Move Down", "Move Left", "Move Right",
    "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right",
    "Button A", "Button B", "Button X", "Button Y",
    "Left Shoulder (L)", "Right Shoulder (R)", "Left Trigger (ZL)", "Right Trigger (ZR)",
    "Left Stick Press (LS)", "Right Stick Press (RS)",
    "Start (+)", "Select (−)",
    "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
    "Motion Shake"
};

// Default Nintendo Layout KeyCodes
static unsigned short g_default_keymap[DmKeyActionCount] = {
    13, 1, 0, 2,       // W, S, A, D (Left Stick)
    126, 125, 123, 124,// Arrow Keys (Right Stick / Camera)
    38, 40, 32, 34,    // J, K, U, I (A, B, X, Y)
    12, 14, 6, 8,      // Q, E, Z, C (L, R, ZL, ZR)
    3, 5,              // F, G (LS, RS)
    36, 48,            // Return, Tab (+, -)
    126, 125, 123, 124,// D-Pad
    49                 // Space (Shake)
};

static unsigned short g_active_keymap[DmKeyActionCount];

static NSString *key_name(unsigned short code) {
    switch (code) {
        case 0: return @"A";
        case 1: return @"S";
        case 2: return @"D";
        case 3: return @"F";
        case 4: return @"H";
        case 5: return @"G";
        case 6: return @"Z";
        case 7: return @"X";
        case 8: return @"C";
        case 9: return @"V";
        case 11: return @"B";
        case 12: return @"Q";
        case 13: return @"W";
        case 14: return @"E";
        case 15: return @"R";
        case 16: return @"Y";
        case 17: return @"T";
        case 18: return @"1";
        case 19: return @"2";
        case 20: return @"3";
        case 21: return @"4";
        case 22: return @"6";
        case 23: return @"5";
        case 24: return @"=";
        case 25: return @"9";
        case 26: return @"7";
        case 27: return @"-";
        case 28: return @"8";
        case 29: return @"0";
        case 30: return @"]";
        case 31: return @"O";
        case 32: return @"U";
        case 33: return @"[";
        case 34: return @"I";
        case 35: return @"P";
        case 36: return @"Return";
        case 37: return @"L";
        case 38: return @"J";
        case 39: return @"'";
        case 40: return @"K";
        case 41: return @";";
        case 42: return @"\\";
        case 43: return @",";
        case 44: return @"/";
        case 45: return @"N";
        case 46: return @"M";
        case 47: return @".";
        case 48: return @"Tab";
        case 49: return @"Space";
        case 50: return @"`";
        case 51: return @"Delete";
        case 53: return @"Escape";
        case 123: return @"Left Arrow";
        case 124: return @"Right Arrow";
        case 125: return @"Down Arrow";
        case 126: return @"Up Arrow";
        default: return [NSString stringWithFormat:@"Key (%d)", code];
    }
}

static void keymap_load(void) {
    NSUserDefaults *df = [NSUserDefaults standardUserDefaults];
    for (int i = 0; i < DmKeyActionCount; i++) {
        NSString *k = [NSString stringWithFormat:@"input.key.%d", i];
        id v = [df objectForKey:k];
        g_active_keymap[i] = v ? (unsigned short)[v intValue] : g_default_keymap[i];
    }
}

static void keymap_save(void) {
    NSUserDefaults *df = [NSUserDefaults standardUserDefaults];
    for (int i = 0; i < DmKeyActionCount; i++) {
        [df setInteger:g_active_keymap[i] forKey:[NSString stringWithFormat:@"input.key.%d", i]];
    }
}

static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    for (int i = 0; i < DmKeyActionCount; i++) {
        if (g_active_keymap[i] == c) {
            switch ((DmKeyAction)i) {
                case DmKeyActionMoveUp:     g_ly_pos = down; break;
                case DmKeyActionMoveDown:   g_ly_neg = down; break;
                case DmKeyActionMoveLeft:   g_lx_neg = down; break;
                case DmKeyActionMoveRight:  g_lx_pos = down; break;
                case DmKeyActionStickRUp:   g_ry_pos = down; break;
                case DmKeyActionStickRDown: g_ry_neg = down; break;
                case DmKeyActionStickRLeft: g_rx_neg = down; break;
                case DmKeyActionStickRRight:g_rx_pos = down; break;
                case DmKeyActionBtnA:       g_btn = down ? (g_btn | DM_BTN_A) : (g_btn & ~DM_BTN_A); break;
                case DmKeyActionBtnB:       g_btn = down ? (g_btn | DM_BTN_B) : (g_btn & ~DM_BTN_B); break;
                case DmKeyActionBtnX:       g_btn = down ? (g_btn | DM_BTN_X) : (g_btn & ~DM_BTN_X); break;
                case DmKeyActionBtnY:       g_btn = down ? (g_btn | DM_BTN_Y) : (g_btn & ~DM_BTN_Y); break;
                case DmKeyActionBtnL:       g_btn = down ? (g_btn | DM_BTN_L) : (g_btn & ~DM_BTN_L); break;
                case DmKeyActionBtnR:       g_btn = down ? (g_btn | DM_BTN_R) : (g_btn & ~DM_BTN_R); break;
                case DmKeyActionBtnZL:      g_btn = down ? (g_btn | DM_BTN_ZL) : (g_btn & ~DM_BTN_ZL); break;
                case DmKeyActionBtnZR:      g_btn = down ? (g_btn | DM_BTN_ZR) : (g_btn & ~DM_BTN_ZR); break;
                case DmKeyActionBtnLS:      g_btn = down ? (g_btn | DM_BTN_LS) : (g_btn & ~DM_BTN_LS); break;
                case DmKeyActionBtnRS:      g_btn = down ? (g_btn | DM_BTN_RS) : (g_btn & ~DM_BTN_RS); break;
                case DmKeyActionBtnPlus:    g_btn = down ? (g_btn | DM_BTN_PLUS) : (g_btn & ~DM_BTN_PLUS); break;
                case DmKeyActionBtnMinus:   g_btn = down ? (g_btn | DM_BTN_MINUS) : (g_btn & ~DM_BTN_MINUS); break;
                case DmKeyActionDpadUp:     g_btn = down ? (g_btn | DM_BTN_UP) : (g_btn & ~DM_BTN_UP); break;
                case DmKeyActionDpadDown:   g_btn = down ? (g_btn | DM_BTN_DOWN) : (g_btn & ~DM_BTN_DOWN); break;
                case DmKeyActionDpadLeft:   g_btn = down ? (g_btn | DM_BTN_LEFT) : (g_btn & ~DM_BTN_LEFT); break;
                case DmKeyActionDpadRight:  g_btn = down ? (g_btn | DM_BTN_RIGHT) : (g_btn & ~DM_BTN_RIGHT); break;
                case DmKeyActionShake:      g_btn = down ? (g_btn | DM_BTN_SHAKE) : (g_btn & ~DM_BTN_SHAKE); break;
                default: break;
            }
        }
    }
    push();
}

static int32_t pad_axis(float v) {
    if (fabsf(v) < g_pad_deadzone) return 0;
    float sign = (v > 0) ? 1.0f : -1.0f;
    float scaled = (fabsf(v) - g_pad_deadzone) / (1.0f - g_pad_deadzone);
    return (int32_t)(sign * fminf(1.0f, scaled) * DM_STICK_MAX);
}

static void pad_read(GCExtendedGamepad *g) {
    int p = -1;
    for (int i = 0; i < DM_PLAYERS; i++) {
        if (g_players[i].type == PLAYER_DEV_CONTROLLER && g_players[i].controller == g.controller) {
            p = i;
            break;
        }
    }
    if (p < 0) return;
    dm_haptics_apple_controller(p, g.controller);
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= bit
    if (g_pad_xbox_layout) {
        // By Button Label (Xbox Style): Bottom is A, Right is B, Left is X, Top is Y
        K(g.buttonA, DM_BTN_A); K(g.buttonB, DM_BTN_B); K(g.buttonX, DM_BTN_X); K(g.buttonY, DM_BTN_Y);
    } else {
        // By Position (Nintendo Pro Style): Right is A, Bottom is B, Top is X, Left is Y
        K(g.buttonB, DM_BTN_A); K(g.buttonA, DM_BTN_B); K(g.buttonY, DM_BTN_X); K(g.buttonX, DM_BTN_Y);
    }
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    dm_pad_set_player(p, m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value), pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}

static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }

static void update_controller_menu(void);

static void sync_pad_players(void) {
    uint32_t mask = 0;
    for (int i = 0; i < DM_PLAYERS; i++) {
        if (g_players[i].type != PLAYER_DEV_DISCONNECTED) {
            mask |= (1u << i);
        }
    }
    dm_pad_players(mask);
}

static void pads_changed(GCController *gone) {
    NSArray<GCController *> *now = GCController.controllers;
    char nb[16];

    // Clean up disconnected controllers
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (g_players[p].type == PLAYER_DEV_CONTROLLER) {
            GCController *c = g_players[p].controller;
            if (!c || c == gone || ![now containsObject:c]) {
                npad_name(p, nb, sizeof nb);
                LOG("controller '%s' disconnected: player %d (npad %s) freed", pad_name(c), p + 1, nb);
                if (c && c.extendedGamepad) c.extendedGamepad.valueChangedHandler = nil;
                dm_haptics_apple_controller(p, nil);
                g_players[p].controller = nil;
                if (p == 0) {
                    g_players[0].type = PLAYER_DEV_KEYBOARD;
                    push();
                } else {
                    g_players[p].type = PLAYER_DEV_DISCONNECTED;
                    dm_pad_set_player(p, 0, 0, 0, 0, 0);
                }
            }
        }
    }

    // Assign controllers to available player slots
    for (GCController *c in now) {
        if (!c.extendedGamepad || c == gone) continue;

        int assigned = 0;
        for (int p = 0; p < DM_PLAYERS; p++) {
            if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller == c) {
                assigned = 1;
                break;
            }
        }
        if (assigned) continue;

        int target = -1;
        if (g_players[0].type == PLAYER_DEV_DISCONNECTED || g_players[0].type == PLAYER_DEV_KEYBOARD) {
            target = 0;
        } else {
            for (int p = 1; p < 4; p++) {
                if (g_players[p].type == PLAYER_DEV_DISCONNECTED) {
                    target = p;
                    break;
                }
            }
        }

        if (target >= 0) {
            if (target == 0 && g_players[0].type == PLAYER_DEV_KEYBOARD) {
                dm_input_set(0, 0, 0, 0, 0);
            }
            g_players[target].type = PLAYER_DEV_CONTROLLER;
            g_players[target].controller = c;
            dm_haptics_apple_controller(target, c);
            c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            npad_name(target, nb, sizeof nb);
            LOG("controller '%s' connected: assigned to player %d (npad %s)", pad_name(c), target + 1, nb);
            pad_read(c.extendedGamepad);
        } else {
            LOG("controller '%s' connected: all 4 player slots occupied, unassigned", pad_name(c));
        }
    }

    // Ensure Player 1 has keyboard input if no controller is attached
    int any_pad = 0;
    for (int p = 0; p < DM_PLAYERS; p++) {
        if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller) {
            any_pad = 1;
            break;
        }
    }
    if (!any_pad) {
        int kbd_assigned = 0;
        for (int p = 0; p < DM_PLAYERS; p++) {
            if (g_players[p].type == PLAYER_DEV_KEYBOARD) {
                kbd_assigned = 1;
                break;
            }
        }
        if (!kbd_assigned) {
            g_players[0].type = PLAYER_DEV_KEYBOARD;
            push();
        }
    }

    sync_pad_players();
    update_controller_menu();
}

static void pads_start(void) {
    for (int p = 0; p < DM_PLAYERS; p++) {
        g_players[p].type = (p == 0) ? PLAYER_DEV_KEYBOARD : PLAYER_DEV_DISCONNECTED;
        g_players[p].controller = nil;
    }

    NSUserDefaults *df = [NSUserDefaults standardUserDefaults];
    g_pad_xbox_layout = [df boolForKey:@"input.pad.xbox_layout"];
    float dz = [df floatForKey:@"input.pad.deadzone"];
    if (dz > 0.01f) g_pad_deadzone = dz;

    pads_changed(nil);
}

@interface DmGameView : NSView @end
@implementation DmGameView
- (void)mouseDown:(NSEvent *)e { [self mouseDragged:e]; }
- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSSize b = self.bounds.size;
    double w = b.width, h = b.height;
    if (w <= 0 || h <= 0) return;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    double x0 = (b.width - w) / 2, y0 = (b.height - h) / 2;
    dm_touch t = {0, (float)fmin(fmax((p.x - x0) / w, 0), 0.999), (float)fmin(fmax(1 - (p.y - y0) / h, 0), 0.999)};
    dm_touch_set(&t, 1);
}
- (void)mouseUp:(NSEvent *)e { (void)e; dm_touch_set(NULL, 0); }
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
    static char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

__attribute__((constructor(101))) static void hud_env(void) {
    @autoreleasepool {
        NSUserDefaults *df = [NSUserDefaults standardUserDefaults];
        NSString *v = [df stringForKey:@"launcher.DM_METAL_HUD"];
        if ([df boolForKey:@"MTL_HUD_ENABLED"] || (v && [v boolValue]) || (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD")))) {
            setenv("MTL_HUD_ENABLED", "1", 1);
        }
    }
}

@interface DmAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, strong) NSWindow *settingsWindow;
@property (nonatomic, strong) NSMenuItem *dockedItem;
@property (nonatomic, strong) NSMenuItem *handheldItem;
@property (nonatomic, strong) NSMenuItem *fpsDefaultItem;
@property (nonatomic, strong) NSMenuItem *fps60Item;
@property (nonatomic, strong) NSMenuItem *fps30Item;
@property (nonatomic, strong) NSMenuItem *scale200Item;
@property (nonatomic, strong) NSMenuItem *scale150Item;
@property (nonatomic, strong) NSMenuItem *scale125Item;
@property (nonatomic, strong) NSMenuItem *scale100Item;
@property (nonatomic, strong) NSMenuItem *scale85Item;
@property (nonatomic, strong) NSMenuItem *scale70Item;
@property (nonatomic, strong) NSMenuItem *scale50Item;
@property (nonatomic, strong) NSMenuItem *metalHudItem;
@property (nonatomic, strong) NSMenuItem *muteItem;
@property (nonatomic, strong) NSMenuItem *muteUnfocusedItem;
@property (nonatomic, strong) NSMenuItem *screenMatchItem;
@property (nonatomic, strong) NSMenuItem *pauseItem;
@property (nonatomic, strong) NSMenuItem *padNintendoItem;
@property (nonatomic, strong) NSMenuItem *padXboxItem;
@property (nonatomic, strong) NSMenu *controllersMenu;
@property (nonatomic, strong) NSTableView *keyTableView;
@property (nonatomic, assign) NSInteger rebindActionIndex;
@end

static DmAppDelegate *g_app_delegate;
static int g_audio_muted = 0;
static int g_mute_unfocused = 0;

@implementation DmAppDelegate

- (void)updateMenuState {
    int op = dm_opmode();
    self.dockedItem.state = (op == 1) ? NSControlStateValueOn : NSControlStateValueOff;
    self.handheldItem.state = (op == 0) ? NSControlStateValueOn : NSControlStateValueOff;

    if (self.fpsDefaultItem) {
        int fps = 0;
        if (dm_fps_state) {
            float f; memcpy(&f, dm_fps_state, sizeof f); fps = (int)f;
        }
        self.fpsDefaultItem.state = (fps == 0) ? NSControlStateValueOn : NSControlStateValueOff;
        self.fps60Item.state = (fps == 60) ? NSControlStateValueOn : NSControlStateValueOff;
        self.fps30Item.state = (fps == 30) ? NSControlStateValueOn : NSControlStateValueOff;
    }

    if (self.scale100Item) {
        float sc = 1.0f;
        if (dm_fps_state) {
            memcpy(&sc, dm_fps_state + 0x10, sizeof sc);
        }
        self.scale200Item.state = (sc >= 1.95f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale150Item.state = (sc >= 1.45f && sc < 1.95f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale125Item.state = (sc >= 1.20f && sc < 1.45f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale100Item.state = (sc >= 0.95f && sc < 1.20f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale85Item.state = (sc >= 0.80f && sc < 0.95f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale70Item.state = (sc >= 0.65f && sc < 0.80f) ? NSControlStateValueOn : NSControlStateValueOff;
        self.scale50Item.state = (sc < 0.65f) ? NSControlStateValueOn : NSControlStateValueOff;
    }

    int hud = (g_layer.developerHUDProperties && [g_layer.developerHUDProperties[@"mode"] isEqual:@"default"]);
    self.metalHudItem.state = hud ? NSControlStateValueOn : NSControlStateValueOff;

    self.muteItem.state = g_audio_muted ? NSControlStateValueOn : NSControlStateValueOff;
    self.muteUnfocusedItem.state = g_mute_unfocused ? NSControlStateValueOn : NSControlStateValueOff;
    self.padNintendoItem.state = (!g_pad_xbox_layout) ? NSControlStateValueOn : NSControlStateValueOff;
    self.padXboxItem.state = g_pad_xbox_layout ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)showAbout:(id)sender {
    [NSApp orderFrontStandardAboutPanel:sender];
}

- (void)openSaveFolder:(id)sender {
    const char *s = getenv("DM_SAVE");
    if (s && *s) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(s)]];
    }
}

- (void)exportSaveBackup:(id)sender {
    const char *s = getenv("DM_SAVE");
    if (!s || !*s) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = @"Export Save Backup Archive";
    NSDateFormatter *fmt = [NSDateFormatter new]; fmt.dateFormat = @"yyyy-MM-dd_HHmmss";
    panel.nameFieldStringValue = [NSString stringWithFormat:@"%s_Save_%@.zip", DM_GAME_NAME, [fmt stringFromDate:[NSDate date]]];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"zip"]];
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        NSTask *task = [NSTask new];
        task.launchPath = @"/usr/bin/ditto";
        task.arguments = @[@"-c", @"-k", @"--sequesterRsrc", @(s), panel.URL.path];
        [task launchAndReturnError:nil];
        [task waitUntilExit];
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Save Backup Exported";
        alert.informativeText = [NSString stringWithFormat:@"Backup successfully written to:\n%@", panel.URL.path];
        [alert runModal];
    }
}

- (void)restoreSaveBackup:(id)sender {
    const char *s = getenv("DM_SAVE");
    if (!s || !*s) return;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.title = @"Select Save Backup Archive (.zip)";
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"zip"]];
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        // Snapshot current save before restoring
        NSString *saveDir = @(s);
        NSString *bakDir = [saveDir stringByAppendingFormat:@".bak_%ld", (long)[[NSDate date] timeIntervalSince1970]];
        [[NSFileManager defaultManager] copyItemAtPath:saveDir toPath:bakDir error:nil];

        NSTask *task = [NSTask new];
        task.launchPath = @"/usr/bin/ditto";
        task.arguments = @[@"-x", @"-k", panel.URL.path, saveDir];
        [task launchAndReturnError:nil];
        [task waitUntilExit];

        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Save Restored";
        alert.informativeText = @"Your save backup was restored. An automatic safety snapshot of the previous save was archived beside it.";
        [alert runModal];
    }
}

- (void)openLogFile:(id)sender {
    NSString *logsDir = [NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES)[0]
                         stringByAppendingPathComponent:@"Logs/NxNative"];
    NSString *logFile = [logsDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.log", DM_GAME_NAME]];
    if (![NSFileManager.defaultManager fileExistsAtPath:logFile]) {
        const char *save = getenv("DM_SAVE");
        if (save) logFile = [@(save).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"log.txt"];
    }
    if ([NSFileManager.defaultManager fileExistsAtPath:logFile]) {
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:logFile]];
    } else {
        NSAlert *a = [NSAlert new]; a.messageText = @"Log Not Found"; a.informativeText = @"No active log file found yet.";
        [a runModal];
    }
}

- (void)takeScreenshot:(id)sender {
    NSString *pics = [NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES)[0]
                      stringByAppendingPathComponent:[NSString stringWithFormat:@"NxNative/%s", DM_GAME_NAME]];
    [NSFileManager.defaultManager createDirectoryAtPath:pics withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *fmt = [NSDateFormatter new]; fmt.dateFormat = @"yyyy-MM-dd_HH-mm-ss";
    NSString *ts = [fmt stringFromDate:[NSDate date]];
    NSString *path = [pics stringByAppendingPathComponent:[NSString stringWithFormat:@"%s_%@.png", DM_GAME_NAME, ts]];
    dm_gfx_shot_at(1, path.fileSystemRepresentation);
    NSBeep();
    LOG("front end: screenshot queued → %s", path.UTF8String);
}

- (void)togglePause:(id)sender {
    static int s_paused = 0;
    s_paused = !s_paused;
    if (s_paused) {
        dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1);
        self.pauseItem.title = @"Resume Game";
    } else {
        dm_gfx_suspend(0); if (!g_audio_muted) dm_audio_suspend(0); dm_focus_set(1);
        self.pauseItem.title = @"Pause Game";
    }
}

- (void)toggleMute:(id)sender {
    g_audio_muted = !g_audio_muted;
    dm_audio_suspend(g_audio_muted);
    [self updateMenuState];
}

- (void)toggleMuteUnfocused:(id)sender {
    g_mute_unfocused = !g_mute_unfocused;
    [[NSUserDefaults standardUserDefaults] setBool:g_mute_unfocused forKey:@"audio.mute_unfocused"];
    [self updateMenuState];
}

- (void)setOpModeDocked:(id)sender {
    dm_set_opmode(1);
    [[NSUserDefaults standardUserDefaults] setObject:@"1" forKey:@"launcher.DM_OPMODE"];
    [self updateMenuState];
}

- (void)setOpModeHandheld:(id)sender {
    dm_set_opmode(0);
    [[NSUserDefaults standardUserDefaults] setObject:@"0" forKey:@"launcher.DM_OPMODE"];
    [self updateMenuState];
}

- (void)setFpsTarget:(NSMenuItem *)sender {
    int t = (int)sender.tag;
    if (dm_fps_state) {
        float f = (float)t;
        memcpy(dm_fps_state, &f, sizeof f);
    }
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithFormat:@"%d", t] forKey:@"launcher.DM_FPS_TARGET"];
    [self updateMenuState];
}

- (void)setRenderScale:(NSMenuItem *)sender {
    float sc = sender.tag / 100.0f;
    if (dm_fps_state) {
        memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    }
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithFormat:@"%.2f", sc] forKey:@"launcher.DM_RENDER_SCALE"];
    [self updateMenuState];
}

- (void)toggleMetalHUD:(id)sender {
    int on = !(g_layer.developerHUDProperties && [g_layer.developerHUDProperties[@"mode"] isEqual:@"default"]);
    g_layer.developerHUDProperties = on ? @{@"mode": @"default"} : @{};
    if (on) setenv("MTL_HUD_ENABLED", "1", 1);
    else unsetenv("MTL_HUD_ENABLED");
    [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"launcher.DM_METAL_HUD"];
    [[NSUserDefaults standardUserDefaults] setBool:on forKey:@"MTL_HUD_ENABLED"];
    [self updateMenuState];
}

- (void)resizeWindowToMultiplier:(NSMenuItem *)sender {
    CGFloat baseH = 720.0f;
    CGFloat mult = sender.tag / 100.0f;
    CGFloat h = baseH * mult;
    CGFloat w = h * 16.0 / 9.0;
    NSRect frame = g_win.frame;
    NSRect content = [g_win contentRectForFrameRect:frame];
    content.size = NSMakeSize(w, h);
    NSRect newFrame = [g_win frameRectForContentRect:content];
    newFrame.origin.x = frame.origin.x + (frame.size.width - newFrame.size.width) / 2.0;
    newFrame.origin.y = frame.origin.y + (frame.size.height - newFrame.size.height) / 2.0;
    [g_win setFrame:newFrame display:YES animate:YES];
}

- (void)setPadLayoutNintendo:(id)sender {
    g_pad_xbox_layout = 0;
    [[NSUserDefaults standardUserDefaults] setBool:NO forKey:@"input.pad.xbox_layout"];
    [self updateMenuState];
    update_controller_menu();
}

- (void)setPadLayoutXbox:(id)sender {
    g_pad_xbox_layout = 1;
    [[NSUserDefaults standardUserDefaults] setBool:YES forKey:@"input.pad.xbox_layout"];
    [self updateMenuState];
    update_controller_menu();
}

- (void)onSelectPlayerController:(NSMenuItem *)sender {
    int p = (int)sender.tag / 100;
    int ci = (int)sender.tag % 100;
    NSArray<GCController *> *ctls = GCController.controllers;
    if (ci >= (int)ctls.count || p < 0 || p >= DM_PLAYERS) return;
    GCController *c = ctls[ci];

    if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller == c) return;

    for (int other = 0; other < DM_PLAYERS; other++) {
        if (other != p && g_players[other].type == PLAYER_DEV_CONTROLLER && g_players[other].controller == c) {
            g_players[other].type = PLAYER_DEV_DISCONNECTED;
            g_players[other].controller = nil;
            dm_pad_set_player(other, 0, 0, 0, 0, 0);
        }
    }

    if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller) {
        g_players[p].controller.extendedGamepad.valueChangedHandler = nil;
    } else if (g_players[p].type == PLAYER_DEV_KEYBOARD) {
        if (p == 0) dm_input_set(0, 0, 0, 0, 0);
        else dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
    dm_pad_set_player(p, 0, 0, 0, 0, 0);

    g_players[p].type = PLAYER_DEV_CONTROLLER;
    g_players[p].controller = c;
    dm_haptics_apple_controller(p, c);
    c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
    pad_read(c.extendedGamepad);

    sync_pad_players();
    update_controller_menu();
}

- (void)onSelectPlayerKeyboard:(NSMenuItem *)sender {
    int p = (int)sender.tag;
    if (p < 0 || p >= DM_PLAYERS) return;

    if (g_players[p].type == PLAYER_DEV_KEYBOARD) return;

    for (int other = 0; other < DM_PLAYERS; other++) {
        if (other != p && g_players[other].type == PLAYER_DEV_KEYBOARD) {
            g_players[other].type = PLAYER_DEV_DISCONNECTED;
            if (other == 0) dm_input_set(0, 0, 0, 0, 0);
            else dm_pad_set_player(other, 0, 0, 0, 0, 0);
        }
    }

    if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller) {
        g_players[p].controller.extendedGamepad.valueChangedHandler = nil;
        dm_haptics_apple_controller(p, nil);
        g_players[p].controller = nil;
    }
    dm_pad_set_player(p, 0, 0, 0, 0, 0);

    g_players[p].type = PLAYER_DEV_KEYBOARD;
    push();

    sync_pad_players();
    update_controller_menu();
}

- (void)onSelectPlayerDisconnect:(NSMenuItem *)sender {
    int p = (int)sender.tag;
    if (p <= 0 || p >= DM_PLAYERS) return;

    if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller) {
        g_players[p].controller.extendedGamepad.valueChangedHandler = nil;
        dm_haptics_apple_controller(p, nil);
        g_players[p].controller = nil;
    } else if (g_players[p].type == PLAYER_DEV_KEYBOARD) {
        dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
    dm_pad_set_player(p, 0, 0, 0, 0, 0);

    g_players[p].type = PLAYER_DEV_DISCONNECTED;

    sync_pad_players();
    update_controller_menu();
}

- (void)clearShaderCache:(id)sender {
    NSString *cacheDir = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)[0]
                          stringByAppendingPathComponent:[NSString stringWithFormat:@"NxNative/%s", DM_GAME_NAME]];
    [[NSFileManager defaultManager] removeItemAtPath:cacheDir error:nil];
    NSAlert *a = [NSAlert new]; a.messageText = @"Shader Cache Cleared"; a.informativeText = @"Transient caches were removed and will be rebuilt as needed.";
    [a runModal];
}

- (void)showSettings:(id)sender {
    if (self.settingsWindow) {
        [self.settingsWindow makeKeyAndOrderFront:nil];
        return;
    }
    NSRect r = NSMakeRect(0, 0, 520, 560);
    NSWindow *w = [[NSWindow alloc] initWithContentRect:r
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    w.title = [NSString stringWithFormat:@"Settings — %s", DM_GAME_NAME];
    w.releasedWhenClosed = NO;
    w.tabbingMode = NSWindowTabbingModeDisallowed;
    [w center];

    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 560)];

    NSTextField *sec1 = [NSTextField labelWithString:@"Display & Graphics"];
    sec1.font = [NSFont boldSystemFontOfSize:13];
    sec1.frame = NSMakeRect(20, 525, 200, 20);
    [v addSubview:sec1];

    CGFloat curY = 495;
    NSTextField *modeL = [NSTextField labelWithString:@"Console Mode:"];
    modeL.frame = NSMakeRect(20, curY, 120, 20);
    [v addSubview:modeL];
    NSPopUpButton *popMode = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, curY - 5, 160, 26) pullsDown:NO];
    [popMode addItemWithTitle:@"Docked"];
    [popMode addItemWithTitle:@"Handheld"];
    [popMode selectItemAtIndex:(dm_opmode() == 1 ? 0 : 1)];
    popMode.target = self; popMode.action = @selector(onModeSelected:);
    [v addSubview:popMode];
    curY -= 35;

    if (DM_GAME_FPS_TARGET != 0 || dm_fps_state) {
        NSTextField *fpsL = [NSTextField labelWithString:@"Target FPS:"];
        fpsL.frame = NSMakeRect(20, curY, 120, 20);
        [v addSubview:fpsL];
        NSPopUpButton *popFps = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, curY - 5, 160, 26) pullsDown:NO];
        [popFps addItemWithTitle:@"Default"];
        [popFps addItemWithTitle:@"60 FPS"];
        [popFps addItemWithTitle:@"30 FPS"];
        int curFps = 0; if (dm_fps_state) { float f; memcpy(&f, dm_fps_state, sizeof f); curFps = (int)f; }
        [popFps selectItemAtIndex:(curFps == 60 ? 1 : curFps == 30 ? 2 : 0)];
        popFps.target = self; popFps.action = @selector(onFpsSelected:);
        [v addSubview:popFps];
        curY -= 35;
    }

    if (dm_fps_state) {
        NSTextField *scaleL = [NSTextField labelWithString:@"Render Scale:"];
        scaleL.frame = NSMakeRect(20, curY, 120, 20);
        [v addSubview:scaleL];
        NSPopUpButton *popScale = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, curY - 5, 160, 26) pullsDown:NO];
        [popScale addItemWithTitle:@"1.0x"];
        [popScale addItemWithTitle:@"1.25x"];
        [popScale addItemWithTitle:@"1.5x"];
        [popScale addItemWithTitle:@"2.0x"];
        [popScale addItemWithTitle:@"0.85x"];
        [popScale addItemWithTitle:@"0.7x"];
        [popScale addItemWithTitle:@"0.5x"];
        float curScale = 1.0f; memcpy(&curScale, dm_fps_state + 0x10, sizeof curScale);
        if (curScale >= 1.95f) [popScale selectItemAtIndex:3];
        else if (curScale >= 1.45f) [popScale selectItemAtIndex:2];
        else if (curScale >= 1.20f) [popScale selectItemAtIndex:1];
        else if (curScale >= 0.95f) [popScale selectItemAtIndex:0];
        else if (curScale >= 0.80f) [popScale selectItemAtIndex:4];
        else if (curScale >= 0.65f) [popScale selectItemAtIndex:5];
        else [popScale selectItemAtIndex:6];
        popScale.target = self; popScale.action = @selector(onScaleSelected:);
        [v addSubview:popScale];
        curY -= 35;
    }

    NSButton *hudBtn = [NSButton checkboxWithTitle:@"Show Metal Performance HUD" target:self action:@selector(toggleMetalHUD:)];
    hudBtn.frame = NSMakeRect(20, curY, 280, 20);
    int hud = (g_layer.developerHUDProperties && [g_layer.developerHUDProperties[@"mode"] isEqual:@"default"]);
    hudBtn.state = hud ? NSControlStateValueOn : NSControlStateValueOff;
    [v addSubview:hudBtn];
    curY -= 15;

    NSBox *sep1 = [[NSBox alloc] initWithFrame:NSMakeRect(20, curY, 480, 1)];
    sep1.boxType = NSBoxSeparator;
    [v addSubview:sep1];

    NSTextField *sec2 = [NSTextField labelWithString:@"Storage & Saves"];
    sec2.font = [NSFont boldSystemFontOfSize:13];
    sec2.frame = NSMakeRect(20, 345, 200, 20);
    [v addSubview:sec2];

    NSTextField *pathSave = [NSTextField labelWithString:@(getenv("DM_SAVE") ?: "")];
    pathSave.font = [NSFont systemFontOfSize:11];
    pathSave.textColor = [NSColor secondaryLabelColor];
    pathSave.frame = NSMakeRect(20, 320, 480, 18);
    [v addSubview:pathSave];

    NSButton *btnOpenSave = [[NSButton alloc] initWithFrame:NSMakeRect(20, 285, 150, 28)];
    btnOpenSave.title = @"Reveal in Finder…";
    btnOpenSave.target = self; btnOpenSave.action = @selector(openSaveFolder:);
    [v addSubview:btnOpenSave];

    NSButton *btnCustom = [[NSButton alloc] initWithFrame:NSMakeRect(175, 285, 150, 28)];
    btnCustom.title = @"Change Folder…";
    btnCustom.target = self; btnCustom.action = @selector(onChooseCustomSaveFolder:);
    [v addSubview:btnCustom];

    NSButton *btnExport = [[NSButton alloc] initWithFrame:NSMakeRect(330, 285, 150, 28)];
    btnExport.title = @"Export Backup…";
    btnExport.target = self; btnExport.action = @selector(exportSaveBackup:);
    [v addSubview:btnExport];

    NSBox *sep2 = [[NSBox alloc] initWithFrame:NSMakeRect(20, 270, 480, 1)];
    sep2.boxType = NSBoxSeparator;
    [v addSubview:sep2];

    NSTextField *sec3 = [NSTextField labelWithString:@"Controls"];
    sec3.font = [NSFont boldSystemFontOfSize:13];
    sec3.frame = NSMakeRect(20, 240, 200, 20);
    [v addSubview:sec3];

    NSTextField *lPad = [NSTextField labelWithString:@"Gamepad Buttons:"];
    lPad.frame = NSMakeRect(20, 215, 120, 20);
    [v addSubview:lPad];
    NSPopUpButton *popPad = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, 210, 180, 26) pullsDown:NO];
    [popPad addItemWithTitle:@"Nintendo Layout"];
    [popPad addItemWithTitle:@"Xbox Layout"];
    [popPad selectItemAtIndex:(g_pad_xbox_layout ? 1 : 0)];
    popPad.target = self; popPad.action = @selector(onPadLayoutSelected:);
    [v addSubview:popPad];

    NSTextField *lDz = [NSTextField labelWithString:@"Stick Deadzone:"];
    lDz.frame = NSMakeRect(340, 215, 95, 20);
    [v addSubview:lDz];
    NSSlider *sliderDz = [[NSSlider alloc] initWithFrame:NSMakeRect(435, 212, 65, 24)];
    sliderDz.minValue = 0.02; sliderDz.maxValue = 0.25; sliderDz.floatValue = g_pad_deadzone;
    sliderDz.target = self; sliderDz.action = @selector(onDeadzoneChanged:);
    [v addSubview:sliderDz];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 50, 480, 150)];
    sv.hasVerticalScroller = YES;
    NSTableView *tv = [[NSTableView alloc] initWithFrame:sv.bounds];
    NSTableColumn *c1 = [[NSTableColumn alloc] initWithIdentifier:@"action"];
    c1.title = @"Action"; c1.width = 240;
    NSTableColumn *c2 = [[NSTableColumn alloc] initWithIdentifier:@"key"];
    c2.title = @"Assigned Key"; c2.width = 210;
    [tv addTableColumn:c1]; [tv addTableColumn:c2];
    tv.dataSource = self; tv.delegate = self;
    sv.documentView = tv;
    self.keyTableView = tv;
    [v addSubview:sv];

    NSButton *resetKbd = [[NSButton alloc] initWithFrame:NSMakeRect(20, 12, 160, 28)];
    resetKbd.title = @"Reset Keys to Default";
    resetKbd.target = self; resetKbd.action = @selector(onResetKeys:);
    [v addSubview:resetKbd];

    w.contentView = v;
    self.settingsWindow = w;
    [w makeKeyAndOrderFront:nil];
}

- (void)onModeSelected:(NSPopUpButton *)btn {
    if (btn.indexOfSelectedItem == 0) [self setOpModeDocked:btn];
    else [self setOpModeHandheld:btn];
}

- (void)onFpsSelected:(NSPopUpButton *)btn {
    int t = (btn.indexOfSelectedItem == 1 ? 60 : btn.indexOfSelectedItem == 2 ? 30 : 0);
    NSMenuItem *item = [NSMenuItem new]; item.tag = t;
    [self setFpsTarget:item];
}

- (void)onScaleSelected:(NSPopUpButton *)btn {
    int scales[] = {100, 125, 150, 200, 85, 70, 50};
    int tag = scales[btn.indexOfSelectedItem];
    NSMenuItem *item = [NSMenuItem new]; item.tag = tag;
    [self setRenderScale:item];
}

- (void)onPadLayoutSelected:(NSPopUpButton *)btn {
    if (btn.indexOfSelectedItem == 0) [self setPadLayoutNintendo:btn];
    else [self setPadLayoutXbox:btn];
}

- (void)onDeadzoneChanged:(NSSlider *)slider {
    g_pad_deadzone = slider.floatValue;
    [[NSUserDefaults standardUserDefaults] setFloat:g_pad_deadzone forKey:@"input.pad.deadzone"];
}

- (void)onResetKeys:(id)sender {
    memcpy(g_active_keymap, g_default_keymap, sizeof g_active_keymap);
    keymap_save();
    [self.keyTableView reloadData];
}

- (void)onChooseCustomSaveFolder:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.title = @"Choose Custom Save Data Directory (iCloud / SSD)";
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        NSString *p = panel.URL.path;
        [[NSUserDefaults standardUserDefaults] setObject:p forKey:@"launcher.DM_CUSTOM_SAVE"];
        setenv("DM_SAVE", p.fileSystemRepresentation, 1);
        NSAlert *a = [NSAlert new]; a.messageText = @"Save Path Updated";
        a.informativeText = [NSString stringWithFormat:@"Save location set to:\n%@\nPlease restart the game for full effect.", p];
        [a runModal];
    }
}

// TableView DataSource & Delegate for Keyboard Remapping
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {
    return DmKeyActionCount;
}

- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSTextField *cell = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, col.width, 20)];
    cell.bezeled = NO; cell.drawsBackground = NO; cell.editable = NO;
    if ([col.identifier isEqualToString:@"action"]) {
        cell.stringValue = @(g_action_names[row]);
    } else {
        cell.stringValue = key_name(g_active_keymap[row]);
    }
    return cell;
}

@end

static void update_controller_menu(void) {
    if (!g_app_delegate || !g_app_delegate.controllersMenu) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        NSMenu *m = g_app_delegate.controllersMenu;
        [m removeAllItems];
        NSArray<GCController *> *ctls = GCController.controllers;

        for (int p = 0; p < 4; p++) {
            NSString *pTitle = [NSString stringWithFormat:@"Player %d", p + 1];
            NSMenuItem *pItem = [m addItemWithTitle:pTitle action:nil keyEquivalent:@""];
            NSMenu *sub = [[NSMenu alloc] initWithTitle:pTitle];

            if (ctls.count > 0) {
                for (NSUInteger ci = 0; ci < ctls.count; ci++) {
                    GCController *c = ctls[ci];
                    float bLevel = c.battery ? c.battery.batteryLevel : -1.0f;
                    NSString *batt = (bLevel >= 0) ? [NSString stringWithFormat:@" (%d%% 🔋)", (int)(bLevel * 100)] : @"";
                    NSString *cTitle = [NSString stringWithFormat:@"%@%@", c.vendorName ?: [NSString stringWithFormat:@"Controller %lu", (unsigned long)(ci + 1)], batt];
                    NSMenuItem *cItem = [sub addItemWithTitle:cTitle action:@selector(onSelectPlayerController:) keyEquivalent:@""];
                    cItem.target = g_app_delegate;
                    cItem.tag = p * 100 + (int)ci;
                    if (g_players[p].type == PLAYER_DEV_CONTROLLER && g_players[p].controller == c) {
                        cItem.state = NSControlStateValueOn;
                    }
                }
                [sub addItem:[NSMenuItem separatorItem]];
            }

            NSMenuItem *kbItem = [sub addItemWithTitle:@"Keyboard" action:@selector(onSelectPlayerKeyboard:) keyEquivalent:@""];
            kbItem.target = g_app_delegate;
            kbItem.tag = p;
            if (g_players[p].type == PLAYER_DEV_KEYBOARD) {
                kbItem.state = NSControlStateValueOn;
            }

            if (p > 0) {
                NSMenuItem *noneItem = [sub addItemWithTitle:@"None" action:@selector(onSelectPlayerDisconnect:) keyEquivalent:@""];
                noneItem.target = g_app_delegate;
                noneItem.tag = p;
                if (g_players[p].type == PLAYER_DEV_DISCONNECTED) {
                    noneItem.state = NSControlStateValueOn;
                }
            }

            [pItem setSubmenu:sub];
        }

        [m addItem:[NSMenuItem separatorItem]];

        NSMenu *padMode = [[NSMenu alloc] initWithTitle:@"Button Layout"];
        g_app_delegate.padNintendoItem = [padMode addItemWithTitle:@"Nintendo Layout" action:@selector(setPadLayoutNintendo:) keyEquivalent:@""];
        g_app_delegate.padNintendoItem.target = g_app_delegate;
        g_app_delegate.padXboxItem = [padMode addItemWithTitle:@"Xbox Layout" action:@selector(setPadLayoutXbox:) keyEquivalent:@""];
        g_app_delegate.padXboxItem.target = g_app_delegate;
        [[m addItemWithTitle:@"Button Layout" action:nil keyEquivalent:@""] setSubmenu:padMode];

        [m addItem:[NSMenuItem separatorItem]];
        NSMenuItem *cfg = [m addItemWithTitle:@"Configure Keyboard Keys…" action:@selector(showSettings:) keyEquivalent:@""];
        cfg.target = g_app_delegate;

        [g_app_delegate updateMenuState];
    });
}

void front_mac_bundle_setup(int *argc, char ***argv) {
    @autoreleasepool {
        NSBundle *b = [NSBundle mainBundle];
        NSString *bp = b.bundlePath;
        if (![bp hasSuffix:@".app"]) return;

        NSString *res = b.resourcePath;

        if (*argc < 2 || (*argv)[1] == NULL || strncmp((*argv)[1], "-psn", 4) == 0) {
            static char *new_argv[64];
            int n = 0;
            new_argv[n++] = (*argv)[0];
            NSString *dmDir = [res stringByAppendingPathComponent:@"dm"];
            new_argv[n++] = strdup(dmDir.fileSystemRepresentation);
            for (int i = 1; i < *argc && n < 63; i++) {
                if (strncmp((*argv)[i], "-psn", 4) != 0) {
                    new_argv[n++] = (*argv)[i];
                }
            }
            new_argv[n] = NULL;
            *argc = n;
            *argv = new_argv;
        }

        if (!getenv("DM_WINDOW")) setenv("DM_WINDOW", "1", 1);
        if (!getenv("DM_AUDIO")) setenv("DM_AUDIO", "1", 1);
        if (!getenv("DM_ROMFS")) {
            NSString *romfs = [res stringByAppendingPathComponent:@"romfs"];
            if ([NSFileManager.defaultManager fileExistsAtPath:romfs]) {
                setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
            }
        }
        if (!getenv("DM_MSL_DIR")) {
            NSString *msl = [res stringByAppendingPathComponent:@"msl"];
            if ([NSFileManager.defaultManager fileExistsAtPath:msl]) {
                setenv("DM_MSL_DIR", msl.fileSystemRepresentation, 0);
            }
        }
        if (!getenv("DM_NVN_SHMAP")) {
            NSString *shmap = [res stringByAppendingPathComponent:@"dm/shader_map.tsv"];
            if ([NSFileManager.defaultManager fileExistsAtPath:shmap]) {
                setenv("DM_NVN_SHMAP", shmap.fileSystemRepresentation, 0);
            }
        }
        if (!getenv("DM_FONT_DIR")) {
            NSString *font = [res stringByAppendingPathComponent:@"dm/font"];
            if ([NSFileManager.defaultManager fileExistsAtPath:font]) {
                setenv("DM_FONT_DIR", font.fileSystemRepresentation, 0);
            }
        }
        if (!getenv("DM_AOC_DIR")) {
            NSString *aoc = [res stringByAppendingPathComponent:@"aoc"];
            if ([NSFileManager.defaultManager fileExistsAtPath:aoc]) {
                setenv("DM_AOC_DIR", aoc.fileSystemRepresentation, 0);
            }
        }

        // Save and temporary directories: ~/Library/Application Support/NxNative/<GameName>
        NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
        NSString *gameName = @DM_GAME_NAME;
        NSString *customSave = [defaults stringForKey:@"launcher.DM_CUSTOM_SAVE"];
        NSString *saveDir = nil;

        if (customSave && [NSFileManager.defaultManager fileExistsAtPath:customSave]) {
            saveDir = customSave;
        } else {
            NSString *appSupport = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0]
                                    stringByAppendingPathComponent:[NSString stringWithFormat:@"NxNative/%@", gameName]];
            saveDir = [appSupport stringByAppendingPathComponent:@"save"];
        }

        NSString *caches = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)[0]
                            stringByAppendingPathComponent:[NSString stringWithFormat:@"NxNative/%@", gameName]];
        NSString *tempDir = [caches stringByAppendingPathComponent:@"temp"];

        [NSFileManager.defaultManager createDirectoryAtPath:saveDir withIntermediateDirectories:YES attributes:nil error:nil];
        [NSFileManager.defaultManager createDirectoryAtPath:tempDir withIntermediateDirectories:YES attributes:nil error:nil];
        if (!getenv("DM_SAVE")) setenv("DM_SAVE", saveDir.fileSystemRepresentation, 0);
        if (!getenv("DM_TEMP")) setenv("DM_TEMP", tempDir.fileSystemRepresentation, 0);
        chdir(saveDir.stringByDeletingLastPathComponent.fileSystemRepresentation);

        // Apply saved NSUserDefaults
        for (NSString *key in @[@"DM_OPMODE", @"DM_FPS_TARGET", @"DM_RENDER_SCALE", @"DM_METAL_HUD", @"DM_LANG", @"DM_SHOW_FPS", @"DM_SCREEN_MATCH"]) {
            NSString *val = [defaults stringForKey:[@"launcher." stringByAppendingString:key]];
            if (val && !getenv(key.UTF8String)) {
                setenv(key.UTF8String, val.UTF8String, 0);
            }
        }
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) {
            setenv("MTL_HUD_ENABLED", "1", 1);
            [defaults setBool:YES forKey:@"MTL_HUD_ENABLED"];
        }

        // Redirect stderr to ~/Library/Logs/NxNative/<GameName>.log when running as a GUI bundle
        if (!isatty(STDERR_FILENO)) {
            NSString *logsDir = [NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES)[0]
                                 stringByAppendingPathComponent:@"Logs/NxNative"];
            [NSFileManager.defaultManager createDirectoryAtPath:logsDir withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *logFile = [logsDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.log", DM_GAME_NAME]];
            NSString *prevFile = [logsDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%s.prev.log", DM_GAME_NAME]];
            [NSFileManager.defaultManager removeItemAtPath:prevFile error:nil];
            [NSFileManager.defaultManager moveItemAtPath:logFile toPath:prevFile error:nil];
            if (freopen(logFile.fileSystemRepresentation, "w", stderr)) {
                setvbuf(stderr, NULL, _IOLBF, 0);
            }
        }
    }
}

void front_run(void (*guest)(void)) {
    static id activity; activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    (void)activity;
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        DmAppDelegate *del = [DmAppDelegate new];
        g_app_delegate = del;
        [NSApp setDelegate:del];

        keymap_load();

        [NSWindow setAllowsAutomaticWindowTabbing:NO];

        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @DM_GAME_NAME;
        win.tabbingMode = NSWindowTabbingModeDisallowed;
        win.contentAspectRatio = NSMakeSize(16, 9);
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;

        NSView *v = win.contentView = [[DmGameView alloc] initWithFrame:r];
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) {
            g_layer.developerHUDProperties = @{@"mode": @"default"};
            LOG("front end: Metal performance HUD on (DM_METAL_HUD)");
        }
        v.layer = g_layer;
        v.wantsLayer = YES;
        [(DmGameView *)v fitDrawable];
        [win center];
        [win makeKeyAndOrderFront:nil];
        g_screen_fps = (int)(win.screen ?: NSScreen.mainScreen).maximumFramesPerSecond;
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];

        NSMenu *bar = [NSMenu new];

        NSMenu *appMenu = [NSMenu new];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %s", DM_GAME_NAME] action:@selector(showAbout:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *settingsItem = [appMenu addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
        settingsItem.target = del;
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %s", DM_GAME_NAME] action:@selector(hide:) keyEquivalent:@"h"];
        NSMenuItem *hideOthers = [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
        hideOthers.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
        [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %s", DM_GAME_NAME] action:@selector(terminate:) keyEquivalent:@"q"];
        [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:appMenu];

        NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        NSMenuItem *saveItem = [fileMenu addItemWithTitle:@"Open Save Folder in Finder" action:@selector(openSaveFolder:) keyEquivalent:@"s"];
        saveItem.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
        saveItem.target = del;
        NSMenuItem *expSave = [fileMenu addItemWithTitle:@"Export Save Backup…" action:@selector(exportSaveBackup:) keyEquivalent:@"e"];
        expSave.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
        expSave.target = del;
        NSMenuItem *resSave = [fileMenu addItemWithTitle:@"Restore Save from Backup…" action:@selector(restoreSaveBackup:) keyEquivalent:@""];
        resSave.target = del;
        [fileMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *logItem = [fileMenu addItemWithTitle:@"Open Log in Finder…" action:@selector(openLogFile:) keyEquivalent:@"l"];
        logItem.keyEquivalentModifierMask = NSEventModifierFlagShift | NSEventModifierFlagCommand;
        logItem.target = del;
        NSMenuItem *shotItem = [fileMenu addItemWithTitle:@"Take Screenshot" action:@selector(takeScreenshot:) keyEquivalent:@"s"];
        shotItem.target = del;
        [[bar addItemWithTitle:@"File" action:nil keyEquivalent:@""] setSubmenu:fileMenu];

        NSMenu *gameMenu = [[NSMenu alloc] initWithTitle:@"Game"];
        del.pauseItem = [gameMenu addItemWithTitle:@"Pause Game" action:@selector(togglePause:) keyEquivalent:@"p"];
        del.pauseItem.target = del;
        [gameMenu addItem:[NSMenuItem separatorItem]];

        del.dockedItem = [gameMenu addItemWithTitle:@"Docked" action:@selector(setOpModeDocked:) keyEquivalent:@""];
        del.dockedItem.target = del;
        del.handheldItem = [gameMenu addItemWithTitle:@"Handheld" action:@selector(setOpModeHandheld:) keyEquivalent:@""];
        del.handheldItem.target = del;
        [gameMenu addItem:[NSMenuItem separatorItem]];

        if (DM_GAME_FPS_TARGET != 0 || dm_fps_state) {
            NSMenu *fpsMenu = [[NSMenu alloc] initWithTitle:@"Target Frame Rate"];
            del.fpsDefaultItem = [fpsMenu addItemWithTitle:@"Default (Game Interval)" action:@selector(setFpsTarget:) keyEquivalent:@""];
            del.fpsDefaultItem.tag = 0; del.fpsDefaultItem.target = del;
            del.fps60Item = [fpsMenu addItemWithTitle:@"60 FPS" action:@selector(setFpsTarget:) keyEquivalent:@""];
            del.fps60Item.tag = 60; del.fps60Item.target = del;
            del.fps30Item = [fpsMenu addItemWithTitle:@"30 FPS" action:@selector(setFpsTarget:) keyEquivalent:@""];
            del.fps30Item.tag = 30; del.fps30Item.target = del;
            [[gameMenu addItemWithTitle:@"Target Frame Rate" action:nil keyEquivalent:@""] setSubmenu:fpsMenu];
        }

        if (dm_fps_state) {
            NSMenu *scaleMenu = [[NSMenu alloc] initWithTitle:@"Render Scale"];
            del.scale200Item = [scaleMenu addItemWithTitle:@"2.0x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale200Item.tag = 200; del.scale200Item.target = del;
            del.scale150Item = [scaleMenu addItemWithTitle:@"1.5x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale150Item.tag = 150; del.scale150Item.target = del;
            del.scale125Item = [scaleMenu addItemWithTitle:@"1.25x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale125Item.tag = 125; del.scale125Item.target = del;
            del.scale100Item = [scaleMenu addItemWithTitle:@"1.0x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale100Item.tag = 100; del.scale100Item.target = del;
            del.scale85Item = [scaleMenu addItemWithTitle:@"0.85x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale85Item.tag = 85; del.scale85Item.target = del;
            del.scale70Item = [scaleMenu addItemWithTitle:@"0.7x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale70Item.tag = 70; del.scale70Item.target = del;
            del.scale50Item = [scaleMenu addItemWithTitle:@"0.5x" action:@selector(setRenderScale:) keyEquivalent:@""];
            del.scale50Item.tag = 50; del.scale50Item.target = del;
            [[gameMenu addItemWithTitle:@"Render Scale" action:nil keyEquivalent:@""] setSubmenu:scaleMenu];
        }

        [gameMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *clearShaders = [gameMenu addItemWithTitle:@"Clear Shader Cache…" action:@selector(clearShaderCache:) keyEquivalent:@""];
        clearShaders.target = del;

        [[bar addItemWithTitle:@"Game" action:nil keyEquivalent:@""] setSubmenu:gameMenu];

        NSMenu *audioMenu = [[NSMenu alloc] initWithTitle:@"Audio"];
        del.muteItem = [audioMenu addItemWithTitle:@"Mute Audio" action:@selector(toggleMute:) keyEquivalent:@"m"];
        del.muteItem.target = del;
        del.muteUnfocusedItem = [audioMenu addItemWithTitle:@"Mute When Unfocused" action:@selector(toggleMuteUnfocused:) keyEquivalent:@""];
        del.muteUnfocusedItem.target = del;
        [[bar addItemWithTitle:@"Audio" action:nil keyEquivalent:@""] setSubmenu:audioMenu];

        NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem *fsItem = [viewMenu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
        fsItem.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;

        NSMenu *sizeMenu = [[NSMenu alloc] initWithTitle:@"Window Scale"];
        NSMenuItem *sz100 = [sizeMenu addItemWithTitle:@"1.0x" action:@selector(resizeWindowToMultiplier:) keyEquivalent:@"1"];
        sz100.tag = 100; sz100.target = del;
        NSMenuItem *sz150 = [sizeMenu addItemWithTitle:@"1.5x" action:@selector(resizeWindowToMultiplier:) keyEquivalent:@"2"];
        sz150.tag = 150; sz150.target = del;
        NSMenuItem *sz200 = [sizeMenu addItemWithTitle:@"2.0x" action:@selector(resizeWindowToMultiplier:) keyEquivalent:@"3"];
        sz200.tag = 200; sz200.target = del;
        NSMenuItem *sz300 = [sizeMenu addItemWithTitle:@"3.0x" action:@selector(resizeWindowToMultiplier:) keyEquivalent:@"4"];
        sz300.tag = 300; sz300.target = del;
        [[viewMenu addItemWithTitle:@"Window Scale" action:nil keyEquivalent:@""] setSubmenu:sizeMenu];

        [viewMenu addItem:[NSMenuItem separatorItem]];
        del.metalHudItem = [viewMenu addItemWithTitle:@"Apple Metal Performance HUD" action:@selector(toggleMetalHUD:) keyEquivalent:@"u"];
        del.metalHudItem.target = del;
        [[bar addItemWithTitle:@"View" action:nil keyEquivalent:@""] setSubmenu:viewMenu];

        NSMenu *ctlMenu = [[NSMenu alloc] initWithTitle:@"Controllers"];
        del.controllersMenu = ctlMenu;
        [[bar addItemWithTitle:@"Controllers" action:nil keyEquivalent:@""] setSubmenu:ctlMenu];

        NSMenu *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [wm addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
        [wm addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
        [wm addItem:[NSMenuItem separatorItem]];
        [wm addItemWithTitle:@"Bring All to Front" action:@selector(arrangeInFront:) keyEquivalent:@""];
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];

        NSMenu *helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
        NSMenuItem *hGuide = [helpMenu addItemWithTitle:@"Configure Controls & Keys…" action:@selector(showSettings:) keyEquivalent:@"?"];
        hGuide.target = del;
        [[bar addItemWithTitle:@"Help" action:nil keyEquivalent:@""] setSubmenu:helpMenu];

        NSApp.mainMenu = bar;
        [del updateMenuState];

        if (getenv("DM_FULLSCREEN")) dispatch_async(dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
        for (NSNotificationName nm in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [NSNotificationCenter.defaultCenter addObserverForName:nm object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                GCController *c = n.object; int conn = n.name == GCControllerDidConnectNotification;
                if (!c.extendedGamepad) LOG("controller %s: %s%s", conn ? "connected" : "disconnected", pad_name(c), c.extendedGamepad ? "" : " (no extended gamepad profile: not used)");
                pads_changed(conn ? nil : c); }];
        dm_haptics_apple_init();
        pads_start();

        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];

        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (win.screen) { g_screen_fps = (int)win.screen.maximumFramesPerSecond; g_layer.contentsScale = win.backingScaleFactor; } screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            if (!win.miniaturized) dm_focus_set(2);
            if (g_mute_unfocused) dm_audio_suspend(1);
        }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            dm_focus_set(1);
            if (g_mute_unfocused && !g_audio_muted) dm_audio_suspend(0);
        }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); if (!g_audio_muted) dm_audio_suspend(0); dm_focus_set(2); }];

        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
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
        g_btn = 0; g_lx_pos = g_lx_neg = g_ly_pos = g_ly_neg = 0; g_rx_pos = g_rx_neg = g_ry_pos = g_ry_neg = 0; push();
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
    (void)max_chars;
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
