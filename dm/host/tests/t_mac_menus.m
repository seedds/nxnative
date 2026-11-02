// Real AppKit actions, isolated preferences and a synthetic runtime settings page; no user data or audio device.
#include <assert.h>
#include "../menus_mac.m"
static int test_mode = 1, test_muted, test_volume = 100;
uint8_t *dm_fps_state;
int dm_opmode(void) { return test_mode; }
void dm_set_opmode(int m) { test_mode = m; }
void dm_audio_mute(int on) { test_muted = on; }
void dm_audio_volume(int percent) { test_volume = percent; }
void dm_gfx_shot_at(long frame, const char *p) { (void)frame; (void)p; }
static NSMenuItem *choice(NSString *env, NSString *value) {
    NSMenuItem *item = [NSMenuItem new]; item.representedObject = @{@"env": env, @"value": value}; return item;
}
static float state_at(size_t offset) { float n; memcpy(&n, dm_fps_state + offset, sizeof n); return n; }
static NSString *saved(NSString *domain, NSString *env) {
    NSString *key = [@"launcher." stringByAppendingString:env];
    CFPropertyListRef value = CFPreferencesCopyAppValue((__bridge CFStringRef)key, (__bridge CFStringRef)domain);
    NSString *s = value && CFGetTypeID(value) == CFStringGetTypeID() ? [(__bridge NSString *)value copy] : nil;
    if (value) CFRelease(value); return s;
}
int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 450) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        NSMenu *bar = [NSMenu new]; [bar addItemWithTitle:@"Cheats" action:nil keyEquivalent:@""];
        mac_menus_install(w, bar);
        assert([bar itemWithTitle:@"Cheats"]); assert([bar itemWithTitle:@"File"]); assert([bar itemWithTitle:@"View"]);
        NSMenu *file = [bar itemWithTitle:@"File"].submenu;
        DmMacMenus *m = [file itemWithTitle:@"Open Save Folder"].target; assert(m);
        m.domain = [@"org.nxnative.menu-test." stringByAppendingString:NSUUID.UUID.UUIDString];
        m.settings = @{
            @"DM_OPMODE": @{@"kind": @"choice", @"label": @"Console mode", @"default": @"1", @"options": @[@[@"1", @"Docked"], @[@"0", @"Handheld"]]},
            @"DM_FPS_TARGET": @{@"kind": @"choice", @"label": @"Frame rate", @"default": @"60", @"options": @[@[@"30", @"30 fps"], @[@"60", @"60 fps"]]},
            @"DM_RENDER_SCALE": @{@"kind": @"range", @"label": @"Render scale", @"default": @"1.0", @"min": @0.5, @"max": @1.0, @"step": @0.05},
            @"DM_METAL_HUD": @{@"kind": @"toggle", @"label": @"Performance HUD", @"default": @"0", @"on": @"1", @"off": @"0"}
        };
        [m chooseSetting:choice(@"DM_OPMODE", @"0")]; assert(test_mode == 0);
        assert([saved(m.domain, @"DM_OPMODE") isEqual:@"0"]);
        [m chooseSetting:choice(@"DM_OPMODE", @"1")]; assert(test_mode == 1 && !saved(m.domain, @"DM_OPMODE"));
        setenv("DM_ENV_LAUNCH_KEYS", "DM_LANG DM_OPMODE", 1);
        [m chooseSetting:choice(@"DM_OPMODE", @"0")]; assert(test_mode == 1 && [m fixed:@"DM_OPMODE"]);
        unsetenv("DM_ENV_LAUNCH_KEYS"); setenv("DM_ENV_FILE_KEYS", "DM_OPMODE", 1);
        [m chooseSetting:choice(@"DM_OPMODE", @"0")]; assert(test_mode == 1 && [m fixed:@"DM_OPMODE"]);
        unsetenv("DM_ENV_FILE_KEYS");
        [m chooseSetting:choice(@"DM_OPMODE", @"0")];
        setenv("DM_SAVED_CHOICES", "0", 1);
        [m chooseSetting:choice(@"DM_OPMODE", @"1")]; assert(test_mode == 1 && [saved(m.domain, @"DM_OPMODE") isEqual:@"0"]);
        assert(![m canChange:@"DM_METAL_HUD" value:@"1"]); unsetenv("DM_SAVED_CHOICES");
        [m chooseSetting:choice(@"DM_OPMODE", @"1")];

        NSMenu *settings = [NSMenu new];
        setting_menu(settings, @"DM_FPS_TARGET", m); assert(settings.numberOfItems == 0); // unsupported runtime capability stays hidden
        _Alignas(8) uint8_t page[64] = {0}; dm_fps_state = page;
        setting_menu(settings, @"DM_FPS_TARGET", m); setting_menu(settings, @"DM_RENDER_SCALE", m);
        NSMenu *fps = [settings itemWithTitle:@"Frame rate"].submenu;
        NSMenu *scale = [settings itemWithTitle:@"Render scale"].submenu;
        assert(fps.numberOfItems == 2 && scale.numberOfItems == 11); // no invented default interval or supersampling options
        NSMenuItem *fps30 = [fps itemWithTitle:@"30 fps"]; assert(fps30);
        [m chooseSetting:fps30]; assert(state_at(0) == 30 && [saved(m.domain, @"DM_FPS_TARGET") isEqual:@"30"]);
        [m chooseSetting:choice(@"DM_FPS_TARGET", @"0")]; assert(state_at(0) == 30);
        [m chooseSetting:choice(@"DM_FPS_TARGET", @"60")]; assert(state_at(0) == 60 && !saved(m.domain, @"DM_FPS_TARGET"));
        NSMenuItem *scale85 = [scale itemWithTitle:@"85%" ]; assert(scale85);
        [m chooseSetting:scale85]; assert(fabs(state_at(0x10) - 0.85) < 1e-6);
        [m chooseSetting:choice(@"DM_RENDER_SCALE", @"1.00")]; assert(state_at(0x10) == 1 && !saved(m.domain, @"DM_RENDER_SCALE"));
        [m chooseSetting:choice(@"DM_RENDER_SCALE", @"2")]; assert(state_at(0x10) == 1);
        setenv("DM_ENV_FILE_KEYS", "DM_RENDER_SCALE", 1);
        [m chooseSetting:scale85]; assert(state_at(0x10) == 1 && ![m validateMenuItem:scale85]); unsetenv("DM_ENV_FILE_KEYS");
        dm_fps_state = NULL; assert(![m canChange:@"DM_FPS_TARGET" value:@"30"]);

        unsetenv("DM_METAL_HUD");
        NSMenu *view = [NSMenu new]; setting_menu(view, @"DM_METAL_HUD", m);
        assert([view itemWithTitle:@"Performance HUD (Next Launch)"]);
        [m chooseSetting:choice(@"DM_METAL_HUD", @"1")]; assert([[m current:@"DM_METAL_HUD"] isEqual:@"1"] && [saved(m.domain, @"DM_METAL_HUD") isEqual:@"1"]);
        setenv("DM_METAL_HUD", "1", 1); setenv("DM_ENV_LAUNCH_KEYS", "", 1); // saved On applied by the launcher for this run
        [m chooseSetting:choice(@"DM_METAL_HUD", @"0")]; assert(!saved(m.domain, @"DM_METAL_HUD") && [[m current:@"DM_METAL_HUD"] isEqual:@"0"]);
        setenv("DM_METAL_HUD", "0", 1); setenv("DM_ENV_LAUNCH_KEYS", "DM_METAL_HUD", 1);
        [m chooseSetting:choice(@"DM_METAL_HUD", @"1")]; assert(!saved(m.domain, @"DM_METAL_HUD") && [[m current:@"DM_METAL_HUD"] isEqual:@"0"]);
        unsetenv("DM_METAL_HUD"); unsetenv("DM_ENV_LAUNCH_KEYS");

        NSMenuItem *volume = [NSMenuItem new]; volume.tag = 50; [m chooseVolume:volume]; assert(test_volume == 50);
        mac_menus_focus(1); [m mute:nil]; assert(test_muted);
        mac_menus_focus(0); mac_menus_focus(1); assert(test_muted && test_volume == 50);
        [m mute:nil]; assert(!test_muted && test_volume == 50);
        [m muteUnfocused:nil]; assert(!test_muted);
        mac_menus_focus(0); assert(test_muted); mac_menus_focus(1); assert(!test_muted);
        volume.tag = 101; [m chooseVolume:volume]; assert(test_volume == 50);
        NSMenuItem *export = [file itemWithTitle:@"Export Save Backup…"], *restore = [file itemWithTitle:@"Restore Save from Backup…"]; assert(export && restore);
        assert(export.keyEquivalentModifierMask == (NSEventModifierFlagCommand | NSEventModifierFlagShift));
        m.busy = YES; assert(![m validateMenuItem:export]);
        m.busy = NO; unsetenv("DM_MAC_RESTORE_DIR"); assert(![m validateMenuItem:restore]);
        assert(![bar itemWithTitle:@"Controllers"]); assert(![bar itemWithTitle:@"Metal HUD"]);
        [w close];
        puts("Mac menus: metadata capabilities, shared choices/defaults/overrides, live state, HUD startup preference, volume and focus pass");
    }
    return 0;
}
