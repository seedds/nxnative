// A Switch 2 controller's motion without a game (scripts/sw2_gyro.sh builds and runs it): connects to a Joy-Con 2 or Pro Controller 2 over Bluetooth with the host's own
// driver (pad_switch2_apple.m, the same pairing, handshake, calibration, axis map and gyro scale the game gets) and prints its motion ten times a second, in the Switch frame
// the game reads (x right, y forward, z up; a controller lying flat reads gravity 0 0 -1). A window shows a block turned as the controller is, from the same fusion the host's
// six-axis sensor runs (motion_fusion.h: the gyro integrated, the tilt pulled toward the measured gravity, the yaw free to drift; Recenter or the space bar turns it back to face
// the screen). --no-window prints only. A press of a button wakes a controller already paired with this Mac; holding its Sync button pairs one (as in a game: pairing again
// makes it forget the console). Ctrl-C or closing the window quits.
#import <Cocoa/Cocoa.h>
#import <SceneKit/SceneKit.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include "pad_switch2_apple.h"
#include "motion_fusion.h"

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_UPTIME_RAW, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// What the driver hands the game's six-axis sensor: kept here, integrated into an orientation and printed by the timer (all on the main queue)
typedef struct { float acc[3], rot[3]; uint64_t t; unsigned n; } sample;
static sample g_pad, g_joy[2];
static motion_state g_fused; static uint64_t g_fused_t;
static void recenter(void);
static void keep(sample *s, const float acc[3], const float rot[3], uint64_t t) { memcpy(s->acc, acc, sizeof s->acc); memcpy(s->rot, rot, sizeof s->rot); s->t = t; s->n++; }
void dm_motion_set_hid(int p, const float acc[3], const float rot[3], uint64_t t) {
    (void)p; keep(&g_pad, acc, rot, t);
    static const motion_map identity = {{0, 1, 2}, {1, 1, 1}, 1, 1};
    float dt = g_fused_t && t > g_fused_t ? (float)((double)(t - g_fused_t) / 1e9) : 0; if (dt > 0.1f) dt = 0;   // a gap: no integration across it
    if (!g_fused_t || t > g_fused_t) {
        int first = !g_fused_t;
        motion_update(&g_fused, &identity, acc, rot, dt, 0.5f); g_fused_t = t;
        if (first) recenter();   // the block starts facing the screen
    }
}
void dm_motion_set_joy(int p, int side, const float acc[3], const float rot[3], uint64_t t) { (void)p; if (side == 0 || side == 1) keep(&g_joy[side], acc, rot, t); }
void dm_motion_clear_hid(int p) { (void)p; }
void dm_motion_clear_joy(int p, int side) { (void)p; (void)side; }
void dm_joy_attach(int p, uint32_t sides) { (void)p; (void)sides; }
void dm_joy_set(int p, int side, uint64_t buttons, int32_t sx, int32_t sy) { (void)p; (void)side; (void)buttons; (void)sx; (void)sy; }
void dm_joy_mouse(int p, int side, int32_t dx, int32_t dy, uint32_t state) { (void)p; (void)side; (void)dx; (void)dy; (void)state; }
int dm_joy_mouse_wanted(void) { return 0; }

// The front end: every controller is player 1, and nothing blocks its input
static int f_attach(s2_pad *p) { printf("connected: %s (motion below; leave it flat and still, then turn it)\n", p->name); return 1; }
static void f_detach(s2_pad *p) { printf("disconnected: %s\n", p->name); }
static int32_t g_sticks[4]; static int g_sticks_have;
static void f_read(s2_pad *p) { uint64_t m; if (s2_pad_input(&p->st, &m, g_sticks)) g_sticks_have = 1; }   // the sticks as the game gets them (dead zone applied)
static int f_player(s2_pad *p) { (void)p; return 0; }

static void line(const char *who, sample *s, unsigned *was) {
    float d = 180.f / 3.14159265f, g[3] = {s->rot[0] * d, s->rot[1] * d, s->rot[2] * d};
    float gm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]), am = sqrtf(s->acc[0] * s->acc[0] + s->acc[1] * s->acc[1] + s->acc[2] * s->acc[2]);
    unsigned rate = (s->n - *was) * 10; *was = s->n;
    const char *state = gm < 3 ? "still" : gm < 30 ? "slow turn" : "turning";
    printf("%-6s gyro %7.1f %7.1f %7.1f deg/s |%6.1f| %-9s  gravity %5.2f %5.2f %5.2f G |%4.2f|  %3u samples/s%s\n", who, g[0], g[1], g[2], gm, state,
           s->acc[0], s->acc[1], s->acc[2], am, rate, gm < 3 && (am < 0.85f || am > 1.15f) ? "  <- not 1 G at rest" : "");
}

// The block: a controller-shaped box (wide, a little deep, thin), its face (z up, the buttons' side) blue, its far edge (y forward, the triggers' side) orange, the rest grey.
// SceneKit's frame is x right, y up, z toward the viewer; the Switch frame's x right, y forward, z up is C = (x, z, -y) in it, and the block's transform is C R C^T
static SCNNode *g_block;
static float g_yaw0;   // Recenter: the world turn that brings the block's forward axis back to the screen
static void recenter(void) { g_yaw0 = atan2f(g_fused.R[0][1], g_fused.R[1][1]); }   // the forward axis (R's second column) in the world's x y plane
static void place_block(void) {
    if (!g_block) return;
    float c = cosf(g_yaw0), s = sinf(g_yaw0), R[3][3];   // the world turned by g_yaw0 about z, then the controller's orientation
    for (int j = 0; j < 3; j++) { R[0][j] = c * g_fused.R[0][j] - s * g_fused.R[1][j]; R[1][j] = s * g_fused.R[0][j] + c * g_fused.R[1][j]; R[2][j] = g_fused.R[2][j]; }
    static const int map[3] = {0, 2, 1}; static const float sign[3] = {1, 1, -1};   // scene axis i = sign * switch axis map[i]
    float M[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = sign[i] * sign[j] * R[map[i]][map[j]];
    SCNMatrix4 t = SCNMatrix4Identity;   // SceneKit's matrices are row-major with the translation in the last row: m11..m33 = M transposed
    t.m11 = M[0][0]; t.m12 = M[1][0]; t.m13 = M[2][0];
    t.m21 = M[0][1]; t.m22 = M[1][1]; t.m23 = M[2][1];
    t.m31 = M[0][2]; t.m32 = M[1][2]; t.m33 = M[2][2];
    g_block.transform = t;
}
static NSColor *rgb(float r, float g, float b) { return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:1]; }
static SCNMaterial *mat(NSColor *c) { SCNMaterial *m = [SCNMaterial material]; m.diffuse.contents = c; return m; }

@interface RecenterView : SCNView @end
@implementation RecenterView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e { if ([e.characters isEqualToString:@" "]) recenter(); else [super keyDown:e]; }
@end
@interface Delegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation Delegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)recenter:(id)sender { (void)sender; recenter(); }
@end

static NSWindow *make_window(Delegate *d) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 420) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    w.title = @"Switch 2 controller motion"; w.delegate = d; w.releasedWhenClosed = NO;
    RecenterView *v = [[RecenterView alloc] initWithFrame:w.contentView.bounds];
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable; v.backgroundColor = rgb(0.12f, 0.12f, 0.14f); v.autoenablesDefaultLighting = YES;
    SCNScene *scene = [SCNScene scene]; v.scene = scene;
    SCNBox *box = [SCNBox boxWithWidth:3.0 height:0.45 length:1.6 chamferRadius:0.12];
    NSColor *grey = rgb(0.55f, 0.55f, 0.58f);
    box.materials = @[mat(grey), mat(grey), mat(rgb(0.95f, 0.55f, 0.15f)), mat(grey), mat(rgb(0.2f, 0.45f, 0.95f)), mat(grey)];   // front (+z: toward the player), right, back (-z: forward, the triggers), left, top (the face), bottom
    g_block = [SCNNode nodeWithGeometry:box]; [scene.rootNode addChildNode:g_block];
    SCNNode *cam = [SCNNode node]; cam.camera = [SCNCamera camera]; cam.position = SCNVector3Make(0, 3.2, 5.2);   // behind and above the player, looking at the block
    cam.constraints = @[[SCNLookAtConstraint lookAtConstraintWithTarget:g_block]]; [scene.rootNode addChildNode:cam];
    SCNNode *axes = [SCNNode nodeWithGeometry:[SCNFloor floor]]; axes.position = SCNVector3Make(0, -1.6, 0); axes.geometry.firstMaterial.diffuse.contents = rgb(0.18f, 0.18f, 0.2f);
    [scene.rootNode addChildNode:axes];
    [w.contentView addSubview:v];
    NSButton *b = [NSButton buttonWithTitle:@"Recenter (space)" target:d action:@selector(recenter:)]; [b sizeToFit];
    b.frame = NSMakeRect(12, 12, b.frame.size.width, b.frame.size.height); b.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin; [w.contentView addSubview:b];
    NSTextField *hint = [NSTextField labelWithString:@"blue: the face   orange: the triggers' edge"]; hint.textColor = rgb(0.7f, 0.7f, 0.72f); [hint sizeToFit];
    hint.frame = NSMakeRect(12, w.contentView.bounds.size.height - hint.frame.size.height - 10, hint.frame.size.width, hint.frame.size.height); hint.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    [w.contentView addSubview:hint];
    [w center]; [w makeKeyAndOrderFront:nil]; [w makeFirstResponder:v];
    return w;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        int window = 1; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-window")) window = 0;
        setenv("DM_SW2", "1", 0);
        setvbuf(stdout, NULL, _IOLBF, 0);
        motion_reset(&g_fused);
        NSApplication *app = [NSApplication sharedApplication];
        Delegate *d = [Delegate new]; app.delegate = d;
        static const dm_switch2_front front = {f_attach, f_detach, f_read, f_player, NULL};
        if (!dm_switch2_start(&front)) { fprintf(stderr, "sw2_gyro: the Switch 2 driver did not start (DM_SW2=0, or no Bluetooth usage description in the tool)\n"); return 1; }
        printf("waiting for a Switch 2 controller: press a button on one paired with this Mac, or hold its Sync button to pair it. Ctrl-C quits.\n");
        signal(SIGINT, SIG_IGN);
        dispatch_source_t sig = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sig, ^{ printf("\n"); exit(0); }); dispatch_resume(sig);
        static unsigned was_pad, was_joy[2];
        [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *t) {
            (void)t; uint64_t now = now_ns();
            if (g_pad.n && now - g_pad.t < 300000000ull) line("motion", &g_pad, &was_pad);
            if (g_sticks_have) printf("       sticks to the game: left %+5.1f %+5.1f %%  right %+5.1f %+5.1f %%%s\n", g_sticks[0] * 100.0 / DM_STICK_MAX, g_sticks[1] * 100.0 / DM_STICK_MAX,
                                      g_sticks[2] * 100.0 / DM_STICK_MAX, g_sticks[3] * 100.0 / DM_STICK_MAX, g_sticks[0] || g_sticks[1] || g_sticks[2] || g_sticks[3] ? "" : "  (both at rest: 0)");
            for (int k = 0; k < 2; k++) if (g_joy[k].n && now - g_joy[k].t < 300000000ull) line(k ? "R" : "L", &g_joy[k], &was_joy[k]);
        }];
        if (window) {
            app.activationPolicy = NSApplicationActivationPolicyRegular;
            make_window(d);
            [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; place_block(); }];
            [app activateIgnoringOtherApps:YES];
            [app run];
        } else CFRunLoopRun();
    }
    return 0;
}
