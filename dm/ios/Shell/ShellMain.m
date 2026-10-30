// Route C shell (docs/launcher-c-plan.md M2): the app that hosts the engine and several games' frameworks. It starts on the
// library (frontend-ios's ShellHost views, ShellApp.swift) instead of booting a game at launch as dm/ios/Sources/main.m does, and
// when the player presses Play, dm_shell_start_game() does what main.m's boot_env() does before UIApplicationMain, for the chosen
// game: the log, the environment (EngineLaunchPlan in ShellHost computes it), dlopen of the game's framework and its mach header for
// the loader, and the host's early loading. dm_shell_present_game() then hands the window to front_ios.m's game view, which starts
// dm_main on the big-stack thread through front_ios_boot() below.
// One game per process: dm_profile() and dm_host_preload_start() run once, so returning to the library is a relaunch of the app.
// This file provides the symbols front_ios.m takes from main.m and Launcher.swift in the one-game app (front_ios_boot,
// front_ios_boot_ok, front_ios_boot_message, front_ios_device_message, dm_locate_game_data, dm_show_launcher): the shell compiles front_ios.m unchanged.
#import <UIKit/UIKit.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "front.h"
#include "loader_image.h"
#include "ShellEngine.h"
#import "DmShell-Swift.h"

int dm_main(int argc, char **argv);   // dm/host/main.c compiled in with -Dmain=dm_main
UIViewController *front_ios_root(void);
void front_ios_focus_event(int ev);

static char **g_argv; static int g_argc; static int g_boot_ok;

static NSString *documents(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0]; }

// One log file per run, the previous one kept beside it (<name>.prev.txt): the shell's own lines go to Documents/shell-log.txt until a
// game starts, the game's (the host's stderr) to Documents/log.txt, the file the per-game app writes and the issue forms ask for.
static void log_to(NSString *log) {
    NSString *prev = [[log stringByDeletingPathExtension] stringByAppendingString:@".prev.txt"];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
    fflush(stderr);
    if (freopen(log.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    NSString *fork_patches = [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMForkPatches"];
    fprintf(stderr, "fork patches: %s\n", [fork_patches isKindOfClass:NSString.class] && fork_patches.length ? fork_patches.UTF8String : "none");
}

const char *dm_shell_start_game(const char *const *keys, const char *const *values, int n, const char *const *argv, int argc,
                                const char *executable, const char *marker, const char *log_path, const char *cwd) {
    static char err[512];
    static int started;
    if (started++) return "A game was already started in this session. Close the app and open it again.";
    if (argc < 2) return "The game's start arguments are incomplete.";
    log_to(@(log_path));
    fprintf(stderr, "shell: starting %s (%s)\n", argv[1], executable);
    for (int i = 0; i < n; i++) {
        setenv(keys[i], values[i], 1);
        fprintf(stderr, "boot: %s=%s\n", keys[i], values[i]);   // every one: a report then shows which of the game's settings reached the engine
    }
    if (cwd && *cwd) chdir(cwd);
    front_hud_env();   // before the early loading below can make a device (main.m's order)
    // dlopen + dlsym + dladdr hand the framework's mach header to the loader, after the environment and before loading starts
    // (the design's order, FrameworkBootstrap.m in the one-game app). RTLD_LAZY: only its sections and the marker are needed.
    void *h = dlopen(executable, RTLD_LAZY);
    if (!h) {
        snprintf(err, sizeof err, "The game's code could not be loaded: %s", dlerror() ?: "no reason given");
        fprintf(stderr, "[RED] boot: dlopen %s failed: %s\n", executable, err);
        return err;
    }
    dlerror();
    void *sym = dlsym(h, marker);
    Dl_info info;
    if (!sym || !dladdr(sym, &info) || !info.dli_fbase) {
        snprintf(err, sizeof err, "The game's code is damaged (no %s symbol). Install the game again.", marker);
        fprintf(stderr, "[RED] boot: %s: %s\n", executable, err);
        return err;
    }
    dm_set_game_image_header(info.dli_fbase);   // valid for the process's life: the framework is never dlclose()d
    fprintf(stderr, "boot: game framework %s (handle %p, image base %p)\n", executable, h, info.dli_fbase);
    g_argc = argc; g_argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_argv[i] = strdup(argv[i]);
    dm_host_preload_start(g_argc, g_argv);   // module tables, relocation and take-over in the background while the game view comes up
    g_boot_ok = 1;
    return NULL;
}

void dm_shell_present_game(void) {
    UIWindow *window = nil;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow || !window) window = w;
    if (!window) { fprintf(stderr, "[RED] shell: no window for the game view\n"); return; }
    UIViewController *game = front_ios_root();   // DmRootVC: landscape, its viewDidAppear starts the game (no settings screen: dm_show_launcher)
    window.rootViewController = game;
    [game setNeedsUpdateOfSupportedInterfaceOrientations];
    [window.windowScene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape]
                                                errorHandler:^(NSError *e) { fprintf(stderr, "shell: landscape request: %s\n", e.localizedDescription.UTF8String); }];
}

// ---- what front_ios.m takes from main.m and Launcher.swift ----
static void *boot_thread(void *p) {
    (void)p;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);   // crash printing must work on the host thread too
    int rc = dm_main(g_argc, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}
void front_ios_boot(void) {   // front_ios.m, once the game view exists
    static int once; if (once++ || !g_boot_ok) return;
    void front_ios_audio_session(void); front_ios_audio_session();
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t; pthread_create(&t, &a, boot_thread, NULL); pthread_detach(t);
}
int front_ios_boot_ok(void) { return g_boot_ok; }
const char *front_ios_boot_message(void) { return NULL; }
const char *front_ios_device_message(void) { return NULL; }   // main.m says why the GPU cannot run the games (no BC textures, seedds#704), the shell does its own device handling: NULL = supported
// The shell found the data before Play (DataLocator, EngineLaunchPlan): DM_ROMFS is set and the game view only checks it is still there
int dm_locate_game_data(void) {
    const char *r = getenv("DM_ROMFS"); struct stat st;
    if (r && !stat(r, &st)) return 1;
    fprintf(stderr, "[RED] shell: the game data at %s is gone\n", r ?: "(not set)");
    return 0;
}
// The shell showed its own settings before Play: the game view starts the game at once
bool dm_show_launcher(void *view_controller, void (*done)(void)) { (void)view_controller; (void)done; return false; }

@interface App : UIResponder <UIApplicationDelegate>
@end
@implementation App
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    return [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role];
}
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [ShellLibrary makeController];
    [self.window makeKeyAndVisible];
}
// front_ios.m ignores these until the game view exists
- (void)sceneWillResignActive:(UIScene *)s { front_ios_focus_event(0); }
- (void)sceneDidEnterBackground:(UIScene *)s { front_ios_focus_event(1); }
- (void)sceneWillEnterForeground:(UIScene *)s { front_ios_focus_event(2); }
- (void)sceneDidBecomeActive:(UIScene *)s { front_ios_focus_event(3); }
@end

// The performance HUD (the per-game setting DM_METAL_HUD) needs MTL_HUD_ENABLED in the environment before Metal starts in this
// process, and the library's views start it long before Play: set at Play it comes too late and the HUD stays off. So the shell turns
// Metal's HUD on for every start, here before UIApplicationMain, and the game's own setting decides at Play: front_ios.m sets the game
// layer's developerHUDProperties mode to default, or with DM_HUD_PER_LAYER to disabled. A change of the setting in the library applies
// at the next Play, as every other setting does (verified on an iPhone 2026-10-30: on and off in one start each, no HUD over the
// library). MTL_HUD_HIDE_METALFX_METRICS stays unset, so the HUD shows MetalFX's rows (input and output size) while the Upscaling
// setting MetalFX scales the picture.
static void metal_hud_per_layer(void) {
    if (getenv("MTL_HUD_ENABLED")) { fprintf(stderr, "shell: MTL_HUD_ENABLED given at launch, the HUD follows it\n"); return; }   // Xcode, devicectl: it wins
    setenv("MTL_HUD_ENABLED", "1", 1);
    setenv("DM_HUD_PER_LAYER", "1", 1);
    fprintf(stderr, "shell: Metal's HUD is ready, each game's Performance HUD setting turns it on at Play\n");
}

int main(int argc, char **argv) {
    @autoreleasepool {
        log_to([documents() stringByAppendingPathComponent:@"shell-log.txt"]);
        NSDictionary *info = NSBundle.mainBundle.infoDictionary;
        fprintf(stderr, "shell: %s %s, commit %s, shader chain %s, iOS %s\n", [info[@"CFBundleIdentifier"] UTF8String], [info[@"CFBundleShortVersionString"] UTF8String] ?: "?",
                [info[@"DMCommit"] UTF8String] ?: "not recorded", [info[@"DMChain"] UTF8String] ?: "?", UIDevice.currentDevice.systemVersion.UTF8String);
        // Free space where the game data goes: the Mac launcher reads this line before it copies a game's data (many GB)
        NSNumber *avail = [[NSURL fileURLWithPath:documents()] resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:nil][NSURLVolumeAvailableCapacityForImportantUsageKey];
        fprintf(stderr, "shell: free space %lld bytes\n", avail ? avail.longLongValue : -1LL);
        for (NSString *fw in [NSFileManager.defaultManager contentsOfDirectoryAtPath:NSBundle.mainBundle.privateFrameworksPath error:nil])
            fprintf(stderr, "shell: framework %s\n", fw.UTF8String);
        metal_hud_per_layer();
    }
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); }
}
