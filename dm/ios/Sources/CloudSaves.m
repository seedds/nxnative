// See CloudSaves.h.

#import "CloudSaves.h"

static NSString *const kCloudSaves = @"cloudSavesBookmark";

// The folder's security scope, held from the first resolve on: the host writes there after every
// save. Never stopped on purpose, the process exits when the game does.
static NSURL *opened = nil;

// The app's own storage, as the folders a picked one must not be inside: Documents (the Files app shows
// it, UIFileSharingEnabled), the Inbox a picker's import copy lands in, and temp. The Inbox is looked up
// off the home directory as well, which is where a launcher's guest gets its imports: LiveContainer links
// Inbox under the guest's home and under the temp folder it was given (LCBootstrap.m, doSymlinkInbox).
static NSArray<NSString *> *own_folders(void) {
    NSMutableArray<NSString *> *own = [NSMutableArray array];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (documents.length) [own addObject:documents];
    NSString *home = NSHomeDirectory();
    if (home.length) [own addObject:[home stringByAppendingPathComponent:@"Inbox"]];
    NSString *temp = NSTemporaryDirectory();
    if (temp.length) [own addObject:temp];
    return own;
}

// `path` is `dir` or below it, not a sibling whose name begins with it (/a/bc is not in /a/b)
static BOOL below(NSString *path, NSString *dir) {
    if (!path.length || !dir.length) return NO;
    if ([path isEqualToString:dir]) return YES;
    return [path hasPrefix:[dir hasSuffix:@"/"] ? dir : [dir stringByAppendingString:@"/"]];
}

// Compared as given and with symlinks resolved: the Inbox a copy lands in is a link to the folder it is
// really in, so the path the picker returns may not name the app's storage even when it is inside it
static BOOL in_own_storage(NSString *path, NSArray<NSString *> *own) {
    NSString *real = path.stringByResolvingSymlinksInPath;
    for (NSString *dir in own) {
        if (below(path, dir) || below(real, dir.stringByResolvingSymlinksInPath)) return YES;
    }
    return NO;
}

NSString *dm_cloud_saves_problem(NSString *path, NSArray<NSString *> *own, BOOL must_write) {
    BOOL folder = NO;
    if (!path.length || ![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&folder] || !folder)
        return @"That folder is not there any more.";
    if (in_own_storage(path, own))
        return @"That folder is inside this app's own storage, so a save written there never reaches iCloud Drive. Choose the folder in iCloud Drive itself; if the app runs in a launcher, turn off its file-picker fix first (LiveContainer's Fix File Picker hands back a copy inside the app).";
    if (!must_write) return nil;
    // A file named .nosync is not uploaded, so the check leaves the folder's contents as they were: the
    // host writes <folder>/<title ID>/save.dmsync there after every save, and a folder it cannot write to
    // would take every save of the game and put none of them in the cloud
    NSString *probe = [path stringByAppendingPathComponent:[NSString stringWithFormat:@".nxnative-check.%d.nosync", (int)getpid()]];
    NSError *error = nil;
    BOOL written = [[NSData dataWithBytes:"x" length:1] writeToFile:probe options:NSDataWritingAtomic error:&error];
    if (written) [NSFileManager.defaultManager removeItemAtPath:probe error:nil];
    if (written) return nil;
    return [NSString stringWithFormat:@"That folder cannot be written to (%@), and cloud saves write the game's save there after every save.", error.localizedDescription ?: @"no reason given"];
}

static NSData *fresh_bookmark(NSURL *url) {
    // The picker grants a security scope that lasts while it is held: the bookmark is written with
    // the scope on. One written without it reads back on this run and resolves to nothing from the
    // next launch on.
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (scoped) [url stopAccessingSecurityScopedResource];
    return bookmark;
}

NSString *dm_cloud_saves_path(BOOL *stale) {
    if (stale) *stale = NO;
    NSData *bookmark = [NSUserDefaults.standardUserDefaults dataForKey:kCloudSaves];
    if (bookmark.length == 0) return nil;
    BOOL isStale = NO;
    NSError *error = nil;
    NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:&isStale error:&error];
    if (!url) {
        // A re-signed build gets a bookmark the system will not resolve for it. Keep the data, the
        // player may install the same signature again, and let the launcher ask for a new pick.
        fprintf(stderr, "boot: the picked cloud-save folder bookmark does not resolve (%s)\n", error.localizedDescription.UTF8String ?: "no reason given");
        if (stale) *stale = YES;
        return nil;
    }
    NSString *why = dm_cloud_saves_problem(url.path, own_folders(), NO);
    if (why) {
        // A folder an older build stored, judged as the picker hands one back today (dm_store_cloud_saves):
        // kept, so a fixed build can still read it, but not used to send the game's saves where iCloud
        // Drive never sees them. The launcher shows Off and asks for a pick.
        fprintf(stderr, "[RED] boot: the picked cloud-save folder cannot be used: %s (%s)\n", url.path.UTF8String ?: "?", why.UTF8String);
        if (stale) *stale = YES;
        return nil;
    }
    if (opened && ![opened.path isEqualToString:url.path]) opened = nil;
    if (!opened) [url startAccessingSecurityScopedResource];   // held for the session (see opened)
    opened = url;
    if (isStale) {
        // The system still resolves it but asks for a rewrite. Same signature, so a fresh bookmark sticks.
        NSData *again = fresh_bookmark(url);
        if (again) [NSUserDefaults.standardUserDefaults setObject:again forKey:kCloudSaves];
        if (stale) *stale = YES;
    }
    return url.path;
}

NSString *dm_store_cloud_saves(NSURL *url) {
    NSString *why = dm_cloud_saves_problem(url.path, own_folders(), YES);
    if (why) {
        fprintf(stderr, "[RED] cloud saves: %s: %s\n", url.path.UTF8String ?: "?", why.UTF8String);
        return why;
    }
    NSData *bookmark = fresh_bookmark(url);
    NSError *error = nil;
    NSURL *back = nil;
    if (bookmark) back = [NSURL URLByResolvingBookmarkData:bookmark options:0 relativeToURL:nil bookmarkDataIsStale:nil error:&error];
    if (!bookmark || !back) {
        // A bookmark that cannot come back is not saved as if it were good.
        fprintf(stderr, "[RED] could not bookmark %s: %s\n", url.path.UTF8String ?: "?",
                error.localizedDescription.UTF8String ?: (bookmark ? "the bookmark does not resolve again" : "no bookmark data"));
        return @"That folder could not be remembered — try a different one.";
    }
    [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:kCloudSaves];
    fprintf(stderr, "launcher: cloud-save folder set to %s\n", url.path.UTF8String ?: "?");
    return nil;
}

void dm_clear_cloud_saves(void) {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kCloudSaves];
}

void dm_cloud_saves_env(void) {
    static BOOL fromPick;   // DM_SAVE_SYNC is the one this set, not an env file's
    if (getenv("DM_SAVE_SYNC") && !fromPick) return;
    BOOL stale = NO;
    NSString *folder = dm_cloud_saves_path(&stale);
    if (stale && !folder) fprintf(stderr, "boot: the cloud-save folder needs to be chosen again; cloud saves are off\n");
    if (folder.length) {
        setenv("DM_SAVE_SYNC", folder.fileSystemRepresentation, 1); fromPick = YES;
        fprintf(stderr, "boot: cloud saves through %s\n", folder.fileSystemRepresentation);
    } else if (fromPick) { unsetenv("DM_SAVE_SYNC"); fromPick = NO; }
}
