// Cloud saves on the iPhone: the copies to and from the iCloud Drive folder the player picked (dm/host/save_sync.h; the folder is a bookmark, CloudSaves.m).
// They go through NSFileCoordinator: a file iCloud has only listed is downloaded before the read, and neither copy crosses iCloud's own writes of the file.
// These replace the host's plain-copy defaults (weak, in save_sync.c) at link time; the host packs, unpacks and decides, this moves the one file.
#import <Foundation/Foundation.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "save_sync.h"

#define MATERIALIZE_S 60   // the longest a fetch waits for a listed iCloud item to download: the launcher's report (SaveSyncReport.waitMs); the host stops waiting sooner at the mount (FETCH_MS)

int front_save_sync_fetch(const char *cloud, const char *local) {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager new];
        NSURL *src = [NSURL fileURLWithPath:@(cloud)], *dst = [NSURL fileURLWithPath:@(local)];
        // Not there, not even as the placeholder iCloud lists for a file it has not downloaded (.<name>.icloud): no save in the cloud yet
        NSString *placeholder = [src.URLByDeletingLastPathComponent URLByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", src.lastPathComponent]].path;
        if (![fm fileExistsAtPath:src.path] && ![fm fileExistsAtPath:placeholder]) return 0;
        // Listed but not on this device: asked for, and waited for until the file is here or iCloud says the download failed (offline, say), so a fetch with no
        // network ends at once instead of at the caller's time limit. The coordinated read below is what Apple's documentation says downloads the item; that it can
        // come back without it (a first start that played this device's save, a second that brought the cloud's in) is the PR #2180 report, not checked here
        if (![fm fileExistsAtPath:src.path]) {
            NSDate *start = [NSDate date];
            NSError *ask = nil;
            BOOL asked = [fm startDownloadingUbiquitousItemAtURL:src error:&ask];   // refused: no wait here, the coordinated read is left to download it, as before
            if (!asked) fprintf(stderr, "[dm] save sync: iCloud did not take the download request (%s); reading the cloud's save as it is\n", ask.localizedDescription.UTF8String ?: "no reason given");
            NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:MATERIALIZE_S];
            while (asked && ![fm fileExistsAtPath:src.path]) {
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
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:src options:0 error:&error byAccessor:^(NSURL *u) {
            NSError *e = nil;
            [fm removeItemAtURL:dst error:nil];
            if ([fm copyItemAtURL:u toURL:dst error:&e]) r = 1;
            else if ([e.domain isEqualToString:NSCocoaErrorDomain] && e.code == NSFileReadNoSuchFileError) r = 0;
            else fprintf(stderr, "[dm] [RED] save sync: copying %s from the cloud failed: %s\n", cloud, e.localizedDescription.UTF8String ?: "no reason given");
        }];
        if (error) fprintf(stderr, "[dm] [RED] save sync: reading %s from the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
        return r;
    }
}

int front_save_sync_store(const char *local, const char *cloud) {
    @autoreleasepool {
        NSFileManager *fm = [NSFileManager new];
        NSURL *dst = [NSURL fileURLWithPath:@(cloud)];
        NSString *src = @(local);
        __block int r = 0;
        NSError *error = nil;
        [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:dst options:NSFileCoordinatorWritingForReplacing error:&error byAccessor:^(NSURL *u) {
            // Beside it under a .nosync name (iCloud Drive does not upload those), then renamed over it: iCloud sees one whole new file
            NSString *part = [u.path stringByAppendingFormat:@".%d.nosync", (int)getpid()];
            NSError *e = nil;
            [fm removeItemAtPath:part error:nil];
            if (![fm copyItemAtPath:src toPath:part error:&e]) {
                fprintf(stderr, "[dm] [RED] save sync: writing beside %s failed: %s\n", cloud, e.localizedDescription.UTF8String ?: "no reason given");
                return;
            }
            if (rename(part.fileSystemRepresentation, u.fileSystemRepresentation)) {
                fprintf(stderr, "[dm] [RED] save sync: replacing %s failed (errno=%d)\n", cloud, errno);
                [fm removeItemAtPath:part error:nil];
                return;
            }
            r = 1;
        }];
        if (error) fprintf(stderr, "[dm] [RED] save sync: writing %s to the cloud failed: %s\n", cloud, error.localizedDescription.UTF8String ?: "no reason given");
        return r;
    }
}
