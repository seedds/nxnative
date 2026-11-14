// iOS App entry: set the environment (game files, saves, shader libraries), write the host log to Documents/log.txt, create the window (front_ios.m), then run the host's dm_main on a big-stack thread.
// The game files (RomFS) ship inside the App at <App>/romfs; Documents/romfs.img (one image, scripts/push_romfs.sh; its name in any letter case) or a Documents/romfs folder is also accepted (lets you swap them without reinstalling). The DLC folder aoc/ likewise: Documents/aoc, else <App>/aoc.
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "game_profile.h"   // game profile constants (the RomFS marker path)
#include "front.h"
#include "romfs_image.h"
#include "romfs_identity.h"
#include "player_env.h"
#ifndef DM_PLAYER_BUILD
#define DM_PLAYER_BUILD 0
#endif

extern char **environ;

int dm_main(int argc, char **argv);   // dm/host/main.c compiled in with -Dmain=dm_main
UIViewController *front_ios_root(void);
void front_ios_focus_event(int ev);

static char *g_argv[4]; static int g_argc;

// Documents/romfs.img, one RomFS image file (dm/tools/romfs_image.py, scripts/push_romfs.sh): only a complete one counts. Its index is written last and ends the file, so a copy cut off partway is skipped
static int romfs_image_complete(NSString *path) {
    romfs_img *m = romfs_image_open(path.fileSystemRepresentation);
    if (!m) {
        if (errno != ENOENT) fprintf(stderr, "[RED] boot: invalid RomFS image %s (errno=%d); trying the folders\n", path.fileSystemRepresentation, errno);
        return 0;
    }
    romfs_image_close(m); return 1;
}
// A build made with DM_COPY_ROMFS=NO carries <App>/romfs.id (dm/host/romfs_identity.h): only a Documents/romfs.img with that identity is used. No romfs.id (the default build): any complete image
static int romfs_identity_accepts(NSString *img) {
    romfs_id want, got;
    int rc = romfs_id_check([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.id"].fileSystemRepresentation, img.fileSystemRepresentation, &want, &got);
    if (rc != ROMFS_ID_OK && rc != ROMFS_ID_NONE) fprintf(stderr, "[RED] boot: Documents/%s is not the image this build needs (verdict %d); not used\n", img.lastPathComponent.UTF8String, rc);
    return rc == ROMFS_ID_OK || rc == ROMFS_ID_NONE;
}
// The images in Documents: romfs.img in any letter case, romfs.img itself first. The NSP script writes the image as RomFS.img (extract/<version>/RomFS.img) and the Mac's
// file system ignores letter case, but iOS's does not: a copy made through Finder or the Files app keeps the source's name, which an exact lookup of romfs.img never finds
static NSArray<NSString *> *romfs_document_images(NSString *doc) {
    NSMutableArray<NSString *> *imgs = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:doc error:nil])
        if ([n caseInsensitiveCompare:@"romfs.img"] == NSOrderedSame) [imgs addObject:[doc stringByAppendingPathComponent:n]];
    [imgs sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        BOOL ea = [a.lastPathComponent isEqualToString:@"romfs.img"], eb = [b.lastPathComponent isEqualToString:@"romfs.img"];
        return ea != eb ? (ea ? NSOrderedAscending : NSOrderedDescending) : [a compare:b];
    }];
    return imgs;
}
static NSString *romfs_locate(NSString *prog) {   // prog: a game of several programs, the running program's folder in the app (p<N>); nil otherwise
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    if (prog) {   // the program's files are in the app (Documents holds one image, one program's)
        NSString *p = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:prog], *img = [p stringByAppendingPathComponent:@"romfs.img"], *d = [p stringByAppendingPathComponent:@"romfs"];
        if (romfs_image_complete(img)) return img;
        return [fm fileExistsAtPath:[d stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]] ? d : nil;
    }
    for (NSString *img in romfs_document_images(doc)) {
        if (!romfs_image_complete(img) || !romfs_identity_accepts(img)) continue;
        // A folder copy from before images is now dead weight of up to many GB (often cut off partway). Deleting its 300,000 files here took longer than iOS allows an app to start
        // (Tears of the Kingdom: killed on a black screen), so it is moved aside at once and deleted in the background, leftovers of an interrupted delete included
        NSString *old = [doc stringByAppendingPathComponent:@"romfs"], *tmp = NSTemporaryDirectory();
        if ([fm fileExistsAtPath:old] && [fm moveItemAtPath:old toPath:[tmp stringByAppendingPathComponent:[@"romfs-old-" stringByAppendingString:NSUUID.UUID.UUIDString]] error:nil])
            fprintf(stderr, "boot: Documents/romfs moved aside for deletion (Documents/%s replaces it)\n", img.lastPathComponent.UTF8String);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
            NSFileManager *bfm = [NSFileManager new];
            for (NSString *n in [bfm contentsOfDirectoryAtPath:tmp error:nil]) if ([n hasPrefix:@"romfs-old-"]) [bfm removeItemAtPath:[tmp stringByAppendingPathComponent:n] error:nil];
        });
        return img;
    }
    NSString *bundledImage = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.img"];
    NSString *documentFolder = [doc stringByAppendingPathComponent:@"romfs"];
    if ([fm fileExistsAtPath:[documentFolder stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]]) return documentFolder;
    if (romfs_image_complete(bundledImage)) return bundledImage;
    for (NSString *d in @[[doc stringByAppendingPathComponent:@"romfs"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs"]])
        if ([fm fileExistsAtPath:[d stringByAppendingPathComponent:@(DM_GAME_ROMFS_MARKER)]]) return d;   // only a directory containing the game profile's romfs_marker path counts (Mario: System/Resource, the resource size table)
    return nil;
}

// Add-on content (DLC): each DLC's RomFS image at <dir>/<index>.img (or a legacy <index>/ folder). Like the game files, Documents/aoc comes before <App>/aoc.
// A DM_AOC_DIR that is already set (env.txt, the launch environment) wins. The host mounts and counts the DLC from DM_AOC_DIR (hle_fs.c, hle_misc.c)
static void aoc_locate(void) {
    if (getenv("DM_AOC_DIR")) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    for (NSString *d in @[[doc stringByAppendingPathComponent:@"aoc"], [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"aoc"]]) {
        BOOL dir = NO;
        if (![fm fileExistsAtPath:d isDirectory:&dir] || !dir) continue;
        setenv("DM_AOC_DIR", d.fileSystemRepresentation, 0);
        fprintf(stderr, "boot: DLC from %s\n", d.fileSystemRepresentation);
        return;
    }
}

// Boot prerequisites (done before UIApplicationMain): directories / environment variables / log -- the host's early load reads these
// Why a slim build found no usable game files: the verdict on Documents/romfs.img (the first of romfs_document_images), in the log and for the screen (front_ios_boot_message). NULL when the build has no romfs.id
static char g_boot_message[640];
const char *front_ios_boot_message(void) { return g_boot_message[0] ? g_boot_message : NULL; }
// Why this device cannot run the games: its GPU has no BC texture formats, which the games' textures use and the host does not decompress (dm/gfx/tex/README.md; the A16 and older,
// GitHub issue #704), so the screen says so instead of Metal aborting at the game's first BC texture. NULL on a supported device. Called by startGame (front_ios.m) once the launcher has set
// its switches: this makes the process's first Metal device, and Metal reads MTL_HUD_ENABLED then; checked at launch, it kept the launcher's Performance HUD off (GitHub issue #840)
const char *front_ios_device_check(void) {
    static char msg[320]; static int done;
    if (done++) return msg[0] ? msg : NULL;
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();   // the system default device, the one the graphics layers take (mtl_init repeats this check for the other routes)
    if (gpu.supportsBCTextureCompression) return NULL;
    snprintf(msg, sizeof msg, "This device's GPU (%s) cannot read BC-compressed textures, which the games use. It needs an iPhone 15 Pro or newer (A17 Pro chip or later) or an iPad with an M1 chip or later.",
             gpu ? gpu.name.UTF8String : "no Metal device");
    fprintf(stderr, "[RED] boot: %s\n", msg);
    return msg;
}
static void romfs_identity_report(NSString *doc) {
    romfs_id want, got;
    int rc = romfs_id_check([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs.id"].fileSystemRepresentation,
                            (romfs_document_images(doc).firstObject ?: [doc stringByAppendingPathComponent:@"romfs.img"]).fileSystemRepresentation, &want, &got);
    if (rc == ROMFS_ID_OK || rc == ROMFS_ID_NONE) return;
    romfs_id_message(rc, &want, &got, NSBundle.mainBundle.bundleIdentifier.UTF8String ?: "<bundle ID>", g_boot_message, sizeof g_boot_message);
    fprintf(stderr, "[RED] boot: %s\n", g_boot_message);
}

static NSString *program_env(void);
static int boot_env(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSString *res = NSBundle.mainBundle.resourcePath;
    for (NSString *d in @[@"save", @"temp"]) [fm createDirectoryAtPath:[doc stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
    // Keep game files and scratch data out of device backups (Documents is backed up by default; a RomFS placed there is many GB). Saves, logs and the pipeline list stay in the backup
    for (NSString *d in [@[@"romfs", @"aoc", @"temp"] arrayByAddingObjectsFromArray:[romfs_document_images(doc) valueForKey:@"lastPathComponent"]]) {
        NSURL *u = [NSURL fileURLWithPath:[doc stringByAppendingPathComponent:d]]; NSError *e = nil;
        if ([fm fileExistsAtPath:u.path] && ![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e]) fprintf(stderr, "boot: %s could not be excluded from backup: %s\n", d.UTF8String, e.localizedDescription.UTF8String);
    }
    // Log: the host only writes to stderr; redirect it to Documents/log.txt (the previous one is kept as log.prev.txt), retrieve it from the Files app when something goes wrong
    NSString *log = [doc stringByAppendingPathComponent:@"log.txt"], *prev = [doc stringByAppendingPathComponent:@"log.prev.txt"];
    [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
    if (freopen(log.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    // Environment switch files (for on-device diagnostics: a sideloaded app cannot be given environment variables from outside): one KEY=VALUE per line, # starts a comment; Documents/env.txt takes precedence over dm/env.txt in the bundle (put there by build_ipa.sh's DM_ENV_FILE)
    NSMutableArray<NSString *> *from_file = [NSMutableArray array], *from_launch = [NSMutableArray array];
    for (char **e = environ; *e; e++) {   // set at launch (Xcode, devicectl): before the env files, and the launcher keeps them
        const char *eq = strchr(*e, '='); NSString *k = eq && !strncmp(*e, "DM_", 3) ? [[NSString alloc] initWithBytes:*e length:(NSUInteger)(eq - *e) encoding:NSUTF8StringEncoding] : nil;
        if (k) [from_launch addObject:k];
    }
#if DM_PLAYER_BUILD
    // Player-facing choices belong to the launcher, including saved choices.
    // Neither old diagnostic files nor bundled defaults may lock those rows.
    NSMutableSet<NSString *> *launcher_keys = [NSMutableSet set];
    NSData *settings_data = [NSData dataWithContentsOfFile:[res stringByAppendingPathComponent:@"dm/launcher.json"]];
    NSDictionary *settings_file = settings_data ? [NSJSONSerialization JSONObjectWithData:settings_data options:0 error:nil] : nil;
    for (NSDictionary *row in settings_file[@"settings"]) if ([row[@"env"] isKindOfClass:NSString.class]) [launcher_keys addObject:row[@"env"]];
#endif
    for (NSString *ef in @[[doc stringByAppendingPathComponent:@"env.txt"], [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"env.txt"]]) {
        NSString *txt = [NSString stringWithContentsOfFile:ef encoding:NSUTF8StringEncoding error:nil];
        for (NSString *raw in [txt componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
            NSString *ln = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; NSRange eq = [ln rangeOfString:@"="];
            if (!ln.length || [ln hasPrefix:@"#"] || eq.location == NSNotFound || !eq.location) continue;
            NSString *k = [ln substringToIndex:eq.location], *v = [ln substringFromIndex:eq.location + 1];
#if DM_PLAYER_BUILD
            if (dm_player_diagnostic_key(k.UTF8String) || [launcher_keys containsObject:k] ||
                ([launcher_keys containsObject:@"DM_METAL_HUD"] && [k isEqualToString:@"MTL_HUD_ENABLED"])) continue;
#endif
            if (!getenv(k.UTF8String)) { setenv(k.UTF8String, v.UTF8String, 0); [from_file addObject:k]; fprintf(stderr, "boot: env %s=%s (%s)\n", k.UTF8String, v.UTF8String, ef.lastPathComponent.UTF8String); }
        }
    }
    setenv("DM_ENV_FILE_KEYS", [from_file componentsJoinedByString:@" "].UTF8String, 1);   // the launcher keeps what an env file set (LauncherSettings.swift)
    setenv("DM_ENV_LAUNCH_KEYS", [from_launch componentsJoinedByString:@" "].UTF8String, 1);   // and what the launch environment set (docs/frontends.md, Saved choices)
    front_hud_env();   // an env file's DM_METAL_HUD; nothing in this process makes a Metal device before startGame (front_ios.m), which applies the launcher's
    // Game language (nn::oe::GetDesiredLanguage, hle_oe.c): the one the IPA was packed with (the NSP script's LANGUAGE / --language → DMLanguage in Info.plist), unless an env file sets DM_LANG
    // (or the launcher, which replaces this default before the game starts)
    NSString *lang = [NSBundle.mainBundle objectForInfoDictionaryKey:@"DMLanguage"];
    if (!getenv("DM_LANG") && [lang isKindOfClass:NSString.class] && lang.length) setenv("DM_LANG", lang.UTF8String, 0);
    fprintf(stderr, "boot: game language %s\n", getenv("DM_LANG") ?: "en-US");
    NSString *dm = program_env();
    if (!dm) { romfs_identity_report(doc); fprintf(stderr, "boot: game files not found (Documents/romfs.img, Documents/romfs or <App>/romfs)\n"); return 0; }
    setenv("DM_SAVE", [doc stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 0);
    setenv("DM_TEMP", [doc stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 0);
    setenv("DM_SYSTEM_DIR", [doc stringByAppendingPathComponent:@"system"].fileSystemRepresentation, 0);
    setenv("DM_GUEST_FILE_DIR", NSTemporaryDirectory().fileSystemRepresentation, 0);   // DM_GUEST_FILE (hle_svc.c) keeps the guest's memory in files there
    setenv("DM_FONT_DIR", [[res stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"font"].fileSystemRepresentation, 0);   // Shared-font stand-ins (standard.ttf / nintendo_ext.ttf): iOS lacks the macOS font path
    aoc_locate();   // add-on content: Documents/aoc, else <App>/aoc; an empty Documents/aoc explicitly selects no DLC
    NSString *pso = [res stringByAppendingPathComponent:@"pso"];   // the pipeline archive made offline (build_ipa.sh, dm/gfx/tools/pso_offline.py): the shader pairs' bases, which the device then does not build
    if ([fm fileExistsAtPath:pso]) setenv("DM_PSO_BUNDLE", pso.fileSystemRepresentation, 0);
    // An app built with --nextendo (build_ipa.sh): the public service profile, unless an env file names another. hle_nextendo_routes.c takes only files of this
    // user's, writable by no one else, and the installer may own the bundle's: a private copy in Library/Application Support each start
    NSString *bundled = [res stringByAppendingPathComponent:@"dm/nextendo-services"];
    if (!getenv("DM_NEXTENDO_ROUTES") && [fm fileExistsAtPath:[bundled stringByAppendingPathComponent:@"routes.tsv"]]) {
        NSString *copy = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:@"nxnative/nextendo-services"];
        BOOL ok = [fm createDirectoryAtPath:copy withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:nil];
        // every file of the profile: routes.tsv, provider-ca.der, manifest.json and the pinned server certificates (<SHA-256>.der, dm/tools/nextendo_services.py)
        NSArray<NSString *> *files = [fm contentsOfDirectoryAtPath:bundled error:nil];
        ok = ok && files.count;
        for (NSString *f in files) {
            NSString *to = [copy stringByAppendingPathComponent:f];
            ok = ok && [[NSData dataWithContentsOfFile:[bundled stringByAppendingPathComponent:f]] writeToFile:to atomically:YES] && !chmod(to.fileSystemRepresentation, 0600);
        }
        if (ok) setenv("DM_NEXTENDO_ROUTES", [copy stringByAppendingPathComponent:@"routes.tsv"].fileSystemRepresentation, 0);
        else fprintf(stderr, "[RED] boot: the Nextendo service profile could not be copied to %s; Nextendo mode refuses every service\n", copy.fileSystemRepresentation);
    }
    chdir(doc.fileSystemRepresentation);
    g_argv[0] = strdup("dmhost"); g_argv[1] = strdup(dm.fileSystemRepresentation); g_argc = 2;
    // Module tables / relocation / takeover run early in the background, in parallel with UIKit startup (and the launcher: the settings it sets are read when the game starts).
    // Not with Nextendo's patched main (dm/nextendo, loader.c nextendo_main): which main loads follows the launcher's Network choice, so loading waits for Play
    if (![fm fileExistsAtPath:[dm stringByAppendingPathComponent:@"nextendo/main.fixups.tsv"]]) dm_host_preload_start(g_argc, g_argv);
    return 1;
}

// The running program's files (an app holding the programs of one game, build_ipa.sh PROGRAMS_TSV: Super Mario 3D All-Stars' launcher and the games it starts): dm/programs.txt lists them, and each has
// its own: <App>/p<N>/romfs.img (html and jit beside it), <App>/msl/p<N>, <App>/dm/p<N> (rewriter tables, shader tables, launcher settings) and its modules in sections tagged p<N> (DM_EMBED_TAG, loader.c).
// The host's profile is the program's (DM_PROGRAM, main.c dm_game_cur). oe::ExecuteProgram records the program and its parameters in Documents/program.bin (DM_EXEC_OUT, hle_oe.c), names it on screen, schedules a
// notification that opens the app (front_ios.m front_program_handoff) and ends; this start reads the record and takes it away, so a later start, after the player quit the game, is at the launcher as on the Switch.
// Started from the record, the settings screen stays away (DM_LAUNCHER=0): the player is on the way into a game. Returns the program's dm folder (the host's modules folder), nil when its game files are missing
static NSString *program_env(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0], *res = NSBundle.mainBundle.resourcePath;
    NSString *dmdir = [res stringByAppendingPathComponent:@"dm"], *msl = [res stringByAppendingPathComponent:@"msl"], *prog = nil;
    NSString *plist = [NSString stringWithContentsOfFile:[dmdir stringByAppendingPathComponent:@"programs.txt"] encoding:NSUTF8StringEncoding error:nil];
    if (plist) {
        NSArray *have = [plist componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString *rec = [doc stringByAppendingPathComponent:@"program.bin"]; NSData *d = [NSData dataWithContentsOfFile:rec]; int32_t n = 0;
        if (d.length >= 4) { memcpy(&n, d.bytes, 4); [fm removeItemAtPath:rec error:nil]; setenv("DM_LAUNCHER", "0", 1); }
        if (![have containsObject:[NSString stringWithFormat:@"%d", n]]) { fprintf(stderr, "[RED] boot: program %d is not in this app; program 0\n", n); n = 0; d = nil; }
        prog = [NSString stringWithFormat:@"p%d", n];
        NSString *lp = [doc stringByAppendingPathComponent:@"launch_param.bin"];
        if (d.length > 4) { [[d subdataWithRange:NSMakeRange(4, d.length - 4)] writeToFile:lp atomically:YES]; setenv("DM_LAUNCH_PARAM", lp.fileSystemRepresentation, 1); }
        else { [fm removeItemAtPath:lp error:nil]; unsetenv("DM_LAUNCH_PARAM"); }
        setenv("DM_PROGRAM", [NSString stringWithFormat:@"%d", n].UTF8String, 1);
        setenv("DM_EMBED_TAG", prog.UTF8String, 1);
        setenv("DM_EXEC_OUT", rec.fileSystemRepresentation, 0);
        dmdir = [dmdir stringByAppendingPathComponent:prog]; msl = [msl stringByAppendingPathComponent:prog];
        setenv("DM_APP_DM", dmdir.fileSystemRepresentation, 1);   // the launcher's settings (Launcher.swift)
        fprintf(stderr, "boot: program %d of %s (%zu bytes of launch parameter)\n", n, [[plist stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String, d.length > 4 ? (size_t)d.length - 4 : 0);
    }
    NSString *romfs = romfs_locate(prog);
    if (!romfs) return nil;
    fprintf(stderr, "boot: game files %s\n", romfs.fileSystemRepresentation);
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
    setenv("DM_NVN_SHMAP", [dmdir stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation, 0);
    setenv("DM_MSL_DIR", msl.fileSystemRepresentation, 0);
    return dmdir;
}
static int g_boot_ok;

static void *boot_thread(void *p) {
    (void)p;
    static char alt[65536]; stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);   // crash printing must work on the host thread too
    int rc = dm_main(g_argc, g_argv);
    fprintf(stderr, "boot: dm_main returned %d\n", rc);
    return NULL;
}
void front_ios_boot(void) {   // Called by front_ios.m once the window and Metal view exist
    static int once; if (once++ || !g_boot_ok) return;
    void front_ios_audio_session(void); front_ios_audio_session();   // set a short I/O buffer before the game opens audio output (front_ios.m)
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);   // the Switch main thread stack is set by npdm; the host gives plenty
    pthread_t t; pthread_create(&t, &a, boot_thread, NULL); pthread_detach(t);
}
int front_ios_boot_ok(void) { return g_boot_ok; }

@interface App : UIResponder <UIApplicationDelegate>
@end
@implementation App
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    return [[UISceneConfiguration alloc] initWithName:[s.role isEqualToString:UIWindowSceneSessionRoleExternalDisplayNonInteractive] ? @"TV" : @"Default" sessionRole:s.role];   // a TV (front_ios.m DmTVSceneDelegate)
}
@end

// Scene lifecycle: create the window + hand focus events to front_ios.m
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = front_ios_root();
    [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)s { front_ios_focus_event(0); }
- (void)sceneDidEnterBackground:(UIScene *)s { front_ios_focus_event(1); }
- (void)sceneWillEnterForeground:(UIScene *)s { front_ios_focus_event(2); }
- (void)sceneDidBecomeActive:(UIScene *)s { front_ios_focus_event(3); }
@end

int main(int argc, char **argv) {
    @autoreleasepool { g_boot_ok = boot_env(); }
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); }
}
