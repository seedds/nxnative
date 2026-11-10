// A small iOS app that checks the screen rate the front end reports (front.h front_screen_max_fps) on the production game view and scene delegate
// (front_ios.m and main.m, included below). The host's entry points they call are empty here: no game files, no host, no controllers.
// tools/build_screen_rate_check.py builds it; it writes its lines to Documents/screen_rate.log and shows the count on screen.
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <time.h>
#include "../Sources/front_ios.m"
#define main production_main
#include "../Sources/main.m"
#undef main

uint64_t g_t0_ns;
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
int dm_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
void dm_host_preload_start(int argc, char **argv) { (void)argc; (void)argv; }
void dm_haptics_apple_init(void) {}
UIView *dm_gamepad_view(CGRect frame) { return [[UIView alloc] initWithFrame:frame]; }
void dm_gamepad_reset(void) {}
void dm_gamepad_active(void) {}
bool dm_show_launcher(void *view, void (*done)(void)) { (void)view; (void)done; return false; }
uint64_t mtl_present_count(void) { return 0; }
void dm_audio_suspend(int on) { (void)on; }
void dm_audio_reset(void) {}
void dm_focus_set(int state) { (void)state; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
void dm_gfx_suspend(int on) { (void)on; }
void dm_cloud_saves_env(void) {}
void dm_gamepad_arrange(UIView *view, DmLayout layout) { (void)view; (void)layout; }
void dm_gamepad_opmode_changed(void) {}
void dm_gamepad_touch_feedback(bool on) { (void)on; }
void dm_haptics_set_enabled(int on) { (void)on; }
int dm_opmode(void) { return 0; }
void dm_set_opmode(int m) { (void)m; }
int save_sync_flush(int timeout_ms) { (void)timeout_ms; return 1; }

static int g_checks, g_failed;
static void check(const char *name, BOOL ok) { g_checks++; g_failed += !ok; fprintf(stderr, "%s: %s\n", ok ? "ok" : "FAILED", name); }

@interface RateScene : SceneDelegate
@end
@implementation RateScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    g_boot_ok = 0;   // the root view controller then starts no host (DmRootVC viewDidLoad)
    [super scene:scene willConnectToSession:session options:options];
    DmRootVC *root = (DmRootVC *)self.window.rootViewController;
    for (UIView *v in root.view.subviews.copy) [v removeFromSuperview];   // the "Game files not found" hint
    UILabel *text = [[UILabel alloc] initWithFrame:root.view.bounds];
    text.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    text.numberOfLines = 0; text.textAlignment = NSTextAlignmentCenter; text.textColor = UIColor.whiteColor;
    text.text = @"Screen rate check\nrunning";
    [root.view addSubview:text];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        UIWindowScene *ws = (UIWindowScene *)scene; int hz = (int)ws.screen.maximumFramesPerSecond;
        check("no rate before the game's view is in a window", front_screen_max_fps() == 0);
        root.game = [[DmGameView alloc] initWithFrame:root.view.bounds]; g_root = root;
        [root.view insertSubview:root.game atIndex:0];
        check("the view in the window: the rate of its scene's screen", hz > 0 && front_screen_max_fps() == hz);
        g_screen_fps = 1; [root.game removeFromSuperview];
        check("the view off every window: the last rate stays", front_screen_max_fps() == 1);
        [root.view insertSubview:root.game atIndex:0];   // as the view's move to a TV's window and back (tv_attach, tv_detach)
        check("the view in a window again: the rate is read again", front_screen_max_fps() == hz);
        g_screen_fps = 1; [self windowScene:ws didUpdateEffectiveGeometry:ws.effectiveGeometry];
        check("the scene's geometry changed: the rate is read again", front_screen_max_fps() == hz);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int there = front_screen_max_fps();
            dispatch_async(dispatch_get_main_queue(), ^{
                check("another thread reads the same rate", there == hz);
                fprintf(stderr, "screen rate %d Hz: %d checks, %d failed\n", hz, g_checks, g_failed);
                text.text = [NSString stringWithFormat:@"Screen rate check\n%d Hz: %d checks, %d failed", hz, g_checks, g_failed];
                [root.view bringSubviewToFront:text];
            });
        });
    });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        NSURL *docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        freopen([docs URLByAppendingPathComponent:@"screen_rate.log"].fileSystemRepresentation, "w", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class));
    }
}
