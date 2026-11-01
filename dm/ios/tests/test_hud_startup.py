"""Run iOS boot/startGame on Foundation with a first-device HUD latch and synthetic files."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]

FIXTURE = r'''
#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "front.h"
#include "player_env.h"
extern char **environ;
static NSString *fixture_doc, *fixture_bundle;
static int phase, first_phase, first_hud, devices, views, boots, errors, gpu_mode, have_romfs;
@protocol MTLDevice <NSObject>
@property(readonly) BOOL supportsBCTextureCompression;
@property(readonly) NSString *name;
@end
@interface FixtureGPU : NSObject <MTLDevice>
@end
@implementation FixtureGPU
- (BOOL)supportsBCTextureCompression { return gpu_mode == 0; }
- (NSString *)name { return @"synthetic GPU"; }
@end
static id<MTLDevice> MTLCreateSystemDefaultDevice(void) {
    if (!devices++) { first_phase = phase; const char *v = getenv("MTL_HUD_ENABLED"); first_hud = v && atoi(v); }
    return gpu_mode == 2 ? nil : [FixtureGPU new];
}
@interface FixtureBundle : NSObject
+ (instancetype)mainBundle;
- (NSString *)resourcePath;
- (id)objectForInfoDictionaryKey:(NSString *)key;
@end
@implementation FixtureBundle
+ (instancetype)mainBundle { return [self new]; }
- (NSString *)resourcePath { return fixture_bundle; }
- (id)objectForInfoDictionaryKey:(NSString *)key { (void)key; return nil; }
@end
#define NSBundle FixtureBundle
#define NSSearchPathForDirectoriesInDomains(a,b,c) @[fixture_doc]
static NSArray *romfs_document_images(NSString *doc) { (void)doc; return @[]; }
static NSString *romfs_locate(void) { return have_romfs ? [fixture_doc stringByAppendingPathComponent:@"romfs"] : nil; }
static void romfs_identity_report(NSString *doc) { (void)doc; }
static void aoc_locate(void) {}
void dm_host_preload_start(int argc, char **argv) { (void)argc; (void)argv; }
static char *g_argv[4]; static int g_argc;
/* DEVICE_CHECK */
/* BOOT_ENV */
@interface CAMetalLayer : NSObject
@property(strong) NSDictionary *developerHUDProperties;
@end
@implementation CAMetalLayer
@end
@interface UIView : NSObject
@property NSRect bounds;
@property NSUInteger autoresizingMask;
@property(strong) CAMetalLayer *layer;
- (instancetype)initWithFrame:(NSRect)frame;
- (void)addSubview:(UIView *)view;
- (void)setNeedsLayout;
- (void)layoutIfNeeded;
@end
@implementation UIView
- (instancetype)initWithFrame:(NSRect)frame { if ((self = [super init])) { self.bounds = frame; self.layer = [CAMetalLayer new]; } return self; }
- (void)addSubview:(UIView *)view { (void)view; }
- (void)setNeedsLayout {}
- (void)layoutIfNeeded {}
@end
@interface DmGameView : UIView
@end
@implementation DmGameView
- (instancetype)initWithFrame:(NSRect)frame { if ((self = [super initWithFrame:frame])) { (void)MTLCreateSystemDefaultDevice(); views++; } return self; }
@end
@interface UIApplication : NSObject
@property BOOL idleTimerDisabled;
+ (instancetype)sharedApplication;
@end
@implementation UIApplication
+ (instancetype)sharedApplication { static UIApplication *a; if (!a) a = [self new]; return a; }
@end
#define UIViewAutoresizingFlexibleWidth 1
#define UIViewAutoresizingFlexibleHeight 2
#define LOG(...) ((void)0)
static CAMetalLayer *g_layer;
static void dm_cloud_saves_env(void) {}
static void dm_haptics_apple_init(void) {}
static UIView *dm_gamepad_view(NSRect frame) { return [[UIView alloc] initWithFrame:frame]; }
static UIView *front_ios_fps_label(void) { return nil; }
static void tv_attach(void) {}
static void front_ios_boot(void) { boots++; }
@interface DmRootVC : NSObject
@property(strong) UIView *view, *pad, *fps;
@property(strong) DmGameView *game;
- (void)showBootError;
- (void)startGame;
@end
@implementation DmRootVC
- (void)showBootError { errors++; }
/* START_GAME */
@end
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 6) return 2;
    fixture_doc = @(argv[1]); fixture_bundle = @(argv[2]); gpu_mode = atoi(argv[4]); have_romfs = atoi(argv[5]);
    phase = 1; int ok = boot_env(), before = devices;
    // This is the launcher's apply boundary: the production method runs after the chosen value is in the environment.
    if (strcmp(argv[3], "keep")) setenv("DM_METAL_HUD", argv[3], 1);
    phase = 2;
    if (ok) { DmRootVC *root = [DmRootVC new]; root.view = [[UIView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)]; [root startGame]; if (root.game) [root startGame]; }
    printf("%d %d %d %d %d %d %d\n", ok, before, first_phase, first_hud, views, boots, errors);
    const char *message = front_ios_device_message(); puts(message ? message : "none");
    return 0;
}}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class HudStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        main = (REPO / 'dm/ios/Sources/main.m').read_text()
        front = (REPO / 'dm/ios/Sources/front_ios.m').read_text()
        device = main[main.index('static char g_device_message'):main.index('static void romfs_identity_report')]
        boot = main[main.index('static int boot_env'):main.index('static int g_boot_ok')]
        start = front[front.index('- (void)startGame {'):front.index('- (void)viewDidLayoutSubviews')]
        source = root / 'startup.m'
        source.write_text(FIXTURE.replace('/* DEVICE_CHECK */', device).replace('/* BOOT_ENV */', boot).replace('/* START_GAME */', start))
        cls.bins = []
        for player in (0, 1):
            exe = root / f'startup-{player}'; cls.bins.append(exe)
            subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Werror', '-Wno-incompatible-pointer-types',
                            f'-DDM_PLAYER_BUILD={player}', '-I', str(REPO / 'dm/host'), '-I', str(REPO / 'dm/ios/Sources'),
                            str(source), '-framework', 'Foundation', '-o', str(exe)], check=True)

    def launch(self, choice='1', gpu=0, romfs=1, player=0, env=None, file=''):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t); doc = root / 'Documents'; app = root / 'App'; doc.mkdir(); (app / 'dm').mkdir(parents=True)
            (doc / 'env.txt').write_text(file)
            (app / 'dm/launcher.json').write_text(json.dumps({'settings': [{'env': 'DM_METAL_HUD'}]}))
            e = {k: v for k, v in os.environ.items() if not k.startswith(('DM_', 'MTL_HUD_'))}; e.update(env or {})
            r = subprocess.run([str(self.bins[player]), str(doc), str(app), choice, str(gpu), str(romfs)], env=e, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            lines = r.stdout.splitlines(); values = tuple(map(int, lines[0].split()))
            return values, lines[1], (doc / 'log.txt').read_text()

    def test_launcher_choice_precedes_first_device_and_host_starts_once(self):
        for player in (0, 1):
            for choice in ('0', '1'):
                with self.subTest(player=player, choice=choice):
                    values, message, _ = self.launch(choice=choice, player=player)
                    self.assertEqual(values, (1, 0, 2, int(choice), 1, 1, 0))
                    self.assertEqual(message, 'none')

    def test_explicit_metal_override_still_wins(self):
        values, _, _ = self.launch(env={'MTL_HUD_ENABLED': '0'})
        self.assertEqual(values, (1, 0, 2, 0, 1, 1, 0))

    def test_env_file_setting_is_preserved_for_regular_build(self):
        values, _, _ = self.launch(choice='keep', file='DM_METAL_HUD=1\n')
        self.assertEqual(values, (1, 0, 2, 1, 1, 1, 0))

    def test_player_build_ignores_old_hud_file_values(self):
        values, _, _ = self.launch(choice='0', player=1, file='DM_METAL_HUD=1\nMTL_HUD_ENABLED=1\n')
        self.assertEqual(values, (1, 0, 2, 0, 1, 1, 0))

    def test_unsupported_or_missing_gpu_never_starts_game(self):
        for gpu, name in ((1, 'synthetic GPU'), (2, 'no Metal device')):
            with self.subTest(gpu=gpu):
                values, message, log = self.launch(gpu=gpu)
                self.assertEqual(values, (1, 0, 2, 1, 0, 0, 1))
                self.assertIn(name, message); self.assertIn('cannot read BC-compressed textures', message)
                self.assertIn('[RED] boot:', log)

    def test_missing_game_files_do_not_create_a_device(self):
        values, message, _ = self.launch(romfs=0)
        self.assertEqual(values, (0, 0, 0, 0, 0, 0, 0)); self.assertEqual(message, 'none')


if __name__ == '__main__': unittest.main()
