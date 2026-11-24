// Retained library input must outlive its temporary source mapping. A synthetic device keeps every buffer, including on failure; no GPU or game files needed.
#import <Metal/Metal.h>
#include <sys/mman.h>
static void *library_map(void *, size_t, int, int, int, off_t);
static int library_unmap(void *, size_t);
#define mmap library_map
#define munmap library_unmap
#include "../nvn/nvn_mtl.m"
#undef mmap
#undef munmap

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed, live_maps, reject_library, loose, source_fd;
static size_t live_bytes, peak_bytes, map_budget;
#define EXPECT(name, want, got) do { checks++; uint64_t w_ = (want), g_ = (got); if (w_ != g_) { failed++; fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); } } while (0)
static size_t pages(size_t n) { size_t pg = (size_t)getpagesize(); return (n + pg - 1) / pg * pg; }
static void *library_map(void *p, size_t n, int prot, int flags, int fd, off_t off) {
    size_t need = pages(n);
    if (need > map_budget || live_bytes > map_budget - need) { errno = ENOMEM; return MAP_FAILED; }
    void *v = mmap(p, n, prot, flags, fd, off);
    if (v != MAP_FAILED) { live_maps++; live_bytes += need; peak_bytes = MAX(peak_bytes, live_bytes); source_fd = fd; }
    return v;
}
static int library_unmap(void *p, size_t n) {
    int r = munmap(p, n); if (!r) { live_maps--; live_bytes -= pages(n); } return r;
}

@interface LibraryDevice : NSObject { @public NSMutableArray *data; }
@end
@implementation LibraryDevice
- (id)newLibraryWithData:(dispatch_data_t)d error:(NSError **)error {
    EXPECT("source mapping gone before Metal retains the data", 0, live_maps);
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
    LibraryDevice *dev = [LibraryDevice new]; g_dev = (id)dev;
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
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_library_data: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
