// Publish a virtual-layout relocated RomFS patch without replacing the user's
// files. Trinity's hash-ordered pack offsets are rebuilt as aligned descriptor
// prefix sums; a verified sidecar maps the virtual file to unchanged source or
// inserted replacement ranges. The 4+ GiB archive is cloned on APFS, so only
// changed blocks and inserted packs consume new physical storage. READY is written
// last and the complete
// directory is activated by one rename.  A crash before that rename leaves an
// ignored .partial directory which the next invocation removes.
#import "romfs_patch.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

static BOOL patch_fail(NSString **failure, NSString *message) {
    fprintf(stderr, "[RED] tvOS RomFS patch: %s\n", message.UTF8String);
    if (failure) *failure = message;
    return NO;
}

static BOOL regular_file(NSString *path, uint64_t expected, int *fdOut,
                         NSString **failure) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != expected) {
        int e = errno;
        if (fd >= 0) close(fd);
        if (failure) patch_fail(failure, [NSString stringWithFormat:
            @"File identity differs for %@ (expected %llu bytes, errno=%d).",
            path.lastPathComponent, (unsigned long long)expected, e]);
        return NO;
    }
    *fdOut = fd;
    return YES;
}

static NSString *digest_fd(int fd, off_t offset, uint64_t bytes) {
    if (lseek(fd, offset, SEEK_SET) < 0) return nil;
    (void)fcntl(fd, F_NOCACHE, 1);
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    const size_t bufferBytes = 1u << 20;
    uint8_t *buffer = malloc(bufferBytes);
    if (!buffer) return nil;
    while (bytes) {
        size_t want = (size_t)MIN(bytes, bufferBytes);
        ssize_t n = read(fd, buffer, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(buffer); return nil; }
        CC_SHA256_Update(&hash, buffer, (CC_LONG)n); bytes -= (uint64_t)n;
    }
    free(buffer);
    uint8_t out[CC_SHA256_DIGEST_LENGTH]; char hex[65];
    CC_SHA256_Final(out, &hash);
    for (unsigned i = 0; i < sizeof out; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    return @(hex);
}

static NSData *read_limited(NSString *path, uint64_t limit, NSString **failure) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > limit) {
        int e = errno; if (fd >= 0) close(fd);
        if (failure) patch_fail(failure, [NSString stringWithFormat:@"Invalid patch file %@ (errno=%d).",
            path.lastPathComponent, e]);
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
    size_t at = 0;
    while (at < data.length) {
        ssize_t n = read(fd, (uint8_t *)data.mutableBytes + at, data.length - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); if (failure) patch_fail(failure, @"Patch file ended early."); return nil; }
        at += (size_t)n;
    }
    close(fd); return data;
}

static NSString *digest_data(NSData *data) {
    uint8_t out[CC_SHA256_DIGEST_LENGTH]; char hex[65];
    CC_SHA256(data.bytes, (CC_LONG)data.length, out);
    for (unsigned i = 0; i < sizeof out; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    return @(hex);
}

static NSNumber *unsigned_number(id value) {
    if (![value isKindOfClass:NSNumber.class]) return nil;
    const char *type = [value objCType];
    if (!strcmp(type, @encode(float)) || !strcmp(type, @encode(double)) ||
        !strcmp(type, @encode(BOOL)) || [value longLongValue] < 0) return nil;
    return value;
}

static NSData *hex_data(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] % 2) return nil;
    NSMutableData *data = [NSMutableData dataWithLength:[value length] / 2];
    uint8_t *out = data.mutableBytes;
    for (NSUInteger i = 0; i < data.length; i++) {
        unichar a = [value characterAtIndex:i * 2], b = [value characterAtIndex:i * 2 + 1];
        int hi = a >= '0' && a <= '9' ? a - '0' : a >= 'a' && a <= 'f' ? a - 'a' + 10 : -1;
        int lo = b >= '0' && b <= '9' ? b - '0' : b >= 'a' && b <= 'f' ? b - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return nil;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return data;
}

static BOOL exact_read(int fd, uint64_t offset, void *buffer, size_t bytes) {
    size_t got = 0;
    while (got < bytes) {
        ssize_t n = pread(fd, (uint8_t *)buffer + got, bytes - got, (off_t)(offset + got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NO;
        got += (size_t)n;
    }
    return YES;
}

static BOOL exact_write(int fd, uint64_t offset, const void *buffer, size_t bytes) {
    size_t put = 0;
    while (put < bytes) {
        ssize_t n = pwrite(fd, (const uint8_t *)buffer + put, bytes - put,
                           (off_t)(offset + put));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return NO;
        put += (size_t)n;
    }
    return YES;
}

static BOOL validate_edits(NSArray *rows, uint64_t fileBytes, NSString *offsetKey,
                           NSString **failure) {
    if (![rows isKindOfClass:NSArray.class] || rows.count > 20000)
        return patch_fail(failure, @"Patch edit table is invalid.");
    for (id object in rows) {
        if (![object isKindOfClass:NSDictionary.class])
            return patch_fail(failure, @"Patch edit entry is invalid.");
        NSDictionary *row = object;
        NSNumber *offsetNumber = unsigned_number(row[offsetKey]);
        NSNumber *bytesNumber = unsigned_number(row[@"bytes"]);
        NSData *source = hex_data(row[@"source_hex"]), *replacement = hex_data(row[@"replacement_hex"]);
        if (!offsetNumber || !bytesNumber || !source || !replacement) return patch_fail(failure, @"Patch edit fields are invalid.");
        uint64_t offset = offsetNumber.unsignedLongLongValue, bytes = bytesNumber.unsignedLongLongValue;
        if (!bytes || bytes > (64u << 20) || source.length != bytes || replacement.length != bytes ||
            offset > fileBytes || bytes > fileBytes - offset)
            return patch_fail(failure, @"Patch edit range is invalid.");
    }
    NSArray *ordered = [rows sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        uint64_t av = [a[offsetKey] unsignedLongLongValue], bv = [b[offsetKey] unsignedLongLongValue];
        return av < bv ? NSOrderedAscending : av > bv ? NSOrderedDescending : NSOrderedSame;
    }];
    uint64_t previousEnd = 0;
    for (NSDictionary *row in ordered) {
        uint64_t offset = [row[offsetKey] unsignedLongLongValue];
        uint64_t bytes = [row[@"bytes"] unsignedLongLongValue];
        if (offset < previousEnd)
            return patch_fail(failure, @"Patch edit ranges overlap.");
        previousEnd = offset + bytes;
    }
    return YES;
}

static BOOL verify_ranges(int fd, NSArray *rows, NSString *offsetKey,
                          BOOL replacement, NSString **failure) {
    for (NSDictionary *row in rows) {
        NSData *expected = hex_data(row[replacement ? @"replacement_hex" : @"source_hex"]);
        uint64_t offset = [row[offsetKey] unsignedLongLongValue];
        NSMutableData *actual = [NSMutableData dataWithLength:expected.length];
        if (!actual || !exact_read(fd, offset, actual.mutableBytes, actual.length) ||
            memcmp(actual.bytes, expected.bytes, expected.length)) {
            if (failure) patch_fail(failure, [NSString stringWithFormat:
                @"Patch %@ bytes differ at offset 0x%llx.", replacement ? @"output" : @"source",
                (unsigned long long)offset]);
            return NO;
        }
    }
    return YES;
}

static BOOL apply_edits(int fd, NSArray *rows, NSString *offsetKey, NSString **failure) {
    for (NSDictionary *row in rows) {
        NSData *data = hex_data(row[@"replacement_hex"]);
        if (!exact_write(fd, [row[offsetKey] unsignedLongLongValue], data.bytes, data.length))
            return patch_fail(failure, @"Could not write a patch edit.");
    }
    return YES;
}

static BOOL fsync_path(NSString *path, BOOL directory) {
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    if (directory) flags |= O_DIRECTORY;
    int fd = open(path.fileSystemRepresentation, flags);
    if (fd < 0) return NO;
    int result; do result = fsync(fd); while (result && errno == EINTR);
    close(fd); return result == 0;
}

static void test_exit(const char *stage) {
    const char *wanted = getenv("DM_TEST_ROMFS_PATCH_EXIT");
    if (wanted && !strcmp(wanted, stage)) _exit(93);
}

static BOOL ready_overlay(NSString *root, NSString *manifestDigest,
                          NSDictionary *manifest, NSString **failure) {
    NSString *readyPath = [root stringByAppendingPathComponent:@"READY.json"];
    NSData *readyData = read_limited(readyPath, 1 << 20, NULL);
    if (!readyData) return NO;
    NSDictionary *ready = [NSJSONSerialization JSONObjectWithData:readyData options:0 error:nil];
    if (![ready isKindOfClass:NSDictionary.class] ||
        ![ready[@"manifest_sha256"] isEqual:manifestDigest]) return NO;
    NSString *arc = [root stringByAppendingPathComponent:@"arc/data.trpfs"];
    NSString *desc = [root stringByAppendingPathComponent:@"arc/data.trpfd"];
    NSDictionary *append = nil, *redirects = manifest[@"redirects"];
    NSString *redirectPath = [root stringByAppendingPathComponent:redirects[@"file"] ?: @""];
    NSString *payloadDigest = nil, *rootDigest = nil, *descriptorDigest = nil,
        *redirectDigest = nil;
    int afd = -1, dfd = -1, rfd = -1; BOOL ok = NO;
    uint64_t archiveBytes = [manifest[@"final_archive_bytes"] unsignedLongLongValue];
    uint64_t descriptorBytes = [manifest[@"final_descriptor_bytes"] unsignedLongLongValue];
    if (!regular_file(arc, archiveBytes, &afd, NULL) ||
        !regular_file(desc, descriptorBytes, &dfd, NULL) ||
        !regular_file(redirectPath, [redirects[@"bytes"] unsignedLongLongValue], &rfd, NULL)) goto out;
    if (!verify_ranges(afd, manifest[@"archive_header_edits"], @"output_offset", YES, NULL) ||
        !verify_ranges(afd, manifest[@"archive_index_edits"], @"output_offset", YES, NULL) ||
        !verify_ranges(dfd, manifest[@"descriptor_size_edits"], @"offset", YES, NULL)) goto out;
    append = manifest[@"append"];
    payloadDigest = digest_fd(afd, (off_t)[append[@"output_offset"] unsignedLongLongValue],
                              [append[@"bytes"] unsignedLongLongValue]);
    rootDigest = digest_fd(afd, (off_t)[manifest[@"final_archive_root_offset"] unsignedLongLongValue],
                           [manifest[@"archive_root_bytes"] unsignedLongLongValue]);
    descriptorDigest = digest_fd(dfd, 0, descriptorBytes);
    redirectDigest = digest_fd(rfd, 0, [redirects[@"bytes"] unsignedLongLongValue]);
    ok = [payloadDigest isEqual:append[@"sha256"]] &&
         [rootDigest isEqual:manifest[@"final_archive_root_sha256"]] &&
         [descriptorDigest isEqual:manifest[@"final_descriptor_sha256"]] &&
         [redirectDigest isEqual:redirects[@"sha256"]];
out:
    if (afd >= 0) close(afd); if (dfd >= 0) close(dfd); if (rfd >= 0) close(rfd);
    if (!ok && failure) *failure = @"Existing RomFS patch overlay did not verify.";
    return ok;
}

BOOL tv_materialize_romfs_patch(NSString *romfs, NSString *writable,
    NSString *bundle, NSString *expectedGame, NSString **overlay,
    NSString **failure) {
    if (overlay) *overlay = nil; if (failure) *failure = nil;
    if (!romfs.length || !writable.length || !bundle.length || !expectedGame.length)
        return patch_fail(failure, @"RomFS patch arguments are incomplete.");

    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *manifestPath = [bundle stringByAppendingPathComponent:@"manifest.json"];
    NSData *manifestData = read_limited(manifestPath, 128ull << 20, failure);
    if (!manifestData) return NO;
    NSString *manifestDigest = digest_data(manifestData);
    NSError *jsonError = nil;
    NSDictionary *manifest = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:&jsonError];
    NSDictionary *append = [manifest isKindOfClass:NSDictionary.class] ? manifest[@"append"] : nil;
    NSDictionary *redirects = [manifest isKindOfClass:NSDictionary.class] ? manifest[@"redirects"] : nil;
    if (![manifest isKindOfClass:NSDictionary.class] || [manifest[@"schema"] integerValue] != 4 ||
        ![manifest[@"mode"] isEqual:@"virtual-pack-layout-before-root"] ||
        ![manifest[@"game"] isEqual:expectedGame] || ![manifest[@"ready_to_apply_to_new_output_directory"] boolValue] ||
        ![manifest[@"archive_name"] isEqual:@"data.trpfs"] ||
        ![manifest[@"descriptor_name"] isEqual:@"data.trpfd"] ||
        ![append isKindOfClass:NSDictionary.class] ||
        ![redirects isKindOfClass:NSDictionary.class])
        return patch_fail(failure, @"RomFS patch manifest is incompatible with this game.");

    NSArray *headerEdits = manifest[@"archive_header_edits"];
    NSArray *archiveEdits = manifest[@"archive_index_edits"];
    NSArray *descriptorEdits = manifest[@"descriptor_size_edits"];
    NSNumber *archiveBytesNumber = unsigned_number(manifest[@"archive_bytes"]);
    NSNumber *descriptorBytesNumber = unsigned_number(manifest[@"descriptor_bytes"]);
    NSNumber *finalArchiveNumber = unsigned_number(manifest[@"final_archive_bytes"]);
    NSNumber *finalDescriptorNumber = unsigned_number(manifest[@"final_descriptor_bytes"]);
    NSNumber *appendBytesNumber = unsigned_number(append[@"bytes"]);
    NSNumber *appendOffsetNumber = unsigned_number(append[@"output_offset"]);
    NSNumber *rootOffsetNumber = unsigned_number(manifest[@"archive_root_offset"]);
    NSNumber *rootBytesNumber = unsigned_number(manifest[@"archive_root_bytes"]);
    NSNumber *finalRootOffsetNumber = unsigned_number(manifest[@"final_archive_root_offset"]);
    NSNumber *virtualRootOffsetNumber = unsigned_number(manifest[@"virtual_archive_root_offset"]);
    NSNumber *virtualArchiveNumber = unsigned_number(manifest[@"virtual_archive_bytes"]);
    NSNumber *redirectBytesNumber = unsigned_number(redirects[@"bytes"]);
    NSNumber *redirectRecordsNumber = unsigned_number(redirects[@"records"]);
    NSNumber *redirectVirtualNumber = unsigned_number(redirects[@"virtual_file_bytes"]);
    NSNumber *redirectPhysicalNumber = unsigned_number(redirects[@"physical_file_bytes"]);
    NSString *appendName = append[@"file"], *redirectName = redirects[@"file"];
    if (!archiveBytesNumber || !descriptorBytesNumber || !finalArchiveNumber ||
        !finalDescriptorNumber || !appendBytesNumber || !appendOffsetNumber ||
        !rootOffsetNumber || !rootBytesNumber || !finalRootOffsetNumber ||
        !virtualRootOffsetNumber || !virtualArchiveNumber || !redirectBytesNumber ||
        !redirectRecordsNumber || !redirectVirtualNumber || !redirectPhysicalNumber ||
        ![appendName isKindOfClass:NSString.class] || !appendName.length ||
        [appendName containsString:@"/"] || [appendName isEqual:@"."] || [appendName isEqual:@".."] ||
        ![redirectName isKindOfClass:NSString.class] ||
        ![redirectName isEqual:@"redirects.tsv"] ||
        ![redirects[@"target"] isEqual:@"arc/data.trpfs"])
        return patch_fail(failure, @"RomFS patch sizes or payload names are invalid.");
    uint64_t archiveBytes = archiveBytesNumber.unsignedLongLongValue;
    uint64_t descriptorBytes = descriptorBytesNumber.unsignedLongLongValue;
    uint64_t appendBytes = appendBytesNumber.unsignedLongLongValue;
    uint64_t rootOffset = rootOffsetNumber.unsignedLongLongValue;
    uint64_t rootBytes = rootBytesNumber.unsignedLongLongValue;
    uint64_t finalRootOffset = finalRootOffsetNumber.unsignedLongLongValue;
    uint64_t virtualRootOffset = virtualRootOffsetNumber.unsignedLongLongValue;
    uint64_t virtualArchiveBytes = virtualArchiveNumber.unsignedLongLongValue;
    if (archiveBytes > INT64_MAX || descriptorBytes > INT64_MAX ||
        appendBytes > (uint64_t)INT64_MAX - archiveBytes ||
        rootOffset < 16 || rootOffset >= archiveBytes || rootBytes > (64u << 20) ||
        rootBytes != archiveBytes - rootOffset ||
        appendOffsetNumber.unsignedLongLongValue != rootOffset ||
        finalRootOffset != rootOffset + appendBytes ||
        finalArchiveNumber.unsignedLongLongValue != archiveBytes + appendBytes ||
        finalRootOffset + rootBytes != finalArchiveNumber.unsignedLongLongValue ||
        virtualRootOffset + rootBytes != virtualArchiveBytes ||
        redirectBytesNumber.unsignedLongLongValue == 0 ||
        redirectBytesNumber.unsignedLongLongValue > (16u << 20) ||
        redirectRecordsNumber.unsignedLongLongValue == 0 ||
        redirectRecordsNumber.unsignedLongLongValue > 20000 ||
        redirectVirtualNumber.unsignedLongLongValue != virtualArchiveBytes ||
        redirectPhysicalNumber.unsignedLongLongValue != finalArchiveNumber.unsignedLongLongValue ||
        finalDescriptorNumber.unsignedLongLongValue != descriptorBytes ||
        headerEdits.count != 1 || archiveEdits.count != 1 || !descriptorEdits.count ||
        !validate_edits(headerEdits, archiveBytes, @"source_offset", failure) ||
        !validate_edits(headerEdits, archiveBytes + appendBytes, @"output_offset", failure) ||
        !validate_edits(archiveEdits, archiveBytes, @"source_offset", failure) ||
        !validate_edits(archiveEdits, archiveBytes + appendBytes, @"output_offset", failure) ||
        !validate_edits(descriptorEdits, descriptorBytes, @"offset", failure))
        return patch_fail(failure, @"RomFS patch layout or edit tables are invalid.");
    NSDictionary *headerEdit = headerEdits.firstObject;
    NSData *headerSource = hex_data(headerEdit[@"source_hex"]);
    NSData *headerReplacement = hex_data(headerEdit[@"replacement_hex"]);
    uint64_t encodedSourceRoot = 0, encodedFinalRoot = 0;
    if (headerSource.length == 8) memcpy(&encodedSourceRoot, headerSource.bytes, 8);
    if (headerReplacement.length == 8) memcpy(&encodedFinalRoot, headerReplacement.bytes, 8);
    if ([headerEdit[@"source_offset"] unsignedLongLongValue] != 8 ||
        [headerEdit[@"output_offset"] unsignedLongLongValue] != 8 ||
        [headerEdit[@"bytes"] unsignedLongLongValue] != 8 ||
        encodedSourceRoot != rootOffset || encodedFinalRoot != virtualRootOffset)
        return patch_fail(failure, @"RomFS root-pointer edit is invalid.");
    NSDictionary *indexEdit = archiveEdits.firstObject;
    uint64_t indexSourceOffset = [indexEdit[@"source_offset"] unsignedLongLongValue];
    uint64_t indexOutputOffset = [indexEdit[@"output_offset"] unsignedLongLongValue];
    uint64_t indexBytes = [indexEdit[@"bytes"] unsignedLongLongValue];
    NSNumber *packCountNumber = unsigned_number(indexEdit[@"pack_count"]);
    if (indexSourceOffset < rootOffset || indexOutputOffset != indexSourceOffset + appendBytes ||
        !packCountNumber || !indexBytes || indexBytes % 8 ||
        packCountNumber.unsignedLongLongValue != indexBytes / 8)
        return patch_fail(failure, @"RomFS shifted root-index edit is invalid.");

    NSString *parent = [writable stringByAppendingPathComponent:@"romfs-patch-overlays"];
    if (![fm createDirectoryAtPath:parent withIntermediateDirectories:YES attributes:nil error:&jsonError])
        return patch_fail(failure, @"Could not create the RomFS patch directory.");
    NSString *lockPath = [parent stringByAppendingPathComponent:@"materialize.lock"];
    int lock = open(lockPath.fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX)) { if (lock >= 0) close(lock); return patch_fail(failure, @"Could not lock the RomFS patch directory."); }

    BOOL success = NO;
    NSString *root = [parent stringByAppendingPathComponent:manifestDigest];
    NSString *partial = [root stringByAppendingString:@".partial"];
    NSString *arcDir = nil, *sourceArchive = nil, *sourceDescriptor = nil,
        *appendPath = nil, *redirectPath = nil, *sourceADigest = nil, *sourceDDigest = nil,
        *sourceRootDigest = nil, *appendDigest = nil, *redirectDigest = nil,
        *outputArchive = nil, *outputDescriptor = nil, *outputRedirects = nil,
        *finalADigest = nil, *finalDDigest = nil,
        *finalRootDigest = nil, *readyIncoming = nil,
        *readyPath = nil;
    NSDictionary *ready = nil;
    NSData *readyData = nil, *outputRedirectData = nil;
    int sourceA = -1, sourceD = -1, appendFD = -1, redirectFD = -1,
        outputA = -1, outputD = -1;
    if (ready_overlay(root, manifestDigest, manifest, NULL)) {
        fprintf(stderr, "tvOS RomFS patch: verified existing overlay %s\n", manifestDigest.UTF8String);
        if (overlay) *overlay = root; success = YES; goto done;
    }
    [fm removeItemAtPath:partial error:nil];
    if ([fm fileExistsAtPath:root]) [fm removeItemAtPath:root error:nil];
    arcDir = [partial stringByAppendingPathComponent:@"arc"];
    if (![fm createDirectoryAtPath:arcDir withIntermediateDirectories:YES attributes:nil error:&jsonError]) {
        patch_fail(failure, @"Could not create the RomFS patch staging directory."); goto done;
    }

    sourceArchive = [romfs stringByAppendingPathComponent:@"arc/data.trpfs"];
    sourceDescriptor = [romfs stringByAppendingPathComponent:@"arc/data.trpfd"];
    appendPath = [bundle stringByAppendingPathComponent:appendName];
    redirectPath = [bundle stringByAppendingPathComponent:redirectName];
    if (!regular_file(sourceArchive, archiveBytes, &sourceA, failure) ||
        !regular_file(sourceDescriptor, descriptorBytes, &sourceD, failure) ||
        !regular_file(appendPath, appendBytes, &appendFD, failure) ||
        !regular_file(redirectPath, redirectBytesNumber.unsignedLongLongValue,
                      &redirectFD, failure)) goto close_sources;
    if (!verify_ranges(sourceA, headerEdits, @"source_offset", NO, failure) ||
        !verify_ranges(sourceA, archiveEdits, @"source_offset", NO, failure) ||
        !verify_ranges(sourceD, descriptorEdits, @"offset", NO, failure)) goto close_sources;
    sourceADigest = digest_fd(sourceA, 0, archiveBytes);
    sourceDDigest = digest_fd(sourceD, 0, descriptorBytes);
    sourceRootDigest = digest_fd(sourceA, (off_t)rootOffset, rootBytes);
    appendDigest = digest_fd(appendFD, 0, appendBytes);
    redirectDigest = digest_fd(redirectFD, 0, redirectBytesNumber.unsignedLongLongValue);
    if (![sourceADigest isEqual:manifest[@"archive_sha256"]] ||
        ![sourceDDigest isEqual:manifest[@"descriptor_sha256"]] ||
        ![sourceRootDigest isEqual:manifest[@"archive_root_sha256"]] ||
        ![appendDigest isEqual:append[@"sha256"]] ||
        ![redirectDigest isEqual:redirects[@"sha256"]]) {
        patch_fail(failure, @"RomFS source or append SHA-256 differs from the manifest."); goto close_sources;
    }

    outputArchive = [arcDir stringByAppendingPathComponent:@"data.trpfs"];
    outputDescriptor = [arcDir stringByAppendingPathComponent:@"data.trpfd"];
    outputRedirects = [partial stringByAppendingPathComponent:redirectName];
    if (clonefile(sourceArchive.fileSystemRepresentation, outputArchive.fileSystemRepresentation,
                  CLONE_NOFOLLOW) ||
        clonefile(sourceDescriptor.fileSystemRepresentation, outputDescriptor.fileSystemRepresentation,
                  CLONE_NOFOLLOW) ||
        clonefile(redirectPath.fileSystemRepresentation, outputRedirects.fileSystemRepresentation,
                  CLONE_NOFOLLOW)) {
        patch_fail(failure, [NSString stringWithFormat:
            @"APFS cloning the RomFS failed (errno=%d); the original remains active.", errno]); goto close_sources;
    }
    test_exit("cloned");
    outputA = open(outputArchive.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    outputD = open(outputDescriptor.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (outputA < 0 || outputD < 0 ||
        ftruncate(outputA, (off_t)(archiveBytes + appendBytes))) {
        patch_fail(failure, @"Could not extend the RomFS patch output.");
        if (outputA >= 0) close(outputA); if (outputD >= 0) close(outputD);
        outputA = outputD = -1; goto close_sources;
    }
    const size_t copyBufferBytes = 1u << 20;
    uint8_t *copyBuffer = malloc(copyBufferBytes);
    if (!copyBuffer) {
        patch_fail(failure, @"Could not allocate the RomFS append buffer.");
        close(outputA); close(outputD); outputA = outputD = -1; goto close_sources;
    }
    uint64_t copiedRoot = 0;
    while (copiedRoot < rootBytes) {
        size_t want = (size_t)MIN(rootBytes - copiedRoot, copyBufferBytes);
        if (!exact_read(sourceA, rootOffset + copiedRoot, copyBuffer, want) ||
            !exact_write(outputA, finalRootOffset + copiedRoot, copyBuffer, want)) {
            patch_fail(failure, @"Could not shift the RomFS root tail."); break;
        }
        copiedRoot += want;
    }
    uint64_t copiedAppend = 0;
    while (copiedRoot == rootBytes && copiedAppend < appendBytes) {
        size_t want = (size_t)MIN(appendBytes - copiedAppend, copyBufferBytes);
        if (!exact_read(appendFD, copiedAppend, copyBuffer, want) ||
            !exact_write(outputA, rootOffset + copiedAppend, copyBuffer, want)) {
            patch_fail(failure, @"Could not insert relocated RomFS packs before the root."); break;
        }
        copiedAppend += want;
    }
    free(copyBuffer);
    if (copiedRoot != rootBytes || copiedAppend != appendBytes ||
        !apply_edits(outputA, headerEdits, @"output_offset", failure) ||
        !apply_edits(outputA, archiveEdits, @"output_offset", failure) ||
        !apply_edits(outputD, descriptorEdits, @"offset", failure) ||
        fsync(outputA) || fsync(outputD) || !fsync_path(outputRedirects, NO)) {
        if (copiedRoot == rootBytes && copiedAppend == appendBytes &&
            (!failure || !*failure)) patch_fail(failure, @"Could not sync the RomFS patch output.");
        close(outputA); close(outputD); outputA = outputD = -1; goto close_sources;
    }
    test_exit("written");
    if (!verify_ranges(outputA, headerEdits, @"output_offset", YES, failure) ||
        !verify_ranges(outputA, archiveEdits, @"output_offset", YES, failure) ||
        !verify_ranges(outputD, descriptorEdits, @"offset", YES, failure)) {
        close(outputA); close(outputD); outputA = outputD = -1; goto close_sources;
    }
    appendDigest = digest_fd(outputA, (off_t)rootOffset, appendBytes);
    finalRootDigest = digest_fd(outputA, (off_t)finalRootOffset, rootBytes);
    finalADigest = digest_fd(outputA, 0, archiveBytes + appendBytes);
    finalDDigest = digest_fd(outputD, 0, descriptorBytes);
    close(outputA); close(outputD); outputA = outputD = -1;
    outputRedirectData = read_limited(outputRedirects, 16u << 20, failure);
    if (![appendDigest isEqual:append[@"sha256"]] ||
        ![finalRootDigest isEqual:manifest[@"final_archive_root_sha256"]] ||
        ![finalADigest isEqual:manifest[@"final_archive_sha256"]] ||
        ![finalDDigest isEqual:manifest[@"final_descriptor_sha256"]] ||
        !outputRedirectData || ![digest_data(outputRedirectData) isEqual:redirects[@"sha256"]]) {
        patch_fail(failure, @"Materialized RomFS patch SHA-256 did not verify."); goto close_sources;
    }
    close(sourceA); sourceA = -1; close(sourceD); sourceD = -1;
    close(appendFD); appendFD = -1; close(redirectFD); redirectFD = -1;

    ready = @{@"schema": @1, @"manifest_sha256": manifestDigest,
        @"archive_sha256": finalADigest, @"descriptor_sha256": finalDDigest,
        @"redirects_sha256": redirectDigest,
        @"virtual_archive_bytes": @(virtualArchiveBytes)};
    readyData = [NSJSONSerialization dataWithJSONObject:ready options:NSJSONWritingPrettyPrinted error:&jsonError];
    readyIncoming = [partial stringByAppendingPathComponent:@"READY.json.incoming"];
    readyPath = [partial stringByAppendingPathComponent:@"READY.json"];
    if (!readyData || ![readyData writeToFile:readyIncoming options:NSDataWritingWithoutOverwriting error:&jsonError] ||
        !fsync_path(readyIncoming, NO) || rename(readyIncoming.fileSystemRepresentation, readyPath.fileSystemRepresentation) ||
        !fsync_path(arcDir, YES) || !fsync_path(partial, YES)) {
        patch_fail(failure, @"Could not publish the RomFS patch READY marker."); goto done;
    }
    test_exit("ready");
    if (rename(partial.fileSystemRepresentation, root.fileSystemRepresentation) ||
        !fsync_path(parent, YES)) {
        patch_fail(failure, @"Could not atomically publish the RomFS patch overlay."); goto done;
    }
    test_exit("published");
    fprintf(stderr, "tvOS RomFS patch: materialized and verified %s\n", manifestDigest.UTF8String);
    if (overlay) *overlay = root; success = YES; goto done;

close_sources:
    if (sourceA >= 0) close(sourceA); if (sourceD >= 0) close(sourceD);
    if (appendFD >= 0) close(appendFD); if (redirectFD >= 0) close(redirectFD);
done:
    if (!success) [fm removeItemAtPath:partial error:nil];
    flock(lock, LOCK_UN); close(lock);
    return success;
}
