// A launcher chooses a parent folder. Publish a complete per-title directory before handing
// its save path to the host. Existing destinations are never merged with another location.
#import <Foundation/Foundation.h>
#include "local_users.h"
#include <errno.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *from_path(const char *p) {
    return [NSFileManager.defaultManager stringWithFileSystemRepresentation:p length:strlen(p)];
}

static NSString *canonical_dir(const char *path) {
    char resolved[PATH_MAX];
    struct stat st;
    if (!path || path[0] != '/' || !realpath(path, resolved) || stat(resolved, &st) || !S_ISDIR(st.st_mode)) return nil;
    return from_path(resolved);
}

// Map the committed legacy, device and per-account names from an arbitrary DM_SAVE basename.
static NSString *store_name(NSString *name, NSString *base) {
    if ([name isEqualToString:base]) return @"save";
    if ([name isEqualToString:[base stringByAppendingString:@"_device"]]) return @"save_device";
    NSString *prefix = [base stringByAppendingString:@".user-"];
    if (![name hasPrefix:prefix]) return nil;
    NSString *uid = [name substringFromIndex:prefix.length];
    if (uid.length != 32 || strspn(uid.UTF8String, "0123456789abcdef") != 32) return nil;
    return [@"save.user-" stringByAppendingString:uid];
}

static BOOL plain_dir(NSString *path) {
    struct stat st;
    return !lstat(path.fileSystemRepresentation, &st) && S_ISDIR(st.st_mode);
}

static BOOL contains_path(NSString *dir, NSString *p) {
    return [dir isEqualToString:p] || [p hasPrefix:[dir stringByAppendingString:@"/"]];
}

static NSString *temp_dir(NSString *parent) {
    char path[PATH_MAX];
    const char *pattern = [parent stringByAppendingPathComponent:@".nxnative-save-XXXXXX"].fileSystemRepresentation;
    if (strlcpy(path, pattern, sizeof path) >= sizeof path || !mkdtemp(path)) return nil;
    return from_path(path);
}

static BOOL supports_transactions(NSString *tmp) {
    NSString *a = [tmp stringByAppendingPathComponent:@"original"], *b = [tmp stringByAppendingPathComponent:@"clone"];
    BOOL ok = !mkdir(a.fileSystemRepresentation, 0700) && !clonefile(a.fileSystemRepresentation, b.fileSystemRepresentation, 0) &&
              !renamex_np(a.fileSystemRepresentation, b.fileSystemRepresentation, RENAME_SWAP);
    rmdir(a.fileSystemRepresentation);
    rmdir(b.fileSystemRepresentation);
    return ok;
}

// Copy only regular files and directories. Guest-name extended attributes on case aliases
// must survive the move to a different APFS volume, just as they do in a local transaction.
static BOOL copy_store(NSString *src, NSString *dst) {
    if (!plain_dir(src)) return NO;
    NSFileManager *fm = NSFileManager.defaultManager;
    __block BOOL readable = YES;
    NSDirectoryEnumerator *entries = [fm enumeratorAtURL:[NSURL fileURLWithPath:src isDirectory:YES] includingPropertiesForKeys:@[]
                                                  options:0 errorHandler:^BOOL(NSURL *url, NSError *error) {
        (void)url; (void)error; readable = NO; return NO;
    }];
    if (!entries) return NO;
    for (NSURL *url in entries) {
        struct stat st;
        if (lstat(url.fileSystemRepresentation, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) return NO;
    }
    return readable && [fm copyItemAtPath:src toPath:dst error:nil];
}

static const char *seed(NSString *src_parent, NSString *base, NSString *root, NSString *stage) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *entries = [fm contentsOfDirectoryAtPath:src_parent error:nil];
    if (!entries) return "Cannot read the previous save folder.";
    for (NSString *entry in entries) {
        NSString *name = store_name(entry, base);
        NSString *src = [src_parent stringByAppendingPathComponent:entry];
        if (!name) continue;
        if (contains_path(src, root)) return "Choose a save folder outside the existing saves.";
        if (!copy_store(src, [stage stringByAppendingPathComponent:name]))
            return "Cannot copy a committed save. Check permissions, free space, and that it contains only files and folders.";
    }
    // The cloud base hash belongs with these exact committed trees. Old backups and scratch
    // directories stay at their original location.
    NSString *sync = [src_parent stringByAppendingPathComponent:[base stringByAppendingString:@".sync"]];
    struct stat st;
    if (!lstat(sync.fileSystemRepresentation, &st)) {
        if (!S_ISREG(st.st_mode) || ![fm copyItemAtPath:sync toPath:[stage stringByAppendingPathComponent:@"save.sync"] error:nil])
            return "Cannot copy the previous cloud save state.";
    } else if (errno != ENOENT) return "Cannot inspect the previous cloud save state.";
    NSString *primary = [stage stringByAppendingPathComponent:@"save"];
    if (!plain_dir(primary) && mkdir(primary.fileSystemRepresentation, 0700)) return "Cannot initialize the game's save directory.";
    return NULL;
}

static const char *prepare(const char *root, const char *title_id, const char *source_save, char *out, size_t capacity) {
    NSString *title_component = nil;
    if (title_id && strlen(title_id) == 16 && strspn(title_id, "0123456789abcdefABCDEF") == 16)
        title_component = @(title_id).uppercaseString;
    if (!title_component) return "The game has no valid title ID.";
    NSString *parent = canonical_dir(root);
    if (!parent) return "The selected save folder is unavailable.";
    NSString *src = source_save && source_save[0] == '/' ? from_path(source_save) : nil;
    NSString *base = src.lastPathComponent;
    NSString *src_parent = src ? canonical_dir(src.stringByDeletingLastPathComponent.fileSystemRepresentation) : nil;
    if (src_parent && [parent hasPrefix:[src_parent stringByAppendingString:@"/"]]) {
        NSString *child = [[parent substringFromIndex:src_parent.length + 1] componentsSeparatedByString:@"/"].firstObject;
        if (store_name(child, base) || [child hasPrefix:[base stringByAppendingString:@"."]] ||
            [child hasPrefix:[base stringByAppendingString:@"_device."]] || [child isEqualToString:[base stringByAppendingString:@"-backups"]])
            return "Choose a top-level folder outside the previous saves and working directories.";
    }
    NSString *title = [parent stringByAppendingPathComponent:title_component];
    NSString *save = [title stringByAppendingPathComponent:@"save"];
    const char *target = save.fileSystemRepresentation;
    // hle_fs also needs room for a secondary UID and its rollback working directory.
    if (strlen(target) >= capacity || strlen(target) + sizeof ".user-00000000000000000000000000000000.work.re" > 1024)
        return "The selected save path is too long.";

    NSFileManager *fm = NSFileManager.defaultManager;
    struct stat st;
    BOOL existing = lstat(title.fileSystemRepresentation, &st) == 0;
    if (existing && !S_ISDIR(st.st_mode)) return "The game's destination must be a directory, not a file or link.";
    if (!existing && errno != ENOENT) return "Cannot inspect the game's destination.";
    NSArray *contents = existing ? [fm contentsOfDirectoryAtPath:title error:nil] : @[];
    if (!contents) return "Cannot read the game's destination.";
    BOOL initialized = NO;
    for (NSString *entry in contents) {
        if (!store_name(entry, @"save")) continue;
        if (!plain_dir([title stringByAppendingPathComponent:entry])) return "A destination save is not a directory or is a link.";
        initialized = YES;
    }
    if (contents.count && !initialized) return "The game's destination already contains other files. Choose a different folder.";
    if (initialized) {
        NSString *probe = temp_dir(title);
        if (!probe) return "Cannot write to the game's save folder.";
        BOOL supported = supports_transactions(probe);
        rmdir(probe.fileSystemRepresentation);
        if (!supported) return "Choose a writable APFS folder supporting directory clones and atomic swaps.";
    } else {
        if (!src_parent || !base.length || [base isEqualToString:@"/"] || [base isEqualToString:@"."] || [base isEqualToString:@".."])
            return "The previous save folder is unavailable. Reconnect it, reopen an existing game library, or use the default location.";
        NSString *stage = temp_dir(parent);
        if (!stage) return "Cannot create the game's directory in the selected folder.";
        const char *problem = supports_transactions(stage) ? seed(src_parent, base, parent, stage)
            : "Choose a writable APFS folder supporting directory clones and atomic swaps.";
        // Plain rename can replace an empty directory only. A concurrently initialized
        // nonempty directory always wins, including when the title folder didn't exist at check time.
        if (!problem && renamex_np(stage.fileSystemRepresentation, title.fileSystemRepresentation, existing ? 0 : RENAME_EXCL))
            problem = "The destination changed or could not be published. Reopen it and try again.";
        if (problem) { [fm removeItemAtPath:stage error:nil]; return problem; }
    }
    strlcpy(out, target, capacity);
    return NULL;
}

int dm_save_prepare_root(const char *root, const char *title_id, const char *source_save,
                         char *out, size_t out_size, char *error, size_t error_size) {
    @autoreleasepool {
        if (!out || !out_size || !error || !error_size) return 0;
        *out = *error = 0;
        const char *problem = prepare(root, title_id, source_save, out, out_size);
        if (problem) strlcpy(error, problem, error_size);
        return !problem;
    }
}
