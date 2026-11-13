// Small OOT-only launcher for a sibling dmhost and an existing external build.
// Compile on macOS: clang -fobjc-arc -framework Cocoa -o nxnative oot_launcher.m
// Does not pack game data, modify saves, or change an already running game.
#import <Cocoa/Cocoa.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <math.h>
extern char **environ;

static NSDictionary<NSString *, NSString *> *read_settings(NSString *text) {
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"#"]) continue;
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound || !eq.location) continue;
        NSString *value = [line substringFromIndex:eq.location + 1];
        if ([value hasSuffix:@"\r"]) value = [value substringToIndex:value.length - 1];
        result[[line substringToIndex:eq.location]] = value; // last assignment wins
    }
    return result;
}

// Leave all unrelated lines, comments, paths and their newline convention intact.
static NSString *merge_settings(NSString *text, NSDictionary<NSString *, NSString *> *changes) {
    NSMutableArray *lines = [[text componentsSeparatedByString:@"\n"] mutableCopy];
    NSMutableSet *found = [NSMutableSet set];
    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *line = lines[i];
        if ([line hasPrefix:@"#"]) continue;
        NSRange eq = [line rangeOfString:@"="];
        if (eq.location == NSNotFound) continue;
        NSString *key = [line substringToIndex:eq.location], *value = changes[key];
        if (!value) continue;
        lines[i] = [NSString stringWithFormat:@"%@=%@%@", key, value, [line hasSuffix:@"\r"] ? @"\r" : @""];
        [found addObject:key];
    }
    NSMutableString *result = [[lines componentsJoinedByString:@"\n"] mutableCopy];
    NSString *newline = [text containsString:@"\r\n"] ? @"\r\n" : @"\n";
    if (result.length && ![result hasSuffix:@"\n"]) [result appendString:newline];
    for (NSString *key in [[changes allKeys] sortedArrayUsingSelector:@selector(compare:)])
        if (![found containsObject:key]) [result appendFormat:@"%@=%@%@", key, changes[key], newline];
    return result;
}

static void show_error(NSString *message) {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Impossible de lancer le jeu";
    alert.informativeText = message;
    [alert addButtonWithTitle:@"Fermer"];
    [alert runModal];
}

@interface OOTSettings : NSObject <NSWindowDelegate>
@property(strong) NSWindow *window;
@property(strong) NSButton *upscale;
@property(strong) NSButton *interpolation;
@property(strong) NSPopUpButton *fps;
@property(strong) NSTextField *summary;
@property BOOL accepted;
- (instancetype)initWithSettings:(NSDictionary<NSString *, NSString *> *)settings;
- (void)update:(id)sender;
- (void)play:(id)sender;
- (void)cancel:(id)sender;
- (NSDictionary<NSString *, NSString *> *)changes;
@end

@implementation OOTSettings
- (instancetype)initWithSettings:(NSDictionary<NSString *, NSString *> *)settings {
    if (!(self = [super init])) return nil;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 312)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"Ocarina of Time";
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    NSView *view = _window.contentView;
    NSTextField *title = [NSTextField labelWithString:@"Réglages du jeu"];
    title.font = [NSFont boldSystemFontOfSize:22];
    title.frame = NSMakeRect(20, 256, 480, 32);
    [view addSubview:title];
    NSTextField *resolution = [NSTextField labelWithString:@"Affichage 2560 × 1440"];
    resolution.textColor = NSColor.secondaryLabelColor;
    resolution.frame = NSMakeRect(20, 232, 480, 24);
    [view addSubview:resolution];
    _upscale = [NSButton checkboxWithTitle:@"Upscaling MetalFX" target:self action:@selector(update:)];
    _upscale.frame = NSMakeRect(20, 188, 480, 24);
    _upscale.state = [settings[@"DM_UPSCALER"] isEqualToString:@"metalfx"] ? NSControlStateValueOn : NSControlStateValueOff;
    [view addSubview:_upscale];
    _interpolation = [NSButton checkboxWithTitle:@"Interpolation d’images MetalFX" target:self action:@selector(update:)];
    _interpolation.frame = NSMakeRect(20, 152, 480, 24);
    _interpolation.state = [settings[@"DM_OOT_INTERPOLATION_PREVIEW"] isEqualToString:@"3"] ? NSControlStateValueOn : NSControlStateValueOff;
    [view addSubview:_interpolation];
    NSTextField *fpsLabel = [NSTextField labelWithString:@"Cadence cible"];
    fpsLabel.frame = NSMakeRect(20, 112, 172, 24);
    [view addSubview:fpsLabel];
    _fps = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(244, 112, 256, 28) pullsDown:NO];
    _fps.menu.autoenablesItems = NO;
    [_fps addItemWithTitle:@"60 FPS"]; _fps.lastItem.tag = 60;
    [_fps addItemWithTitle:@"90 FPS — pas encore disponible"]; _fps.lastItem.tag = 90; _fps.lastItem.enabled = NO;
    [_fps addItemWithTitle:@"120 FPS"]; _fps.lastItem.tag = 120;
    _fps.target = self; _fps.action = @selector(update:);
    NSInteger native = [settings[@"DM_FPS_TARGET"] integerValue];
    [_fps selectItemWithTag:(_interpolation.state == NSControlStateValueOn && native >= 60) ? 120 : 60];
    [view addSubview:_fps];
    _summary = [NSTextField wrappingLabelWithString:@""];
    _summary.textColor = NSColor.secondaryLabelColor;
    _summary.frame = NSMakeRect(20, 64, 480, 40);
    [view addSubview:_summary];
    NSButton *quit = [NSButton buttonWithTitle:@"Quitter" target:self action:@selector(cancel:)];
    quit.frame = NSMakeRect(292, 20, 100, 32); quit.keyEquivalent = @"\033";
    [view addSubview:quit];
    NSButton *play = [NSButton buttonWithTitle:@"Jouer" target:self action:@selector(play:)];
    play.frame = NSMakeRect(400, 20, 100, 32); play.keyEquivalent = @"\r";
    [view addSubview:play];
    [self update:nil];
    [_window center];
    return self;
}
- (void)update:(id)sender {
    (void)sender;
    BOOL fi = self.interpolation.state == NSControlStateValueOn;
    [self.fps.menu itemWithTag:120].enabled = fi;
    if (!fi && self.fps.selectedTag != 60) [self.fps selectItemWithTag:60];
    NSInteger target = self.fps.selectedTag;
    self.summary.stringValue = fi
        ? [NSString stringWithFormat:@"Cible : %ld images natives + %ld interpolées par seconde.\nLes vidéos à 60 FPS gardent leur cadence native.", (long)(target/2), (long)(target/2)]
        : @"Cible : 60 images natives par seconde. Interpolation désactivée.";
}
- (void)play:(id)sender { (void)sender; self.accepted = YES; [NSApp stopModal]; }
- (void)cancel:(id)sender { (void)sender; self.accepted = NO; [NSApp stopModal]; }
- (BOOL)windowShouldClose:(NSWindow *)sender { (void)sender; [self cancel:nil]; return NO; }
- (NSDictionary<NSString *, NSString *> *)changes {
    BOOL fi = self.interpolation.state == NSControlStateValueOn;
    BOOL upscale = self.upscale.state == NSControlStateValueOn;
    return @{@"DM_WINDOW":@"1", @"DM_FPS_TARGET":[NSString stringWithFormat:@"%ld", (long)(fi ? self.fps.selectedTag/2 : 60)],
        @"DM_PRESENT_HZ":@"60", @"DM_OOT_INTERPOLATION_PREVIEW":fi ? @"3" : @"0",
        @"DM_OOT_FRAMEGEN_ASYNC":fi ? @"1" : @"0", @"DM_PRESENT_UPSCALE":upscale ? @"metalfx" : @"off",
        @"DM_UPSCALER":upscale ? @"metalfx" : @"game", @"DM_UPSCALER_EXPERIMENTAL":upscale ? @"1" : @"0",
        @"DM_OOT_HDR":@"0", @"DM_OOT_HDR_OUTPUT_ALIAS":@"0", @"DM_DRAWABLE_SIZE":@"2560x1440",
        @"DM_PRESENT_FIFO":@"1", @"DM_PRESENT_LINK":@"0", @"DM_PRESENT_SNAP":@"2",
        @"DM_SCREEN_MATCH":@"0", @"DM_SCREEN_NATIVE":@"0", @"DM_FPS_HOLD":@"off"};
}
@end

static BOOL is_diagnostic(NSString *key) {
    return [key isEqualToString:@"DM_KEYS"] || [key isEqualToString:@"DM_CTL"] ||
        [key isEqualToString:@"DM_NVN_TSV"] || [key isEqualToString:@"DM_NVN_FLOW"] ||
        [key isEqualToString:@"DM_HITCH_DUMP"] || [key hasPrefix:@"DM_EXIT_"] ||
        [key hasPrefix:@"DM_FRAMEGEN_TIMES"] || [key hasPrefix:@"DM_FRAMEGEN_ASYNC_GPU_TIMES"] ||
        [key hasPrefix:@"DM_LOW_ROUTE_"];
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *home = NSHomeDirectory();
        NSString *config = [home stringByAppendingPathComponent:@"Library/Application Support/nxnative/0400486024DEC000/env.txt"];
        NSError *error = nil;
        BOOL snapshot = argc == 4 && !strcmp(argv[1], "--snapshot");
        BOOL selftest = argc == 2 && !strcmp(argv[1], "--self-test");
        NSString *text = snapshot || selftest ? @"DM_UPSCALER=metalfx\nDM_FPS_TARGET=60\nDM_OOT_INTERPOLATION_PREVIEW=0\n" : [NSString stringWithContentsOfFile:config encoding:NSUTF8StringEncoding error:&error];
        BOOL validate = argc > 1 && !strcmp(argv[1], "--validate-config");
        if (!text) {
            fprintf(stderr, "[RED] launcher: cannot read config: %s\n", error.localizedDescription.UTF8String);
            if (!validate) { [NSApplication sharedApplication]; show_error(error.localizedDescription); }
            return 3;
        }
        NSDictionary *settings = read_settings(text);
        if (validate) {
            NSDictionary *changes = @{@"DM_FPS_TARGET":@"60", @"DM_OOT_INTERPOLATION_PREVIEW":@"0"};
            NSString *merged = merge_settings(text, changes);
            NSDictionary *parsed = read_settings(merged);
            if (![parsed[@"DM_FPS_TARGET"] isEqualToString:@"60"] || ![parsed[@"DM_OOT_INTERPOLATION_PREVIEW"] isEqualToString:@"0"]) return 5;
            NSString *fixture = @"# keep comment\r\nDM_ROOT=/path with spaces\r\nDM_FPS_TARGET=30\r\nOTHER=a=b\r\nDM_FPS_TARGET=120";
            NSString *expected = @"# keep comment\r\nDM_ROOT=/path with spaces\r\nDM_FPS_TARGET=60\r\nOTHER=a=b\r\nDM_FPS_TARGET=60\r\nDM_OOT_INTERPOLATION_PREVIEW=0\r\n";
            if (![merge_settings(fixture, changes) isEqualToString:expected]) return 5;
            puts("launcher: config parsed; managed merge valid; no file or process changed"); return 0;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (snapshot) NSApp.appearance = [NSAppearance appearanceNamed:!strncmp(argv[2], "dark", 4) ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
        OOTSettings *controller = [[OOTSettings alloc] initWithSettings:settings];
        NSMenu *menu = [[NSMenu alloc] init];
        NSMenuItem *appItem = [[NSMenuItem alloc] init]; [menu addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Ocarina of Time"];
        [appMenu addItemWithTitle:@"Quitter Ocarina of Time" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu; NSApp.mainMenu = menu;
        if (snapshot && strstr(argv[2], "-fi")) { controller.interpolation.state = NSControlStateValueOn; [controller.fps selectItemWithTag:60]; [controller update:nil]; }
        if (selftest) {
            assert(![controller.fps.menu itemWithTag:90].enabled);
            for (int up = 0; up < 2; up++) for (int fi = 0; fi < 2; fi++) {
                controller.upscale.state = up; controller.interpolation.state = fi;
                [controller.fps selectItemWithTag:120]; [controller update:nil];
                NSDictionary *c = controller.changes;
                assert([c[@"DM_FPS_TARGET"] isEqualToString:@"60"]);
                assert([c[@"DM_UPSCALER"] isEqualToString:up ? @"metalfx" : @"game"]);
                if (fi) { [controller.fps selectItemWithTag:60]; [controller update:nil]; assert([controller.changes[@"DM_FPS_TARGET"] isEqualToString:@"30"]); }
            }
            for (NSView *v in controller.window.contentView.subviews) {
                NSRect f = v.frame; assert(NSMinX(f) >= 20 && NSMaxX(f) <= 500);
                assert(fmod(f.origin.x, 4) == 0 && fmod(f.origin.y, 4) == 0);
            }
            puts("selector: combinations, native cap mapping, disabled choices and layout grid pass"); return 0;
        }
        [controller.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        [controller.window makeFirstResponder:nil];
        if (snapshot) {
            NSString *path = @(argv[3]);
            NSTimer *capture = [NSTimer timerWithTimeInterval:1 repeats:NO block:^(NSTimer *timer) {
                (void)timer; NSTask *shot = [[NSTask alloc] init]; shot.launchPath = @"/usr/sbin/screencapture";
                shot.arguments = @[@"-x", @"-l", [NSString stringWithFormat:@"%ld", (long)controller.window.windowNumber], path];
                [shot launch]; [shot waitUntilExit]; exit(shot.terminationStatus);
            }];
            [[NSRunLoop currentRunLoop] addTimer:capture forMode:NSModalPanelRunLoopMode];
        }
        [NSApp runModalForWindow:controller.window];
        if (!controller.accepted) return 0;
        // A settings window may remain open while another tool updates unrelated options.
        text = [NSString stringWithContentsOfFile:config encoding:NSUTF8StringEncoding error:&error];
        if (!text) { show_error(error.localizedDescription); return 3; }
        settings = read_settings(text);
        NSString *modules = argc > 1 ? @(argv[1]) : [settings[@"DM_ROOT"] stringByAppendingPathComponent:@"patched_v131072"];
        char self[PATH_MAX], resolved[PATH_MAX]; uint32_t size = sizeof self;
        if (_NSGetExecutablePath(self, &size) || !realpath(self, resolved)) return 2;
        NSString *host = [[@(resolved) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"dmhost"];
        BOOL directory = NO;
        if (access(host.fileSystemRepresentation, X_OK) || !modules || ![[NSFileManager defaultManager] fileExistsAtPath:modules isDirectory:&directory] || !directory) {
            show_error(@"Le moteur ou le dossier du jeu est introuvable. Les réglages n’ont pas été modifiés."); return 4;
        }
        NSDictionary *changes = controller.changes;
        NSString *merged = merge_settings(text, changes);
        NSDictionary *attributes = [[NSFileManager defaultManager] attributesOfItemAtPath:config error:nil];
        if (![merged writeToFile:config atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
            show_error(error.localizedDescription); return 5;
        }
        if (attributes[NSFilePosixPermissions]) [[NSFileManager defaultManager] setAttributes:@{NSFilePosixPermissions:attributes[NSFilePosixPermissions]} ofItemAtPath:config error:nil];
        // Finder normally has none, but never inherit a benchmark/control launch environment.
        NSMutableArray<NSString *> *remove = [NSMutableArray array];
        for (char **entry = environ; *entry; entry++) {
            NSString *line = @(*entry); NSRange eq = [line rangeOfString:@"="];
            if (eq.location == NSNotFound) continue;
            NSString *key = [line substringToIndex:eq.location];
            if ([key hasPrefix:@"DM_"] || [key hasPrefix:@"MTL_"]) [remove addObject:key];
        }
        for (NSString *key in remove) unsetenv(key.UTF8String);
        NSDictionary<NSString *, NSString *> *effective = read_settings(merged);
        for (NSString *key in effective)
            if (!is_diagnostic(key)) setenv(key.UTF8String, effective[key].UTF8String, 1);
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/nxnative"];
        [[NSFileManager defaultManager] createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
        int fd = open([[logs stringByAppendingPathComponent:@"OOT-battery.log"] fileSystemRepresentation], O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        fprintf(stderr, "OOT selector: target=%ld, native=%s, upscale=%s, interpolation=%s\n", (long)controller.fps.selectedTag, getenv("DM_FPS_TARGET"), getenv("DM_UPSCALER"), getenv("DM_OOT_INTERPOLATION_PREVIEW"));
        [controller.window orderOut:nil];
        char *arguments[] = {(char *)host.fileSystemRepresentation, (char *)modules.fileSystemRepresentation, NULL};
        execv(arguments[0], arguments); perror("dmhost"); return 6;
    }
}
