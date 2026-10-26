// iOS front end (the iOS implementation of front.h): a centered 16:9 Metal view (black bars on both sides) + a full-screen virtual gamepad layer (DmGamepad.m, controls drawn over the picture).
// The host runs on the big-stack thread started by main.m; front_run runs the guest directly on the calling thread.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <Metal/Metal.h>
#import <AVFAudio/AVFAudio.h>
#include <errno.h>
#include "front.h"
#include "hle.h"
#include "dm.h"
#include "DmAudioRetry.h"
#include "DmDisplayRefresh.h"

UIView *dm_gamepad_view(CGRect frame);
void dm_gamepad_set_active(int active);
void front_ios_boot(void);
int front_ios_boot_ok(void);
// iPadOS 16 compatibility build: start directly with the generated defaults.
// This avoids SwiftUI's compiler-plugin macros, which are unavailable in the
// iPad-hosted macOS build environment. Settings remain overridable through
// Documents/env.txt exactly as before.
bool dm_show_launcher(void *view_controller, void (*done)(void)) { (void)view_controller; (void)done; return false; }

static CAMetalLayer *g_layer;
static void front_ios_power_watch(void);
static void front_ios_audio_watch(void);
static UILabel *front_ios_fps_label(void);
static void front_ios_display_refresh_start(UIView *view);
static void front_ios_display_refresh_active(int active);
static void front_ios_present_hz_clamp(UIView *view);

@interface DmGameView : UIView
@end
@implementation DmGameView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.contentsScale = UIScreen.mainScreen.nativeScale; l.framebufferOnly = NO; l.opaque = YES;
        self.backgroundColor = UIColor.blackColor;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale);
}
@end

@interface DmRootVC : UIViewController
@property (nonatomic, strong) DmGameView *game;
@property (nonatomic, strong) UIView *pad;
@property (nonatomic, strong) UILabel *fps;
@property (nonatomic) BOOL launched;
- (void)startGame;
@end
static DmRootVC *g_root;
static void launcher_done(void) { [g_root startGame]; }
@implementation DmRootVC
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    front_ios_power_watch();
    if (!front_ios_boot_ok()) {   // No game files: show a hint and do not start the host
        UILabel *lb = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 40)];
        lb.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        lb.numberOfLines = 0; lb.textAlignment = NSTextAlignmentCenter; lb.textColor = UIColor.whiteColor;
        lb.text = @"Game files not found.\nPut the RomFS folder into this App's Documents as \"romfs\" (Files app), then restart.";
        [self.view addSubview:lb]; return;
    }
    front_ios_audio_watch();
}
// The game view, the pad and the frame-rate counter are made once the launcher has set the switches they read (DM_SHOW_FPS, DM_METAL_HUD); the host's loading began at app launch (main.m)
- (void)startGame {
    if (self.game) return;
    front_hud_env();   // before the game view makes its device (an env file's DM_METAL_HUD sets it in main.m already)
    self.game = [[DmGameView alloc] initWithFrame:self.view.bounds];
    [self.view addSubview:self.game];
    g_layer = (CAMetalLayer *)self.game.layer;
    if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) { g_layer.developerHUDProperties = @{@"mode": @"default"}; LOG("front end: Metal performance HUD on (DM_METAL_HUD)"); }   // Apple's HUD over the game: frame rate, frame and GPU time, memory
    dm_haptics_apple_init();
    self.pad = dm_gamepad_view(self.view.bounds);
    self.pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.pad];
    self.fps = front_ios_fps_label();   // above the pad, which hides while a controller is connected
    if (self.fps) [self.view addSubview:self.fps];
    [self.view setNeedsLayout]; [self.view layoutIfNeeded];
    front_ios_display_refresh_start(self.game);
    front_ios_present_hz_clamp(self.game);
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    front_ios_boot();
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds; if (!self.game) return;
    CGFloat w = b.size.width, h = b.size.height;   // Fit 16:9 into the whole screen
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;
    self.game.frame = CGRectMake(floor((b.size.width - w) / 2), floor((b.size.height - h) / 2), w, h);
    if (self.fps) {   // the top row's free stretch: right of DmGamepad.m's L3 key (102 + 17 of its units from the safe edge), level with − / + (22 units down); u is its unit
        CGRect s = UIEdgeInsetsInsetRect(b, self.view.safeAreaInsets); CGFloat u = MIN(b.size.height / 430.0, 1.3);
        self.fps.frame = CGRectMake(floor(CGRectGetMinX(s) + 130 * u), floor(CGRectGetMinY(s) + 22 * u - 11), 64, 22);
    }
}
- (void)viewDidAppear:(BOOL)a {
    [super viewDidAppear:a];
    if (self.launched || !front_ios_boot_ok()) return;
    self.launched = YES; g_root = self;
    if (!dm_show_launcher((__bridge void *)self, launcher_done)) [self startGame];   // the settings screen first (Launcher.swift), when the app has one
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }   // Edge gestures need two swipes, to avoid accidental exits
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end
UIViewController *front_ios_root(void) { return [DmRootVC new]; }

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
// On when the IPA was built with it (the NSP script's SHOW_FPS / --show-fps → DMShowFPS in Info.plist); DM_SHOW_FPS=0 / 1 in Documents/env.txt overrides that without a rebuild. NULL when off
uint64_t mtl_present_count(void);

// iOS 16 has CADisplayLink even though it cannot use CAMetalDisplayLink.
// This link requests a screen cadence and measures its timestamps; it never
// obtains a drawable, submits a frame or changes the game's 60 Hz timebase.
@interface DmDisplayRefreshObserver : NSObject {
    CADisplayLink *_link;
    __weak UIView *_view;
    int _requested, _supported, _preferred;
    dm_display_probe _probe;
}
- (instancetype)initWithView:(UIView *)view requested:(int)requested;
- (void)configure;
- (void)tick:(CADisplayLink *)link;
- (void)setActive:(BOOL)active;
- (void)invalidate;
@end
@implementation DmDisplayRefreshObserver
- (instancetype)initWithView:(UIView *)view requested:(int)requested {
    if ((self = [super init])) {
        _view = view; _requested = requested;
        _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick:)];
        _link.paused = YES;
        [self configure];
        [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    return self;
}
- (void)configure {
    UIScreen *screen = _view.window.screen ?: UIScreen.mainScreen;
    _supported = (int)screen.maximumFramesPerSecond;
    _preferred = dm_display_hz_select(_requested, _supported);
    _link.preferredFrameRateRange = CAFrameRateRangeMake(MIN(60, _preferred), _preferred, _preferred);
    dm_display_probe_reset(&_probe);
    LOG("display refresh: requested=%d Hz supported_max=%d Hz preferred=%d Hz; screen cadence request only, game pacing unchanged",
        _requested, _supported, _preferred);
}
- (void)setActive:(BOOL)active {
    if (active) [self configure];
    dm_display_probe_reset(&_probe);
    _link.paused = !active;
}
- (void)tick:(CADisplayLink *)link {
    UIScreen *screen = _view.window.screen ?: UIScreen.mainScreen;
    if ((int)screen.maximumFramesPerSecond != _supported) [self configure];
    dm_display_probe_result result;
    if (dm_display_probe_tick(&_probe, link.timestamp, link.targetTimestamp, mtl_present_count(), &result))
        LOG("display cadence: requested=%d preferred=%d supported_max=%d Hz; measured_callback=%.2f Hz target_period=%.3f ms callback_period_min/max=%.3f/%.3f ms; actual_drawable=%.2f fps frames=%llu over=%.3f s; callback cadence is not rendered FPS",
            _requested, _preferred, _supported, result.callback_hz, result.target_period_ms,
            result.min_period_ms, result.max_period_ms, result.presented_fps,
            (unsigned long long)result.presented_frames, result.seconds);
}
- (void)invalidate { [_link invalidate]; _link = nil; }
@end
static DmDisplayRefreshObserver *g_display_refresh;
static void front_ios_display_refresh_start(UIView *view) {
    int requested = dm_display_hz_parse(getenv("DM_DISPLAY_HZ"));
    if (requested < 0) { LOG("[RED] DM_DISPLAY_HZ expects 60 or 120; retaining system display policy"); return; }
    if (!requested) return;
    [g_display_refresh invalidate];
    g_display_refresh = [[DmDisplayRefreshObserver alloc] initWithView:view requested:requested];
    [g_display_refresh setActive:UIApplication.sharedApplication.applicationState == UIApplicationStateActive];
}
static void front_ios_display_refresh_active(int active) { [g_display_refresh setActive:active != 0]; }
static void front_ios_present_hz_clamp(UIView *view) {
    // This runs on the main thread before front_ios_boot starts the guest. The
    // NVN pacer has not cached its explicit environment request at this point.
    // Never turn the existing 60 Hz default into an implicit high-rate mode.
    int requested = dm_display_hz_parse(getenv("DM_PRESENT_HZ"));
    if (requested != 120) return;
    UIScreen *screen = view.window.screen ?: UIScreen.mainScreen;
    int supported = (int)screen.maximumFramesPerSecond;
    int selected = dm_display_hz_select(requested, supported);
    char value[16]; snprintf(value, sizeof value, "%d", selected);
    setenv("DM_PRESENT_HZ", value, 1);
    LOG("present refresh: explicit request=%d Hz supported_max=%d Hz selected=%d Hz; configured before guest startup", requested, supported, selected);
}

static UILabel *front_ios_fps_label(void) {
    const char *e = getenv("DM_SHOW_FPS");
    if (!(e ? atoi(e) != 0 : [[NSBundle.mainBundle objectForInfoDictionaryKey:@"DMShowFPS"] boolValue])) return nil;
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
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
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
int front_screen_max_fps(void) { return (int)UIScreen.mainScreen.maximumFramesPerSecond; }
void front_screen_match(int fps) { (void)fps; }   // iOS chooses the display mode; the Metal layer is paced separately.
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
// SoloAmbient remains the default, including respect for Silent Mode. An inactive
// Stage Manager card releases its non-mixing session while graphics may continue.
static int g_session_off, g_audio_bg, g_audio_intr, g_audio_inactive, g_audio_lost;
static dm_audio_retry g_audio_retry;
static unsigned g_audio_route_logs;
static int g_audio_activation_logged, g_audio_error_seen;
static NSInteger g_audio_error_code;
static NSString *g_audio_error_domain;
static int g_audio_category_mode, g_audio_category_initialized;
static void front_ios_audio_apply(void);
static void front_ios_audio_retry_schedule(void);
static void front_ios_audio_deactivate(const char *why);
void dm_audio_diagnostics(const char *why);
void dm_audio_env_callback_set(void (*fn)(const char *, const char *));
static int front_ios_audio_blocked(void) { return g_audio_bg || g_audio_intr || g_audio_inactive || g_audio_lost; }
static const char *front_ios_audio_category_name(void) { return g_audio_category_mode == DM_AUDIO_SESSION_PLAYBACK_MIX ? "playback_mix" : "solo"; }
static void front_ios_audio_route_log_impl(const char *why, int force) {
    // Events are bounded; PCM summaries are produced off the real-time callback.
    if (!force && g_audio_route_logs++ >= 64) return;
    AVAudioSession *s = AVAudioSession.sharedInstance;
    NSMutableArray<NSString *> *ports = [NSMutableArray array];
    for (AVAudioSessionPortDescription *p in s.currentRoute.outputs)
        [ports addObject:[NSString stringWithFormat:@"%@:%@(%lu channels)", p.portType, p.portName, (unsigned long)p.channels.count]];
    LOG("audio session status (%s): requested=%s category=%s mode=%s options=%#lx outputVolume=%.3f route=%s rate=%.0f buffer_ms=%.3f outputLatency_ms=%.3f outputChannels=%ld background=%d inactive=%d interrupted=%d lost=%d session_off=%d retry=%u/%d epoch=%llu secondarySilenceHint=%d",
        why, front_ios_audio_category_name(), s.category.UTF8String, s.mode.UTF8String, (unsigned long)s.categoryOptions, s.outputVolume,
        ports.description.UTF8String, s.sampleRate, s.IOBufferDuration * 1000, s.outputLatency * 1000, (long)s.outputNumberOfChannels,
        g_audio_bg, g_audio_inactive, g_audio_intr, g_audio_lost, g_session_off, g_audio_retry.attempts, g_audio_retry.pending,
        (unsigned long long)g_audio_retry.generation, s.secondaryAudioShouldBeSilencedHint);
    dm_audio_diagnostics(why);
}
static void front_ios_audio_route_log(const char *why) { front_ios_audio_route_log_impl(why, 0); }
void front_ios_audio_session(void) {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(), ^{ front_ios_audio_session(); }); return; }
    if (front_ios_audio_blocked()) { g_session_off = 1; front_ios_audio_apply(); return; }
    if (!g_audio_category_initialized) {
        g_audio_category_initialized = 1;
        int mode = dm_audio_session_category_parse(getenv("DM_AUDIO_SESSION_CATEGORY"));
        if (mode >= 0) g_audio_category_mode = mode;
        else LOG("[RED] invalid DM_AUDIO_SESSION_CATEGORY; preserving SoloAmbient default");
    }
    unsigned attempt = g_audio_retry.attempts;
    int report = !g_audio_activation_logged || attempt <= 3;
    AVAudioSession *s = AVAudioSession.sharedInstance; NSError *e = nil;
    AVAudioSessionCategory category = g_audio_category_mode == DM_AUDIO_SESSION_PLAYBACK_MIX ? AVAudioSessionCategoryPlayback : AVAudioSessionCategorySoloAmbient;
    AVAudioSessionCategoryOptions options = g_audio_category_mode == DM_AUDIO_SESSION_PLAYBACK_MIX ? AVAudioSessionCategoryOptionMixWithOthers : 0;
    if (![s setCategory:category mode:AVAudioSessionModeDefault options:options error:&e] && report) LOG("[RED] audio session category could not be set: %s", e.localizedDescription.UTF8String);
    e = nil;
    if (![s setPreferredSampleRate:48000 error:&e] && report) LOG("[RED] audio session preferred sample rate failed: %s", e.localizedDescription.UTF8String);
    e = nil;
    if (![s setPreferredIOBufferDuration:0.005 error:&e] && report) LOG("[RED] audio session I/O buffer duration could not be set: %s", e.localizedDescription.UTF8String);
    e = nil;
    g_session_off = ![s setActive:YES error:&e];
    if (g_session_off) {
        NSString *domain = e.domain ?: @"";
        int changed = !g_audio_error_seen || g_audio_error_code != e.code || ![g_audio_error_domain isEqualToString:domain];
        report |= changed;
        if (report) LOG("[RED] audio session activation failed: %s", e.localizedDescription.UTF8String);
        g_audio_error_seen = 1; g_audio_error_code = e.code; g_audio_error_domain = domain;
    } else {
        report |= g_audio_error_seen;
        g_audio_error_seen = 0; g_audio_error_domain = nil;
        dm_audio_retry_reset(&g_audio_retry); // stale scheduled blocks cannot restart a later lifecycle epoch
    }
    front_ios_audio_apply();
    if (report) {
        LOG("audio session: sample rate %.0f Hz, I/O buffer %.2f ms (asked for 5 ms)", s.sampleRate, s.IOBufferDuration * 1000);
        front_ios_audio_route_log(g_session_off ? "activation failed" : "activated");
    }
    g_audio_activation_logged = 1;
    if (g_session_off) front_ios_audio_retry_schedule();
}
static void front_ios_audio_retry_schedule(void) {
    uint64_t token = 0;
    unsigned milliseconds = dm_audio_retry_plan(&g_audio_retry, front_ios_audio_blocked(), g_session_off, &token);
    if (!milliseconds) return;
    if (g_audio_retry.attempts <= 4)
        LOG("audio session: activation retry %u in %u ms (epoch %llu); later unchanged failures remain eligible at 8-second intervals", g_audio_retry.attempts, milliseconds, (unsigned long long)token);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)milliseconds * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (!dm_audio_retry_fire(&g_audio_retry, token, front_ios_audio_blocked(), g_session_off)) return;
        front_ios_audio_session();
    });
}
static void front_ios_audio_deactivate(const char *why) {
    dm_audio_retry_reset(&g_audio_retry);
    g_session_off = 1; front_ios_audio_apply(); // stop our units before releasing the shared session
    NSError *e = nil;
    if (![AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&e])
        LOG("audio session deactivation (%s) returned code=%ld: %s", why, (long)e.code, e.localizedDescription.UTF8String);
    front_ios_audio_route_log(why);
}
static void front_ios_audio_env_changed(const char *key, const char *value) {
    if (!key || strcmp(key, "DM_AUDIO_SESSION_CATEGORY")) return;
    int mode = dm_audio_session_category_parse(value);
    if (mode < 0) { LOG("[RED] audio session category diagnostic rejected: %s", value ?: "(null)"); return; }
    // Parse while CTL's line buffer is valid; capture only the enum across queues.
    dispatch_async(dispatch_get_main_queue(), ^{
        g_audio_category_mode = mode; g_audio_category_initialized = 1;
        dm_audio_retry_reset(&g_audio_retry); // cancel any previous policy's retry
        if (front_ios_audio_blocked()) {
            g_session_off = 1; front_ios_audio_apply();
            front_ios_audio_route_log_impl("category diagnostic deferred until active", 1);
            return;
        }
        uint64_t before = dm_audio_render_calls();
        front_ios_audio_route_log_impl("category diagnostic before transition", 1);
        front_ios_audio_deactivate("category diagnostic transition");
        front_ios_audio_session();
        front_ios_audio_route_log_impl("category diagnostic after transition", 1);
        uint64_t epoch = g_audio_retry.generation;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (epoch != g_audio_retry.generation || mode != g_audio_category_mode || front_ios_audio_blocked()) return;
            uint64_t after = dm_audio_render_calls();
            LOG("audio category diagnostic settled: mode=%s new_callbacks=%llu", front_ios_audio_category_name(), (unsigned long long)(after >= before ? after - before : 0));
            front_ios_audio_route_log_impl("category diagnostic settled", 1);
        });
    });
}

// Real audio output is suspended (the silent sink paces the game) while the app is in the background or another audio session has interrupted ours (Siri, an alarm, a call).
// Without the interruption observer the AudioUnits stayed "started" while iOS no longer called them, and a game waiting for its buffers back lost its sound until restart.
// All of this runs on the main queue, like the scene callbacks
static void front_ios_audio_apply(void) { dm_audio_suspend(front_ios_audio_blocked() || g_session_off); }
static void front_ios_audio_watch(void) {
    static int installed; if (installed++) return;
    dm_audio_env_callback_set(front_ios_audio_env_changed);
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue], reason = [n.userInfo[AVAudioSessionInterruptionReasonKey] unsignedIntegerValue];
        if (type == AVAudioSessionInterruptionTypeBegan) {
            LOG("audio session: interrupted (reason %lu)", (unsigned long)reason);
            g_audio_intr = 1; g_session_off = 1; dm_audio_retry_reset(&g_audio_retry);
        }
        else {
            LOG("audio session: interruption ended (options %#lx)", (unsigned long)[n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue]);
            g_audio_intr = 0; g_session_off = 1; dm_audio_retry_reset(&g_audio_retry);
            // Apple permits games without a media Play/Pause UI to resume here.
            if (!front_ios_audio_blocked()) front_ios_audio_session();
        }
        front_ios_audio_apply();
        front_ios_audio_route_log("interruption notification");
    }];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        NSUInteger reason = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
        char why[64]; snprintf(why, sizeof why, "route changed reason=%lu", (unsigned long)reason);
        front_ios_audio_route_log(why);
        // Category changes can be generated by our own setup. Do not recursively
        // reconfigure/reactivate on every route notification or reset its budget.
        if (g_session_off) front_ios_audio_retry_schedule();
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereLostNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; g_audio_lost = 1; g_session_off = 1; dm_audio_retry_reset(&g_audio_retry);
        front_ios_audio_apply(); front_ios_audio_route_log("media services lost");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; LOG("audio session: the media services were reset; setting the session and the outputs up again");
        g_audio_lost = 0; g_session_off = 1; dm_audio_retry_reset(&g_audio_retry);
        front_ios_audio_apply(); dm_audio_reset();
        if (!front_ios_audio_blocked()) front_ios_audio_session();
        front_ios_audio_route_log("media services reset");
    }];
}

// Scene lifecycle (called by SceneDelegate in main.m): 0 resign active, 1 enter background, 2 return to foreground, 3 become active. On entering background, freeze the guest clock and suspend after in-flight GPU work completes (submitting GPU work in background gets the process killed).
// The screen stays on while the game is active: only touches reset the idle timer, so with a controller (or during a cutscene) the screen dimmed and locked mid-game
void front_ios_focus_event(int ev) {
    switch (ev) {
        case 0:
            front_ios_display_refresh_active(0);
            g_audio_inactive = 1; front_ios_audio_deactivate("scene resigned active");
            dm_gamepad_set_active(0);
            if (g_layer) dm_focus_set(2); break;
        case 1:
            front_ios_display_refresh_active(0);
            g_audio_bg = 1; front_ios_audio_deactivate("scene entered background");
            dm_gamepad_set_active(0);
            if (g_layer) { UIApplication.sharedApplication.idleTimerDisabled = NO; dm_focus_set(3); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); } break;
        case 2:
            g_audio_bg = 0; // still inactive until didBecomeActive; keep audio released
            if (g_layer) { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_focus_set(2); } break;
        case 3:   // Becoming active again also ends an interruption whose "ended" never came (not every one sends it), and retries a session that would not activate on the way back
            {
                front_ios_display_refresh_active(1);
                int transitioned = g_audio_inactive || g_audio_bg || g_audio_intr;
                g_audio_inactive = 0; g_audio_bg = 0;
                dm_gamepad_set_active(1);
                if (g_layer) UIApplication.sharedApplication.idleTimerDisabled = YES;
                if (g_audio_intr) { LOG("audio session: active again with no interruption-ended notice; resuming"); g_audio_intr = 0; g_session_off = 1; }
                if (transitioned) dm_audio_retry_reset(&g_audio_retry);
                if (g_session_off && !g_audio_retry.pending && !g_audio_retry.attempts) front_ios_audio_session();
                front_ios_audio_apply(); if (g_layer) dm_focus_set(1);
                front_ios_audio_route_log("scene became active");
            } break;
    }
}
