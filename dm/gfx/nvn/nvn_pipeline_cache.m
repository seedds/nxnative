#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "nvn_pipeline_cache.h"
#include "dmsh_chain.h"
#include "game_profile.h"

#define PCLOG(...) do { fprintf(stderr, "[pipeline-cache] " __VA_ARGS__); fputc('\n', stderr); } while (0)
// Bump only when descriptor generation or utility shader semantics change.
#define CACHE_ABI "nvn-ios17-pso-4"
#define CACHE_MAX_BYTES (256ull << 20)
#define CACHE_RESERVE_BYTES (128ull << 20)
static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static id<MTLBinaryArchive> archive;
static NSString *cache_file;
static nvn_cache_status stats;
static int initialized, save_scheduled;

static NSString *digest(NSData *data) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    NSMutableString *s = [NSMutableString string];
    for (unsigned i = 0; i < sizeof hash; ++i) [s appendFormat:@"%02x", hash[i]];
    return s;
}
static NSString *file_digest(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    return d ? digest(d) : @"missing";
}
// Called under cache_mu. The IPA build number is deliberately absent: compatible
// updates reuse binaries. Game modules, shader tables, GPU and OS partition them.
static void cache_init(id<MTLDevice> device) {
    if (initialized) return;
    initialized = 1;
    const char *override = getenv("DM_PIPELINE_CACHE_DIR");
    if (getenv("DM_PIPELINE_CACHE") && !strcmp(getenv("DM_PIPELINE_CACHE"), "off")) { stats.disabled = 1; return; }
#if !TARGET_OS_IPHONE
    if (!override) { stats.disabled = 1; return; } // Host fixtures opt in explicitly.
#endif
    NSString *dm = getenv("DM_NVN_SHMAP") ? [@(getenv("DM_NVN_SHMAP")) stringByDeletingLastPathComponent] : nil;
    if (!dm) { stats.disabled = 1; return; }
    char osbuild[128] = {0}; size_t size = sizeof osbuild;
    sysctlbyname("kern.osversion", osbuild, &size, NULL, 0);
    NSMutableArray *identity = [NSMutableArray arrayWithArray:@[@CACHE_ABI, @DM_GAME_ID, @DMSH_CHAIN,
        device.name ?: @"unknown", @(device.registryID), @(osbuild),
        NSProcessInfo.processInfo.operatingSystemVersionString]];
    NSArray *files = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dm error:nil] sortedArrayUsingSelector:@selector(compare:)];
    BOOL has_main = NO;
    for (NSString *name in files) if ([name hasSuffix:@".fixups.tsv"] || [@[@"shader_map.tsv", @"c1_full.tsv"] containsObject:name]) {
        [identity addObject:name]; [identity addObject:file_digest([dm stringByAppendingPathComponent:name])];
        if ([name isEqualToString:@"main.fixups.tsv"]) has_main = YES;
    }
#if TARGET_OS_IPHONE
    if (!has_main) { stats.disabled = 1; PCLOG("missing module identity; persistence disabled"); return; }
#else
    (void)has_main;
#endif
    NSString *key = digest([NSJSONSerialization dataWithJSONObject:identity options:0 error:nil]);
    NSString *root = override ? @(override) : [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject] stringByAppendingPathComponent:@"NXPipelineCache"];
    NSString *dir = [[root stringByAppendingPathComponent:@DM_GAME_ID] stringByAppendingPathComponent:key];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&error]) {
        stats.disabled = stats.write_failed = 1; PCLOG("directory unavailable: %s", error.localizedDescription.UTF8String); return;
    }
    [[NSURL fileURLWithPath:root] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    cache_file = [dir stringByAppendingPathComponent:@"pipelines.metalarc"];
    MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
    NSDictionary *attr = [NSFileManager.defaultManager attributesOfItemAtPath:cache_file error:nil];
    if (attr && [attr fileSize] <= CACHE_MAX_BYTES) d.url = [NSURL fileURLWithPath:cache_file];
    archive = [device newBinaryArchiveWithDescriptor:d error:&error];
    if (!archive && d.url) {
        PCLOG("archive rejected; rebuilding safely: %s", error.localizedDescription.UTF8String);
        d.url = nil; error = nil; archive = [device newBinaryArchiveWithDescriptor:d error:&error];
    } else if (archive && d.url) stats.loaded = 1;
    if (!archive) { stats.disabled = 1; PCLOG("archive unavailable: %s", error.localizedDescription.UTF8String); }
    PCLOG("identity=%s loaded=%d disabled=%d", key.UTF8String, stats.loaded, stats.disabled);
}

static BOOL can_archive(id<MTLFunction> function) {
    if (!function) return YES;
    // Device serialization of specialized functions is only supported from iOS18/
    // macOS15. Conservatively bypass functions declaring constants on older OSes.
    if (@available(iOS 18.0, macOS 15.0, *)) return YES;
    return function.functionConstantsDictionary.count == 0;
}
static void schedule_save_locked(void) {
    if (save_scheduled || !archive || stats.write_failed) return;
    save_scheduled = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        nvn_cache_flush();
        pthread_mutex_lock(&cache_mu); save_scheduled = 0; pthread_mutex_unlock(&cache_mu);
    });
}

id<MTLRenderPipelineState> nvn_cached_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error) {
    return nvn_cached_render_specialized(device, desc, 0, error);
}
id<MTLRenderPipelineState> nvn_cached_render_specialized(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, int specialized, NSError **error) {
    pthread_mutex_lock(&cache_mu); cache_init(device);
    BOOL eligible = archive && can_archive(desc.vertexFunction) && can_archive(desc.fragmentFunction);
    if (specialized) {
        if (@available(iOS 18.0, macOS 15.0, *)) { /* Device serialization supported. */ }
        else eligible = NO;
    }
    MTLRenderPipelineDescriptor *d = eligible ? [desc copy] : nil;
    id<MTLRenderPipelineState> result = nil;
    if (eligible) {
        d.binaryArchives = @[archive]; NSError *miss = nil;
        result = [device newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&miss];
        if (result) stats.hits++; else stats.misses++;
    } else stats.bypassed++;
    pthread_mutex_unlock(&cache_mu);
    if (result) { if (error) *error = nil; return result; }
    // Ordinary compilation stays outside the archive mutex so prediction retains
    // its parallelism. A cache/storage failure never changes the actual descriptor.
    result = [device newRenderPipelineStateWithDescriptor:desc error:error];
    if (result && eligible) {
        pthread_mutex_lock(&cache_mu); NSError *add_error = nil;
        d.binaryArchives = nil;
        if ([archive addRenderPipelineFunctionsWithDescriptor:d error:&add_error]) { stats.added++; schedule_save_locked(); }
        else { stats.bypassed++; if (stats.bypassed < 5) PCLOG("render archive fallback: %s", add_error.localizedDescription.UTF8String); }
        pthread_mutex_unlock(&cache_mu);
    }
    return result;
}
id<MTLComputePipelineState> nvn_cached_compute(id<MTLDevice> device, id<MTLFunction> function, NSError **error) {
    pthread_mutex_lock(&cache_mu); cache_init(device);
    BOOL eligible = archive && can_archive(function);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = function;
    id<MTLComputePipelineState> result = nil;
    if (eligible) {
        d.binaryArchives = @[archive]; NSError *miss = nil;
        result = [device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&miss];
        if (result) stats.hits++; else stats.misses++;
    } else stats.bypassed++;
    pthread_mutex_unlock(&cache_mu);
    if (result) { if (error) *error = nil; return result; }
    result = [device newComputePipelineStateWithFunction:function error:error];
    if (result && eligible) {
        pthread_mutex_lock(&cache_mu); NSError *add_error = nil; d.binaryArchives = nil;
        if ([archive addComputePipelineFunctionsWithDescriptor:d error:&add_error]) { stats.added++; schedule_save_locked(); }
        else { stats.bypassed++; if (stats.bypassed < 5) PCLOG("compute archive fallback: %s", add_error.localizedDescription.UTF8String); }
        pthread_mutex_unlock(&cache_mu);
    }
    return result;
}
void nvn_cache_snapshot(nvn_cache_status *out) {
    // UI polling must never wait behind a compiler or a disk serialization.
    static _Thread_local nvn_cache_status last;
    if (!pthread_mutex_trylock(&cache_mu)) { last = stats; pthread_mutex_unlock(&cache_mu); }
    *out = last;
}
void nvn_cache_flush(void) {
    @autoreleasepool {
        pthread_mutex_lock(&cache_mu);
        if (!archive || stats.added == stats.saved || stats.write_failed) { pthread_mutex_unlock(&cache_mu); return; }
        NSString *dir = cache_file.stringByDeletingLastPathComponent;
        NSString *temp = [dir stringByAppendingPathComponent:@"pipelines.pending"];
        struct statvfs space; NSError *error = nil;
        BOOL ok = !statvfs(dir.fileSystemRepresentation, &space) && (uint64_t)space.f_bavail * space.f_frsize > CACHE_MAX_BYTES + CACHE_RESERVE_BYTES;
        if (ok) ok = [archive serializeToURL:[NSURL fileURLWithPath:temp] error:&error];
        NSDictionary *attr = ok ? [NSFileManager.defaultManager attributesOfItemAtPath:temp error:nil] : nil;
        ok = ok && attr && [attr fileSize] > 0 && [attr fileSize] <= CACHE_MAX_BYTES;
        if (ok) { int fd = open(temp.fileSystemRepresentation, O_RDONLY); ok = fd >= 0 && fsync(fd) == 0; if (fd >= 0) close(fd); }
        if (ok) ok = rename(temp.fileSystemRepresentation, cache_file.fileSystemRepresentation) == 0;
        if (ok) {
            stats.saved = stats.added;
            int fd = open(dir.fileSystemRepresentation, O_RDONLY); if (fd >= 0) { fsync(fd); close(fd); }
            PCLOG("saved added=%llu hits=%llu misses=%llu bypassed=%llu bytes=%llu", stats.saved, stats.hits, stats.misses, stats.bypassed, [attr fileSize]);
            // Retain at most the current and most recent previous identity. Only
            // our fixed archive/temp filenames in hash-named directories qualify.
            NSString *parent = dir.stringByDeletingLastPathComponent;
            NSMutableArray<NSString *> *old = [NSMutableArray array];
            NSCharacterSet *nonhex = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet];
            for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:parent error:nil])
                if (name.length == 64 && [name rangeOfCharacterFromSet:nonhex].location == NSNotFound && ![name isEqual:dir.lastPathComponent]) [old addObject:[parent stringByAppendingPathComponent:name]];
            [old sortUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
                NSDate *da = [[NSFileManager.defaultManager attributesOfItemAtPath:[a stringByAppendingPathComponent:@"pipelines.metalarc"] error:nil] fileModificationDate] ?: NSDate.distantPast;
                NSDate *db = [[NSFileManager.defaultManager attributesOfItemAtPath:[b stringByAppendingPathComponent:@"pipelines.metalarc"] error:nil] fileModificationDate] ?: NSDate.distantPast;
                return [db compare:da];
            }];
            for (NSUInteger i = 1; i < old.count; ++i) {
                for (NSString *name in @[@"pipelines.metalarc", @"pipelines.pending"]) [NSFileManager.defaultManager removeItemAtPath:[old[i] stringByAppendingPathComponent:name] error:nil];
                rmdir(old[i].fileSystemRepresentation); // Never recursively delete unknown content.
            }
        } else { stats.write_failed = 1; PCLOG("persistence failed; previous archive retained: %s", error ? error.localizedDescription.UTF8String : "space/size/atomic write check"); }
        [NSFileManager.defaultManager removeItemAtPath:temp error:nil];
        pthread_mutex_unlock(&cache_mu);
    }
}
void nvn_cache_flush_async(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ nvn_cache_flush(); });
}
