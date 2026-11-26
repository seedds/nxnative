// Cloud saves on Mac and iOS: NSFileCoordinator wrapper around save_sync.c's POSIX fetch/store primitives (dm/host/save_sync.h).
// Replaces save_sync.c's weak defaults at link time so iCloud Drive files are downloaded before reading and coordinated with bird/fileproviderd across writes,
// while ss_store_conditional still holds the per-title directory flock and verifies the observed destination identity before renaming.
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "save_sync.h"

#define MATERIALIZE_S 60   // the longest a fetch waits for a listed iCloud item to download: the launcher's report (SaveSyncReport.waitMs); the host stops waiting sooner at the mount (FETCH_MS)

int front_save_sync_fetch(const char *cloud, const char *local, ss_cloud_id *id) {
    @autoreleasepool {
        if (id) memset(id, 0, sizeof *id);
        NSFileManager *fm = [NSFileManager new];
        NSURL *src = [NSURL fileURLWithPath:@(cloud)];
        // Not there, not even as the placeholder iCloud lists for a file it has not downloaded (.<name>.icloud): no save in the cloud yet
        NSString *placeholder = [src.URLByDeletingLastPathComponent URLByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", src.lastPathComponent]].path;
        struct stat st;
        if (lstat(cloud, &st) != 0) {
            if (errno != ENOENT) return -1;
            if (lstat(placeholder.fileSystemRepresentation, &st) != 0) return errno == ENOENT ? 0 : -1;
            // Listed but not on this device: asked for, and waited for until the file is here or iCloud says the download failed (offline, say), so a fetch with no
            // network ends at once instead of at the caller's time limit.
            NSDate *start = [NSDate date];
            NSError *ask = nil;
            BOOL asked = [fm startDownloadingUbiquitousItemAtURL:src error:&ask];   // refused: no wait here, the coordinated read is left to download it, as before
            if (!asked) fprintf(stderr, "[dm] save sync: iCloud did not take the download request (%s); reading the cloud's save as it is\n", ask.localizedDescription.UTF8String ?: "no reason given");
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:MATERIALIZE_S];
            while (asked && lstat(cloud, &st) != 0) {
                if (errno != ENOENT) return -1;
                NSError *failed = nil;
                [src removeAllCachedResourceValues];
                [src getResourceValue:&failed forKey:NSURLUbiquitousItemDownloadingErrorKey error:nil];
                if (failed || [deadline timeIntervalSinceNow] <= 0) {
                    fprintf(stderr, "[dm] save sync: the cloud's save did not download in %.0f s%s%s\n", -[start timeIntervalSinceNow],
                            failed ? ": " : "", failed ? (failed.localizedDescription.UTF8String ?: "no reason given") : "");
                    return -1;
                }
                usleep(150 * 1000);
            }
            if (asked) fprintf(stderr, "[dm] save sync: the cloud's save downloaded in %.1f s\n", -[start timeIntervalSinceNow]);
        }
        __block int r = -1;
        __block int saved_errno = 0;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:src options:0 error:&error byAccessor:^(NSURL *u) {
            r = ss_fetch_copy(u.fileSystemRepresentation, local, id);
            if (r < 0) saved_errno = errno;
        }];
        if (error) {
            fprintf(stderr, "[dm] [RED] save sync: reading %s from the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
            errno = EIO;
            return -1;
        }
        if (r < 0 && saved_errno) errno = saved_errno;
        return r;
    }
}

int front_save_sync_store(const char *local, const char *cloud, const ss_cloud_id *expected) {
    @autoreleasepool {
        NSURL *dst = [NSURL fileURLWithPath:@(cloud)];
        __block int r = SS_STORE_ERR;
        __block int saved_errno = 0;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:dst options:NSFileCoordinatorWritingForReplacing error:&error byAccessor:^(NSURL *u) {
            r = ss_store_conditional(local, u.fileSystemRepresentation, expected);
            if (r == SS_STORE_ERR) saved_errno = errno;
        }];
        if (error) {
            fprintf(stderr, "[dm] [RED] save sync: writing %s to the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
            errno = EIO;
            return SS_STORE_ERR;
        }
        if (r == SS_STORE_ERR && saved_errno) errno = saved_errno;
        return r;
    }
}

void front_save_backup_exclude(const char *dir) {
#if !TARGET_OS_OSX
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:@(dir) isDirectory:YES];
        NSError *e = nil;
        if (![u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&e])
            fprintf(stderr, "[dm] save backup: %s could not be excluded from device backup: %s\n",
                    dir, e.localizedDescription.UTF8String ?: "no reason given");
    }
#else
    (void)dir;
#endif
}
