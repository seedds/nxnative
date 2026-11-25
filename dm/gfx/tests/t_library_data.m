// Retained library input must outlive its temporary source mapping. A synthetic device keeps every buffer, including on failure; no GPU or game files needed.
#import <Metal/Metal.h>
#include <sys/mman.h>
static void *library_map(void *, size_t, int, int, int, off_t);
static int library_unmap(void *, size_t);
static dispatch_data_t library_data_create(const void *, size_t, dispatch_queue_t, dispatch_block_t);
#define mmap library_map
#define munmap library_unmap
#define dispatch_data_create library_data_create
#include "../nvn/nvn_mtl.m"
#undef mmap
#undef munmap
#undef dispatch_data_create

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static _Atomic int live_maps; static int checks, failed, reject_library, fail_data_create, loose, source_fd;
static _Atomic size_t live_bytes; static size_t peak_bytes, map_budget;   // a borrowed mapping's destructor runs on a global queue
#define EXPECT(name, want, got) do { checks++; uint64_t w_ = (want), g_ = (got); if (w_ != g_) { failed++; fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); } } while (0)
static size_t pages(size_t n) { size_t pg = (size_t)getpagesize(); return (n + pg - 1) / pg * pg; }
static void *library_map(void *p, size_t n, int prot, int flags, int fd, off_t off) {
    size_t need = pages(n);
    if (need > map_budget || live_bytes > map_budget - need) { errno = ENOMEM; return MAP_FAILED; }
    void *v = mmap(p, n, prot, flags, fd, off);
    if (v != MAP_FAILED) { live_maps++; live_bytes += need; peak_bytes = MAX(peak_bytes, (size_t)live_bytes); source_fd = fd; }
    return v;
}
static int library_unmap(void *p, size_t n) {
    int r = munmap(p, n); if (!r) { live_bytes -= pages(n); live_maps--; } return r;   // bytes first: live_maps at 0 means the accounting is done
}
static dispatch_data_t library_data_create(const void *buf, size_t sz, dispatch_queue_t q, dispatch_block_t dtor) {
    if (fail_data_create) return NULL;
    return dispatch_data_create(buf, sz, q, dtor);
}

@interface LibraryDevice : NSObject { @public NSMutableArray *data; }
@end
@implementation LibraryDevice
- (id)newLibraryWithData:(dispatch_data_t)d error:(NSError **)error {
    if (atomic_load(&g_lib_borrow)) EXPECT("borrowed mapping still live when Metal takes the data", 1, live_maps > 0);
    else EXPECT("source mapping gone before Metal retains the data", 0, live_maps);
    if (loose) EXPECT("loose descriptor closed before entering Metal", -1, fcntl(source_fd, F_GETFD));
    if (!data) data = [NSMutableArray array]; [data addObject:d];
    if (reject_library) { if (error) *error = [NSError errorWithDomain:@"fixture" code:1 userInfo:nil]; return nil; }
    return [NSObject new];
}
@end

int main(void) { @autoreleasepool {
    char tmp[] = "/tmp/t_library_data.XXXXXX"; if (!mkdtemp(tmp)) return 2;
    NSString *root = @(tmp), *folder = [root stringByAppendingPathComponent:@"msl"], *file = [folder stringByAppendingPathComponent:@"fixture.metallib"];
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableData *bytes = [NSMutableData dataWithLength:65537];
    for (NSUInteger i = 0; i < bytes.length; i++) ((uint8_t *)bytes.mutableBytes)[i] = (uint8_t)(i * 17);
    EXPECT("loose fixture written", 1, [bytes writeToFile:file atomically:YES]);
    LibraryDevice *dev = [LibraryDevice new]; g_dev = (id)dev; g_lib_borrow = 0;   // owned copy (iOS) first; borrowed mapping (Mac) below
    map_budget = pages(bytes.length); loose = 1;   // one source mapping fits; retained input buffers must not consume the mapping budget
    for (int i = 0; i < 128; i++) { @autoreleasepool {
        id lib = lib_from_file(file, NULL);
        EXPECT("every library loads under a one-library mapping budget", 1, lib != nil);
        EXPECT("library keeps its content digest", 1, [objc_getAssociatedObject(lib, &g_lib_tag) isEqual:lib_digest(bytes.bytes, bytes.length)]);
    } }
    EXPECT("peak source mappings fit one library", map_budget, peak_bytes);
    reject_library = 1; NSError *error = nil;
    EXPECT("Metal load failure returned", 1, lib_from_file(file, &error) == nil && error != nil);
    EXPECT("failure leaves no source mapping", 0, live_maps); reject_library = 0;
    map_budget = 0;
    EXPECT("source mmap failure preserves ENOMEM", 1, lib_from_file(file, &error) == nil && [error.domain isEqual:NSPOSIXErrorDomain] && error.code == ENOMEM);
    map_budget = pages(bytes.length); loose = 0;

    // The image's unaligned payload must be copied exactly, then its entire page-aligned mapping released.
    NSString *packer = [[@(__FILE__) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../../tools/shader_lib.py"];
    NSTask *pack = [NSTask new]; pack.executableURL = [NSURL fileURLWithPath:@"/usr/bin/python3"];
    pack.arguments = @[packer, @"pack", folder];
    if (![pack launchAndReturnError:nil]) return 2;
    [pack waitUntilExit]; EXPECT("image packed", 0, pack.terminationStatus);
    g_msl_dir = folder;
    id lib = lib_from_file(file, NULL);
    EXPECT("image library loads under the same budget", 1, lib != nil);
    EXPECT("image has the same content digest", 1, [objc_getAssociatedObject(lib, &g_lib_tag) isEqual:lib_digest(bytes.bytes, bytes.length)]);
    EXPECT("no source mappings survive any library", 0, live_maps);
    EXPECT("owned library data count matches succeeded loads", 129, atomic_load(&g_lib_data_n));
    EXPECT("owned library data bytes match succeeded loads", 129 * bytes.length, atomic_load(&g_lib_data_bytes));
    EXPECT("owned library data allocation failures zero", 0, atomic_load(&g_lib_data_fails));
    [NSFileManager.defaultManager removeItemAtPath:[folder stringByAppendingString:@".img"] error:nil];
    for (dispatch_data_t d in dev->data) {
        const void *p = NULL; size_t n = 0; dispatch_data_t mapped = dispatch_data_create_map(d, &p, &n);
        EXPECT("retained data has exact bytes after source deletion", 1, mapped && n == bytes.length && !memcmp(p, bytes.bytes, n));
    }
    romfs_image_close(atomic_exchange(&g_msl_img, NULL)); g_msl_dir = nil; dev->data = nil;
    EXPECT("missing file reports an error", 1, lib_from_file(file, &error) == nil && error != nil);

    // The Mac borrows the mapping (lib_borrow): it lives as long as Metal's data, for loose and packed-image libraries alike, and releases on failure too.
    unsetenv("DM_MSL_LIB_COPY"); g_lib_borrow = -1; EXPECT("the Mac borrows by default", TARGET_OS_OSX, lib_borrow());
    setenv("DM_MSL_LIB_COPY", "", 1); g_lib_borrow = -1; EXPECT("empty DM_MSL_LIB_COPY keeps default", TARGET_OS_OSX, lib_borrow());
    setenv("DM_MSL_LIB_COPY", "1", 1); g_lib_borrow = -1; EXPECT("DM_MSL_LIB_COPY=1 copies", 0, lib_borrow());
    setenv("DM_MSL_LIB_COPY", "01", 1); g_lib_borrow = -1; EXPECT("DM_MSL_LIB_COPY=01 copies", 0, lib_borrow());
    setenv("DM_MSL_LIB_COPY", "0", 1); g_lib_borrow = -1; EXPECT("DM_MSL_LIB_COPY=0 borrows", 1, lib_borrow()); unsetenv("DM_MSL_LIB_COPY");

    g_lib_borrow = 1; loose = 1; map_budget = 64 * pages(bytes.length);
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    EXPECT("loose fixture written again", 1, [bytes writeToFile:file atomically:YES]);
    @autoreleasepool { for (int i = 0; i < 8; i++) {
        id l = lib_from_file(file, NULL);
        EXPECT("borrowed loose library loads", 1, l != nil);
        EXPECT("borrowed loose library keeps its content digest", 1, [objc_getAssociatedObject(l, &g_lib_tag) isEqual:lib_digest(bytes.bytes, bytes.length)]);
    } }
    EXPECT("one live mapping per loose library Metal holds", 8, live_maps);
    for (dispatch_data_t d in dev->data) {
        const void *p = NULL; size_t n = 0; dispatch_data_t mapped = dispatch_data_create_map(d, &p, &n);
        EXPECT("borrowed loose data has the file's bytes", 1, mapped && n == bytes.length && !memcmp(p, bytes.bytes, n));
    }
    reject_library = 1; error = nil;
    @autoreleasepool { EXPECT("borrowed load failure returned with Metal's error", 1, lib_from_file(file, &error) == nil && [error.domain isEqual:@"fixture"]); }
    reject_library = 0;
    EXPECT("Metal keeps the rejected library's mapping while it holds the data", 9, live_maps);
    fail_data_create = 1; error = nil;
    @autoreleasepool { EXPECT("borrowed dispatch_data failure returns ENOMEM", 1, lib_from_file(file, &error) == nil && [error.domain isEqual:NSPOSIXErrorDomain] && error.code == ENOMEM); }
    fail_data_create = 0;
    EXPECT("borrowed dispatch_data failure unmaps immediately", 9, live_maps);
    EXPECT("borrowed dispatch_data failure recorded", 1, atomic_load(&g_lib_data_fails));
    EXPECT("borrowed dispatch_data failure bytes recorded", bytes.length, atomic_load(&g_lib_data_fail_bytes));
    dev->data = nil;
    for (int i = 0; i < 200 && live_maps; i++) usleep(10000);   // the destructor runs on a global queue
    EXPECT("every borrowed loose mapping released with Metal's data", 0, live_maps);
    EXPECT("every borrowed loose mapping byte released", 0, live_bytes);

    // Packed image entry in borrowed mode: unaligned payload offset inside page-aligned mapping, still readable after image unlink/close, unmapped cleanly on release.
    NSTask *pack2 = [NSTask new]; pack2.executableURL = [NSURL fileURLWithPath:@"/usr/bin/python3"];
    pack2.arguments = @[packer, @"pack", folder];
    if (![pack2 launchAndReturnError:nil]) return 2;
    [pack2 waitUntilExit]; EXPECT("image packed for borrow test", 0, pack2.terminationStatus);
    g_msl_dir = folder; loose = 0;
    @autoreleasepool { for (int i = 0; i < 4; i++) {
        id l = lib_from_file(file, NULL);
        EXPECT("borrowed image library loads", 1, l != nil);
        EXPECT("borrowed image library keeps its content digest", 1, [objc_getAssociatedObject(l, &g_lib_tag) isEqual:lib_digest(bytes.bytes, bytes.length)]);
    } }
    EXPECT("one live mapping per packed image library Metal holds", 4, live_maps);
    [NSFileManager.defaultManager removeItemAtPath:[folder stringByAppendingString:@".img"] error:nil];
    romfs_image_close(atomic_exchange(&g_msl_img, NULL)); g_msl_dir = nil;
    for (dispatch_data_t d in dev->data) {
        const void *p = NULL; size_t n = 0; dispatch_data_t mapped = dispatch_data_create_map(d, &p, &n);
        EXPECT("borrowed image data has exact bytes after image unlink and close", 1, mapped && n == bytes.length && !memcmp(p, bytes.bytes, n));
    }
    dev->data = nil;
    for (int i = 0; i < 200 && live_maps; i++) usleep(10000);
    EXPECT("every borrowed image mapping released with Metal's data", 0, live_maps);
    EXPECT("every borrowed image mapping byte released", 0, live_bytes);
    EXPECT("library data count includes succeeded borrowed loads", 141, atomic_load(&g_lib_data_n));
    EXPECT("library data bytes include succeeded borrowed loads", 141 * bytes.length, atomic_load(&g_lib_data_bytes));

    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_library_data: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
