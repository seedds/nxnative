// Native iOS probe of the production rendering view and scene callbacks. The
// host endpoints below are inert: no game files, motion or controller discovery.
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#include <stdatomic.h>
#include <time.h>
#include "../Sources/front_ios.m"
#define main unused_production_main
#include "../Sources/main.m"
#undef main

uint64_t g_t0_ns;
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
int dm_main(int argc, char **argv) { (void)argc; (void)argv; return 0; }
void dm_host_preload_start(int argc, char **argv) { (void)argc; (void)argv; }
void dm_haptics_apple_init(void) {}
UIView *dm_gamepad_view(CGRect frame) { return [[UIView alloc] initWithFrame:frame]; }
void dm_gamepad_reset(void) {}
bool dm_show_launcher(void *view, void (*done)(void)) { (void)view; (void)done; return false; }
uint64_t mtl_present_count(void) { return 0; }
void dm_audio_suspend(int on) { (void)on; }
void dm_audio_reset(void) {}
void dm_focus_set(int state) { (void)state; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
void dm_gfx_suspend(int on) { (void)on; }

static NSMutableArray *checks;
static void check(NSString *name, BOOL passed) {
    [checks addObject:@{@"name":name, @"passed":@(passed)}];
    fprintf(stderr, "[display] %s: %s\n", name.UTF8String, passed ? "PASS" : "FAIL");
}
static void checkSize(DmRootVC *root, CGSize size) {
    root.view.bounds = (CGRect){CGPointZero, size};
    [root viewDidLayoutSubviews];
    [root.game setNeedsLayout]; [root.game layoutIfNeeded];
    CGRect frame = root.game.frame;
    DmDisplayMetrics value = display_snapshot();
    CAMetalLayer *layer = (CAMetalLayer *)root.game.layer;
    NSString *prefix = [NSString stringWithFormat:@"%.0fx%.0f", size.width, size.height];
    check([prefix stringByAppendingString:@" preserves 16:9"], fabs(frame.size.width / frame.size.height - 16.0/9.0) < 0.00001);
    check([prefix stringByAppendingString:@" retains the complete picture"], CGRectContainsRect(root.view.bounds, frame));
    check([prefix stringByAppendingString:@" uses the rendering scene scale"], value.scale == root.game.window.windowScene.screen.nativeScale);
    check([prefix stringByAppendingString:@" publishes the actual drawable size"], value.width == layer.drawableSize.width && value.height == layer.drawableSize.height);
    check([prefix stringByAppendingString:@" converts points to physical pixels"], fabs(value.width - frame.size.width * value.scale) < 0.001 && fabs(value.height - frame.size.height * value.scale) < 0.001);
}

@interface MetricsScene : SceneDelegate
@end
@implementation MetricsScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    g_boot_ok = 0;
    [super scene:scene willConnectToSession:session options:options];
    DmRootVC *root = (DmRootVC *)self.window.rootViewController;
    root.launched = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        checks = [NSMutableArray array];
        root.game = [[DmGameView alloc] initWithFrame:root.view.bounds];
        [root.view addSubview:root.game];
        g_layer = (CAMetalLayer *)root.game.layer; g_root = root;
        CGSize original = root.view.bounds.size;
        for (NSValue *size in @[[NSValue valueWithCGSize:CGSizeMake(390,844)],
                               [NSValue valueWithCGSize:CGSizeMake(844,390)],
                               [NSValue valueWithCGSize:CGSizeMake(1024,768)]])
            checkSize(root, size.CGSizeValue);
        checkSize(root, original);
        UIWindowScene *windowScene = (UIWindowScene *)scene;
        [self windowScene:windowScene didUpdateEffectiveGeometry:windowScene.effectiveGeometry];
        DmDisplayMetrics metrics = display_snapshot();
        check(@"scene refresh capacity reaches the any-thread bridge", front_screen_max_fps() == windowScene.screen.maximumFramesPerSecond);
        check(@"headroom snapshots contain valid ratios", isfinite(metrics.headroom) && metrics.headroom >= 1 && isfinite(metrics.potential_headroom) && metrics.potential_headroom >= 1);
        check(@"headroom usage follows the rendering view trait", metrics.headroom_active == (root.game.traitCollection.hdrHeadroomUsageLimit == UIHDRHeadroomUsageLimitActive));
        CAMetalLayer *layer = (CAMetalLayer *)root.game.layer;
        check(@"the output format stays SDR", layer.pixelFormat == MTLPixelFormatBGRA8Unorm);
        check(@"the default dynamic range stays standard", [layer.preferredDynamicRange isEqual:CADynamicRangeStandard]);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        check(@"the real scene produces a drawable", drawable != nil);
        check(@"drawable pixels match the snapshot", drawable && fabs((double)drawable.texture.width - metrics.width) < 1 && fabs((double)drawable.texture.height - metrics.height) < 1);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            int fps = front_screen_max_fps();
            dispatch_async(dispatch_get_main_queue(), ^{
                check(@"a worker reads the published refresh without UIKit", fps == metrics.max_fps);
                NSInteger failed = 0; for (NSDictionary *item in checks) if (![item[@"passed"] boolValue]) failed++;
                NSDictionary *report = @{@"checks":checks, @"failed":@(failed), @"externalDisplayTested":@NO,
                    @"EDROutputEnabled":@NO, @"scale":@(metrics.scale), @"width":@(metrics.width), @"height":@(metrics.height),
                    @"maximumFPS":@(metrics.max_fps), @"currentEDRHeadroom":@(metrics.headroom), @"potentialEDRHeadroom":@(metrics.potential_headroom),
                    @"systemVersion":UIDevice.currentDevice.systemVersion};
                NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
                [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:NULL]
                    writeToURL:[documents URLByAppendingPathComponent:@"results.json"] options:NSDataWritingAtomic error:NULL];
            });
        });
    });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        freopen([[documents URLByAppendingPathComponent:@"display.log"] fileSystemRepresentation], "w", stderr);
        setvbuf(stderr, NULL, _IOLBF, 0);
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class));
    }
}
