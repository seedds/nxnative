"""Exercise Mac display-sleep assertions and production focus callbacks without a window."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'requires macOS and clang')
class MacDisplaySleep(unittest.TestCase):
    def test_focus_notifications_hold_and_release_display_assertion(self):
        source = (REPO / 'dm/host/front_mac.m').read_text()
        start = source.index('static id g_display_activity;')
        end = source.index('static void pads_start(void)', start)
        code = source[start:end]
        # Run the actual observer blocks through NotificationCenter. The window is a
        # state-only double; no application is activated and no real window is opened.
        events = ('NSWindowDidResignKeyNotification', 'NSWindowDidBecomeKeyNotification',
                  'NSWindowDidMiniaturizeNotification', 'NSWindowDidDeminiaturizeNotification')
        observers = '\n'.join(line for line in source.splitlines()
                              if '[nc addObserverForName:' in line and any(e in line for e in events))
        self.assertEqual(observers.count('[nc addObserverForName:'), len(events))
        startup = 'display_sleep_active(win.isKeyWindow);'
        # A headless return must precede startup acquisition, which must follow observers.
        self.assertLess(source.index('if (!getenv("DM_WINDOW"))'), source.index(startup))
        self.assertLess(source.index(observers.splitlines()[-1].strip()), source.index(startup))
        harness = r'''
#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <assert.h>
#include <unistd.h>
#include "dm_input.h"
static int g_input_on = 1, g_merge, g_w, g_a, g_s, g_d, focus, audio, gfx;
static uint64_t g_btn;
static GCController *g_held[DM_PLAYERS];
static void push(void) {}
static void pad_read(GCExtendedGamepad *g) { (void)g; }
static void dm_motion_pro_active(int active) { (void)active; }
static void dm_touch_set(const void *touch, int n) { (void)touch; (void)n; }
void dm_pad_set(uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) {
    (void)b; (void)x; (void)y; (void)rx; (void)ry;
}
void dm_pad_set_player(int p, uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) {
    (void)p; dm_pad_set(b, x, y, rx, ry);
}
static void dm_focus_set(int value) { focus = value; }
static void dm_audio_suspend(int value) { audio = value; }
static void dm_gfx_suspend(int value) { gfx = value; }
@interface TestWindow : NSObject
@property BOOL isKeyWindow, miniaturized;
@end
@implementation TestWindow @end
static int assertions(CFStringRef type) {
    CFDictionaryRef all = NULL;
    assert(IOPMCopyAssertionsByProcess(&all) == kIOReturnSuccess);
    NSDictionary *processes = CFBridgingRelease(all);
    int count = 0;
    for (NSDictionary *entry in processes[@(getpid())]) {
        if ([entry[(__bridge NSString *)kIOPMAssertionTypeKey] isEqualToString:(__bridge NSString *)type] &&
            [entry[(__bridge NSString *)kIOPMAssertionLevelKey] intValue] == kIOPMAssertionLevelOn) count++;
    }
    return count;
}
static int display_assertions(void) { return assertions(kIOPMAssertionTypePreventUserIdleDisplaySleep); }
'''
        checks = r'''
int main(void) { @autoreleasepool {
    id latency = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    int baseline = display_assertions();
    int system = assertions(kIOPMAssertionTypePreventUserIdleSystemSleep);
    assert(system > 0);
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    TestWindow *win = [TestWindow new];
    OBSERVERS
    STARTUP
    assert(!g_display_activity && display_assertions() == baseline); // not key at startup
    win.isKeyWindow = YES;
    STARTUP
    assert(g_display_activity && display_assertions() > baseline); // already key at startup
    id first = g_display_activity;
    [nc postNotificationName:NSWindowDidBecomeKeyNotification object:win];
    assert(g_display_activity == first && focus == 1 && g_input_on); // duplicate acquire
    [nc postNotificationName:NSWindowDidResignKeyNotification object:win];
    assert(!g_display_activity && display_assertions() == baseline && focus == 2 && !g_input_on);
    assert(assertions(kIOPMAssertionTypePreventUserIdleSystemSleep) == system); // preserve latency activity
    [nc postNotificationName:NSWindowDidResignKeyNotification object:win];
    assert(!g_display_activity && display_assertions() == baseline); // duplicate release
    [nc postNotificationName:NSWindowDidBecomeKeyNotification object:win];
    assert(g_display_activity && display_assertions() > baseline && focus == 1);
    win.miniaturized = YES;
    [nc postNotificationName:NSWindowDidMiniaturizeNotification object:win];
    assert(!g_display_activity && display_assertions() == baseline && focus == 3 && audio && gfx);
    win.miniaturized = NO;
    [nc postNotificationName:NSWindowDidDeminiaturizeNotification object:win];
    assert(!g_display_activity && display_assertions() == baseline && focus == 2 && !audio && !gfx);
    [nc postNotificationName:NSWindowDidBecomeKeyNotification object:win];
    assert(g_display_activity && display_assertions() > baseline && focus == 1);
    [nc postNotificationName:NSWindowDidResignKeyNotification object:win];
    assert(display_assertions() == baseline);
    [NSProcessInfo.processInfo endActivity:latency];
} return 0; }
'''.replace('OBSERVERS', observers).replace('STARTUP', startup)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'sleep.m').write_text(harness + code + checks)
            build = subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(REPO / 'dm/host'), '-framework', 'AppKit',
                                    '-framework', 'GameController', '-framework', 'IOKit',
                                    str(root / 'sleep.m'), '-o', str(root / 'sleep')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(root / 'sleep')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
