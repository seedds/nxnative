// Native, windowless integration: compile the entire frontend, not extracted source snippets.
// Only OS side effects and host input sinks are substituted; key handling, routing,
// publication through pad_set, and focus lifecycle remain production code.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <assert.h>
#include "../dm.h"
#import "../motion_pro_mac.h"
static void test_pro_start(void (*changed)(void)) { (void)changed; }
#define dm_motion_pro_start test_pro_start
#include "../front_mac.m"
#undef dm_motion_pro_start

static DmPlayerState keyboard_state, player_state[DM_PLAYERS];
static unsigned keyboard_writes, player_writes[DM_PLAYERS], hides, shows, activities, ended, touches;
static uint32_t player_mask;
void dm_input_set(uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) {
    keyboard_state = (DmPlayerState){b, x, y, rx, ry}; keyboard_writes++;
}
void dm_pad_set_player(int p, uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) {
    assert(p >= 0 && p < DM_PLAYERS);
    player_state[p] = (DmPlayerState){b, x, y, rx, ry}; player_writes[p]++;
}
void dm_pad_set(uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) { dm_pad_set_player(0, b, x, y, rx, ry); }
void dm_pad_players(uint32_t mask) { player_mask = mask; }
uint32_t dm_player_npad(int p) { assert(p >= 0 && p < DM_PLAYERS); return (uint32_t)p; }
void dm_haptics_apple_controller(int p, GCController *c) { (void)p; (void)c; }
void dm_motion_set(int p, const float a[3], const float r[3]) { (void)p; (void)a; (void)r; }
void dm_motion_clear(int p) { (void)p; }
void dm_motion_clear_hid(int p) { (void)p; }
void dm_touch_set(const dm_touch *t, int n) { assert(!t && n == 0); touches++; }

// Whole-file Objective-C classes retain the cheats menu methods at link time.
// Fail closed if this focused test ever reaches unrelated host/UI services.
#define UNREACHABLE(ret, name, args) ret name args { abort(); }
UNREACHABLE(void, dm_focus_set, (int state))
UNREACHABLE(int, dm_cheats_found, (void))
UNREACHABLE(int, dm_cheats_ncheats, (void))
UNREACHABLE(int, dm_cheats_nothers, (void))
UNREACHABLE(int, dm_cheats_nskipped, (void))
UNREACHABLE(int, dm_cheats_nsources, (void))
UNREACHABLE(int, dm_cheats_master, (void))
UNREACHABLE(int, dm_cheats_safe, (void))
UNREACHABLE(const char *, dm_cheats_build, (void))
UNREACHABLE(const char *, dm_cheats_dir_path, (void))
UNREACHABLE(const char *, dm_cheats_report, (void))
UNREACHABLE(const char *, dm_cheats_version, (void))
UNREACHABLE(const char *, dm_cheats_other, (int i))
UNREACHABLE(const char *, dm_cheats_other_version, (int i))
UNREACHABLE(const char *, dm_cheats_skipped_name, (int i))
UNREACHABLE(const char *, dm_cheats_skipped_why, (int i))
UNREACHABLE(const char *, dm_cheats_source, (int i))
UNREACHABLE(const char *, dm_cheat_name_at, (int i))
UNREACHABLE(const char *, dm_cheat_why_at, (int i))
UNREACHABLE(const char *, dm_cheat_section_at, (int i))
UNREACHABLE(const char *, dm_cheat_note_at, (int i))
UNREACHABLE(const char *, dm_cheat_type_at, (int i))
UNREACHABLE(const char *, dm_cheat_value_bounds, (int i))
UNREACHABLE(int, dm_cheat_kind_at, (int i))
UNREACHABLE(int, dm_cheat_on_at, (int i))
UNREACHABLE(int, dm_cheat_flags_at, (int i))
UNREACHABLE(int, dm_cheat_preview, (int i, char *out, size_t cap))
UNREACHABLE(int, dm_cheat_value_get, (int i, char *out, size_t cap))
UNREACHABLE(int, dm_cheat_value_set, (int i, const char *text))
UNREACHABLE(void, dm_cheat_toggle, (int i))
UNREACHABLE(void, dm_cheats_reload, (void))
UNREACHABLE(void, dm_cheats_master_set, (int on))
UNREACHABLE(void, dm_cheats_safe_restore, (void))
#undef UNREACHABLE

// These objects are deliberately NOT NSWindow/NSView/NSApplication instances:
// no AppKit connection, window server, preferences, controller discovery or GPU.
@interface InputEnvironment : NSObject
@property(nonatomic) BOOL active, keyWindow;
@end
@implementation InputEnvironment
- (NSView *)contentView { return (NSView *)(id)self; }
- (NSPoint)mouseLocationOutsideOfEventStream { return NSMakePoint(5, 5); }
- (NSRect)bounds { return NSMakeRect(0, 0, 10, 10); }
- (NSPoint)convertPoint:(NSPoint)p fromView:(NSView *)v { (void)v; return p; }
- (BOOL)isActive { return _active; }
- (BOOL)isKeyWindow { return _keyWindow; }
@end
@interface InputKey : NSObject
@property(nonatomic) unsigned short keyCode;
@property(nonatomic) NSEventModifierFlags modifierFlags;
@end
@implementation InputKey
@end
static void key(unsigned short code, BOOL shift, int down) {
    InputKey *e = [InputKey new]; e.keyCode = code; e.modifierFlags = shift ? NSEventModifierFlagShift : 0;
    on_key((NSEvent *)(id)e, down);
}
static void replace_method(Method method, IMP implementation) {
    assert(method);
    method_setImplementation(method, implementation);
}
static void cursor(id self, SEL cmd, BOOL hidden) { (void)self; (void)cmd; if (hidden) hides++; else shows++; }
static id activity(id self, SEL cmd, NSActivityOptions options, NSString *reason) {
    (void)self; (void)cmd; (void)reason; assert(options == NSActivityIdleDisplaySleepDisabled); activities++; return @"test activity";
}
static void end_activity(id self, SEL cmd, id token) { (void)self; (void)cmd; assert(token); ended++; }
static NSArray *controllers(id self, SEL cmd) { (void)self; (void)cmd; return @[]; }
static void expect(DmPlayerState s, uint64_t b, int x, int y, int rx, int ry) {
    assert(s.buttons == b && s.lx == x && s.ly == y && s.rx == rx && s.ry == ry);
}
static DmPlayerState state(void) { return g_players ? player_state[3] : keyboard_state; }
static void arrows(void) {
    key(13, NO, 1); key(38, NO, 1); key(123, YES, 1); key(126, YES, 1);
    expect(state(), DM_BTN_A, 0, 32767, -32767, 32767);
    key(124, YES, 1); key(125, YES, 1);
    expect(state(), DM_BTN_A, 0, 32767, 0, 0);
    key(123, NO, 0); key(126, NO, 0); // Shift may be released before the arrows.
    expect(state(), DM_BTN_A, 0, 32767, 32767, -32767);
    key(124, NO, 0); key(125, NO, 0); key(13, NO, 0); key(38, NO, 0);
    expect(state(), 0, 0, 0, 0, 0);
    key(123, NO, 1); key(126, NO, 1);
    expect(state(), DM_BTN_LEFT | DM_BTN_UP, 0, 0, 0, 0);
    key(123, YES, 0); key(126, YES, 0); // Ordinary D-pad releases survive a new Shift.
    expect(state(), 0, 0, 0, 0, 0);
}
static void lifecycle(void) {
    key(0, NO, 1); key(38, NO, 1); key(124, YES, 1); key(125, YES, 1);
    g_mouse_down = 1;
    unsigned show = shows, touch = touches;
    input_release();
    assert(!g_input_on && !g_mouse_down && !g_pro_active && shows == show + 1 && touches == touch + 1);
    if (g_players) {
        assert(!g_players.active);
        [g_players keyboard:(DmPlayerState){DM_BTN_B, 1, 2, 3, 4}]; // Configured callbacks must also stay neutral while inactive.
    }
    assert(!g_btn && !g_w && !g_a && !g_s && !g_d && !g_rl && !g_rr && !g_ru && !g_rd);
    expect(state(), 0, 0, 0, 0, 0);
    key(13, NO, 1); key(123, YES, 1); expect(state(), 0, 0, 0, 0, 0);
    input_resume(); assert(g_input_on && g_pro_active && (!g_players || g_players.active)); expect(state(), 0, 0, 0, 0, 0);
    key(126, YES, 1); expect(state(), 0, 0, 0, 0, 32767);
    key(126, NO, 0); expect(state(), 0, 0, 0, 0, 0);
    input_release(); input_resume(); expect(state(), 0, 0, 0, 0, 0);
}
int main(void) {
    @autoreleasepool {
        replace_method(class_getClassMethod(NSCursor.class, @selector(setHiddenUntilMouseMoves:)), (IMP)cursor);
        replace_method(class_getInstanceMethod(NSProcessInfo.class, @selector(beginActivityWithOptions:reason:)), (IMP)activity);
        replace_method(class_getInstanceMethod(NSProcessInfo.class, @selector(endActivity:)), (IMP)end_activity);
        replace_method(class_getClassMethod(GCController.class, @selector(controllers)), (IMP)controllers);
        InputEnvironment *env = [InputEnvironment new]; env.active = env.keyWindow = YES;
        NSApp = (NSApplication *)(id)env; g_win = (NSWindow *)(id)env;
        arrows(); lifecycle(); assert(keyboard_writes > 0);
        unsigned legacy = keyboard_writes;
        g_players = [DmMacPlayers new]; g_players.keyboardPlayer = 3;
        pads_start(); // Installs the REAL configured output callback, including pad_set cursor handling.
        assert(player_mask == (1u << 3));
        for (int p = 0; p < DM_PLAYERS; p++) { assert(player_writes[p] == 1); expect(player_state[p], 0, 0, 0, 0, 0); }
        unsigned hidden = hides;
        key(124, YES, 1); expect(state(), 0, 0, 0, 32767, 0);
        assert(hides == hidden + 1); // Shift-arrow returns before on_key's own cursor call.
        key(124, NO, 0);
        hidden = hides;
        [g_players keyboard:(DmPlayerState){0, DM_STICK_MAX * 15 / 100, 0, 0, 0}]; assert(hides == hidden);
        [g_players keyboard:(DmPlayerState){0, DM_STICK_MAX * 15 / 100 + 1, 0, 0, 0}]; assert(hides == hidden + 1);
        [g_players keyboard:(DmPlayerState){0}];
        arrows(); lifecycle(); assert(g_players.active && keyboard_writes == legacy);
        // Focus release now force-publishes neutral state for every configured player.
        for (int p = 0; p < DM_PLAYERS; p++) if (p != 3) expect(player_state[p], 0, 0, 0, 0, 0);
        input_release(); assert(activities == ended && !g_display_activity);
        g_players = nil; g_win = nil; NSApp = nil;
        puts("front_mac input: keyboard axes, configured pad_set publication, release/resume passed");
    }
}
