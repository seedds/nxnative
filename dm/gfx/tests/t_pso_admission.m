// Drive the real archive initialization with a phone-sized VM map. Sparse, valid containers stand in for large archives; the device counts opens and compiles, so a
// rejected set must never reach Metal, create a serializer, or retire a usable file. Each case runs in a fresh process (pso4_init is dispatch_once).
#import <Metal/Metal.h>
#include <mach/mach.h>
static kern_return_t admission_task_info(task_name_t, task_flavor_t, task_info_t, mach_msg_type_number_t *);
static kern_return_t admission_region(vm_map_t, vm_address_t *, vm_size_t *, natural_t *, vm_region_recurse_info_t, mach_msg_type_number_t *);
#define task_info admission_task_info
#define vm_region_recurse_64 admission_region
#include "../nvn/nvn_mtl.m"
#undef task_info
#undef vm_region_recurse_64

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed, unknown, native, map_fail;
static uint64_t vm_lo, vm_hi;
static struct { uint64_t at, size; } regions[4];
static int nr;
#define EXPECT(name, want, got) do { checks++; uint64_t w = (want), g = (got); if (w != g) { failed++; fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); } } while (0)

static kern_return_t admission_task_info(task_name_t task, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t *n) {
    if (native) return task_info(task, flavor, out, n);
    if (unknown || flavor != TASK_VM_INFO) return KERN_FAILURE;
    task_vm_info_data_t *v = (void *)out; memset(v, 0, sizeof *v);
    v->min_address = vm_lo; v->max_address = vm_hi; *n = TASK_VM_INFO_COUNT;
    return KERN_SUCCESS;
}
static kern_return_t admission_region(vm_map_t task, vm_address_t *a, vm_size_t *size, natural_t *depth, vm_region_recurse_info_t out, mach_msg_type_number_t *n) {
    if (native) return vm_region_recurse_64(task, a, size, depth, out, n);
    if (map_fail) return KERN_FAILURE;
    for (int i = 0; i < nr; i++) if (regions[i].at + regions[i].size > *a) {
        *a = regions[i].at; *size = regions[i].size;
        vm_region_submap_info_data_64_t *in = (void *)out; memset(in, 0, sizeof *in);
        in->is_submap = i == 0;   // the whole top-level submap is occupied, including any gaps inside it
        *n = VM_REGION_SUBMAP_INFO_COUNT_64; return KERN_SUCCESS;
    }
    return KERN_INVALID_ADDRESS;
}

@interface AdmissionGPU : NSObject
@property(readonly) NSString *name;
@end
@implementation AdmissionGPU
- (NSString *)name { return @"testgpu"; }
@end
@interface AdmissionDevice : NSObject { @public int opens, compilers, renders, computes; id pipeline; }
@end
@implementation AdmissionDevice
- (id)architecture { return [AdmissionGPU new]; }
- (id)newArchiveWithURL:(NSURL *)url error:(NSError **)error { opens++; return [NSObject new]; }
- (id)newCompilerWithDescriptor:(id)descriptor error:(NSError **)error { compilers++; return nil; }
- (id)newRenderPipelineStateWithDescriptor:(id)descriptor error:(NSError **)error { renders++; return pipeline; }
- (id)newComputePipelineStateWithFunction:(id)function error:(NSError **)error { computes++; return pipeline; }
@end

static void archive(NSString *path, uint64_t bytes) {
    [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    int fd = open(path.fileSystemRepresentation, O_CREAT | O_EXCL | O_WRONLY, 0600); if (fd < 0) abort();
    struct mach_header_64 h = {.magic = MH_MAGIC_64, .filetype = MH_GPU_EXECUTE, .ncmds = 1, .sizeofcmds = sizeof(struct note_command)};
    struct note_command note = {.cmd = LC_NOTE, .cmdsize = sizeof note, .offset = sizeof h + sizeof note, .size = 1};
    memcpy(note.data_owner, "AIR_PIPELINE", sizeof "AIR_PIPELINE");
    if (write(fd, &h, sizeof h) != sizeof h || write(fd, &note, sizeof note) != sizeof note || ftruncate(fd, (off_t)bytes)) abort();
    close(fd); EXPECT("fixture container valid", 1, arch_file_ok(path));
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    const uint64_t G = 1ull << 30;
    int exact = !strcmp(argv[1], "exact"), fragmented = !strcmp(argv[1], "fragmented");
    int admit = !strcmp(argv[1], "ample") || !strcmp(argv[1], "native") || exact, empty = !strcmp(argv[1], "empty");
    unknown = !strcmp(argv[1], "unknown"); native = !strcmp(argv[1], "native");
    map_fail = !strcmp(argv[1], "map-fail");
    vm_lo = G; vm_hi = vm_lo + (admit ? 16 : empty ? 5 : fragmented ? 9 : 7) * G;
    if (exact) vm_hi = vm_lo + 7 * G + G / 2;
    regions[nr++] = (typeof(regions[0])){vm_lo, 4 * G};   // guest reservation
    if (fragmented) regions[nr++] = (typeof(regions[0])){vm_lo + 5 * G + G / 2, G / 2};
    char tmp[] = "/tmp/t_pso_admission.XXXXXX"; if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *bundle = [root stringByAppendingPathComponent:@"bundle"], *device = [root stringByAppendingPathComponent:@"device"];
    NSArray<NSString *> *paths = @[[bundle stringByAppendingPathComponent:@"testgpu.mtl4a"], [bundle stringByAppendingPathComponent:@"testgpu-prewarm-00.mtl4a"], [device stringByAppendingPathComponent:@"0.mtl4a"]];
    if (!empty) for (NSString *p in paths) archive(p, G / 2);
    NSString *stamp = [device stringByAppendingPathComponent:@"prebuilt.tsv"];
    [NSFileManager.defaultManager createDirectoryAtPath:device withIntermediateDirectories:YES attributes:nil error:nil];
    [@"keep this stamp\n" writeToFile:stamp atomically:YES encoding:NSUTF8StringEncoding error:nil];
    setenv("DM_PSO_BUNDLE", bundle.UTF8String, 1); setenv("DM_PSO_ARCHIVE", device.UTF8String, 1); setenv("DM_PSO_MTL4", "1", 1);
    AdmissionDevice *dev = [AdmissionDevice new]; dev->pipeline = [NSObject new]; g_dev = (id)dev;
    pso4_init();
    EXPECT("archive set admitted together", admit ? 3 : 0, dev->opens);
    EXPECT("no compiler on rejection", admit ? 1 : 0, dev->compilers);
    EXPECT("no serializer on rejection", 0, g_arch_cap);
    if (admit && !native) {
        // Newly prebuilt p*.mtl4a files must also obey the 2 GiB address-space reserve before being opened in-run.
        // exact: 1.5 GiB existing + 2.0 GiB reserve fills the 3.5 GiB gap (no room for +0.5 GiB); ample: 12 GiB gap fits it.
        // Adjust vm_hi for exact to reflect the 1.5 GiB already mapped by pso4_init (mocked regions do not grow on open).
        if (exact) regions[nr++] = (typeof(regions[0])){vm_lo + 4 * G, G + G / 2};
        NSString *pnew = [device stringByAppendingPathComponent:@"p00001.mtl4a"];
        archive(pnew, G / 2);
        g_comp4 = (id)[NSObject new];
        arch_adopt_prebuilt();
        g_comp4 = nil;
        EXPECT("adopt respects VA reserve", exact ? 3 : 4, dev->opens);
        unlink(pnew.fileSystemRepresentation);
    }
    EXPECT("fallback disables hybrids", 0, hybrid_on());
    g_shmap = [NSMutableDictionary dictionary]; g_warm_started = 1;   // no shaders or prewarm tasks in this fixture
    EXPECT("fallback skips prebuild", -1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> lib = [MTLCreateSystemDefaultDevice() newLibraryWithSource:@"#include <metal_stdlib>\nvertex metal::float4 v() { return 0; }" options:nil error:nil];
    d.vertexFunction = [lib newFunctionWithName:@"v"];
    EXPECT("fixture vertex function", 1, d.vertexFunction != nil);
    EXPECT("render through the device", 1, pso_build(d, 1, NULL) == dev->pipeline);
    EXPECT("compute through the device", 1, cps_new(nil, NULL) == dev->pipeline);
    EXPECT("render device call", 1, dev->renders); EXPECT("compute device call", 1, dev->computes);
    if (!empty) for (NSString *p in paths) EXPECT("valid archive preserved", 1, [NSFileManager.defaultManager fileExistsAtPath:p]);
    EXPECT("prebuild stamp preserved", 1, [[NSString stringWithContentsOfFile:stamp encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"keep this stamp\n"]);
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_admission %s: %d checks, %d failed\n", argv[1], checks, failed);
    return failed != 0;
} }
