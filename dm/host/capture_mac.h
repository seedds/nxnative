// Bounded local diagnostics; called only for an explicit capture, never on a frame tick.
#pragma once
#import <Foundation/Foundation.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSData *capture_log_slice(NSString *path, BOOL tail, NSUInteger limit) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) { close(fd); return nil; }
    NSUInteger n = (NSUInteger)MIN((uint64_t)st.st_size, (uint64_t)limit);
    off_t off = tail ? st.st_size - (off_t)n : 0;
    NSMutableData *data = [NSMutableData dataWithLength:n];
    NSUInteger got = 0;
    while (got < n) {
        ssize_t k = pread(fd, (uint8_t *)data.mutableBytes + got, n - got, off + (off_t)got);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) { close(fd); return nil; }
        if (k <= 0) break;
        got += (NSUInteger)k;
    }
    close(fd); data.length = got; return data;
}

static NSString *capture_stderr_path(void) {
    char path[PATH_MAX]; struct stat st;
    if (fstat(STDERR_FILENO, &st) || !S_ISREG(st.st_mode) || fcntl(STDERR_FILENO, F_GETPATH, path)) return nil;
    return [NSString stringWithUTF8String:path];
}

static NSDictionary *capture_metadata(void) {
    NSMutableDictionary *settings = [NSMutableDictionary dictionary];
    for (NSString *key in @[@"DM_FPS_TARGET", @"DM_DYNAMIC_RESOLUTION", @"DM_UPSCALER", @"DM_PRESENT_LINK", @"DM_PRESENT_UPSCALE", @"DM_FULLSCREEN", @"DM_NVN_ENC_THREAD", @"DM_NVN_FB_DSNAP", @"DM_NVN_FAMILY_NOCOMPRESS", @"DM_NVN_PASSDUMP_ALL"])
        { const char *s = getenv(key.UTF8String); if (s) settings[key] = [NSString stringWithUTF8String:s] ?: @"invalid UTF-8"; }
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    return @{@"title_id": @DM_GAME_ID, @"commit": info[@"DMCommit"] ?: @"not recorded", @"shader_chain": info[@"DMChain"] ?: @"not recorded",
             @"pid": @(getpid()), @"requested_pass_frames": @2, @"settings": settings,
             @"note": @"Local capture only. Readback and tracing can disturb timing. Logs may contain local paths; review before sharing."};
}
