// tvOS entry point. The bundled game is immutable; writable cache and saves
// use Caches. tvOS local storage can be purged by the system.
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <os/log.h>
#include <os/proc.h>
#include <mach/mach.h>
#include "game_profile.h"
#include "dm.h"
#include "front.h"
#import "romfs_patch.h"

UIViewController *front_tv_root(void);
void front_tv_focus(int active);
void front_tv_resources_ready(NSString *failure);
BOOL tv_unpack_shaders(NSString *writable, NSString *expectedSHA256, NSString **failure);
int dm_main(int argc, char **argv);
static char *host_argv[3];
static int boot_ready;
static int resources_pending;
static int shaders_pending;
static int preparation_started;
static int host_started;
static NSString *boot_writable;
static NSString *boot_romfs;
static NSString *boot_patch_bundle;
static int early_boot_diag_enabled(void) {
    const char *enabled = getenv("DM_TV_EARLY_BOOT_DIAG");
    return enabled && !strcmp(enabled, "1");
}
// Opt-in process-limit diagnostics also run while the render thread is loading.
// Metal allocation totals alone do not identify the process's dirty-memory cap.
static void memory_diag_sample(const char *reason) {
    task_vm_info_data_t vm = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO,
        (task_info_t)&vm, &count);
    size_t available = os_proc_available_memory();
    fprintf(stderr, "tvOS memory: reason=%s pid=%d task_info=%d footprint=%llu resident=%llu virtual=%llu available=%zu footprint_plus_available=%llu\n",
        reason, getpid(), result, (unsigned long long)vm.phys_footprint,
        (unsigned long long)vm.resident_size, (unsigned long long)vm.virtual_size,
        available, (unsigned long long)vm.phys_footprint + available);
}
static void *memory_diag_loop(void *unused) {
    (void)unused;
    for (;;) { memory_diag_sample("sample"); usleep(250000); }
    return NULL;
}
static void memory_diag_start(void) {
    const char *enabled = getenv("DM_TV_MEMORY_DIAG");
    if (!enabled || strcmp(enabled, "1")) return;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, memory_diag_loop, NULL)) pthread_detach(thread);
}
static void *prepare_resources(void *unused) {
    @autoreleasepool {
        NSString *failure = nil;
        BOOL ok = !shaders_pending || tv_unpack_shaders(boot_writable,
            [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMShaderArchiveSHA256"], &failure);
        NSString *overlay = nil;
        if (ok && boot_patch_bundle) {
            NSString *game = [NSString stringWithFormat:@"%016llX",
                (unsigned long long)DM_GAME_PROGRAM_ID];
            ok = tv_materialize_romfs_patch(boot_romfs, boot_writable,
                boot_patch_bundle, game, &overlay, &failure);
        }
        if (ok) {
            if (shaders_pending)
                setenv("DM_MSL_DIR", [boot_writable stringByAppendingPathComponent:@"msl"].fileSystemRepresentation, 1);
            if (overlay) {
                setenv("DM_ROMFS_OVERLAY", overlay.fileSystemRepresentation, 1);
                NSString *redirects = [overlay stringByAppendingPathComponent:@"redirects.tsv"];
                setenv("DM_ROMFS_REDIRECTS", redirects.fileSystemRepresentation, 1);
            }
            boot_ready = 1; resources_pending = 0;
            dm_host_preload_start(2, host_argv);
        }
        if (!ok && !failure) failure = @"Game files could not be prepared.";
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ok) preparation_started = 0; // a later foreground/start event may retry
            front_tv_resources_ready(ok ? nil : failure);
        });
    }
    return NULL;
}
int front_tv_ready(void) { return boot_ready; }
int front_tv_preparing(void) { return resources_pending; }

static void *validate_loader(void *unused) {
    (void)unused;
    void crash_install(void); crash_install();
    g_t0_ns = now_ns();
    static const char *modules[] = DM_GAME_MODULES;
    load_modules(host_argv[1], modules, DM_GAME_NMOD);
    int (*compare)(const void *, const void *, size_t) = guest_sym("memcmp");
    if (!compare || !module_of((const void *)compare) ||
        compare("same", "same", 4) || compare("a", "b", 1) >= 0 || compare("b", "a", 1) <= 0)
        die("tvOS loader validation: original guest memcmp execution failed");
    fprintf(stderr, "tvOS loader validation PASS: signed guest execution, static RW data, key pages and relocation\n");
    NSLog(@"tvOS loader validation PASS: signed guest execution, static RW data, key pages and relocation");
    return NULL;
}

static void *run_host(void *unused) {
    (void)unused;
    dm_main(2, host_argv);
    return NULL;
}
void front_tv_start(void) {
    if (resources_pending) {
        if (!preparation_started) {
            pthread_attr_t attr;
            int result = pthread_attr_init(&attr);
            int attrInitialized = result == 0;
            if (!result) result = pthread_attr_setstacksize(&attr, 8u << 20);
            pthread_t thread;
            if (!result) result = pthread_create(&thread, &attr, prepare_resources, NULL);
            if (attrInitialized) {
                int destroyResult = pthread_attr_destroy(&attr);
                if (destroyResult) fprintf(stderr, "[RED] tvOS resource thread attributes could not be destroyed: %d\n", destroyResult);
            }
            if (!result) {
                preparation_started = 1;
                pthread_detach(thread);
            } else {
                fprintf(stderr, "[RED] tvOS resource preparation thread creation failed: %d\n", result);
                preparation_started = 0;
                front_tv_resources_ready(@"Could not start game-file preparation. Reopen the app to retry.");
            }
        }
        return;
    }
    if (!boot_ready || host_started) return;
    pthread_attr_t attr; int result = pthread_attr_init(&attr);
    int attrInitialized = result == 0;
    if (!result) result = pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread;
    if (!result) result = pthread_create(&thread, &attr, run_host, NULL);
    if (attrInitialized) {
        int destroyResult = pthread_attr_destroy(&attr);
        if (destroyResult) fprintf(stderr, "[RED] tvOS host thread attributes could not be destroyed: %d\n", destroyResult);
    }
    if (!result) {
        host_started = 1;
        pthread_detach(thread);
    } else {
        fprintf(stderr, "[RED] tvOS host thread creation failed: %d\n", result);
        host_started = 0;
        front_tv_resources_ready(@"Could not start the game. Reopen the app to retry.");
    }
}

static void boot(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *res = NSBundle.mainBundle.resourcePath;
    NSString *foundation = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
        NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"nxnative"];
    NSString *staged = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/nxnative"];
    NSString *cache = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
        NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"nxnative"];
    // Use the platform's writable cache directory. Accept already staged
    // support resources only when the application can actually read them.
    BOOL stagedGame = [fm fileExistsAtPath:[[staged stringByAppendingPathComponent:@"romfs"]
        stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]];
    BOOL cacheGame = [fm fileExistsAtPath:[[cache stringByAppendingPathComponent:@"romfs"]
        stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]];
    NSString *writable = cacheGame || !stagedGame ? cache : staged;
    if (early_boot_diag_enabled()) {
        NSString *early = [NSHomeDirectory() stringByAppendingPathComponent:@"tmp/tv-early-boot.txt"];
        int diagnostic = open(early.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (diagnostic >= 0) {
            NSString *marker = [[staged stringByAppendingPathComponent:@"romfs"] stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER];
            int source = open(marker.fileSystemRepresentation, O_RDONLY);
            int sourceError = source >= 0 ? 0 : errno;
            if (source >= 0) close(source);
            dprintf(diagnostic, "Foundation support=%s home=%s staged_marker=%d support_open_errno=%d cache=%s cache_marker=%d selected=%s uid=%u executable=%s\n",
                foundation.fileSystemRepresentation ?: "(nil)", NSHomeDirectory().fileSystemRepresentation,
                stagedGame, sourceError, cache.fileSystemRepresentation, cacheGame, writable.fileSystemRepresentation,
                getuid(), NSBundle.mainBundle.executablePath.fileSystemRepresentation);
            close(diagnostic);
        }
    }
    boot_writable = writable;
    [fm createDirectoryAtPath:writable withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *log = [writable stringByAppendingPathComponent:@"log.txt"];
    freopen(log.fileSystemRepresentation, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0);
    fprintf(stderr, "tvOS boot: writable=%s\n", writable.fileSystemRepresentation);
    for (NSString *folder in @[@"save", @"temp"]) [fm createDirectoryAtPath:
        [writable stringByAppendingPathComponent:folder] withIntermediateDirectories:YES attributes:nil error:nil];
    for (NSString *ef in @[[writable stringByAppendingPathComponent:@"env.txt"],
                          [res stringByAppendingPathComponent:@"dm/env.txt"]]) {
        NSString *txt = [NSString stringWithContentsOfFile:ef encoding:NSUTF8StringEncoding error:nil];
        for (NSString *line in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
            NSString *s = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            NSRange eq = [s rangeOfString:@"="];
            if (!s.length || [s hasPrefix:@"#"] || eq.location == NSNotFound || !eq.location) continue;
            setenv([s substringToIndex:eq.location].UTF8String,
                   [s substringFromIndex:eq.location + 1].UTF8String, 0);
        }
    }
    NSString *romfs = [res stringByAppendingPathComponent:@"romfs"];
    NSString *external = [writable stringByAppendingPathComponent:@"romfs"];
    if ([fm fileExistsAtPath:[external stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]]) romfs = external;
    boot_romfs = romfs;
    boot_ready = [fm fileExistsAtPath:[romfs stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]];
    NSString *externalMSL = [writable stringByAppendingPathComponent:@"msl"];
    NSString *bundledMSL = [res stringByAppendingPathComponent:@"msl"];
    NSString *archiveSHA = [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMShaderArchiveSHA256"];
    NSString *importedSHA = [[NSString stringWithContentsOfFile:[externalMSL stringByAppendingPathComponent:@".archive-sha256"]
        encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    BOOL externalDirectory = NO, bundledDirectory = NO;
    [fm fileExistsAtPath:externalMSL isDirectory:&externalDirectory];
    [fm fileExistsAtPath:bundledMSL isDirectory:&bundledDirectory];
    BOOL imported = externalDirectory && archiveSHA.length == 64 && [importedSHA isEqualToString:archiveSHA];
    BOOL archiveAvailable = [fm fileExistsAtPath:[writable stringByAppendingPathComponent:@"msl.tar.gz"]];
    // Prefer a verified external import.  Otherwise an available transfer must
    // be imported before boot; with no transfer, only immutable bundled shaders
    // are trusted.  A stale writable directory is never selected by existence.
    shaders_pending = boot_ready && !imported && (archiveAvailable || !bundledDirectory);
    const char *patchSpec = getenv("DM_ROMFS_PATCH");
    if (patchSpec && patchSpec[0]) {
        NSString *specified = @(patchSpec);
        boot_patch_bundle = specified.isAbsolutePath ? specified :
            [writable stringByAppendingPathComponent:specified];
        fprintf(stderr, "tvOS RomFS patch: explicitly enabled from %s\n",
            boot_patch_bundle.fileSystemRepresentation);
    }
    resources_pending = boot_ready && (shaders_pending || boot_patch_bundle != nil);
    if (imported) fprintf(stderr, "tvOS resources: verified external import marker matches archive\n");
    else if (archiveAvailable) fprintf(stderr, "tvOS resources: shader archive is pending verified import\n");
    else if (bundledDirectory) fprintf(stderr, "tvOS resources: using immutable bundled shaders\n");
    else if (boot_ready) fprintf(stderr, "[RED] tvOS resources: no verified external, transfer, or bundled shaders\n");
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
    setenv("DM_SAVE", [writable stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 0);
    setenv("DM_TEMP", [writable stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 0);
    NSString *msl = imported ? externalMSL : bundledMSL;
    setenv("DM_MSL_DIR", msl.fileSystemRepresentation, 0);
    setenv("DM_NVN_SHMAP", [res stringByAppendingPathComponent:@"dm/shader_map.tsv"].fileSystemRepresentation, 0);
    setenv("DM_FONT_DIR", [res stringByAppendingPathComponent:@"dm/font"].fileSystemRepresentation, 0);
    setenv("DM_LANG", "zh-CN", 0); setenv("DM_PRESENT_INTERVAL", "game", 0);
    setenv("DM_PIPELINE_CACHE", "off", 0); setenv("DM_NVN_MIN_REDUCTION", "1", 0);
    setenv("DM_NVN_CPU_DECODE_CACHE", "0", 0); setenv("DM_SHOW_FPS", "0", 0);
    chdir(writable.fileSystemRepresentation);
    memory_diag_start();
    host_argv[0] = strdup("dmhost"); host_argv[1] = strdup([res stringByAppendingPathComponent:@"dm"].fileSystemRepresentation);
    if (getenv("DM_TV_VALIDATE_LOADER")) {
        boot_ready = 0; resources_pending = 0;
        pthread_t thread;
        int result = pthread_create(&thread, NULL, validate_loader, NULL);
        if (!result) pthread_detach(thread);
        else fprintf(stderr, "[RED] tvOS loader validation thread creation failed: %d\n", result);
    }
    else if (boot_ready && !resources_pending) dm_host_preload_start(2, host_argv);
    if (resources_pending) boot_ready = 0;
    else if (!boot_ready && !getenv("DM_TV_VALIDATE_LOADER"))
        fprintf(stderr, "tvOS boot: game resources not staged yet\n");
}

@interface TVSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end
@implementation TVSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = front_tv_root(); [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)scene { front_tv_focus(0); }
- (void)sceneDidBecomeActive:(UIScene *)scene { front_tv_focus(1); }
@end
@interface TVApp : UIResponder <UIApplicationDelegate>
@end
@implementation TVApp
- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application {
    const char *enabled = getenv("DM_TV_MEMORY_DIAG");
    if (enabled && !strcmp(enabled, "1")) memory_diag_sample("UIKit-warning");
}
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = TVSceneDelegate.class; return c;
}
@end
int main(int argc, char **argv) {
    // Leave a libc-only marker before autorelease pools, Foundation path lookup,
    // directory creation and UIKit. This separates startup from log-path faults.
    if (early_boot_diag_enabled()) {
        const char *home = getenv("HOME");
        dprintf(STDERR_FILENO, "tvOS main: before Foundation, pid=%d HOME=%s\n", getpid(), home ?: "(unset)");
        os_log(OS_LOG_DEFAULT, "DmTV main entered before Foundation");
        if (home) {
            char path[PATH_MAX]; snprintf(path, sizeof path, "%s/tmp/tv-early-boot.txt", home);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) { dprintf(fd, "before Foundation pid=%d HOME=%s\n", getpid(), home); close(fd); }
        }
    }
    @autoreleasepool { boot(); return UIApplicationMain(argc, argv, nil, NSStringFromClass(TVApp.class)); }
}
