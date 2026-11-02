// Exercise the real AppKit actions against an isolated preferences domain; no game, audio device or user save is used.
#include <assert.h>
#include "../menus_mac.m"
static int test_mode = 1, test_muted;
int dm_opmode(void) { return test_mode; }
void dm_set_opmode(int m) { test_mode = m; }
void dm_audio_mute(int on) { test_muted = on; }
void dm_gfx_shot_at(long frame, const char *p) { (void)frame; (void)p; }

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, 450) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        NSMenu *bar = [NSMenu new]; [bar addItemWithTitle:@"Cheats" action:nil keyEquivalent:@""];
        mac_menus_install(w, bar);
        assert([bar itemWithTitle:@"Cheats"]); assert([bar itemWithTitle:@"File"]); assert([bar itemWithTitle:@"View"]);
        NSMenu *file = [bar itemWithTitle:@"File"].submenu;
        DmMacMenus *m = [file itemWithTitle:@"Open Save Folder"].target;
        m.domain = [@"org.nxnative.menu-test." stringByAppendingString:NSUUID.UUID.UUIDString];
        m.mode = @{@"default": @"1"};
        CFStringRef domain = (__bridge CFStringRef)m.domain;
        NSMenuItem *handheld = [NSMenuItem new]; handheld.tag = 0;
        NSMenuItem *docked = [NSMenuItem new]; docked.tag = 1;
        [m chooseMode:handheld]; assert(test_mode == 0);
        CFPropertyListRef saved = CFPreferencesCopyAppValue(CFSTR("launcher.DM_OPMODE"), domain);
        assert(saved && CFGetTypeID(saved) == CFStringGetTypeID() && CFEqual(saved, CFSTR("0"))); CFRelease(saved);
        [m chooseMode:docked]; assert(test_mode == 1);
        assert(!CFPreferencesCopyAppValue(CFSTR("launcher.DM_OPMODE"), domain));
        setenv("DM_ENV_LAUNCH_KEYS", "DM_LANG DM_OPMODE", 1);
        [m chooseMode:handheld]; assert(test_mode == 1 && m.modeFixed);
        unsetenv("DM_ENV_LAUNCH_KEYS"); setenv("DM_ENV_FILE_KEYS", "DM_OPMODE", 1);
        [m chooseMode:handheld]; assert(test_mode == 1 && m.modeFixed);
        unsetenv("DM_ENV_FILE_KEYS"); setenv("DM_SAVED_CHOICES", "0", 1);
        [m chooseMode:handheld]; assert(test_mode == 0);
        assert(!CFPreferencesCopyAppValue(CFSTR("launcher.DM_OPMODE"), domain)); unsetenv("DM_SAVED_CHOICES");
        mac_menus_focus(1); [m mute:nil]; assert(test_muted);
        mac_menus_focus(0); mac_menus_focus(1); assert(test_muted);   // focus does not undo explicit mute
        [m mute:nil]; assert(!test_muted);
        [m muteUnfocused:nil]; assert(!test_muted);
        mac_menus_focus(0); assert(test_muted); mac_menus_focus(1); assert(!test_muted);
        m.busy = YES; assert(![m validateMenuItem:[file itemWithTitle:@"Export Save Backup…"]]);
        m.busy = NO; unsetenv("DM_MAC_RESTORE_DIR"); assert(![m validateMenuItem:[file itemWithTitle:@"Restore Save from Backup…"]]);
        assert(![bar itemWithTitle:@"Controllers"]); assert(![bar itemWithTitle:@"Metal HUD"]);
        [w close];
        puts("Mac menus: existing menus, shared preferences/default removal, override precedence, session mute/focus and unavailable restore pass");
    }
    return 0;
}
