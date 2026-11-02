// In-game actions extracted from #104. Launch settings share the launcher's model and preferences.
#import "menus_mac.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "hle.h"
#include "save_backup_mac.h"
#include "save_sync.h"
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_muted, g_mute_unfocused, g_active = 1;
void mac_menus_focus(int active) { g_active = active; dm_audio_mute(g_muted || (g_mute_unfocused && !g_active)); }

static NSString *save_folder(void) { const char *s = getenv("DM_SAVE"); return s && *s ? [NSURL fileURLWithPath:@(s) isDirectory:YES].path : nil; }
static NSString *log_file(void) {
    char p[PATH_MAX]; struct stat st;
    return !fstat(STDERR_FILENO, &st) && S_ISREG(st.st_mode) && !fcntl(STDERR_FILENO, F_GETPATH, p) ? @(p) : nil;
}
static void message(NSString *title, NSString *detail) {
    NSAlert *a = [NSAlert new]; a.messageText = title; a.informativeText = detail; [a runModal];
}

@interface DmMacMenus : NSObject <NSMenuItemValidation>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSDictionary *mode;
@property (nonatomic, copy) NSString *domain;
@property (nonatomic) BOOL busy;
@end
@implementation DmMacMenus
- (BOOL)modeFixed {
    NSString *keys = [NSString stringWithFormat:@"%s %s", getenv("DM_ENV_LAUNCH_KEYS") ?: "", getenv("DM_ENV_FILE_KEYS") ?: ""];
    if ([[keys componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet] containsObject:@"DM_OPMODE"]) return YES;
    // A direct CLI host has no shared app settings; allow a session change only when it has no explicit override.
    return getenv("DM_ENV_LAUNCH_KEYS") == NULL && getenv("DM_OPMODE") != NULL;
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    SEL a = item.action;
    if (a == @selector(chooseMode:)) {
        item.state = dm_opmode() == item.tag ? NSControlStateValueOn : NSControlStateValueOff;
        return !self.modeFixed;
    }
    if (a == @selector(mute:)) item.state = g_muted ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(muteUnfocused:)) item.state = g_mute_unfocused ? NSControlStateValueOn : NSControlStateValueOff;
    if (a == @selector(resize:)) return !(self.window.styleMask & NSWindowStyleMaskFullScreen);
    if (a == @selector(openSave:)) return save_folder() != nil;
    if (a == @selector(openLog:)) return log_file() != nil;
    if (a == @selector(exportSave:)) return !self.busy && save_folder() != nil;
    if (a == @selector(restoreSave:)) return !self.busy && save_folder() != nil && getenv("DM_MAC_RESTORE_DIR") != NULL;
    return YES;
}
- (void)chooseMode:(NSMenuItem *)item {
    if (self.modeFixed) return;
    NSString *value = item.tag ? @"1" : @"0";
    const char *off = getenv("DM_SAVED_CHOICES"); BOOL ignoring = off && !strcmp(off, "0");
    if (self.domain && !ignoring) {
        CFStringRef domain = (__bridge CFStringRef)self.domain;
        CFPreferencesSetAppValue(CFSTR("launcher.DM_OPMODE"), [value isEqual:self.mode[@"default"]] ? NULL : (__bridge CFStringRef)value, domain);
        if (!CFPreferencesAppSynchronize(domain)) { message(@"Console mode was not saved", @"The app could not write its saved choices. Try again."); return; }
    }
    dm_set_opmode((int)item.tag);
}
- (void)mute:(id)sender { (void)sender; g_muted = !g_muted; mac_menus_focus(g_active); }
- (void)muteUnfocused:(id)sender { (void)sender; g_mute_unfocused = !g_mute_unfocused; mac_menus_focus(g_active); }
- (void)resize:(NSMenuItem *)item {
    if (self.window.styleMask & NSWindowStyleMaskFullScreen) return;
    CGFloat scale = self.window.backingScaleFactor;
    NSSize size = NSMakeSize(1280 * item.tag / 100.0 / scale, 720 * item.tag / 100.0 / scale);
    NSRect available = [self.window contentRectForFrameRect:self.window.screen.visibleFrame];
    CGFloat fit = fmin(1, fmin(available.size.width / size.width, available.size.height / size.height));
    size.width *= fit; size.height *= fit;
    NSRect old = self.window.frame, content = [self.window contentRectForFrameRect:old];
    content.size = size;
    NSRect frame = [self.window frameRectForContentRect:content];
    frame.origin = NSMakePoint(NSMidX(old) - frame.size.width / 2, NSMidY(old) - frame.size.height / 2);
    frame = [self.window constrainFrameRect:frame toScreen:self.window.screen];
    [self.window setFrame:frame display:YES animate:YES];
}
- (void)openSave:(id)sender { (void)sender; NSString *p = save_folder(); if (p) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:p]]; }
- (void)openLog:(id)sender { (void)sender; NSString *p = log_file(); if (p) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:p]]]; }
- (void)screenshot:(id)sender {
    (void)sender;
    NSURL *pictures = [NSFileManager.defaultManager URLForDirectory:NSPicturesDirectory inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    NSString *folder = [[pictures.path stringByAppendingPathComponent:@"nxnative"] stringByAppendingPathComponent:@DM_GAME_ID];
    NSError *error;
    if (!folder || ![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:&error]) {
        message(@"Screenshot could not be saved", error.localizedDescription ?: @"The Pictures folder is unavailable."); return;
    }
    NSString *p = [folder stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.png", NSUUID.UUID.UUIDString]];
    dm_gfx_shot_at(1, p.fileSystemRepresentation);
    LOG("front end: screenshot requested: %s", p.fileSystemRepresentation);
}
- (void)exportSave:(id)sender {
    (void)sender; if (self.busy) return;
    self.busy = YES;
    NSSavePanel *panel = [NSSavePanel savePanel]; panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"dmsync"] ?: UTTypeData];
    panel.nameFieldStringValue = [NSString stringWithFormat:@"%@-%@.dmsync", @DM_GAME_ID, NSUUID.UUID.UUIDString];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        self.busy = NO;
        if (result != NSModalResponseOK) return;
        NSURL *destination = panel.URL;
        NSString *save = save_folder(), *out = destination.path;
        if (!save || !out || !destination) return;
        // A backup must never replace a file in the active save or one of its sibling working/device directories.
        NSString *root = save.stringByResolvingSymlinksInPath, *dest = out.stringByResolvingSymlinksInPath;
        for (NSString *suffix in @[@"", @".work", @"_device", @"_device.work"]) {
            NSString *folder = [root stringByAppendingString:suffix];
            if ([dest isEqual:folder] || [dest hasPrefix:[folder stringByAppendingString:@"/"]]) {
                message(@"Choose another backup folder", @"Keep exported backups outside the active save folders."); return;
            }
        }
        self.busy = YES;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            int r = mac_save_export(save.fileSystemRepresentation, DM_GAME_ID, out.fileSystemRepresentation), e = errno;
            dispatch_async(dispatch_get_main_queue(), ^{
                self.busy = NO;
                if (r) message(@"Save backup failed", @(strerror(e)));
                else [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[destination]];
            });
        });
    }];
}
- (void)restoreSave:(id)sender {
    (void)sender; const char *dir = getenv("DM_MAC_RESTORE_DIR"); if (self.busy || !dir) return;
    NSString *request = @(dir); self.busy = YES;
    NSOpenPanel *panel = [NSOpenPanel openPanel]; panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"dmsync"] ?: UTTypeData]; panel.allowsMultipleSelection = NO;
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) { self.busy = NO; return; }
        NSURL *source = panel.URL;
        if (!source) { self.busy = NO; return; }
        NSAlert *a = [NSAlert new]; a.messageText = @"Close the game and restore this backup?";
        a.informativeText = @"Unsaved progress will be lost. Your current saved progress will be kept in a recovery folder. Start the game again after the restore finishes.";
        [a addButtonWithTitle:@"Cancel"]; [a addButtonWithTitle:@"Close and Restore"];
        [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse answer) {
            if (answer != NSAlertSecondButtonReturn) { self.busy = NO; return; }
            self.busy = YES;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                NSString *copy = [request stringByAppendingPathComponent:@"backup.dmsync"];
                NSError *error; ss_meta meta;
                BOOL ok = [NSFileManager.defaultManager copyItemAtURL:source toURL:[NSURL fileURLWithPath:copy] error:&error];
                if (ok) ok = !ss_read(copy.fileSystemRepresentation, NULL, &meta) && !strcasecmp(meta.title, DM_GAME_ID) && meta.files > 0;
                if (ok) ok = [NSData.data writeToFile:[request stringByAppendingPathComponent:@"ready"] options:NSDataWritingAtomic error:&error];
                if (!ok) [NSFileManager.defaultManager removeItemAtPath:copy error:nil];
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.busy = NO;
                    if (ok) [NSApp terminate:nil];   // launcher.c restores only after waitpid: no guest or cloud thread can write over it
                    else message(@"Save backup could not be restored", error.localizedDescription ?: @"The backup is damaged, empty, or belongs to another game. Your current save was kept.");
                });
            });
        }];
    }];
}
@end

static NSMenuItem *action(NSMenu *menu, NSString *title, SEL selector, NSString *key, DmMacMenus *target) {
    NSMenuItem *item = [menu addItemWithTitle:title action:selector keyEquivalent:key]; item.target = target; return item;
}
static NSMenu *submenu(NSMenu *bar, NSString *title) {
    NSMenu *m = [[NSMenu alloc] initWithTitle:title]; [[bar addItemWithTitle:title action:nil keyEquivalent:@""] setSubmenu:m]; return m;
}
void mac_menus_install(NSWindow *window, NSMenu *bar) {
    static DmMacMenus *menus; menus = [DmMacMenus new]; menus.window = window;
    NSBundle *bundle = NSBundle.mainBundle; menus.domain = bundle.bundleIdentifier;
    NSURL *url = [bundle URLForResource:@"launcher" withExtension:@"json" subdirectory:@"dm"];
    NSData *data = url ? [NSData dataWithContentsOfURL:url] : nil;
    id list = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if ([list isKindOfClass:NSDictionary.class] && [list[@"settings"] isKindOfClass:NSArray.class])
        for (id s in list[@"settings"]) if ([s isKindOfClass:NSDictionary.class] && [s[@"env"] isEqual:@"DM_OPMODE"]) menus.mode = s;
    NSMenu *file = submenu(bar, @"File");
    action(file, @"Open Save Folder", @selector(openSave:), @"", menus);
    action(file, @"Reveal Log in Finder", @selector(openLog:), @"l", menus);
    [file addItem:NSMenuItem.separatorItem];
    action(file, @"Export Save Backup…", @selector(exportSave:), @"", menus);
    action(file, @"Restore Save from Backup…", @selector(restoreSave:), @"", menus);
    // The screenshot queue belongs to NVN. Vulkan/GL apps do not carry a shader map.
    if (getenv("DM_NVN_SHMAP")) {
        [file addItem:NSMenuItem.separatorItem];
        action(file, @"Take Screenshot", @selector(screenshot:), @"s", menus);
        NSMenuItem *f12 = action(file, @"Take Screenshot", @selector(screenshot:), [NSString stringWithFormat:@"%C", (unichar)NSF12FunctionKey], menus);
        f12.keyEquivalentModifierMask = 0; f12.hidden = YES; f12.allowsKeyEquivalentWhenHidden = YES;
    }
    NSMenu *game = submenu(bar, @"Game");
    NSMenu *mode = submenu(game, @"Console Mode");
    NSString *docked = @"Docked", *handheld = @"Handheld";
    if (menus.modeFixed) { docked = @"Docked (launch override)"; handheld = @"Handheld (launch override)"; }
    action(mode, docked, @selector(chooseMode:), @"", menus).tag = 1;
    action(mode, handheld, @selector(chooseMode:), @"", menus).tag = 0;
    NSMenu *audio = submenu(bar, @"Audio");
    action(audio, @"Mute Audio", @selector(mute:), @"", menus);
    action(audio, @"Mute When Unfocused", @selector(muteUnfocused:), @"", menus);
    NSMenu *view = submenu(bar, @"View"), *sizes = submenu(view, @"Window Scale");
    for (NSNumber *n in @[@100, @150, @200, @300]) action(sizes, [NSString stringWithFormat:@"%.1f×", n.doubleValue / 100], @selector(resize:), @"", menus).tag = n.integerValue;
}
