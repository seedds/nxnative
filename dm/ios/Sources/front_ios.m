// iOS front end (the iOS implementation of front.h): a 16:9 Metal view + a full-screen virtual gamepad layer (DmGamepad.m). Where they go is DmLayout.h's: in landscape the picture
// is centered (black bars) and the controls are drawn over it; in portrait, or on an iPhone Duo half folded, the picture sits above the controls. With a TV connected the Metal view
// is on the TV and the phone shows the pad (DmTVSceneDelegate).
// The host runs on the big-stack thread started by main.m; front_run runs the guest directly on the calling thread.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFAudio/AVFAudio.h>
#include <errno.h>
#include "front.h"
#include "hle.h"
#include "dm.h"
#include "haptics_apple.h"
#include "save_sync.h"
#include "home_quit.h"
#import <GameController/GameController.h>
#import "CloudSaves.h"
#import "quit_prompt.h"

void dm_gamepad_active(void);
void dm_gamepad_pads_read(void);
static void quit_close(void);
#import "DmLayout.h"
#import "DmGameMenu.h"

void front_ios_boot(void);
int front_ios_boot_ok(void);
const char *front_ios_boot_message(void);   // main.m: why a build that keeps the game files off the IPA found none usable (NULL = the generic hint)
const char *front_ios_device_check(void);   // main.m: why this device cannot run the games (its GPU has no BC textures; NULL = supported); makes the first Metal device
bool dm_show_launcher(void *view_controller, void (*done)(void));   // Launcher.swift: false = no launcher (no dm/launcher.json, or DM_LAUNCHER=0)

static CAMetalLayer *g_layer;
static void front_ios_power_watch(void);
static void front_ios_audio_watch(void);
static UILabel *front_ios_fps_label(void);

@interface DmGameView : UIView
@property (nonatomic) CGFloat maxPixelWidth;   // 0: the screen's own pixels; a TV caps the drawable at 1920 across (tv_attach)
@end
@implementation DmGameView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        [self registerForTraitChanges:@[UITraitDisplayScale.class] withHandler:^(id<UITraitEnvironment> environment, UITraitCollection *previous) {
            [(UIView *)environment setNeedsLayout];
        }];
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.contentsScale = MAX(self.traitCollection.displayScale, 1); l.framebufferOnly = NO; l.opaque = YES;
        self.backgroundColor = UIColor.blackColor;
    }
    return self;
}
- (void)didMoveToWindow { [super didMoveToWindow]; [self setNeedsLayout]; }   // the phone's screen or a TV's (DmTVSceneDelegate)
- (void)layoutSubviews {   // render at the pixels of the screen the view is on (an iPad window can move to another), at most maxPixelWidth across
    [super layoutSubviews];
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    UIScreen *screen = self.window.windowScene.screen;
    CGFloat scale = screen ? screen.nativeScale : MAX(self.traitCollection.displayScale, 1);
    if (self.maxPixelWidth > 0) scale = MIN(scale, self.maxPixelWidth / MAX(self.bounds.size.width, 1));
    l.contentsScale = scale;
    l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale);
}
@end
static void tv_attach(void);

@interface DmRootView : UIView
@end
@implementation DmRootView
+ (BOOL)requiresConstraintBasedLayout { return YES; }
- (void)updateConstraints {
#if __has_include(<UIKit/UIHingeInteraction.h>)
    if (@available(iOS 27.1, *)) {
        // Reading here registers UIKit's automatic reserved-region invalidation.
        [self reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind options:UIViewReservedRegionQueryOptionsIncludeInactive];
        [self setNeedsLayout];
    }
#endif
    [super updateConstraints];
}
@end

@interface DmRootVC : UIViewController
@property (nonatomic, strong) DmGameView *game;
@property (nonatomic, strong) UIView *pad;
@property (nonatomic, strong) UILabel *fps;
@property (nonatomic) BOOL launched;
@property (nonatomic, strong) id tv;   // the TV scene accessory's registration (iOS 27)
@property (nonatomic) BOOL fill;   // DM_PICTURE_FIT=fill: in landscape the 16:9 picture covers the screen, its edges cropped (DmLayout.h dm_layout_picture)
@property (nonatomic) BOOL partiallyOpen;   // an iPhone Duo's hinge half open (UIHingeInteraction, iOS 27.1)
@property (nonatomic) NSInteger hingeStatus;
@property (nonatomic, copy) NSString *layoutLog;   // the last layout line logged
#if __has_include(<UIKit/UIHingeInteraction.h>)
- (void)hingeChanged:(UIHinge *)hinge API_AVAILABLE(ios(27.1));
#endif
- (void)startGame;
- (void)showFPS:(BOOL)on;
@end
static DmRootVC *g_root;
static void launcher_done(void) { [g_root startGame]; }
@implementation DmRootVC
- (void)loadView { self.view = [DmRootView new]; }
- (void)showHint:(NSString *)text {   // why the game cannot start, centered over the black screen
    UILabel *lb = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 40)];
    lb.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    lb.numberOfLines = 0; lb.textAlignment = NSTextAlignmentCenter; lb.textColor = UIColor.whiteColor;
    lb.text = text; [self.view addSubview:lb];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
#if __has_include(<UIKit/UIHingeInteraction.h>)
    if (@available(iOS 27.1, *)) {
        __weak DmRootVC *weakSelf = self;
        [self.view addInteraction:[[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction *interaction, UIHingeInteractionUpdate *update) {
            [weakSelf hingeChanged:update.hinge];
        }]];
    }
#endif
    front_ios_power_watch();
    if (!front_ios_boot_ok()) {   // No game files: show a hint and do not start the host
        const char *why = front_ios_boot_message();
        [self showHint:why ? [NSString stringWithFormat:@"Game files unusable.\n%s", why] : @"Game files not found.\nPut the RomFS folder into this App's Documents as \"romfs\" (Files app), then restart."]; return;
    }
    front_ios_audio_watch();
#if defined(__IPHONE_27_0)   // compiled only with an SDK that has the scene accessory API
    if (@available(iOS 27.0, *)) {   // iOS 27 offers a TV scene only to a registered scene accessory, no longer from the Info.plist entry alone (that path stays for iOS 26)
        UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"TV" sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
        c.delegateClass = NSClassFromString(@"DmTVSceneDelegate");
        self.tv = [self registerSceneAccessory:[UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:c]];
    }
#endif
}
#if __has_include(<UIKit/UIHingeInteraction.h>)
- (void)hingeChanged:(UIHinge *)hinge API_AVAILABLE(ios(27.1)) {
    NSInteger status = hinge ? hinge.status : UIHingeStatusUnknown;
    if (self.hingeStatus != status) {
        self.hingeStatus = status;
        LOG("front end: Duo hinge status %ld, angle %.3f", (long)status, hinge.angle);
    }
    self.partiallyOpen = status == UIHingeStatusPartiallyOpen;
    [self.view setNeedsUpdateConstraints];
    [self.view setNeedsLayout];
}
#endif
// The game view, the pad and the frame-rate counter are made once the launcher has set the switches they read (DM_SHOW_FPS, DM_METAL_HUD); the host's loading began at app launch (main.m)
- (void)startGame {
    if (self.game) return;
    dm_cloud_saves_env();   // DM_SAVE_SYNC from the launcher's Cloud saves pick, before the game's first save mount reads it (CloudSaves.m, save_sync.h)
    front_hud_env();   // before the process's first Metal device, which reads MTL_HUD_ENABLED (an env file's DM_METAL_HUD sets it in main.m already; GitHub issue #840)
    const char *dev = front_ios_device_check();   // that first device: a GPU without BC textures is refused here, not at launch, so the launcher's HUD switch counts
    if (dev) { [self showHint:[NSString stringWithFormat:@"This device is not supported.\n%s", dev]]; return; }   // the host does not start
    self.fill = dm_picture_fill(getenv("DM_PICTURE_FIT"));
    if (self.fill) LOG("front end: picture Fill (DM_PICTURE_FIT=fill), the 16:9 picture covers the screen in landscape and its edges are cropped");
    self.view.clipsToBounds = YES;   // with Fill the game view is larger than the screen
    self.game = [[DmGameView alloc] initWithFrame:self.view.bounds];
    [self.view addSubview:self.game];
    g_layer = (CAMetalLayer *)self.game.layer;
    if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) { g_layer.developerHUDProperties = @{@"mode": @"default"}; LOG("front end: Metal performance HUD on (DM_METAL_HUD)"); }   // Apple's HUD over the game: frame rate, frame and GPU time, memory
    dm_haptics_apple_init();
    self.pad = dm_gamepad_view(self.view.bounds);
    self.pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.pad];
    const char *fps = getenv("DM_SHOW_FPS");
    [self showFPS:fps ? atoi(fps) != 0 : [[NSBundle.mainBundle objectForInfoDictionaryKey:@"DMShowFPS"] boolValue]];
    [self.view setNeedsLayout]; [self.view layoutIfNeeded];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    tv_attach();   // a TV connected while the launcher showed
    front_ios_boot();
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds; if (!self.game) return;
    CGRect division = CGRectNull;   // an iPhone Duo's fold, as UIKit reports it (iOS 27.1)
    NSMutableArray<NSString *> *regionLog = [NSMutableArray array];
#if __has_include(<UIKit/UIHingeInteraction.h>)
    if (@available(iOS 27.1, *)) {
        for (UIViewReservedRegion *region in [self.view reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind options:UIViewReservedRegionQueryOptionsIncludeInactive]) {
            [regionLog addObject:[NSString stringWithFormat:@"%@ active=%d", NSStringFromCGRect(region.frame), region.isActive]];
            CGRect candidate = dm_horizontal_division(b, region.frame);
            if (CGRectIsNull(division) && !CGRectIsNull(candidate)) division = candidate;
        }
    }
#endif
    BOOL duoPortrait = b.size.height > b.size.width && (self.hingeStatus != 0 || !CGRectIsNull(division));
    UIEdgeInsets gameInsets = dm_game_safe_insets(b, self.view.safeAreaInsets, duoPortrait);
    BOOL onTV = self.game.superview != self.view;   // the TV's view lays the picture out; the phone is all pad
    DmLayout layout = onTV ? dm_layout_pad_only(b) : dm_layout_picture(dm_layout(b, gameInsets, self.partiallyOpen, division), b, self.fill);
    if (!onTV) self.game.frame = layout.game;
    layout.topSafeOnly = duoPortrait;
    NSString *line = [NSString stringWithFormat:@"%@, hinge %ld%s, divisions %@, picture %@, controls %@", onTV ? @"pad only (picture on the TV)" : layout.separated ? @"split" : self.fill ? @"overlay, Fill" : @"overlay",
                      (long)self.hingeStatus, self.partiallyOpen ? " (half open)" : "", regionLog, NSStringFromCGRect(layout.game), NSStringFromCGRect(layout.controls)];
    if (![line isEqualToString:self.layoutLog]) { self.layoutLog = line; LOG("front end: layout %s", line.UTF8String); }
    self.pad.frame = b;
    dm_gamepad_arrange(self.pad, layout);
    if (self.fps) {   // the top row's free stretch: right of DmGamepad.m's L3 key (102 + 17 of its units from the safe edge), level with − / + (22 units down); u is its unit
        CGRect s = CGRectIntersection(layout.controls, UIEdgeInsetsInsetRect(b, gameInsets));
        CGFloat u = layout.separated ? MIN(MIN(s.size.height / 430.0, s.size.width / 700.0), 1.3) : MIN(b.size.height / 430.0, 1.3);
        self.fps.frame = CGRectMake(floor(CGRectGetMinX(s) + 130 * u), floor(CGRectGetMinY(s) + 22 * u - 11), 64, 22);
    }
}
- (void)showFPS:(BOOL)on {   // the frame-rate counter: DM_SHOW_FPS at Play, then the in-game menu
    if (on && !self.fps) { self.fps = front_ios_fps_label(); [self.view addSubview:self.fps]; [self.view setNeedsLayout]; }   // above the pad, which hides while a controller is connected
    self.fps.hidden = !on;
}
- (void)viewDidAppear:(BOOL)a {
    [super viewDidAppear:a];
    if (!front_ios_boot_ok() && front_ios_boot_message() && !self.presentedViewController) {   // a slim build whose game file image is missing or another version (main.m)
        UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Game files" message:@(front_ios_boot_message()) preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:a animated:YES completion:nil];
    }
    if (self.launched || !front_ios_boot_ok()) return;
    self.launched = YES; g_root = self;
    if (!dm_show_launcher((__bridge void *)self, launcher_done)) [self startGame];   // the settings screen first (Launcher.swift), when the app has one
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }   // Edge gestures need two swipes, to avoid accidental exits
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskAll; }
@end
UIViewController *front_ios_root(void) { return [DmRootVC new]; }

// A TV (cable or AirPlay): iOS gives the app the TV as a scene of its own instead of mirroring the phone. The game's view moves there, 16:9 fills the TV, and the game is
// docked as a Switch on its dock (1080p, the operation mode change notification); the phone keeps the pad. Unplugging puts the view back and the mode the game had before
@interface DmTVVC : UIViewController
@end
@implementation DmTVVC
- (void)viewDidLoad { [super viewDidLoad]; self.view.backgroundColor = UIColor.blackColor; }
- (void)viewDidLayoutSubviews { [super viewDidLayoutSubviews]; for (UIView *v in self.view.subviews) v.frame = dm_aspect_fit(self.view.bounds); }
@end
static UIWindow *g_tv; static int g_tv_mode_was = -1;
static void tv_attach(void) {
    DmGameView *v = g_root.game; if (!g_tv || !v || v.superview == g_tv.rootViewController.view) return;
    // at most 1080p: the game renders 1080p docked, and a 4K drawable only adds an upscale and a 4K stream (an AirPlay TV showed black, the GPU went from 15 to 60 ms a frame).
    // Taken from the 16:9 picture, not the screen, so a display of another shape still gets the full 1920 pixels across the picture (DmGameView layoutSubviews applies it)
    v.maxPixelWidth = 1920;
    [g_tv.rootViewController.view addSubview:v]; [g_tv.rootViewController.view setNeedsLayout]; [g_root.view setNeedsLayout];   // the phone's pad takes the whole screen
    CGRect f = dm_aspect_fit(g_tv.bounds); CGFloat scale = MIN(g_tv.windowScene.screen.nativeScale, 1920.0 / MAX(f.size.width, 1));
    if (g_tv_mode_was < 0) g_tv_mode_was = dm_opmode();
    dm_set_opmode(1); dm_gamepad_opmode_changed();
    LOG("front end: TV connected (%.0fx%.0f points), the picture moves there at %.0fx%.0f, console mode", g_tv.bounds.size.width, g_tv.bounds.size.height, f.size.width * scale, f.size.height * scale);
}
static void tv_detach(void) {
    DmGameView *v = g_root.game; g_tv = nil; v.maxPixelWidth = 0;
    if (v && v.superview != g_root.view) { [g_root.view insertSubview:v atIndex:0]; [g_root.view setNeedsLayout]; }
    if (g_tv_mode_was >= 0) { dm_set_opmode(g_tv_mode_was); g_tv_mode_was = -1; dm_gamepad_opmode_changed(); }
    LOG("front end: TV disconnected, the picture is back on the phone");
}

// The settings the in-game menu offers (GameMenu.swift, from the gamepad's menu button), each taking effect while the game runs; the launcher's other settings need the next start.
// value NULL: only whether env is one of them; otherwise apply it (GameMenu.swift has saved it and put it in the environment). Console mode changes as on a Switch put in or taken out of its
// dock (the operation mode change notification); while a TV shows the picture the game stays docked, and the choice applies when the TV is unplugged
bool dm_setting_live(const char *env, const char *value) {
    if (!env || !g_root.game) return false;
    if (!strcmp(env, "DM_PICTURE_FIT")) {
        if (value) { g_root.fill = dm_picture_fill(value); [g_root.view setNeedsLayout]; LOG("front end: picture %s (in-game menu)", g_root.fill ? "Fill" : "Fit"); }
        return true;
    }
    if (!strcmp(env, "DM_OPMODE")) {
        if (!value) return true;
        int m = atoi(value) ? 1 : 0;
        if (g_tv_mode_was >= 0) { g_tv_mode_was = m; LOG("front end: console mode %s once the TV is unplugged (in-game menu)", m ? "docked" : "handheld"); return true; }
        dm_set_opmode(m); dm_gamepad_opmode_changed(); LOG("front end: console mode %s (in-game menu)", m ? "docked" : "handheld");
        return true;
    }
    if (!strcmp(env, "DM_SHOW_FPS")) { if (value) [g_root showFPS:atoi(value) != 0]; return true; }
    if (!strcmp(env, "DM_TOUCH_HAPTICS")) { if (value) dm_gamepad_touch_feedback(atoi(value) != 0); return true; }
    if (!strcmp(env, "DM_RUMBLE")) { if (value) { dm_haptics_set_enabled(atoi(value) != 0); LOG("front end: game rumble %s (in-game menu)", atoi(value) ? "on" : "off"); } return true; }
    return false;
}
@interface DmTVSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation DmTVSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [DmTVVC new];
    self.window.hidden = NO;   // without a window iOS goes on mirroring the phone; not key, so the phone's window keeps the keyboard
    g_tv = self.window; tv_attach();
}
- (void)sceneDidDisconnect:(UIScene *)s { if (g_tv == self.window) tv_detach(); self.window = nil; }
@end

// Thermal state and screen brightness for the power and present rows every 300 frames (hle_nvn.c; its weak defaults return -1 on the Mac). thermalState: 0 nominal, 1 fair, 2 serious, 3 critical.
// Brightness is read on the main thread at start and on each change; every change of the thermal state and of Low Power Mode (which lowers the CPU clock) is logged with the time since launch
static _Atomic double g_brightness = -1;
int dm_thermal_now(void) { return (int)NSProcessInfo.processInfo.thermalState; }
int dm_low_power_now(void) { return NSProcessInfo.processInfo.lowPowerModeEnabled; }   // the frame step does not probe in it (hle_nvn.c fstep_cpu_slowed)
double dm_brightness_now(void) { return g_brightness; }
static void front_ios_power_watch(void) {
    g_brightness = UIScreen.mainScreen.brightness;
    [NSNotificationCenter.defaultCenter addObserverForName:UIScreenBrightnessDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; g_brightness = UIScreen.mainScreen.brightness; }];
    [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        (void)n; LOG("front end: thermal state → %d (%.0f s after launch)", dm_thermal_now(), (now_ns() - g_t0_ns) / 1e9); }];
    [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
        (void)n; LOG("front end: Low Power Mode → %s (%.0f s after launch)", dm_low_power_now() ? "on" : "off", (now_ns() - g_t0_ns) / 1e9); }];
    LOG("front end: thermal state %d, brightness %.2f, Low Power Mode %s", dm_thermal_now(), g_brightness, dm_low_power_now() ? "on" : "off");
}

// Frame-rate counter: the frames put on screen in the last second (nvn_mtl.m counts them for NVN and for the Vulkan layer alike; frames the display link merged away are not counted).
// On when the IPA was built with it (the NSP script's SHOW_FPS / --show-fps → DMShowFPS in Info.plist); DM_SHOW_FPS=0 / 1 (the launcher, Documents/env.txt) overrides that without a rebuild,
// and the in-game menu turns it on and off ([DmRootVC showFPS:])
uint64_t mtl_present_count(void);
static UILabel *front_ios_fps_label(void) {
    UILabel *l = [UILabel new];
    l.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightSemibold]; l.textColor = UIColor.whiteColor; l.textAlignment = NSTextAlignmentCenter;
    l.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45]; l.layer.cornerRadius = 6; l.layer.masksToBounds = YES; l.userInteractionEnabled = NO;
    l.text = @"-- fps";
    __block uint64_t n0 = mtl_present_count(); __block double t0 = CACurrentMediaTime();
    [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
        (void)t; uint64_t n = mtl_present_count(); double now = CACurrentMediaTime();
        if (now - t0 < 1.5) l.text = [NSString stringWithFormat:@"%.0f fps", (n - n0) / (now - t0)];   // a longer span is the time in the background: no reading
        n0 = n; t0 = now;
    }];
    LOG("front end: frame-rate counter on");
    return l;
}

// Progress screen of the prepare step (front.h front_progress): a dark view over the game's view and the pad, the title, a bar and a count, centered; UIKit views, so it draws while the game's thread waits
static UIView *g_prog; static UILabel *g_prog_count; static UIProgressView *g_prog_bar; static UILabel *g_prog_title;
void (*front_progress_hook)(const char *title, long done, long total);
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
    void (*hook)(const char *, long, long) = front_progress_hook;
    if (hook) { dispatch_async(dispatch_get_main_queue(), ^{ hook(tt.UTF8String, done, total); }); return; }   // the host app draws it (front.h)
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!tt) { [g_prog removeFromSuperview]; g_prog = nil; return; }
        if (!g_prog) {
            UIView *root = g_root.view; if (!root) return;
            g_prog = [[UIView alloc] initWithFrame:root.bounds]; g_prog.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            g_prog.backgroundColor = [UIColor colorWithWhite:0.06 alpha:0.94];
            UIView *box = [[UIView alloc] initWithFrame:CGRectMake(floor((root.bounds.size.width - 320) / 2), floor((root.bounds.size.height - 96) / 2), 320, 96)];
            box.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
            g_prog_title = [[UILabel alloc] initWithFrame:CGRectMake(0, 4, 320, 30)]; g_prog_title.textAlignment = NSTextAlignmentCenter; g_prog_title.textColor = UIColor.whiteColor;
            g_prog_title.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
            g_prog_bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault]; g_prog_bar.frame = CGRectMake(20, 48, 280, 4);
            g_prog_count = [[UILabel alloc] initWithFrame:CGRectMake(0, 64, 320, 22)]; g_prog_count.textAlignment = NSTextAlignmentCenter;
            g_prog_count.textColor = [UIColor colorWithWhite:0.72 alpha:1]; g_prog_count.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
            [box addSubview:g_prog_title]; [box addSubview:g_prog_bar]; [box addSubview:g_prog_count]; [g_prog addSubview:box];
            [root addSubview:g_prog];
        }
        g_prog_title.text = tt; g_prog_bar.progress = total > 0 ? (float)done / (float)total : 0;
        g_prog_count.text = [NSString stringWithFormat:@"%@ of %@", [NSNumberFormatter localizedStringFromNumber:@(done) numberStyle:NSNumberFormatterDecimalStyle],
                             [NSNumberFormatter localizedStringFromNumber:@(total) numberStyle:NSNumberFormatterDecimalStyle]];
    });
}
void *front_metal_layer(void) { return (__bridge void *)g_layer; }
void front_run(void (*guest)(void)) { guest(); }   // Already on main.m's big-stack thread

// Software keyboard: show a UIAlertController on the main thread, the calling thread waits for the result
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)max_chars;
    if (!g_layer) return -1;
    __block int r = 0; __block NSString *text = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    NSString *t = title && *title ? @(title) : @"", *m = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
    NSString *init = initial ? @(initial) : @"", *ph = guide ? @(guide) : @"";
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *ac = [UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
        [ac addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = init; f.placeholder = ph; }];
        [ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; r = 1; text = ac.textFields.firstObject.text; dispatch_semaphore_signal(done); }]];
        [ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; dispatch_semaphore_signal(done); }]];
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow) vc = w.rootViewController;
        if (!vc) { dispatch_semaphore_signal(done); return; }
        [vc presentViewController:ac animated:YES completion:nil];
    });
    // The guest clock keeps running meanwhile, as on the Switch (hle_swkbd.c): a freeze here as well stopped Octopath Traveler 0's frame loop for as long as the box was up and made its sound stutter
    // (4,977 ms and 2,577 ms frames, 5,181 wakeups a second). The inline keyboard freezes around its own call (hle_swkbd.c)
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (!r) return 0;
    snprintf(out, out_cap, "%s", text.UTF8String ? text.UTF8String : "");
    return 1;
}

// Save write failed (most likely the phone's storage is full): show a notice on the main thread without blocking the game. hle_fs.c calls this when a save transaction is refused or a mount has to fall back to read-only;
// the iOS front end used not to implement it, so there was only a log line and the player never learned that progress was not being saved
void front_save_failed(int err) {
    LOG("front end: save data write failed errno=%d, telling the player", err);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow) vc = w.rootViewController;
        while (vc.presentedViewController) vc = vc.presentedViewController;
        if (!vc) { LOG("[RED] front end: no window for the save-failure alert errno=%d", err); return; }
        NSString *m = err == ENOSPC ? @"Storage is full, so progress was not saved. Free up space and save again."
                                    : [NSString stringWithFormat:@"Saving failed (errno %d); progress was not saved.", err];
        UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Save failed" message:m preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:nil];
    });
}

// Audio session: the frames RemoteIO asks for in each callback follow the I/O buffer duration, about 21 ms by default on iOS (1024 frames at 48 kHz). The game keeps only 4 blocks × 5 ms = 20 ms in flight,
// so every callback came up 1.3 ms short and was zero-filled, about 47 gaps per second (crackling; reproducible on the Mac by forcing 1024-frame callbacks: 1999 underruns in 2000 callbacks). Ask for 5 ms and the system picks the nearest it supports;
// the category is Playback, as a game's or media app's: the ring / silent switch does not mute it (the default SoloAmbient follows the switch, which silenced the game whenever the phone was on silent) and it still interrupts other apps' audio.
// Set once at boot and again on return to the foreground (the system may reset it after an interruption)
static int g_session_off;   // the last activation failed (another app or a call still held the session): tried again when the app becomes active
void front_ios_audio_session(void) {
    AVAudioSession *s = AVAudioSession.sharedInstance; NSError *e = nil;
    if (![s setCategory:AVAudioSessionCategoryPlayback error:&e]) LOG("[RED] audio session category could not be set: %s", e.localizedDescription.UTF8String);
    [s setPreferredSampleRate:48000 error:nil];
    if (![s setPreferredIOBufferDuration:0.005 error:&e]) LOG("[RED] audio session I/O buffer duration could not be set: %s", e.localizedDescription.UTF8String);
    g_session_off = ![s setActive:YES error:&e];
    if (g_session_off) LOG("[RED] audio session activation failed: %s", e.localizedDescription.UTF8String);
    LOG("audio session: sample rate %.0f Hz, I/O buffer %.2f ms (asked for 5 ms)", s.sampleRate, s.IOBufferDuration * 1000);
}

// Real audio output is suspended (the silent sink paces the game) while the app is in the background or another audio session has interrupted ours (Siri, an alarm, a call).
// Without the interruption observer the AudioUnits stayed "started" while iOS no longer called them, and a game waiting for its buffers back lost its sound until restart.
// All of this runs on the main queue, like the scene callbacks
static int g_audio_bg, g_audio_intr;
static void front_ios_audio_apply(void) { dm_audio_suspend(g_audio_bg || g_audio_intr); }
static void front_ios_audio_watch(void) {
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue], reason = [n.userInfo[AVAudioSessionInterruptionReasonKey] unsignedIntegerValue];
        if (type == AVAudioSessionInterruptionTypeBegan) { LOG("audio session: interrupted (reason %lu)", (unsigned long)reason); g_audio_intr = 1; }
        else {
            LOG("audio session: interruption ended"); g_audio_intr = 0;
            if (!g_audio_bg) front_ios_audio_session();
        }
        front_ios_audio_apply();
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; LOG("audio session: the media services were reset; setting the session and the outputs up again");
        front_ios_audio_session(); dm_audio_reset();
    }];
}

// Scene lifecycle (called by SceneDelegate in main.m): 0 resign active, 1 enter background, 2 return to foreground, 3 become active. On entering background, freeze the guest clock and suspend after in-flight GPU work completes (submitting GPU work in background gets the process killed).
// The screen stays on while the game is active: only touches reset the idle timer, so with a controller (or during a cutscene) the screen dimmed and locked mid-game
// Cloud saves (save_sync.h): what the game committed goes out before iOS suspends the app, without the sender's quiet wait. iOS gives a background task about half a minute
static void save_sync_background(void) {
    UIApplication *app = UIApplication.sharedApplication;
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    void (^end)(void) = ^{ if (task != UIBackgroundTaskInvalid) { [app endBackgroundTask:task]; task = UIBackgroundTaskInvalid; } };   // on the main queue, as the expiry handler runs
    task = [app beginBackgroundTaskWithName:@"cloud save" expirationHandler:end];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        if (!save_sync_flush(20000)) LOG("front end: the save did not reach the cloud before the app was suspended; it goes after the next save or at the next start");
        dispatch_async(dispatch_get_main_queue(), end);
    });
}

void front_ios_focus_event(int ev) {
    if (!g_layer) return;
    switch (ev) {
        case 0: quit_close(); dm_gamepad_reset(); dm_focus_set(2); break;
        case 1: UIApplication.sharedApplication.idleTimerDisabled = NO; dm_focus_set(3); g_audio_bg = 1; front_ios_audio_apply(); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); save_sync_background(); break;
        case 2: dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); front_ios_audio_session(); g_audio_bg = 0; front_ios_audio_apply(); dm_focus_set(2); break;
        case 3:   // Becoming active again also ends an interruption whose "ended" never came (not every one sends it), and retries a session that would not activate on the way back
            UIApplication.sharedApplication.idleTimerDisabled = YES;
            if (g_audio_intr) { LOG("audio session: active again with no interruption-ended notice; resuming"); g_audio_intr = 0; g_session_off = 1; }
            if (g_session_off) front_ios_audio_session();
            front_ios_audio_apply(); dm_gamepad_active(); dm_focus_set(1); break;
    }
}

// ---- Quit to the launcher (LNPANDY/NX2Apple#421) ----
// Two ways open the quit prompt (quit_prompt.h) over the game: Quit at the end of the in-game menu (the gear at the top centre, GameMenu.swift, which stays while
// a controller hides the other touch controls), and holding a controller's Home button for 2 seconds (home_quit.h, as on the Mac; the indicator at the top fills
// while it is held). A tap on Quit or A quits, a tap on Cancel or outside the card, B or a new press of Home closes it. While it is open the game is told it lost
// the focus, as the Switch's HOME menu does, and gets no touch or controller input.
// Quitting ends the process: one game runs per process (dm_profile() and the host's early loading run once, the shell's ShellMain.m), so the engine cannot hand
// the window back to a launcher and start another game. It goes the way of a SIGTERM (dm_host_shutdown: audio and graphics stopped, in-flight GPU work drained,
// _exit(0)), after the cloud saves have gone out as they do when the app goes to the background. What the game saved is on disk already (hle_fs.c writes each
// commit), progress it did not save is lost, as the prompt says. iOS has no way for an app to start itself again: the player opens it from the Home Screen.
// DM_HOME_QUIT=0 turns the Home hold off. DM_HOME_QUIT_TEXT carries the texts in the player's language, one per line (home_quit.h's order), English otherwise.
@interface DmQuitView : UIView
@property (nonatomic) qp_layout layout;
@property (nonatomic, strong) CALayer *prompt, *actions;
@property (nonatomic) int down;          // what the touch went down on (QP_HIT_*); a tap counts when it is released on the same thing
@property (nonatomic) double opened;     // a touch in the first moments after the prompt opened is not taken: a second tap meant for the menu or the game
@end
static home_quit g_hq; static int g_hq_on = -1, g_hq_busy;
static char g_hq_text[HQ_TEXTS][256];
static DmQuitView *g_hq_view; static CALayer *g_hq_hold; static CAShapeLayer *g_hq_ring; static NSTimer *g_hq_timer;
static void quit_changed(hq_phase was, hq_phase now, GCController *holder);
static void quit_touch(int hit);
void front_ios_quit_poll(void);

@implementation DmQuitView
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    self.down = CACurrentMediaTime() - self.opened < 0.25 ? -1 : qp_hit(&_layout, [ts.anyObject locationInView:self]);
    [self press:self.down == QP_HIT_QUIT ? 0 : self.down == QP_HIT_CANCEL ? 1 : -1];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    int hit = qp_hit(&_layout, [ts.anyObject locationInView:self]), down = self.down;
    [self press:-1]; self.down = -1;
    if (down >= 0 && hit == down && hit != QP_HIT_NONE) quit_touch(hit);
}
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self press:-1]; self.down = -1; }
- (void)press:(int)i {   // a pill held down dims a little, as a button does
    NSArray<CALayer *> *pills = self.actions.sublayers;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (NSUInteger k = 0; k < pills.count; k++) pills[k].opacity = (int)k == i ? 0.7 : 1;
    [CATransaction commit];
}
- (BOOL)isAccessibilityElement { return NO; }
@end

static NSString *quit_text(int i) { return @(g_hq_text[i]); }
static void quit_start(void) {
    if (g_hq_on >= 0) return;
    g_hq_on = hq_enabled(getenv("DM_HOME_QUIT"));
    char title[256]; snprintf(title, sizeof title, "Quit %s?", DM_GAME_NAME);
    const char *def[HQ_TEXTS] = {"Keep holding to quit", title, "Unsaved progress will be lost. The app closes.", "Quit", "Cancel"};
    const char *text = getenv("DM_HOME_QUIT_TEXT");   // the launcher's, in the player's language: one line per text, in home_quit.h's order
    for (int i = 0; i < HQ_TEXTS; i++) hq_text_line(text, i, def[i], g_hq_text[i], sizeof g_hq_text[i]);
    LOG("front end: hold the Home button to quit %s", g_hq_on ? "is on" : "is off (DM_HOME_QUIT=0)");
}
static void quit_buttons(int *home, int *a, int *b, GCController *__strong *holder) {
    *home = *a = *b = 0;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad; if (!g) continue;
        if (g.buttonHome.isPressed) { *home = 1; if (!*holder) *holder = c; }
        *a |= g.buttonA.isPressed; *b |= g.buttonB.isPressed;
    }
}
static void quit_hide_hold(void) { [g_hq_timer invalidate]; g_hq_timer = nil; [g_hq_hold removeFromSuperlayer]; g_hq_hold = nil; g_hq_ring = nil; }
static void quit_hide_prompt(void) {
    DmQuitView *v = g_hq_view; g_hq_view = nil; if (!v) return;
    [UIView animateWithDuration:0.18 animations:^{ v.alpha = 0; } completion:^(BOOL done) { (void)done; [v removeFromSuperview]; }];
    UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, nil);
}
static UIImage *quit_glyph(NSString *symbol, NSString *fallback) {
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:48 weight:UIImageSymbolWeightMedium];
    return [UIImage systemImageNamed:symbol ?: fallback withConfiguration:cfg] ?: [UIImage systemImageNamed:fallback withConfiguration:cfg];
}
static void quit_show_prompt(GCController *holder) {
    UIView *root = g_root.view; if (!root) return;
    GCController *pad = holder; if (!pad) for (GCController *c in GCController.controllers) if (c.extendedGamepad) { pad = c; break; }
    GCExtendedGamepad *g = pad.extendedGamepad;   // with a controller the pills show its own A and B, with touch alone the words only
    DmQuitView *v = [[DmQuitView alloc] initWithFrame:root.bounds];
    v.multipleTouchEnabled = NO; v.down = -1; v.opened = CACurrentMediaTime();
    v.layout = qp_layout_make(root.bounds.size, quit_text(HQ_TEXT_TITLE), quit_text(HQ_TEXT_NOTE), quit_text(HQ_TEXT_QUIT), quit_text(HQ_TEXT_CANCEL), g != nil);
    UIImage *ga = g ? quit_glyph(g.buttonA.sfSymbolsName, @"a.circle") : nil, *gb = g ? quit_glyph(g.buttonB.sfSymbolsName, @"b.circle") : nil;
    CALayer *actions = nil; qp_layout l = v.layout;
    v.prompt = qp_prompt_layer(root.bounds, &l, quit_text(HQ_TEXT_TITLE), quit_text(HQ_TEXT_NOTE), quit_text(HQ_TEXT_QUIT), quit_text(HQ_TEXT_CANCEL),
                               ga.CGImage, gb.CGImage, MAX(root.traitCollection.displayScale, 1), &actions);
    v.actions = actions;
    [v.layer addSublayer:v.prompt];
    // VoiceOver reads the title and the note and offers the two actions
    UIAccessibilityElement *(^element)(NSString *, CGRect, UIAccessibilityTraits) = ^(NSString *label, CGRect frame, UIAccessibilityTraits traits) {
        UIAccessibilityElement *x = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:v];
        x.accessibilityLabel = label; x.accessibilityFrameInContainerSpace = frame; x.accessibilityTraits = traits; return x;
    };
    v.accessibilityElements = @[element([NSString stringWithFormat:@"%@ %@", quit_text(HQ_TEXT_TITLE), quit_text(HQ_TEXT_NOTE)], l.card, UIAccessibilityTraitHeader),
                                element(quit_text(HQ_TEXT_QUIT), l.quit, UIAccessibilityTraitButton), element(quit_text(HQ_TEXT_CANCEL), l.cancel, UIAccessibilityTraitButton)];
    v.alpha = 0; [root addSubview:v];
    [UIView animateWithDuration:0.18 animations:^{ v.alpha = 1; }];
    g_hq_view = v;
    UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, v.accessibilityElements.firstObject);
}
// The prompt's quit: the cloud saves go out (as when the app goes to the background), then the host ends as on a SIGTERM. Off the main queue: both wait
static void quit_now(void) {
    LOG("front end: quit from the quit prompt, the app closes (one game per process)");
    g_hq_view.userInteractionEnabled = NO;
    [CATransaction begin]; [CATransaction setAnimationDuration:0.2]; g_hq_view.actions.opacity = 0.4; [CATransaction commit];
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (!save_sync_flush(8000)) LOG("front end: the save did not reach the cloud before quitting, it goes at the next start");
        dm_host_shutdown("the quit prompt");
    });
}
static void quit_changed(hq_phase was, hq_phase now, GCController *holder) {
    if (was == HQ_HOLDING) quit_hide_hold();
    switch (now) {
    case HQ_HOLDING:
        g_hq_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; front_ios_quit_poll(); }];
        break;
    case HQ_PROMPT:
        dm_gamepad_reset();   // nothing the touch gamepad or a controller held stays pressed while the prompt is open
        dm_focus_set(2);
        quit_show_prompt(holder);
        LOG("front end: quit prompt open (%s)", holder ? "Home held" : "the in-game menu");
        break;
    case HQ_SETTLE:
        quit_hide_prompt(); dm_focus_set(1);
        LOG("front end: quit prompt closed, the game goes on");
        break;
    case HQ_IDLE:
        if (was == HQ_SETTLE) dm_gamepad_pads_read();   // the game reads the controllers again, as they are now
        break;
    case HQ_QUIT:
        quit_now();
        break;
    }
}
void front_ios_quit_poll(void) {   // DmGamepad.m on a controller's change, and the hold's timer (main queue)
    quit_start();
    if (!g_hq_on || !g_root.game || g_hq_busy) return;
    g_hq_busy = 1;
    int home, a, b; GCController *holder = nil; quit_buttons(&home, &a, &b, &holder);
    double now = CACurrentMediaTime();
    hq_phase was = g_hq.phase, ph = hq_step(&g_hq, now, home, a, b);
    if (ph != was) quit_changed(was, ph, holder);
    double fill = hq_ring(&g_hq, now);
    if (fill >= 0) {
        if (!g_hq_hold) {
            CAShapeLayer *ring = nil; UIView *root = g_root.view;
            g_hq_hold = qp_hold_layer(root.bounds, quit_text(HQ_TEXT_HOLD), MAX(root.traitCollection.displayScale, 1), &ring); g_hq_ring = ring;
            [root.layer addSublayer:g_hq_hold];
        }
        qp_hold_set(g_hq_ring, fill);
    }
    g_hq_busy = 0;
}
static void quit_open(void) {   // the in-game menu's Quit
    quit_start();
    if (!g_root.game) return;
    int home, a, b; GCController *holder = nil; quit_buttons(&home, &a, &b, &holder);
    hq_phase was = g_hq.phase;
    if (hq_touch_open(&g_hq, home, a, b) != was) quit_changed(was, HQ_PROMPT, nil);
}
static void quit_touch(int hit) {   // DmQuitView: a tap on Quit, on Cancel or outside the card
    hq_phase was = g_hq.phase, now = hit == QP_HIT_QUIT ? hq_touch_quit(&g_hq) : hq_touch_cancel(&g_hq);
    if (now == was) return;
    quit_changed(was, now, nil);
    if (now == HQ_SETTLE) front_ios_quit_poll();   // with every button up the game has its input again at once
}
int front_ios_quit_blocks_input(void) { return hq_blocks_input(g_hq.phase); }
static void quit_close(void) {   // the app is leaving the foreground: whatever is shown closes, a Home still held needs a new press
    if (g_hq.phase == HQ_IDLE || g_hq.phase == HQ_QUIT) return;
    if (g_hq.phase == HQ_PROMPT || g_hq.phase == HQ_SETTLE) LOG("front end: quit prompt closed, the app left the foreground");
    hq_cancel(&g_hq); quit_hide_hold(); quit_hide_prompt();
}
// The in-game menu's last item: "Quit <game>" (the prompt's title without its question mark), destructive, opening the prompt. A retained UIMenu (DmGameMenu.h)
void *dm_quit_menu(void) {
    quit_start();
    NSString *title = [quit_text(HQ_TEXT_TITLE) stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"?？  "]];
    UIAction *quit = [UIAction actionWithTitle:title image:[UIImage systemImageNamed:@"xmark.circle"] identifier:nil handler:^(UIAction *a) { (void)a; quit_open(); }];
    quit.attributes = UIMenuElementAttributesDestructive;
    return (void *)CFBridgingRetain([UIMenu menuWithTitle:@"" image:nil identifier:nil options:UIMenuOptionsDisplayInline children:@[quit]]);
}
// The Home button setting for apps, once, when the first controller is there (GameController reads it only while one is connected): the Home hold works when
// the app decides ("app"). Either way the in-game menu's Quit stays, as the gear stays with a controller
void front_ios_quit_controllers(void) {
    quit_start();
#if defined(__IPHONE_27_0)
    if (@available(iOS 27.0, *)) {
        static int done; if (done || !g_hq_on) return;
        int any = 0; for (GCController *c in GCController.controllers) if (c.extendedGamepad) any = 1;
        if (!any) return;
        done = 1;
        GCControllerHomeButtonSettingCustomizationStatus by = GCControllerHomeButtonSettingCustomizationDefault; NSError *err = nil;
        GCControllerHomeButtonSettingInAppAction v = [[GCControllerHomeButtonSettingsManager new] readControllerHomeButtonInAppAction:&by withError:&err];
        LOG("front end: Home button in apps: %s (%s)%s%s", hq_in_app_name((long)v),
            by == GCControllerHomeButtonSettingCustomizationUser ? "set by the player" : "the system's default", err ? ", " : "", err ? err.localizedDescription.UTF8String : "");
        return;
    }
#endif
    static int said; if (!said++) LOG("front end: the Home button setting is read on iOS 27, the Home hold works where the system lets the press through");
}
