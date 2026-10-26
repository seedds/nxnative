// Apple TV front end: Metal, physical gamepads and UIKit Remote input.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <GameController/GameController.h>
#import <AVFAudio/AVFAudio.h>
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "hle.h"
#include "tv_remote.h"
#include <math.h>

void front_tv_start(void);
int front_tv_ready(void);
int front_tv_preparing(void);
static CAMetalLayer *tv_layer;
static UIViewController *tv_root;
static BOOL tv_active = YES;
static TVRemoteState remote_state;
static TVRemoteOutput physical_state;
static void input_publish(void) {
    TVRemoteOutput r = tv_remote_output(&remote_state), p = physical_state;
    if (!tv_active) { dm_pad_set(0, 0, 0, 0, 0); return; }
    dm_pad_set(p.buttons | r.buttons, r.lx ?: p.lx, r.ly ?: p.ly, r.rx ?: p.rx, r.ry ?: p.ry);
}
static void controller_read(GCExtendedGamepad *p) {
    if (!tv_active) { dm_pad_set(0, 0, 0, 0, 0); return; }
    uint64_t b = 0;
    // Nintendo's letters follow the physical positions used by the host's
    // existing controllers: bottom B, right A, left Y, top X.
    if (p.buttonA.pressed) b |= DM_BTN_B;
    if (p.buttonB.pressed) b |= DM_BTN_A;
    if (p.buttonX.pressed) b |= DM_BTN_Y;
    if (p.buttonY.pressed) b |= DM_BTN_X;
    if (p.leftShoulder.pressed) b |= DM_BTN_L;
    if (p.rightShoulder.pressed) b |= DM_BTN_R;
    if (p.leftTrigger.pressed) b |= DM_BTN_ZL;
    if (p.rightTrigger.pressed) b |= DM_BTN_ZR;
    if (p.leftThumbstickButton.pressed) b |= DM_BTN_LS;
    if (p.rightThumbstickButton.pressed) b |= DM_BTN_RS;
    if (p.buttonMenu.pressed) b |= DM_BTN_PLUS;
    if (p.buttonOptions.pressed) b |= DM_BTN_MINUS;
    if (p.dpad.left.pressed) b |= DM_BTN_LEFT;
    if (p.dpad.right.pressed) b |= DM_BTN_RIGHT;
    if (p.dpad.up.pressed) b |= DM_BTN_UP;
    if (p.dpad.down.pressed) b |= DM_BTN_DOWN;
    physical_state = (TVRemoteOutput){ b, p.leftThumbstick.xAxis.value * 32767, p.leftThumbstick.yAxis.value * 32767,
        p.rightThumbstick.xAxis.value * 32767, p.rightThumbstick.yAxis.value * 32767 };
    input_publish();
}
static void controllers_update(void) {
    physical_state = (TVRemoteOutput){0}; input_publish();
    for (GCController *c in GCController.controllers) {
        if (!c.extendedGamepad) continue;
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *p, GCControllerElement *element) {
            dispatch_async(dispatch_get_main_queue(), ^{ controller_read(p); });
        };
        controller_read(c.extendedGamepad); break;
    }
}
void front_tv_focus(int active) {
    tv_active = !!active; tv_remote_cancel(&remote_state, 1);
    physical_state = (TVRemoteOutput){0}; input_publish(); dm_input_set(0, 0, 0, 0, 0);
    [[NSNotificationCenter defaultCenter] postNotificationName:@"TVRemoteReset" object:nil];
    if (tv_layer) { dm_focus_set(active ? 1 : 3); guest_clock_freeze_for(1, !active); dm_gfx_suspend(!active); }
    NSError *error = nil;
    [AVAudioSession.sharedInstance setActive:!!active error:&error];
    if (active) controllers_update();
}
@interface TVMetalView : UIView
@end
@implementation TVMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer;
        l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm;
        l.framebufferOnly = NO; l.opaque = YES;
        l.drawableSize = CGSizeMake(1920, 1080);
    } return self;
}
@end
@interface TVRootVC : UIViewController
@property(strong, nonatomic) UILabel *preparationLabel;
@property(strong, nonatomic) UILabel *remoteHint;
@property(strong, nonatomic) UIView *remotePanel;
@property(strong, nonatomic) UIPanGestureRecognizer *remotePan;
@property(strong, nonatomic) NSMutableArray<UIButton *> *remoteButtons;
@property(nonatomic) NSUInteger pulseSerial;
- (void)updateRemoteHint;
- (void)createGame;
@end
@implementation TVRootVC
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.blackColor;
    if (!front_tv_ready()) {
        UILabel *label = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 70, 70)];
        label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        label.numberOfLines = 0; label.textColor = UIColor.whiteColor; label.textAlignment = NSTextAlignmentCenter;
        label.text = front_tv_preparing() ? @"Preparing game files…" : @"Game resources have not been staged for Apple TV yet.";
        self.preparationLabel = label; [self.view addSubview:label]; return;
    }
    [self createGame];
}
- (void)createGame {
    [self.preparationLabel removeFromSuperview]; self.preparationLabel = nil;
    TVMetalView *view = [[TVMetalView alloc] initWithFrame:self.view.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:view]; tv_layer = (CAMetalLayer *)view.layer;
    NSError *error = nil; AVAudioSession *audio = AVAudioSession.sharedInstance;
    [audio setCategory:AVAudioSessionCategoryPlayback error:&error];
    [audio setPreferredSampleRate:48000 error:&error];
    [audio setPreferredIOBufferDuration:0.005 error:&error];
    [audio setActive:YES error:&error];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { controllers_update(); }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { controllers_update(); }];
    controllers_update(); UIApplication.sharedApplication.idleTimerDisabled = YES;
    remote_state.mode = (int)[NSUserDefaults.standardUserDefaults integerForKey:@"TVRemoteMode"];
    if (remote_state.mode < TV_REMOTE_MOVE || remote_state.mode > TV_REMOTE_DPAD) remote_state.mode = TV_REMOTE_MOVE;
    self.remotePan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(remotePanned:)];
    [self.view addGestureRecognizer:self.remotePan];
    self.remoteHint = [[UILabel alloc] initWithFrame:CGRectMake(50, self.view.bounds.size.height - 66, self.view.bounds.size.width - 100, 42)];
    self.remoteHint.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
    self.remoteHint.font = [UIFont systemFontOfSize:23 weight:UIFontWeightSemibold];
    self.remoteHint.textColor = UIColor.whiteColor;
    self.remoteHint.backgroundColor = [UIColor colorWithWhite:0 alpha:0.70];
    self.remoteHint.textAlignment = NSTextAlignmentCenter;
    self.remoteHint.accessibilityIdentifier = @"remote-hint";
    [self.view addSubview:self.remoteHint]; [self updateRemoteHint];
    [NSNotificationCenter.defaultCenter addObserverForName:@"TVRemoteReset" object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        self.pulseSerial++; [self updateRemoteHint];
    }];
    [self becomeFirstResponder];
}
- (BOOL)canBecomeFirstResponder { return YES; }
- (NSArray<id<UIFocusEnvironment>> *)preferredFocusEnvironments {
    return self.remotePanel && self.remoteButtons.count ? @[self.remoteButtons.firstObject] : @[];
}
- (void)updateRemoteHint {
    NSArray *modes = @[@"MOVE", @"CAMERA", @"D-PAD"];
    NSMutableArray *held = [NSMutableArray array];
    for (NSArray *entry in @[@[@"L", @(DM_BTN_L)], @[@"R", @(DM_BTN_R)], @[@"ZL", @(DM_BTN_ZL)], @[@"ZR", @(DM_BTN_ZR)]])
        if (remote_state.held & [entry[1] unsignedLongLongValue]) [held addObject:entry[0]];
    self.remoteHint.text = [NSString stringWithFormat:@"%@  |  Click: A    Back: B    Play/Pause: all controls%@", modes[remote_state.mode],
        held.count ? [@"  |  Holding: " stringByAppendingString:[held componentsJoinedByString:@" + "]] : @""];
}
- (void)remotePanned:(UIPanGestureRecognizer *)pan {
    if (self.remotePanel || self.presentedViewController || !tv_active) return;
    CGPoint d = [pan translationInView:self.view];
    if (pan.state == UIGestureRecognizerStateEnded || pan.state == UIGestureRecognizerStateCancelled || pan.state == UIGestureRecognizerStateFailed)
        remote_state.x = remote_state.y = 0;
    else {
        remote_state.x = lrint(fmax(-1, fmin(1, d.x / 120.0)) * 32767);
        remote_state.y = lrint(fmax(-1, fmin(1, -d.y / 120.0)) * 32767);
    }
    input_publish();
}
- (void)hideRemotePanel {
    [self.remotePanel removeFromSuperview]; self.remotePanel = nil;
    self.remoteButtons = nil; self.remotePan.enabled = YES;
    tv_remote_cancel(&remote_state, 0); self.pulseSerial++; input_publish();
    [self updateRemoteHint]; [self becomeFirstResponder]; [self setNeedsFocusUpdate];
}
- (void)showRemotePanel {
    if (self.remotePanel) { [self hideRemotePanel]; return; }
    tv_remote_cancel(&remote_state, 0); self.pulseSerial++; input_publish();
    self.remotePan.enabled = NO;
    UIView *panel = [[UIView alloc] initWithFrame:CGRectInset(self.view.bounds, 80, 70)];
    panel.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    panel.backgroundColor = [UIColor colorWithWhite:0.04 alpha:0.96]; panel.layer.cornerRadius = 28;
    panel.accessibilityIdentifier = @"remote-controls"; self.remotePanel = panel;
    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(30, 20, panel.bounds.size.width - 60, 100)];
    title.numberOfLines = 2; title.textAlignment = NSTextAlignmentCenter; title.textColor = UIColor.whiteColor;
    title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightSemibold];
    title.text = @"Remote controls\nChoose a stick mode or press a game button. Hold switches stay on until released.";
    [panel addSubview:title]; self.remoteButtons = [NSMutableArray array];
    NSArray *labels = @[@"Resume / Move", @"Camera stick", @"D-pad", @"Release held buttons",
        @"A", @"B", @"X", @"Y", @"L", @"R", @"ZL", @"ZR", @"+", @"−", @"L stick click", @"R stick click",
        @"Hold L", @"Hold R", @"Hold ZL", @"Hold ZR"];
    CGFloat width = (panel.bounds.size.width - 180) / 4;
    for (NSUInteger i = 0; i < labels.count; i++) {
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        b.frame = CGRectMake(35 + (i % 4) * (width + 35), 150 + (i / 4) * ((panel.bounds.size.height - 175) / 5), width, (panel.bounds.size.height - 175) / 5 - 23);
        b.tag = i; [b setTitle:labels[i] forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont systemFontOfSize:26 weight:UIFontWeightSemibold];
        b.accessibilityIdentifier = [@"remote-" stringByAppendingString:labels[i]];
        [b addTarget:self action:@selector(remoteAction:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [panel addSubview:b]; [self.remoteButtons addObject:b];
    }
    [self.view addSubview:panel]; [self setNeedsFocusUpdate]; [self updateFocusIfNeeded];
    fprintf(stderr, "tvOS Remote: controls opened\n");
}
- (void)remoteAction:(UIButton *)button {
    NSInteger i = button.tag;
    if (i < 3) {
        tv_remote_mode(&remote_state, (int)i);
        [NSUserDefaults.standardUserDefaults setInteger:i forKey:@"TVRemoteMode"];
        fprintf(stderr, "tvOS Remote: mode=%ld\n", (long)i); [self hideRemotePanel]; return;
    }
    if (i == 3) { tv_remote_cancel(&remote_state, 1); input_publish(); [self updateRemoteHint]; return; }
    static const uint64_t buttons[] = { DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR,
        DM_BTN_PLUS, DM_BTN_MINUS, DM_BTN_LS, DM_BTN_RS, DM_BTN_L, DM_BTN_R, DM_BTN_ZL, DM_BTN_ZR };
    uint64_t bit = buttons[i - 4];
    if (i >= 16) {
        remote_state.held ^= bit; input_publish(); [self updateRemoteHint];
        [button setTitle:[NSString stringWithFormat:@"%@ %@", remote_state.held & bit ? @"Release" : @"Hold", @[@"L", @"R", @"ZL", @"ZR"][i-16]] forState:UIControlStateNormal];
        return;
    }
    // The focused button produces one bounded pulse, never an accidental hold.
    remote_state.pulse = bit; input_publish(); NSUInteger serial = ++self.pulseSerial;
    fprintf(stderr, "tvOS Remote: pulse=0x%llx\n", (unsigned long long)bit);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (self.pulseSerial != serial) return;
        remote_state.pulse = 0; input_publish();
    });
}
- (BOOL)remotePress:(UIPress *)press down:(BOOL)down {
    if (!tv_active || self.presentedViewController || !self.remoteHint) return NO;
    if (press.type == UIPressTypePlayPause) { if (down) [self showRemotePanel]; return YES; }
    if (self.remotePanel) {
        if (press.type == UIPressTypeMenu) { if (down) [self hideRemotePanel]; return YES; }
        return NO;
    }
    uint64_t button = press.type == UIPressTypeSelect ? DM_BTN_A : press.type == UIPressTypeMenu ? DM_BTN_B : 0;
    int direction = press.type == UIPressTypeLeftArrow ? TV_REMOTE_LEFT : press.type == UIPressTypeRightArrow ? TV_REMOTE_RIGHT :
        press.type == UIPressTypeUpArrow ? TV_REMOTE_UP : press.type == UIPressTypeDownArrow ? TV_REMOTE_DOWN : 0;
    if (!button && !direction) return NO;
    if (down) { remote_state.pressed |= button; remote_state.directions |= direction; }
    else { remote_state.pressed &= ~button; remote_state.directions &= ~direction; }
    input_publish(); return YES;
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    NSMutableSet *unhandled = [NSMutableSet set];
    for (UIPress *p in presses) if (![self remotePress:p down:YES]) [unhandled addObject:p];
    if (unhandled.count) [super pressesBegan:unhandled withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    NSMutableSet *unhandled = [NSMutableSet set];
    for (UIPress *p in presses) if (![self remotePress:p down:NO]) [unhandled addObject:p];
    if (unhandled.count) [super pressesEnded:unhandled withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    tv_remote_cancel(&remote_state, 0); input_publish();
    [super pressesCancelled:presses withEvent:event];
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; [self becomeFirstResponder]; front_tv_start(); }
@end
UIViewController *front_tv_root(void) { tv_root = [TVRootVC new]; return tv_root; }
void front_tv_resources_ready(NSString *failure) {
    TVRootVC *root = (TVRootVC *)tv_root;
    if (failure) {
        if (!root.preparationLabel) {
            UILabel *label = [[UILabel alloc] initWithFrame:CGRectInset(root.view.bounds, 70, 70)];
            label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
            label.numberOfLines = 0; label.textColor = UIColor.whiteColor;
            label.backgroundColor = [UIColor colorWithWhite:0 alpha:0.82];
            label.textAlignment = NSTextAlignmentCenter;
            root.preparationLabel = label; [root.view addSubview:label];
        }
        root.preparationLabel.text = failure; return;
    }
    [root createGame]; front_tv_start();
}
void *front_metal_layer(void) { return (__bridge void *)tv_layer; }
int front_screen_max_fps(void) { return 60; }
void front_screen_match(int fps) { (void)fps; }
void front_run(void (*guest)(void)) { guest(); }
int dm_thermal_now(void) { return (int)NSProcessInfo.processInfo.thermalState; }
int dm_low_power_now(void) { return 0; }
double dm_brightness_now(void) { return -1; }
void front_save_failed(int err) {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Save failed"
            message:[NSString stringWithFormat:@"Progress was not saved (error %d).", err] preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [tv_root presentViewController:alert animated:YES completion:nil];
    });
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    __block int accepted = 0; __block NSString *text = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@(title ?: "")
            message:@(sub ?: "") preferredStyle:UIAlertControllerStyleAlert];
        [alert addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = @(initial ?: ""); f.placeholder = @(guide ?: ""); }];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a) { text = alert.textFields.firstObject.text; accepted = 1; dispatch_semaphore_signal(done); }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *a) { dispatch_semaphore_signal(done); }]];
        [tv_root presentViewController:alert animated:YES completion:nil];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (accepted) snprintf(out, out_cap, "%s", text.UTF8String ?: "");
    return accepted;
}
