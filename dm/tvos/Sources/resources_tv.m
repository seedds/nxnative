// Stream an owned shader archive into private staging, then publish it once
// gzip integrity and all tar entries have been verified. No shell is involved.
#import <Foundation/Foundation.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <CommonCrypto/CommonDigest.h>

static BOOL read_exact(gzFile archive, void *buffer, size_t size) {
    uint8_t *p = buffer;
    while (size) {
        int n = gzread(archive, p, (unsigned)MIN(size, (size_t)32768));
        if (n <= 0) return NO;
        p += n; size -= (size_t)n;
    }
    return YES;
}
static BOOL octal(const char *p, size_t size, uint64_t *out) {
    uint64_t value = 0; BOOL digits = NO;
    while (size && (*p == ' ' || *p == '\0')) { p++; size--; }
    while (size && *p >= '0' && *p <= '7') {
        if (value > UINT64_MAX / 8) return NO;
        value = value * 8 + (*p++ - '0'); size--; digits = YES;
    }
    while (size && (*p == ' ' || *p == '\0')) { p++; size--; }
    *out = value; return !size && digits;
}
static NSString *field(const char *p, size_t size) {
    return [[NSString alloc] initWithBytes:p length:strnlen(p, size) encoding:NSUTF8StringEncoding];
}
static BOOL write_bytes(int fd, const void *p, size_t size) {
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n <= 0) return NO;
        p = (const uint8_t *)p + n; size -= (size_t)n;
    }
    return YES;
}
static BOOL sync_directory(NSString *path) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return NO;
    int result;
    do result = fsync(fd); while (result && errno == EINTR);
    close(fd); return result == 0;
}
// The digest is the commit marker for a completely extracted shader tree.
// Publish and sync it last; a failed marker must leave the transfer archive
// available for a clean retry.
static BOOL publish_marker(NSString *dest, NSString *digest) {
#if defined(DM_TV_RESOURCE_TESTS)
    if (getenv("DM_TEST_TV_RESOURCE_MARKER_FAIL")) { errno = ENOSPC; return NO; }
#endif
    NSString *marker = [dest stringByAppendingPathComponent:@".archive-sha256"];
    NSString *incoming = [marker stringByAppendingString:@".incoming"];
    int fd = open(incoming.fileSystemRepresentation,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    NSData *bytes = [digest dataUsingEncoding:NSUTF8StringEncoding];
    BOOL ok = fd >= 0 && write_bytes(fd, bytes.bytes, bytes.length);
    if (ok) { int result; do result = fsync(fd); while (result && errno == EINTR); ok = result == 0; }
    if (fd >= 0 && close(fd)) ok = NO;
    if (ok && rename(incoming.fileSystemRepresentation, marker.fileSystemRepresentation)) ok = NO;
    if (ok) ok = sync_directory(dest);
    if (!ok) unlink(incoming.fileSystemRepresentation);
    return ok;
}
BOOL tv_unpack_shaders(NSString *writable, NSString *expectedSHA256, NSString **failure) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *archivePath = [writable stringByAppendingPathComponent:@"msl.tar.gz"];
    int input = open(archivePath.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW);
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash); uint8_t digest[CC_SHA256_DIGEST_LENGTH], chunk[32768];
    ssize_t nread = -1;
    if (input >= 0) {
        while ((nread = read(input, chunk, sizeof chunk)) > 0) CC_SHA256_Update(&hash, chunk, (CC_LONG)nread);
        close(input);
    }
    CC_SHA256_Final(digest, &hash);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (unsigned i = 0; i < sizeof digest; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (nread < 0 || expectedSHA256.length != 64 || ![expectedSHA256 isEqualToString:@(hex)]) {
        *failure = @"The game-file transfer is incomplete. Finish copying the archive and reopen the app.";
        return NO;
    }
    fprintf(stderr, "tvOS resources: archive SHA-256 verified: %s\n", hex);
    NSString *stage = [writable stringByAppendingPathComponent:@".msl-stage"];
    [fm removeItemAtPath:stage error:nil];
    if (![fm createDirectoryAtPath:stage withIntermediateDirectories:YES attributes:nil error:nil]) {
        *failure = @"Could not prepare game files."; return NO;
    }
    gzFile archive = gzopen(archivePath.fileSystemRepresentation, "rb");
    int root = open(stage.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    BOOL ok = archive && root >= 0, ended = NO;
    unsigned entries = 0; uint64_t total = 0; uint8_t header[512], buffer[32768];
    while (ok) {
        if (!read_exact(archive, header, sizeof header)) { ok = NO; break; }
        BOOL zero = YES; for (unsigned i = 0; i < sizeof header; i++) if (header[i]) zero = NO;
        if (zero) { ended = YES; break; }
        uint64_t size, checksum; unsigned sum = 0;
        for (unsigned i = 0; i < sizeof header; i++) sum += (i >= 148 && i < 156) ? ' ' : header[i];
        if (!octal((const char *)header + 124, 12, &size) ||
            !octal((const char *)header + 148, 8, &checksum) || checksum != sum ||
            memcmp(header + 257, "ustar", 5) ||
            (header[156] != '0' && header[156] != 0 && header[156] != '5') ||
            size > (8ull << 30) || total + size > (8ull << 30) || ++entries > 200000) { ok = NO; break; }
        NSString *name = field((const char *)header, 100), *prefix = field((const char *)header + 345, 155);
        if (!name || !prefix) { ok = NO; break; }
        if (prefix.length) name = [prefix stringByAppendingFormat:@"/%@", name];
        if ([name hasSuffix:@"/"]) name = [name substringToIndex:name.length - 1];
        NSArray<NSString *> *components = [name componentsSeparatedByString:@"/"];
        if (![components.firstObject isEqualToString:@"msl"]) { ok = NO; break; }
        for (NSString *part in components)
            if (!part.length || [part isEqualToString:@"."] || [part isEqualToString:@".."] ||
                [part containsString:@"\\"] || [part lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 240) ok = NO;
        if (!ok) break;
        BOOL directory = header[156] == '5';
        if (directory && size) { ok = NO; break; }
        int parent = dup(root);
        for (NSUInteger i = 0; ok && i < components.count - 1; i++) {
            const char *part = components[i].fileSystemRepresentation;
            if (mkdirat(parent, part, 0700) && errno != EEXIST) { ok = NO; break; }
            int next = openat(parent, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            close(parent); parent = next; if (next < 0) ok = NO;
        }
        NSString *leaf = components.lastObject;
        if (ok && directory) {
            if (mkdirat(parent, leaf.fileSystemRepresentation, 0700) && errno != EEXIST) ok = NO;
            int dir = openat(parent, leaf.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (dir < 0) ok = NO; else close(dir);
        } else if (ok) {
            NSString *partial = [leaf stringByAppendingString:@".incoming"];
            int file = openat(parent, partial.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
            if (file < 0) ok = NO;
            uint64_t remaining = size;
            while (ok && remaining) {
                size_t n = (size_t)MIN(remaining, sizeof buffer);
                if (!read_exact(archive, buffer, n) || !write_bytes(file, buffer, n)) ok = NO;
                remaining -= n;
            }
            if (file >= 0 && close(file)) ok = NO;
            if (ok && renameat(parent, partial.fileSystemRepresentation, parent, leaf.fileSystemRepresentation)) ok = NO;
        }
        if (parent >= 0) close(parent);
        if (!ok) break;
        if (size % 512 && !read_exact(archive, buffer, (size_t)(512 - size % 512))) { ok = NO; break; }
        total += size;
        if (!(entries % 4000)) fprintf(stderr, "tvOS resources: extracted %u entries, %llu bytes\n", entries, total);
    }
    // Consume all tar padding so zlib verifies the final stream CRC.
    if (ok && ended) {
        int n;
        while ((n = gzread(archive, buffer, sizeof buffer)) > 0)
            for (int i = 0; i < n; i++) if (buffer[i]) ok = NO;
        if (n < 0) ok = NO;
    }
    if (archive && gzclose(archive) != Z_OK) ok = NO;
    if (root >= 0) close(root);
    NSString *source = [stage stringByAppendingPathComponent:@"msl"];
    NSString *dest = [writable stringByAppendingPathComponent:@"msl"];
    NSString *backup = [writable stringByAppendingPathComponent:@".msl-before"];
    if (!ended || !ok || ![fm fileExistsAtPath:source]) {
        *failure = @"Game files could not be prepared. Copy the resource archive again and reopen the app.";
        return NO; // Keep the input archive for a complete transfer/retry.
    }
    if ([fm fileExistsAtPath:dest]) {
        [fm removeItemAtPath:backup error:nil];
        if (![fm moveItemAtPath:dest toPath:backup error:nil]) { *failure = @"Could not update game files."; return NO; }
    }
    if (![fm moveItemAtPath:source toPath:dest error:nil]) {
        if ([fm fileExistsAtPath:backup]) [fm moveItemAtPath:backup toPath:dest error:nil];
        *failure = @"Could not activate game files."; return NO;
    }
    if (!sync_directory(writable) || !publish_marker(dest, expectedSHA256)) {
        [fm removeItemAtPath:dest error:nil];
        if ([fm fileExistsAtPath:backup]) [fm moveItemAtPath:backup toPath:dest error:nil];
        (void)sync_directory(writable);
        *failure = @"Could not finish updating game files. The transfer archive was kept for retry.";
        return NO;
    }
    [fm removeItemAtPath:archivePath error:nil]; [fm removeItemAtPath:stage error:nil];
    fprintf(stderr, "tvOS resources: complete, %u entries, %llu bytes; gzip integrity verified\n", entries, total);
    return YES;
}
