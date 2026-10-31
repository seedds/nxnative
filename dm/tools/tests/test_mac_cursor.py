#!/usr/bin/env python3
"""Compile the real Mac keyboard/cursor path against windowless AppKit doubles."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FRONT_MAC = REPO / 'dm/host/front_mac.m'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'requires macOS and clang')
class MacCursor(unittest.TestCase):
    def test_keyboard_hides_only_over_active_game_content(self):
        source = FRONT_MAC.read_text()
        # Include the implementation, not a duplicate of the cursor policy.
        code = source[source.index('static void push(void)'):source.index('// Physical controllers')]
        harness = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#define DM_BTN_SHAKE (1ull << 32)
static int hides;
@interface NSCursor : NSObject
+ (void)setHiddenUntilMouseMoves:(BOOL)flag;
@end
@implementation NSCursor
+ (void)setHiddenUntilMouseMoves:(BOOL)flag { if (flag) hides++; }
@end
@interface NSView : NSObject
@property NSRect bounds;
- (NSPoint)convertPoint:(NSPoint)p fromView:(id)view;
@end
@implementation NSView
- (NSPoint)convertPoint:(NSPoint)p fromView:(id)view { (void)view; return p; }
@end
@interface TestWindow : NSObject
@property BOOL isKeyWindow;
@property(strong) NSView *contentView;
@property NSPoint mouseLocationOutsideOfEventStream;
@end
@implementation TestWindow @end
@interface TestApplication : NSObject
@property BOOL isActive;
@end
@implementation TestApplication @end
@interface NSEvent : NSObject
@property unsigned short keyCode;
@end
@implementation NSEvent @end
static TestWindow *g_win;
static TestApplication *NSApp;
static uint64_t g_btn;
static int g_input_on = 1, g_w, g_a, g_s, g_d;
static void dm_input_set(uint64_t b, int32_t x, int32_t y, int32_t rx, int32_t ry) {
    (void)b; (void)x; (void)y; (void)rx; (void)ry;
}
'''
        checks = r'''
int main(void) { @autoreleasepool {
    NSApp = [TestApplication new]; NSApp.isActive = YES;
    NSEvent *event = [NSEvent new]; event.keyCode = 38; // J = A
    on_key(event, 1); assert(hides == 0); // no window (headless)
    g_win = [TestWindow new]; g_win.contentView = [NSView new];
    g_win.contentView.bounds = NSMakeRect(0, 0, 100, 100);
    g_win.mouseLocationOutsideOfEventStream = NSMakePoint(50, 50);
    on_key(event, 1); assert(hides == 0); // window not key
    g_win.isKeyWindow = YES; NSApp.isActive = NO;
    on_key(event, 1); assert(hides == 0); // another app is active
    NSApp.isActive = YES;
    g_win.mouseLocationOutsideOfEventStream = NSMakePoint(50, 120);
    on_key(event, 1); assert(hides == 0); // title bar / outside content
    g_win.mouseLocationOutsideOfEventStream = NSMakePoint(50, 50);
    on_key(event, 1); assert(hides == 1); // recognized gameplay press
    on_key(event, 0); assert(hides == 1); // release does not re-hide
    event.keyCode = 99; on_key(event, 1); assert(hides == 1); // unknown key
    event.keyCode = 13; on_key(event, 1); assert(hides == 2); // W stick input
    g_input_on = 0; on_key(event, 1); assert(hides == 2); // input suspended
    puts("Mac cursor: headless, focus, content bounds and keyboard input: OK");
} return 0; }
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cursor.m').write_text(harness + code + checks)
            build = subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-framework', 'Foundation', str(root / 'cursor.m'), '-o', str(root / 'cursor')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(root / 'cursor')], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
