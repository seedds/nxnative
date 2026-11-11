// Metal 4 pipelines (pso_build): one base per pipeline with its colour state unspecialized, made into pipelines by specialization, kept in the pipeline archive.
// Checks: pipelines that differ only in colour state share one base; a specialized pipeline draws the same pixels as one built through the MTLDevice; a flushed archive
// file serves the base again without compiling; a flush of bases without a vertex layout is a p<seq> file, where a base with a layout is not looked up; a function with no
// Metal 4 descriptor keeps the MTLDevice path; pso_key_norm drops what no pipeline depends on; compaction merges the files the prewarm took bases from into one per full period despite a
// mid-prewarm timer flush, removing them only after its last period, counting them for the prepare line, and a compaction that did not finish is not tried again on the same system build; a timer flush waits for any prewarm, so a cold start writes a file per full period; depth-only draws write depth; concurrent colour variants compile
// one base; archive flushes preserve unrelated compiler state; a function with function constants is specialized by its pipeline's descriptor, not through its library,
// and an MTLDevice pipeline gets it specialized; the descriptor for a specialization is the caller's whether the driver returns it retained or autoreleased; a compute pipeline is captured into a plain file and taken from it later, one without a Metal 4 descriptor never looked up; a native mesh pipeline is captured into a p file and taken from it later, with or without a task shader or a pixel shader, captured again by a compaction, and one without a Metal 4 descriptor built through the MTLDevice; a capture that fails builds the pipeline all the same and hands a full period over, or stops capturing after a failure in a period that captured nothing; run files are compacted by size tier from the pipeline count in their names (a tier of more than PSO_ARCH_COMPACT files and every smaller one, never a full period's, old and malformed names as the largest tier, the same across fresh processes), and a compute or task / mesh pipeline in a file left out is not captured again
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// Keep the host's cache observations/deletions in a directory owned by this test.
// Metal itself still uses its real compiler cache; only this translation unit's
// confstr calls see the fixture parent, including when testing the former cleanup.
static char archive_cache_root[] = "/tmp/t_pso_archive_cache.XXXXXX";
static size_t archive_confstr(int name, char *buffer, size_t length) {
    if (name != _CS_DARWIN_USER_CACHE_DIR) return confstr(name, buffer, length);
    size_t size = strlen(archive_cache_root) + 1;
    if (length && buffer) { size_t n = size < length ? size : length; memcpy(buffer, archive_cache_root, n); buffer[n - 1] = 0; }
    return size;
}
#define confstr archive_confstr
#include "../nvn/nvn_mtl.m"
static void archive_cache_cleanup(void) {
    [NSFileManager.defaultManager removeItemAtPath:@(archive_cache_root) error:nil];
}
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static id<MTLLibrary> g_lib, g_lib2;   // g_lib2: the same source again, for functions with no Metal 4 descriptor (a library hands out one function object per name)
static id<MTLCommandQueue> g_q;
static NSArray<NSString *> *archive_files(NSString *dir) {   // the archive's files, without their indexes (<file>.keys)
    return [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]];
}
static id<MTLFunction> test_fn(id<MTLLibrary> lib, NSString *name, int registered) {
    id<MTLFunction> f = [lib newFunctionWithName:name];
    MTLFunctionConstantValues *cv = f.functionConstantsDictionary.count ? [MTLFunctionConstantValues new] : nil;
    if (cv) f = [lib newFunctionWithName:name constantValues:cv error:nil];
    return registered ? fn4_note(f, lib, name, cv) : f;
}
// Hold one function's first library specialization while another caller requests it and an unrelated function specializes.
@interface FunctionGateLibrary : NSObject {
@public id<MTLLibrary> library; NSString *name; dispatch_semaphore_t entered, extra, release; _Atomic int calls;
}
@end
@implementation FunctionGateLibrary
- (id)forwardingTargetForSelector:(SEL)selector { return library; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)entry constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
    if ([entry isEqualToString:name]) {
        if (!atomic_fetch_add(&calls, 1)) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
        else dispatch_semaphore_signal(extra);
    }
    return [library newFunctionWithName:entry constantValues:values error:error];
}
@end
static void concurrent_functions(id<MTLLibrary> library) {
    FunctionGateLibrary *gate = [FunctionGateLibrary new]; gate->library = library; gate->name = @"cvs";
    gate->entered = dispatch_semaphore_create(0); gate->extra = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0);
    id<MTLLibrary> lib = (id<MTLLibrary>)gate;
    id<MTLFunction> vf = fn_spec(lib, @"cvs", [MTLFunctionConstantValues new], NULL), ff = fn_spec(lib, @"cfs", [MTLFunctionConstantValues new], NULL);
    dispatch_group_t group = dispatch_group_create(), other = dispatch_group_create(); dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_semaphore_t started = dispatch_semaphore_create(0); __block id<MTLFunction> first, second, separate;
    dispatch_group_async(group, q, ^{ @autoreleasepool { first = fn_dev(vf); } });
    EXPECT("concurrent functions: first specialization entered", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_group_async(group, q, ^{ @autoreleasepool { dispatch_semaphore_signal(started); second = fn_dev(vf); } });
    EXPECT("concurrent functions: overlapping caller started", 0, dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_group_async(other, q, ^{ @autoreleasepool { separate = fn_dev(ff); } });
    EXPECT("concurrent functions: unrelated function progresses", 0, dispatch_group_wait(other, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("concurrent functions: unrelated result specialized", 1, separate && separate != ff && !separate.functionConstantsDictionary.count);
    EXPECT("concurrent functions: overlapping caller does not specialize again", 1, dispatch_semaphore_wait(gate->extra, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) != 0);
    dispatch_semaphore_signal(gate->release); dispatch_group_wait(group, DISPATCH_TIME_FOREVER); dispatch_group_wait(other, DISPATCH_TIME_FOREVER);
    EXPECT("concurrent functions: both callers share the specialized result", 1, first && first != vf && first == second && !first.functionConstantsDictionary.count);
    // Re-registering the same library entry must preserve the function's cached specialization.
    id<MTLFunction> again = fn_spec(lib, @"cvs", [MTLFunctionConstantValues new], NULL);
    EXPECT("concurrent functions: reloading retains the result", 1, fn_dev(again) == first);
    EXPECT("concurrent functions: one library specialization", 1, gate->calls);
}
// Hold the first base build while other callers request colour variants and an unrelated base.
@interface ArchiveGateCompiler : NSObject {
@public id<MTL4Compiler> compiler; dispatch_semaphore_t entered, release; _Atomic int calls, lookup_calls; int fail;
}
@end
@implementation ArchiveGateCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    if (opts.lookupArchives.count) atomic_fetch_add(&lookup_calls, 1);
    int call = atomic_fetch_add(&calls, 1);
    if (!call) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
    if (fail) { if (error) *error = [NSError errorWithDomain:@"test" code:1 userInfo:nil]; return nil; }
    return [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
@end
// Counts the compute pipeline lookups in an archive file
@interface LookupCountArchive : NSObject { @public id<MTL4Archive> archive; _Atomic int lookups; } @end
@implementation LookupCountArchive
- (id)forwardingTargetForSelector:(SEL)selector { return archive; }
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor *)d error:(NSError **)error {
    atomic_fetch_add(&lookups, 1); return [archive newComputePipelineStateWithDescriptor:d error:error];
}
@end
// Runs kernel k of compute_pipelines over 64 threads: 1 when every o[i] is i * 3 + 1
static int run_kernel(id<MTLComputePipelineState> ps) {
    if (!ps) return 0;
    id<MTLBuffer> b = [g_dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared]; memset(b.contents, 0, 64 * 4);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0]; [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    const uint32_t *o = b.contents; for (uint32_t i = 0; i < 64; i++) if (o[i] != i * 3 + 1) return 0;
    return 1;
}
// Compute pipelines (cps_new) beside the bases: one the bundle does not hold is built through the MTLDevice and compiled into the flush period in the background, written to a
// plain file (never a p<seq> one), and a later start takes it from that file without building it; compaction captures it again; a function without a Metal 4 descriptor
// (constants, the embedded kernels) is built through the MTLDevice, not captured and never looked up
static void compute_pipelines(void) {
    char kdir[] = "/tmp/t_pso_compute.XXXXXX"; if (!mkdtemp(kdir)) abort();
    NSString *saved = g_arch_dir; g_arch_dir = @(kdir); g_arch4 = @[]; g_arch4p = @[]; NSError *err = nil;
    arch_flush_wait();   // from an empty period
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * 3 + 1; }\n";
    id<MTLLibrary> kl = [g_dev newLibraryWithSource:src options:nil error:&err], ul = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// unregistered\n"] options:nil error:&err];
    id<MTLFunction> kf = fn4_note([kl newFunctionWithName:@"k"], kl, @"k", nil), uf = [ul newFunctionWithName:@"k"];
    uint64_t built = g_cps_built, run = g_cps_run, arch = g_cps_arch; int pending = g_arch_new;
    id<MTLComputePipelineState> kp = cps_new(kf, &err);
    EXPECT("compute: a pipeline", 1, kp != nil);
    EXPECT("compute: built", built + 1, g_cps_built);
    dispatch_sync(arch_harvest_q(), ^{});   // its capture, in the background
    EXPECT("compute: captured for the archive", pending + 1, g_arch_new);
    EXPECT("compute: its dispatch writes", 1, run_kernel(kp));
    arch_flush_wait();
    NSArray<NSString *> *names = archive_files(@(kdir));
    EXPECT("compute: one file written", 1, names.count);
    EXPECT("compute: a plain file, not a p file", 1, names.count == 1 && ![names[0] hasPrefix:@"p"]);
    EXPECT("compute: nothing left to write", 0, g_arch_new);
    LookupCountArchive *la = [LookupCountArchive new];
    NSString *path = names.count ? [@(kdir) stringByAppendingPathComponent:names[0]] : nil;
    if (!path) abort();
    la->archive = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&err];
    g_arch4 = @[(id<MTL4Archive>)la];   // as a new start with that file
    built = g_cps_built; pending = g_arch_new;
    id<MTLComputePipelineState> kp2 = cps_new(kf, &err);
    EXPECT("compute again: from this device's archive", run + 1, g_cps_run);
    EXPECT("compute again: not built", built, g_cps_built);
    EXPECT("compute again: not from a bundle", arch, g_cps_arch);
    EXPECT("compute again: nothing captured", pending, g_arch_new);
    EXPECT("compute again: its dispatch writes", 1, run_kernel(kp2));
    // compaction during the prewarm: captured again from its function, for the file that replaces the old ones
    g_compact_enabled = 1; g_compacted = 0; atomic_store(&g_warm_running, 1);
    EXPECT("compute, compaction: a pipeline", 1, cps_new(kf, &err) != nil);
    EXPECT("compute, compaction: from the archive", run + 2, g_cps_run);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("compute, compaction: captured again", 1, g_compacted);
    g_compact_state = calloc(g_arch4.count + 1, 1); g_compacted = 0;   // a pick that leaves the file it is in: not captured again
    EXPECT("compute, compaction of other files: a pipeline", 1, cps_new(kf, &err) != nil);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("compute, compaction of other files: not captured again", 0, g_compacted);
    g_compact_state = NULL;
    EXPECT("compute, compaction: waits for the flush", pending + 1, g_arch_new);
    atomic_store(&g_warm_running, 0); g_compact_enabled = 0; g_compacted = 0;
    arch_flush_wait();
    // no Metal 4 descriptor: built through the MTLDevice, no lookup, nothing captured
    int looked = la->lookups; built = g_cps_built; pending = g_arch_new;
    id<MTLComputePipelineState> up = cps_new(uf, &err);
    EXPECT("compute without a descriptor: a pipeline", 1, up != nil);
    EXPECT("compute without a descriptor: never looked up", looked, la->lookups);
    EXPECT("compute without a descriptor: built", built + 1, g_cps_built);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("compute without a descriptor: not captured", pending, g_arch_new);
    EXPECT("compute without a descriptor: its dispatch writes", 1, run_kernel(up));
    [NSFileManager.defaultManager removeItemAtPath:@(kdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
}
// Draws mesh pipeline ps (one triangle over the target) into a 4x4 RGBA8 target cleared to red: 1 when the centre pixel is the fragment's colour, or red with no fragment
static int mesh_draw_ok(id<MTLRenderPipelineState> ps, int fragment) {
    if (!ps) return 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    MTLSize one = MTLSizeMake(1, 1, 1);
    [en setRenderPipelineState:ps]; [en drawMeshThreadgroups:one threadsPerObjectThreadgroup:one threadsPerMeshThreadgroup:one]; [en endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    uint8_t px[4]; [t getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
    return fragment ? px[0] == 0 && px[1] == 255 && px[2] == 0 && px[3] == 255 : px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255;
}
// Native mesh pipelines (mtk_build) beside the bases: one built through the MTLDevice is compiled into the flush period in the background, written to a p file (no vertex layout), and a later start takes it from
// that file without compiling, with or without a task shader; a compacting prewarm captures one taken from a plain file again; a depth-only one (no pixel shader) takes the noted
// empty fragment and goes the same way; one whose function has no Metal 4 descriptor is built through the MTLDevice and not captured
static void mesh_pipelines(void) {
    char mdir[] = "/tmp/t_pso_mesh.XXXXXX"; if (!mkdtemp(mdir)) abort();
    NSString *saved = g_arch_dir; g_arch_dir = @(mdir); g_arch4 = @[]; g_arch4p = @[]; NSError *err = nil;
    arch_flush_wait();   // from an empty period
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct MV { float4 p [[position]]; };\nusing Tri = mesh<MV, void, 3, 1, topology::triangle>;\n"
        "struct PL { uint n; }; struct BL { uint first; };\n"
        "[[object]] void os(object_data PL &pl [[payload]], mesh_grid_properties g) { pl.n = 1; g.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"
        // a mesh shader with no task has an object function of its own that launches its blocks (dmsh_meshstage.py main0_o)
        "[[object]] void ob(object_data BL &b [[payload]], mesh_grid_properties g) { b.first = 0; g.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"
        "[[mesh]] void ms(Tri out, const object_data BL &b [[payload]]) { out.set_primitive_count(1 + b.first); out.set_vertex(0, MV{float4(-1, -1, 0.5, 1)}); out.set_vertex(1, MV{float4(3, -1, 0.5, 1)}); out.set_vertex(2, MV{float4(-1, 3, 0.5, 1)});\n"
        "  out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 2); }\n"
        "[[mesh]] void mt(Tri out, const object_data PL &pl [[payload]]) { out.set_primitive_count(pl.n); out.set_vertex(0, MV{float4(-1, -1, 0.5, 1)}); out.set_vertex(1, MV{float4(3, -1, 0.5, 1)}); out.set_vertex(2, MV{float4(-1, 3, 0.5, 1)});\n"
        "  out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 2); }\n"
        "fragment float4 mf() { return float4(0, 1, 0, 1); }\n";
    id<MTLLibrary> ml = [g_dev newLibraryWithSource:src options:nil error:&err], ul = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// unregistered\n"] options:nil error:&err];
    EXPECT("mesh: fixture library", 1, ml != nil && ul != nil);
    if (!ml || !ul) { g_arch_dir = saved; return; }
    NvShader *ms = [NvShader new], *mt = [NvShader new], *os = [NvShader new], *fs = [NvShader new], *um = [NvShader new];
    ms->fn = test_fn(ml, @"ms", 1); ms->stage = 4; ms->mesh_kind = 2; ms->mesh_threads = 1; ms->mesh_notask = 1; ms->mesh_obj = test_fn(ml, @"ob", 1); strlcpy(ms->key, "mesh", sizeof ms->key);
    mt->fn = test_fn(ml, @"mt", 1); mt->stage = 4; mt->mesh_kind = 2; mt->mesh_threads = 1; strlcpy(mt->key, "mesh-task", sizeof mt->key);
    os->fn = test_fn(ml, @"os", 1); os->stage = 3; os->mesh_kind = 1; os->mesh_threads = 1; os->mesh_payload = 4; strlcpy(os->key, "task", sizeof os->key);
    fs->fn = test_fn(ml, @"mf", 1); fs->stage = 1; strlcpy(fs->key, "pixel", sizeof fs->key);
    um->fn = test_fn(ul, @"ms", 0); um->stage = 4; um->mesh_kind = 2; um->mesh_threads = 1; um->mesh_notask = 1; um->mesh_obj = test_fn(ul, @"ob", 0); strlcpy(um->key, "mesh-u", sizeof um->key);
    pso_key k; memset(&k, 0, sizeof k); k.cfmt[0] = MTLPixelFormatRGBA8Unorm; k.chmask = 0xf; k.samples = 1;
    pso_key kz; memset(&kz, 0, sizeof kz); kz.cfmt[0] = MTLPixelFormatRGBA8Unorm; kz.samples = 1;   // depth only: no pixel shader, no colour written
    uint64_t built = g_mtk_built, arch = g_mtk_arch, dev = g_mtk_dev; int pending = g_arch_new, got = 0;
    id<MTLRenderPipelineState> p1 = mtk_build(&k, nil, ms, fs, &err, &got), p2 = mtk_build(&k, os, mt, fs, &err, NULL), p3 = mtk_build(&kz, nil, ms, nil, &err, NULL);
    EXPECT("mesh: pipelines", 1, p1 && p2 && p3);
    EXPECT("mesh: built", built + 3, g_mtk_built);
    EXPECT("mesh: not from the archive", 0, got);
    dispatch_sync(arch_harvest_q(), ^{});   // their captures, in the background
    EXPECT("mesh: captured for the archive", pending + 3, g_arch_new);
    EXPECT("mesh: its draw writes", 1, mesh_draw_ok(p1, 1));
    EXPECT("mesh with a task shader: its draw writes", 1, mesh_draw_ok(p2, 1));
    EXPECT("mesh, depth only: draws nothing into colour", 1, mesh_draw_ok(p3, 0));
    arch_flush_wait();
    NSArray<NSString *> *names = archive_files(@(mdir));
    EXPECT("mesh: one file written", 1, names.count);
    EXPECT("mesh: a p file (no vertex layout)", 1, names.count == 1 && [names[0] hasPrefix:@"p"]);
    NSString *first = names.firstObject;
    id<MTL4Archive> file = first ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:[NSString stringWithFormat:@"%s/%@", mdir, first]] error:&err] : nil;
    g_arch4p = file ? @[file] : @[];   // as a new start with that file
    built = g_mtk_built; pending = g_arch_new;
    id<MTLRenderPipelineState> q1 = mtk_build(&k, nil, ms, fs, &err, &got), q2 = mtk_build(&k, os, mt, fs, &err, NULL), q3 = mtk_build(&kz, nil, ms, nil, &err, NULL);
    EXPECT("mesh again: from the archive", arch + 3, g_mtk_arch);
    EXPECT("mesh again: reported as archived", 1, got);
    EXPECT("mesh again: not built", built, g_mtk_built);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("mesh again: nothing captured", pending, g_arch_new);
    EXPECT("mesh again: its draw writes", 1, mesh_draw_ok(q1, 1));
    EXPECT("mesh again with a task shader: its draw writes", 1, mesh_draw_ok(q2, 1));
    EXPECT("mesh again, depth only: draws nothing into colour", 1, mesh_draw_ok(q3, 0));
    pso_key kb = k; kb.cfmt[0] = MTLPixelFormatBGRA8Unorm;   // other colour state: another pipeline, not in the file
    EXPECT("mesh, other colour format: built", 1, mtk_build(&kb, nil, ms, fs, &err, NULL) != nil && g_mtk_built == built + 1);
    dispatch_sync(arch_harvest_q(), ^{});
    arch_flush_wait();
    // the same file as a plain one during a compacting prewarm: captured again from its functions, for the file that replaces the old ones
    g_arch4p = @[]; g_arch4 = file ? @[file] : @[];
    g_compact_enabled = 1; g_compacted = 0; atomic_store(&g_warm_running, 1); pending = g_arch_new;
    EXPECT("mesh, compaction: a pipeline", 1, mtk_build(&k, nil, ms, fs, &err, NULL) != nil);
    EXPECT("mesh, compaction: from the archive", arch + 4, g_mtk_arch);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("mesh, compaction: captured again", 1, g_compacted);
    g_compact_state = calloc(g_arch4.count + 1, 1); g_compacted = 0;   // a pick that leaves the file it is in: not captured again
    EXPECT("mesh, compaction of other files: a pipeline", 1, mtk_build(&k, nil, ms, fs, &err, NULL) != nil);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("mesh, compaction of other files: not captured again", 0, g_compacted);
    g_compact_state = NULL;
    EXPECT("mesh, compaction: waits for the flush", pending + 1, g_arch_new);
    atomic_store(&g_warm_running, 0); g_compact_enabled = 0; g_compacted = 0;
    arch_flush_wait();
    // no Metal 4 descriptor: built through the MTLDevice, nothing captured
    pending = g_arch_new; built = g_mtk_built;
    id<MTLRenderPipelineState> up = mtk_build(&k, nil, um, fs, &err, NULL);
    EXPECT("mesh without a descriptor: a pipeline", 1, up != nil);
    EXPECT("mesh without a descriptor: MTLDevice build", dev + 1, g_mtk_dev);
    EXPECT("mesh without a descriptor: not counted as kept", built, g_mtk_built);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("mesh without a descriptor: not captured", pending, g_arch_new);
    EXPECT("mesh without a descriptor: its draw writes", 1, mesh_draw_ok(up, 1));
    [NSFileManager.defaultManager removeItemAtPath:@(mdir) error:nil]; g_arch_dir = saved; g_arch4 = @[]; g_arch4p = @[];
}
// Notes whether the compaction marker exists while the serializer writes (issue #342: a flush that does not return leaves it)
@interface MarkerSerializer : NSObject { @public id<MTL4PipelineDataSetSerializer> ser; int calls, marked; } @end
@implementation MarkerSerializer
- (id)forwardingTargetForSelector:(SEL)selector { return ser; }
- (BOOL)serializeAsArchiveAndFlushToURL:(NSURL *)url error:(NSError **)error {
    calls++; marked += [[NSString stringWithContentsOfFile:arch_compact_marker() encoding:NSUTF8StringEncoding error:nil] isEqualToString:arch_compact_stamp()];
    return [ser serializeAsArchiveAndFlushToURL:url error:error];
}
@end
// Model another compiler creating state before the real compiler call returns.
// The UUID fixture belongs exclusively to this test; no discovered folder is removed.
@interface ArchiveForeignCompiler : NSObject {
@public id<MTL4Compiler> compiler; NSString *fixture; NSError *fixture_error;
    unsigned calls; BOOL created, marker_created;
}
@end
@implementation ArchiveForeignCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    calls++;
    id<MTLRenderPipelineState> ps = [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
    if (ps && !created) {
        if (mkdir(fixture.fileSystemRepresentation, 0700)) fixture_error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        else {
            created = YES;
            NSString *marker = [fixture stringByAppendingPathComponent:@"PersistentState"];
            int fd = open(marker.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) fixture_error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
            else { close(fd); marker_created = YES; }
        }
    }
    return ps;
}
@end
// a full-screen triangle; colour 0 = (0.5, 0.25, 1, 0.5), colour 1 = (0.2, 0.4, 0.6, 0.8); attribute 0 (a float4) scales nothing, so a vertex descriptor is part of the pipeline
static MTLRenderPipelineDescriptor *desc(MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1, int registered) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLFunction> v = test_fn(g_lib2, @"vs", 0), f = test_fn(g_lib2, @"fs", 0);
    static id<MTLFunction> rv, rf;   // the registered ones are made once, as shader_load makes a shader's functions once
    if (registered) { if (!rv) { rv = test_fn(g_lib, @"vs", 1); rf = test_fn(g_lib, @"fs", 1); } v = rv; f = rf; }
    d.vertexFunction = v; d.fragmentFunction = f;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 16; vd.layouts[16].stride = 16; vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[16].stepRate = 0;
    d.vertexDescriptor = vd;
    d.colorAttachments[0].pixelFormat = f0;
    if (blend) { d.colorAttachments[0].blendingEnabled = YES; d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                 d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha; }
    d.colorAttachments[1].pixelFormat = f1; d.colorAttachments[1].writeMask = m1;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;   // a Metal 4 pipeline names no depth format; drawn into a pass with this depth attachment
    return d;
}
// draws with ps into fresh targets cleared to (1, 0, 0, 1) and returns texel (1, 1) of both as floats
static void draw_with(id<MTLRenderPipelineState> ps, MTLPixelFormat f0, MTLPixelFormat f1, float out[2][4]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; id<MTLTexture> t[2];
    MTLPixelFormat fm[2] = { f0, f1 };
    for (int i = 0; i < 2; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fm[i] width:4 height:4 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        t[i] = [g_dev newTextureWithDescriptor:td];
        rp.colorAttachments[i].texture = t[i]; rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].clearColor = MTLClearColorMake(1, 0, 0, 1); rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:4 height:4 mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    rp.depthAttachment.texture = [g_dev newTextureWithDescriptor:dd]; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    static id<MTLBuffer> one; if (!one) { float v[4] = { 1, 1, 1, 1 }; one = [g_dev newBufferWithBytes:v length:16 options:MTLResourceStorageModeShared]; }
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e setVertexBuffer:one offset:0 atIndex:16]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    for (int i = 0; i < 2; i++) {
        if (fm[i] == MTLPixelFormatRGBA8Unorm) { uint8_t p[4]; [t[i] getBytes:p bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c] / 255.0f; }
        else { __fp16 p[4]; [t[i] getBytes:p bytesPerRow:32 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c]; }
    }
}
static void same_pixels(const char *name, MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1) {
    NSError *e = nil;
    id<MTLRenderPipelineState> a = pso_build(desc(f0, blend, f1, m1, 1), &e), b = [g_dev newRenderPipelineStateWithDescriptor:desc(f0, blend, f1, m1, 0) error:&e];
    checks++; if (!a || !b) { fprintf(stderr, "%s: no pipeline (%s)\n", name, e.localizedDescription.UTF8String); failed++; return; }
    float pa[2][4], pb[2][4]; draw_with(a, f0, f1, pa); draw_with(b, f0, f1, pb);
    checks++; if (memcmp(pa, pb, sizeof pa)) { fprintf(stderr, "%s: the specialized pipeline drew (%g %g %g %g | %g %g %g %g), the MTLDevice one (%g %g %g %g | %g %g %g %g)\n", name,
        pa[0][0], pa[0][1], pa[0][2], pa[0][3], pa[1][0], pa[1][1], pa[1][2], pa[1][3], pb[0][0], pb[0][1], pb[0][2], pb[0][3], pb[1][0], pb[1][1], pb[1][2], pb[1][3]); failed++; }
}
static void depth_only(void) {
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatInvalid, 0, MTLPixelFormatInvalid, MTLColorWriteMaskNone, 1);
    d.fragmentFunction = nil; NSError *err = nil; uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> ps = pso_build(d, &err);
    EXPECT("depth only: pipeline", 1, ps != nil);
    EXPECT("depth only: legacy path", legacy + 1, g_pb_legacy);
    if (!ps) return;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) abort();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.depthAttachment.texture = t;
    rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new]; ds.depthWriteEnabled = YES; ds.depthCompareFunction = MTLCompareFunctionAlways;
    float v[4] = {1, 1, 1, 1}; id<MTLBuffer> vb = [g_dev newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps]; [enc setDepthStencilState:[g_dev newDepthStencilStateWithDescriptor:ds]];
    [enc setVertexBuffer:vb offset:0 atIndex:16]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 1)
                toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024]; [blit endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("depth only: command completed", MTLCommandBufferStatusCompleted, cb.status);
    EXPECT("depth only: rasterization wrote zero depth", 1, ((float *)out.contents)[65] == 0.0f);
}
static void concurrent_bases(int fail) {
    nvn_hm_clear(&g_pbase, hm_rel);
    if (fail) {   // the period has captured a pipeline: a capture that fails then means its store is full
        MTLRenderPipelineDescriptor *pd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); pd.vertexDescriptor.layouts[16].stride = 2048;
        NSError *err = nil; EXPECT("failed capture: a capture before", 1, pso_build(pd, &err) != nil && atomic_load(&g_ep->n) > 0);
    }
    ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->fail = fail;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0); ep->comp = (id<MTL4Compiler>)gate;
    NSMutableArray<MTLRenderPipelineDescriptor *> *ds = [NSMutableArray array];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, i & 1, MTLPixelFormatRGBA8Unorm, i, 1);
        d.vertexDescriptor.layouts[16].stride = 512; [ds addObject:d];
    }
    dispatch_group_t group = dispatch_group_create(); __block int good = 0; NSObject *lock = [NSObject new];
    for (MTLRenderPipelineDescriptor *d in ds) dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { NSError *err = nil; id ps = pso_build(d, &err);
            @synchronized(lock) { good += ps != nil && !err; }   // a capture that fails builds the pipeline by the plain compiler all the same
        }
    });
    EXPECT("concurrent: first compiler entered", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    // An unrelated key must complete while the first remains blocked.
    MTLRenderPipelineDescriptor *other = [ds[0] copy]; other.vertexDescriptor = [other.vertexDescriptor copy]; other.vertexDescriptor.layouts[16].stride = 1024;
    dispatch_group_t separate = dispatch_group_create();
    dispatch_group_async(separate, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { pso_build(other, NULL); } });
    EXPECT("concurrent: unrelated base progresses", 0, dispatch_group_wait(separate, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER); dispatch_group_wait(separate, DISPATCH_TIME_FOREVER);
    EXPECT("concurrent: all callers receive result", 8, good);
    if (!fail) EXPECT("concurrent: one compile per base", 2, gate->calls);
    EXPECT("concurrent: no unfinished builds", 0, g_pbase_pending.n);
    ep->comp = gate->compiler;
    if (!fail) return;
    // the full period was handed over once, and the run still captures, into a fresh period
    EXPECT("failed capture: a new period", 1, g_ep != ep);
    EXPECT("failed capture: handed over once", 1, ep->failed);
    EXPECT("failed capture: still capturing", 1, arch_capturing());
    NSError *err = nil; int n0 = atomic_load(&g_ep->n); nvn_hm_clear(&g_pbase, hm_rel);
    EXPECT("failed capture: the base captured in the new period", 1, pso_build(ds[0], &err) != nil && atomic_load(&g_ep->n) == n0 + 1);
    // a capture that fails in a period that has captured nothing: not the store's size, so the run captures no more, and builds the pipeline all the same
    arch_flush_wait();
    ArchEpoch *fresh = g_ep; ArchiveGateCompiler *fg = [ArchiveGateCompiler new]; fg->compiler = fresh->comp; fg->fail = 1; fg->calls = 1; fresh->comp = (id<MTL4Compiler>)fg;
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("failed capture in a new period: built", 1, pso_build(ds[1], &err) != nil && !err);
    EXPECT("failed capture in a new period: no more captures", 0, arch_capturing());
    int calls = fg->calls; nvn_hm_clear(&g_pbase, hm_rel);
    EXPECT("no more captures: built by the plain compiler", 1, pso_build(ds[2], &err) != nil && fg->calls == calls && g_ep == fresh);
    // a pipeline both compilers refuse fails, with the plain compiler's error
    atomic_store(&g_arch_cap_off, 0); id<MTL4Compiler> plain = g_comp4;
    ArchiveGateCompiler *pg = [ArchiveGateCompiler new]; pg->compiler = plain; pg->fail = 1; pg->calls = 1; g_comp4 = (id<MTL4Compiler>)pg;
    atomic_store(&fresh->n, 1);   // as a period that holds a capture, so the failure is not taken for a broken capture
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("both refuse: no pipeline, the error", 1, pso_build(ds[3], &err) == nil && [err.domain isEqualToString:@"test"]);
    EXPECT("both refuse: not a failed capture", 0, fresh->failed);
    atomic_store(&fresh->n, 0); g_comp4 = plain; fresh->comp = fg->compiler;
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("concurrent: failed base can retry", 1, pso_build(ds[0], &err) != nil);
}
static void capture_preserves_foreign_folder(void) {
    arch_flush_wait();   // Finish earlier captures: the captures below go into a new flush period
    NSError *error = nil;
    MTLRenderPipelineDescriptor *prime = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    prime.vertexDescriptor.layouts[16].stride = 1536;
    // Establish the period's working folder with a capture of its own compiler
    EXPECT("foreign capture: prime real compiler", 1,
           [g_ep->comp newRenderPipelineStateWithDescriptor:pso_desc4(prime, 1) compilerTaskOptions:nil error:&error] != nil);
    char root[PATH_MAX];
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, root, sizeof root)) { EXPECT("foreign capture: cache path available", 1, 0); return; }
    ArchEpoch *ep = g_ep; ArchiveForeignCompiler *proxy = [ArchiveForeignCompiler new]; proxy->compiler = ep->comp;
    NSString *cache = [@(root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    proxy->fixture = [cache stringByAppendingPathComponent:[@"gpuarchiver-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    MTLRenderPipelineDescriptor *fresh = [prime copy]; fresh.vertexDescriptor = [prime.vertexDescriptor copy];
    fresh.vertexDescriptor.layouts[16].stride = 2048;
    ep->comp = (id<MTL4Compiler>)proxy;   // the current flush period's compiler, which the captures go through
    EXPECT("foreign capture: real pipeline", 1, pso_build(fresh, &error) != nil);
    ep->comp = proxy->compiler;
    EXPECT("foreign capture: one real compiler call", 1, proxy->calls);
    EXPECT("foreign capture: fixture created", 1, proxy->marker_created);
    if (proxy->fixture_error) fprintf(stderr, "foreign capture: fixture creation failed: %s\n", proxy->fixture_error.localizedDescription.UTF8String);
    arch_flush_wait();
    EXPECT("foreign capture: PersistentState survives flush", 1,
           [NSFileManager.defaultManager fileExistsAtPath:[proxy->fixture stringByAppendingPathComponent:@"PersistentState"]]);
    if (proxy->created && [NSFileManager.defaultManager fileExistsAtPath:proxy->fixture])
        EXPECT("foreign capture: test-owned fixture removed", 1, [NSFileManager.defaultManager removeItemAtPath:proxy->fixture error:nil]);
}
// Stand-ins for a base pipeline's newRenderPipelineDescriptorForSpecialization: one returns the descriptor retained, as its name promises (macOS 27.0), the other autoreleased,
// as iOS 26.0's driver does. Plain functions, so ARC's naming convention does not decide what they return
static __weak MTL4RenderPipelineDescriptor *g_spec_made;
static void *spec_retained(id self, SEL cmd) { (void)self; (void)cmd; MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new]; g_spec_made = d; return (void *)CFBridgingRetain(d); }
static void *spec_autoreleased(id self, SEL cmd) { (void)self; (void)cmd; MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new]; g_spec_made = d; return (void *)CFAutorelease(CFBridgingRetain(d)); }
// spec_desc owns the descriptor either way: it outlives every pool once returned (no over-release; the warm task's pool crashed on iOS 26.0) and goes away once dropped (no leak),
// on the first call, which finds out how the driver returns it, and on later ones
static void spec_desc_ownership(void) {
    SEL sel = @selector(newRenderPipelineDescriptorForSpecialization);
    const char *names[2] = {"retained", "autoreleased"}; IMP imps[2] = {(IMP)spec_retained, (IMP)spec_autoreleased};
    for (int k = 0; k < 2; k++) {
        Class c = objc_allocateClassPair(NSObject.class, k ? "TSpecBaseAutoreleased" : "TSpecBaseRetained", 0);
        if (!c) { fprintf(stderr, "spec descriptor %s: no stand-in class\n", names[k]); failed++; continue; }
        class_addMethod(c, sel, imps[k], "@@:"); objc_registerClassPair(c);
        id base = [c new]; _Atomic int own = 0;
        for (int call = 0; call < 2; call++) {
            char name[96];
            __weak MTL4RenderPipelineDescriptor *kept;   // not w: EXPECT declares its own
            @autoreleasepool {
                MTL4RenderPipelineDescriptor *s;
                @autoreleasepool { s = spec_desc(base, &own); }
                kept = s;
                snprintf(name, sizeof name, "spec descriptor %s, call %d: the caller's after its pools", names[k], call + 1);
                EXPECT(name, 1, s != nil && s == g_spec_made);
            }
            snprintf(name, sizeof name, "spec descriptor %s, call %d: freed once dropped", names[k], call + 1);
            EXPECT(name, 1, kept == nil);
            snprintf(name, sizeof name, "spec descriptor %s, call %d: ownership found", names[k], call + 1);
            EXPECT(name, k + 1, atomic_load(&own));
        }
    }
}
static long compacting_n(void) { long n = 0; for (NSUInteger i = 0; g_compact_state && i < g_compact_files.count; i++) n += g_compact_state[i] == 1; return n; }   // files compaction replaces
// How many run files a start with these would compact (0: no compaction): groups of {count, pipelines in each} (0: named before the count was, -1: a count that does not parse)
static long pick_n(NSArray<NSArray<NSNumber *> *> *groups) {
    NSMutableArray<NSString *> *names = [NSMutableArray array]; int seq = 0;
    for (NSArray<NSNumber *> *g in groups) for (int i = 0; i < g[0].intValue; i++, seq++)
        [names addObject:g[1].intValue > 0 ? [NSString stringWithFormat:@"/x/%05d-%d.mtl4a", seq, g[1].intValue] : g[1].intValue ? [NSString stringWithFormat:@"/x/%05d-x.mtl4a", seq]
                                            : [NSString stringWithFormat:@"/x/%05d.mtl4a", seq]];
    arch_flush_wait(); dispatch_sync(g_arch_q, ^{});   // an earlier test's queued write or finish must not land between the pick and the count
    g_compacted = 0; g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL; arch_compact_pick(names);
    long n = g_compact_enabled ? compacting_n() : 0;
    g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL;
    return n;
}
// g_pbase_miss: an archive that never has the base, with the real compiler. A miss is searched for once, for its own base only; it keeps neither the compile nor the base
// it builds from a draw, and the compile skips the search
@interface MissingBaseArchive : NSObject { @public unsigned calls; } @end
@implementation MissingBaseArchive
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d error:(NSError **)e {
    (void)d; (void)e; calls++; return nil;
}
@end
static void repeated_archive_miss(void) {
    NSArray *saved = g_arch4, *savedp = g_arch4p;
    MissingBaseArchive *archive = [MissingBaseArchive new];
    g_arch4 = @[(id<MTL4Archive>)archive]; g_arch4p = @[];
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    d.vertexDescriptor.layouts[16].stride = 1536;
    NSError *e = nil;
    for (int i = 0; i < 100; i++) EXPECT("negative probe: no archived pipeline", 1, pso_build_ex(d, &e, 0) == nil);
    EXPECT("negative probe: one lookup for 100 misses", 1, archive->calls);
    EXPECT("negative probe: no unfinished owner", 0, g_pbase_pending.n);
    d.vertexDescriptor.layouts[16].stride = 1552;
    EXPECT("negative probe: another layout is independent", 1, pso_build_ex(d, &e, 0) == nil);
    EXPECT("negative probe: another key looked up", 2, archive->calls);
    d.vertexDescriptor.layouts[16].stride = 1536;
    EXPECT("negative probe: demand can still compile", 1, pso_build_ex(d, &e, 1) != nil);
    EXPECT("negative probe: the compile does not search again", 2, archive->calls);
    unsigned after = archive->calls;
    EXPECT("negative probe: compiled base overrides miss", 1, pso_build_ex(d, &e, 0) != nil);
    EXPECT("negative probe: completed base needs no archive scan", after, archive->calls);
    g_arch4 = saved; g_arch4p = savedp;
    nvn_hm_clear(&g_pbase_miss, hm_rel);
}

int main(int argc, char **argv) { @autoreleasepool {
    if (!mkdtemp(archive_cache_root)) abort();
    atexit(archive_cache_cleanup);
    NSString *cache_path = [@(archive_cache_root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    if (!cache_path) abort();
    if (![NSFileManager.defaultManager createDirectoryAtPath:cache_path
                               withIntermediateDirectories:NO attributes:nil error:nil]) abort();
    char dir[] = "/tmp/t_pso_archive.XXXXXX"; if (!mkdtemp(dir)) abort();
    setenv("DM_PSO_ARCHIVE", argc == 3 ? argv[2] : dir, 1);
    mtl_init(); g_q = [g_dev newCommandQueue]; NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nstruct I { float4 a [[attribute(0)]]; };\n"
        "vertex V vs(I in [[stage_in]], uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, in.a.w); return o; }\n"
        "struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\nfragment O fs() { return { float4(0.5, 0.25, 1.0, 0.5), float4(0.2, 0.4, 0.6, 0.8) }; }\n"
        "fragment O fs2() { return { float4(1), float4(0) }; }\n"
        "vertex V vs0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n";
    if (getenv("DM_TEST_ARCHIVE_CONSTANTS")) src = [src stringByReplacingOccurrencesOfString:@"fragment O fs() { return" withString:@"constant bool enabled [[function_constant(0)]]; fragment O fs() { if (is_function_constant_defined(enabled) && enabled) discard_fragment(); return"];
    if (argc == 3) {
        NSString *metal = [@(argv[2]) stringByAppendingPathComponent:@"fixture.metal"], *library = [metal stringByAppendingString:@"lib"];
        if (strstr(argv[1], "-seed")) {   // a mode that writes the archive builds the shader library
            [src writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil];
            NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
            task.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", library];
            if (![task launchAndReturnError:&err]) return 1;
            [task waitUntilExit]; if (task.terminationStatus) return 1;
        }
        g_lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:library] error:&err];
    } else g_lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    if (!g_lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    g_lib2 = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// another library: the same source would come back as the same library\n"] options:nil error:&err];
    lib_tag(g_lib, @"fixture.metallib"); lib_tag(g_lib2, @"fixture2.metallib");   // as lib_from_file names the game's: their pipelines have index keys
    if (argc == 3 && !strncmp(argv[1], "--cold-", 7)) {   // across fresh processes: a cold start's prewarm with a timer flush after each new base, then the next start
        g_arch_period_max = 4; int seed = !strcmp(argv[1], "--cold-seed"), cut = !strcmp(argv[1], "--cold-cut-seed"), after_cut = !strcmp(argv[1], "--cold-after-cut");
        int quit = !strcmp(argv[1], "--cold-quit-seed"), after_quit = !strcmp(argv[1], "--cold-after-quit");
        // a crashed compaction's marker, left by an earlier run: the quit's write must not take it away
        if (quit) [arch_compact_stamp() writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"compacting"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        pso4_init();
        EXPECT("cold restart: no compaction", 0, g_compact_enabled);
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built, asks0 = g_arch_asks;
        atomic_store(&g_warm_running, 1);
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 70);
            EXPECT("cold restart: pipeline", 1, pso_build(cd, &err) != nil);
            if (seed || cut || quit) dispatch_sync(g_arch_q, ^{ arch_flush(); });   // the timer's
            if (cut && i == 5) {   // the cold start is killed here, in its prewarm: the full period is written, the 2 captures after it are not
                dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER);
                printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
                _exit(failed != 0);
            }
            if (quit && i == 5) {   // the player quits here, in the prewarm: exit() writes the 2 captures after the full period too (mtl_arch_save)
                printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
                exit(failed != 0);
            }
        }
        atomic_store(&g_warm_running, 0); arch_flush_wait();
        int expect_archived = seed ? 0 : after_cut ? 4 : after_quit ? 6 : PSO_ARCH_COMPACT + 1;
        EXPECT("cold restart: compiled in the cold start, from the archive in the next", expect_archived, (int)(g_pb_archived - arch0));
        EXPECT("cold restart: compiled where not in the archive", PSO_ARCH_COMPACT + 1 - expect_archived, (int)(g_pb_built - built0));
        EXPECT("cold restart: the index asks one file for each base found and none for a miss", expect_archived, (int)(g_arch_asks - asks0));
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    // A fresh process is essential: a compiler that built the source archives still has their online binaries.
    if (argc == 3 && !strncmp(argv[1], "--oldname-", 10)) {   // across fresh processes: nine run files named before the count (the test renames them) are compacted once, then not again
        int seed = !strcmp(argv[1], "--oldname-seed"), replay = !strcmp(argv[1], "--oldname-replay");
        pso4_init();
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [descs addObject:cd];
        }
        if (seed) for (MTLRenderPipelineDescriptor *cd in descs) { EXPECT("old names restart: pipeline", 1, pso_build(cd, &err) != nil); arch_flush_wait(); }
        else {
            EXPECT("old names restart: the nine old names compacted once", replay ? PSO_ARCH_COMPACT + 1 : 0, g_compact_enabled ? compacting_n() : 0);
            uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
            atomic_store(&g_warm_running, 1);
            for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
            atomic_store(&g_warm_running, 0);
            EXPECT("old names restart: every base from disk", arch0 + descs.count, g_pb_archived);
            EXPECT("old names restart: none compiled", built0, g_pb_built);
            arch_flush_wait();
        }
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    if (argc == 3 && !strncmp(argv[1], "--tier-", 7)) {   // by tier across fresh processes: one full period's file and nine of one base; the nine compacted; then nothing to compact
        g_arch_period_max = 4; int seed = !strcmp(argv[1], "--tier-seed"), replay = !strcmp(argv[1], "--tier-replay");
        pso4_init();
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i < 4 + PSO_ARCH_COMPACT + 1; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i < 4 ? i + 20 : i - 2); [descs addObject:cd];
        }
        if (seed) {
            for (int i = 0; i < 4; i++) EXPECT("tier restart: pipeline", 1, pso_build(descs[i], &err) != nil);   // the 4th capture hands the period over
            arch_flush_wait();
            for (NSUInteger i = 4; i < descs.count; i++) { EXPECT("tier restart: pipeline", 1, pso_build(descs[i], &err) != nil); arch_flush_wait(); }
        } else {
            EXPECT("tier restart: the nine small files compacted, not the full one", replay ? PSO_ARCH_COMPACT + 1 : 0, g_compact_enabled ? compacting_n() : 0);
            uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
            atomic_store(&g_warm_running, 1);
            for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
            atomic_store(&g_warm_running, 0);
            EXPECT("tier restart: every base from disk", arch0 + descs.count, g_pb_archived);
            EXPECT("tier restart: none compiled", built0, g_pb_built);
            EXPECT("tier restart: the small files' bases alone captured again", replay ? PSO_ARCH_COMPACT + 1 : 0, g_compacted);
            arch_flush_wait();
        }
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    if (argc == 3) {
        int seed = !strcmp(argv[1], "--compact-seed");
        pso4_init();
        EXPECT("restart: opened run archives", seed ? 0 : !strcmp(argv[1], "--compact-replay") ? PSO_ARCH_COMPACT + 1 : 1, g_arch4.count + g_arch_ixf.count);
        atomic_store(&g_warm_running, !seed);
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2);
            [descs addObject:cd];
            if (seed) { EXPECT("restart: pipeline", 1, pso_build(cd, &err) != nil); arch_flush_wait(); }
        }
        if (!seed) {
            int fresh = getenv("DM_TEST_ARCHIVE_MIXED") != NULL, compacting = g_compact_files != nil;
            int outcomes[PSO_ARCH_COMPACT + 1] = {0}; int *results = outcomes;
            dispatch_apply(descs.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
                @autoreleasepool { results[i] = pso_build(descs[i], NULL) != nil; }
            });
            for (int i = 0; i <= PSO_ARCH_COMPACT; i++) EXPECT("restart: pipeline", 1, outcomes[i]);
            if (fresh) {
                MTLRenderPipelineDescriptor *extra = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
                extra.fragmentFunction = test_fn(g_lib, @"fs2", 1);
                extra.vertexDescriptor.layouts[16].stride = 4096;
                EXPECT("restart: fresh pipeline mixed with recaptures", 1, pso_build(extra, &err) != nil);
            }
            EXPECT("restart: every base from disk", PSO_ARCH_COMPACT + 1 + (fresh && !compacting), g_pb_archived);
            EXPECT("restart: compiled bases", fresh && compacting, g_pb_built);
            dispatch_sync(g_arch_q, ^{ arch_flush(); });
            if (g_compact_files) EXPECT("restart: timed flush keeps recaptures pending", PSO_ARCH_COMPACT + 1 + fresh, g_arch_new);
            atomic_store(&g_warm_running, 0);
            arch_flush_wait();
        }
        if (seed && getenv("DM_TEST_ARCHIVE_MIXED")) [arch_os_build() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    // pipelines differing only in colour state share one base
    same_pixels("RGBA8 blended, RGBA16F all channels", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("first pipeline: its base compiled", 1, g_pb_built);
    same_pixels("RGBA16F, RGBA8 red and green", MTLPixelFormatRGBA16Float, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskRed | MTLColorWriteMaskGreen);
    same_pixels("RGBA8, RGBA8 no channel", MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskNone);
    EXPECT("other colour states: the same base", 1, g_pb_built);
    EXPECT("other colour states: base reused", 2, g_pb_reused);
    EXPECT("one specialization per pipeline", 3, g_pb_spec);
    // the archive: flushed to a file, a later start takes the base from it without compiling
    EXPECT("one base waits for the archive", 1, g_arch_new);
    ArchEpoch *ep0 = g_ep;
    arch_flush_wait();
    NSString *file = [@(dir) stringByAppendingPathComponent:@"00000-1.mtl4a"];   // <seq>-<pipelines it holds>
    EXPECT("archive file written", 1, [NSFileManager.defaultManager fileExistsAtPath:file]);
    EXPECT("the flush began a new period", 1, g_ep != ep0);
    EXPECT("nothing left to write", 0, g_arch_new);
    {   // its index: the one base's key, complete; a key is the descriptor's, the same for the same pipeline and another for another, none without names
        int complete = -1; NSData *keys = arch_keys_read(arch_keys_path(file), &complete);
        MTLRenderPipelineDescriptor *k1 = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll, 1), *k2 = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll, 1);
        uint64_t a = arch_key(pso_desc4(k1, 1)), b = arch_key(pso_desc4(k2, 1));
        EXPECT("index: written beside the file, one key", 1, keys.length == sizeof(uint64_t));
        EXPECT("index: complete", 1, complete);
        EXPECT("index: the base's key", 1, keys.length == sizeof(uint64_t) && !memcmp(keys.bytes, &a, sizeof a));
        EXPECT("key: the same pipeline, the same key", 1, a && a == b);
        k2.vertexDescriptor.layouts[16].stride = 32;
        EXPECT("key: another layout, another key", 1, arch_key(pso_desc4(k2, 1)) != a);
        MTL4RenderPipelineDescriptor *u = pso_desc4(k1, 1); MTL4LibraryFunctionDescriptor *uf = [MTL4LibraryFunctionDescriptor new];
        uf.library = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// a library with no name\n"] options:nil error:&err]; uf.name = @"vs"; u.vertexFunctionDescriptor = uf;
        EXPECT("key: none for a library with no name", 0, arch_key(u));
        uf.library = g_lib;
        uint64_t (^spec_key)(MTLFunctionConstantValues *) = ^uint64_t(MTLFunctionConstantValues *cv) {   // the descriptors copy what they are given: set, then assign
            MTL4SpecializedFunctionDescriptor *sf = [MTL4SpecializedFunctionDescriptor new]; sf.functionDescriptor = uf; sf.constantValues = cv; u.vertexFunctionDescriptor = sf;
            return arch_key(u);
        };
        MTLFunctionConstantValues *c1 = [MTLFunctionConstantValues new], *c2 = [MTLFunctionConstantValues new]; bool on = 1, off = 0; int seven = 7;
        [c1 setConstantValue:&on type:MTLDataTypeBool atIndex:3]; [c1 setConstantValue:&seven type:MTLDataTypeInt atIndex:0];
        [c2 setConstantValue:&seven type:MTLDataTypeInt atIndex:0]; [c2 setConstantValue:&on type:MTLDataTypeBool atIndex:3];   // the same values, set in another order
        uint64_t c0 = spec_key([MTLFunctionConstantValues new]), ka = spec_key(c1), kb = spec_key(c2);
        [c2 setConstantValue:&off type:MTLDataTypeBool atIndex:3]; uint64_t kc = spec_key(c2);
        EXPECT("key: constant values by their values", 1, c0 && ka && ka != c0 && ka == kb && kc != ka);
        int partial = -1; NSString *kp = [@(dir) stringByAppendingPathComponent:@"t.keys"];
        EXPECT("index: one with a pipeline with no key is not complete", 1, arch_keys_write(kp, keys, 0) && arch_keys_read(kp, &partial) && partial == 0);
        unlink(kp.fileSystemRepresentation);
    }
    nvn_hm_clear(&g_pbase, hm_rel);   // as a new start: no base in memory, the archive opened
    g_arch4 = @[[g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err]];
    uint64_t built = g_pb_built;
    same_pixels("from the archive: RGBA8 blended, RGBA16F", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("base from the archive", 1, g_pb_archived);
    EXPECT("no base compiled", built, g_pb_built);
    // a flush whose bases all read their vertices themselves (no vertex layout) is a p<seq> file; a base with a layout is never looked up in one
    MTLRenderPipelineDescriptor *nd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    nd.vertexFunction = fn4_note([g_lib newFunctionWithName:@"vs0"], g_lib, @"vs0", nil); nd.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    EXPECT("no layout: a pipeline", 1, pso_build(nd, &err) != nil);
    EXPECT("no layout: its base compiled", built + 1, g_pb_built);
    arch_flush_wait();
    NSString *pfile = [@(dir) stringByAppendingPathComponent:@"p00001.mtl4a"];
    EXPECT("no layout: written to a p file", 1, [NSFileManager.defaultManager fileExistsAtPath:pfile]);
    nvn_hm_clear(&g_pbase, hm_rel);
    id<MTL4Archive> pa = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:pfile] error:&err], va = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err];
    g_arch4 = @[]; g_arch4p = @[pa, va];   // the layout base's file among the p files: not looked in
    uint64_t arch0 = g_pb_archived; built = g_pb_built;
    EXPECT("no layout again: a pipeline", 1, pso_build(nd, &err) != nil);
    EXPECT("no layout again: base from the p file", arch0 + 1, g_pb_archived);
    same_pixels("layout base with only p files open", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("layout base: not taken from a p file, compiled", built + 1, g_pb_built);
    EXPECT("layout base: one waits for a plain file", 1, g_arch_new_vd);
    // compaction: with more than PSO_ARCH_COMPACT files, the bases the prewarm takes from them are captured again; after the flush that follows, one file holds them and the old ones are gone
    {
        char cdir[] = "/tmp/t_pso_compact.XXXXXX"; if (!mkdtemp(cdir)) abort();
        NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {   // one base per file: the stride of the layout tells them apart
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [descs addObject:cd];
            if (!pso_build(cd, &err)) { fprintf(stderr, "compaction: no pipeline %d\n", i); failed++; }
            arch_flush_wait();
        }
        NSArray<NSString *> *names = [archive_files(@(cdir)) sortedArrayUsingSelector:@selector(compare:)];
        EXPECT("compaction: one file per flush", PSO_ARCH_COMPACT + 1, names.count);
        NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array]; NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSString *n in names.reverseObjectEnumerator) { NSString *f = [@(cdir) stringByAppendingPathComponent:n]; [paths addObject:f]; [open addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:f] error:&err]]; }
        nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = open;   // as a new start with that many files
        g_compact_enabled = 1; g_compact_files = paths; g_compacted = 0; g_compact_bases = 0; g_compact_ns = 0;
        g_arch_period_max = 4;   // the recaptured bases fill two periods and part of a third: each full one is handed over, the timer's flushes wait
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
        ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->calls = 1; ep->comp = (id<MTL4Compiler>)gate;
        atomic_store(&g_warm_running, 1);
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        EXPECT("compaction: captures original shader functions, not archive imports", 0, gate->lookup_calls);
        ep->comp = gate->compiler;
        dispatch_sync(g_arch_q, ^{ arch_flush(); });   // the timer fires while prewarm still has other work
        arch_flush_wait();   // the timer's flush waits for the prewarm; the full periods are written
        EXPECT("compaction: timed flush keeps the last recaptured bases pending", descs.count % 4, g_arch_new);
        EXPECT("compaction: a file per full period", PSO_ARCH_COMPACT + 1 + descs.count / 4, archive_files(@(cdir)).count);
        int kept = 0; for (NSString *f in paths) kept += [NSFileManager.defaultManager fileExistsAtPath:f];
        EXPECT("compaction: the old files stay until its last period is written", paths.count, kept);
        atomic_store(&g_warm_running, 0);
        EXPECT("compaction: every base from the archive", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled", built0, g_pb_built);
        EXPECT("compaction: every base captured again", descs.count, g_compacted);
        EXPECT("compaction: the prepare line counts them", descs.count, g_compact_bases);
        EXPECT("compaction: and their time", 1, g_compact_ns > 0);
        MarkerSerializer *ms = [MarkerSerializer new]; ms->ser = g_ep->ser; g_ep->ser = (id<MTL4PipelineDataSetSerializer>)ms;   // the period being written
        arch_flush_wait();
        EXPECT("compaction: the marker exists while the flush writes", 1, ms->calls == 1 && ms->marked == 1);
        EXPECT("compaction: the flush that returned removed the marker", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        names = archive_files(@(cdir));
        EXPECT("compaction: a file per period left", (descs.count + 3) / 4, names.count);
        EXPECT("compaction: done", 0, g_compact_periods + !g_compact_closed + g_compact_bad);
        g_arch_period_max = ARCH_PERIOD_MAX;
        // a marker left by a compaction that did not finish: the next start on this system build does not compact, one from another build is removed and compaction tried
        EXPECT("compaction: allowed without a marker", 1, arch_compact_allowed());
        [arch_compact_stamp() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: not after one that did not finish on this system build", 0, arch_compact_allowed());
        EXPECT("compaction: that marker stays", 1, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        [[@"os\t1A1\n" stringByAppendingString:@"capture\toriginal-functions-v1\n"] writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: tried again on another system build", 1, arch_compact_allowed());
        EXPECT("compaction: the other build's marker removed", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        [arch_os_build() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: old archive-import marker permits the new capture method", 1, arch_compact_allowed());
        EXPECT("compaction: old capture marker removed", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        nvn_hm_clear(&g_pbase, hm_rel); g_compact_enabled = 0; g_compacted = 0;
        NSMutableArray<id<MTL4Archive>> *left = [NSMutableArray array];
        for (NSString *n in names) [left addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[@(cdir) stringByAppendingPathComponent:n]] error:&err]];
        g_arch4 = left;
        arch0 = g_pb_archived; built0 = g_pb_built;
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        EXPECT("compaction: the new files hold every base", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled from the new files", built0, g_pb_built);
        [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
    }
    // compaction by size tier: which run files a start compacts, from their names
    {
        g_arch_period_max = ARCH_PERIOD_MAX;
        EXPECT("tier: nine files of one pipeline", 9, pick_n(@[@[@9, @1]]));
        EXPECT("tier: nine full periods, none", 0, pick_n(@[@[@9, @(ARCH_PERIOD_MAX)]]));
        EXPECT("tier: nine small files, not the three larger ones", 9, pick_n(@[@[@9, @1], @[@3, @16]]));
        EXPECT("tier: nine in a larger tier take the smaller files with them", 11, pick_n(@[@[@2, @1], @[@9, @16]]));
        EXPECT("tier: eight in each tier, none", 0, pick_n(@[@[@8, @1], @[@8, @16], @[@8, @100], @[@8, @1000]]));
        EXPECT("tier: nine files named before the count, as the largest tier", 9, pick_n(@[@[@9, @0]]));
        EXPECT("tier: five named before the count stay beside nine small files", 9, pick_n(@[@[@5, @0], @[@9, @1]]));
        int F = ARCH_PERIOD_MAX;   // a compaction's full periods beside the files of play
        EXPECT("tier: five full, a session of 5 files under 8 and 4 under 64", 0, pick_n(@[@[@5, @(F)], @[@5, @6], @[@4, @20]]));
        EXPECT("tier: five full, a session of 30 small files", 30, pick_n(@[@[@5, @(F)], @[@30, @6]]));
        EXPECT("tier: five full, a session of 273 small files", 273, pick_n(@[@[@5, @(F)], @[@273, @6]]));
        EXPECT("tier: nine full periods (more than 16,000 bases), none", 0, pick_n(@[@[@9, @(F)], @[@1, @300]]));
        EXPECT("tier: nine named before the count beside five full, those nine", 9, pick_n(@[@[@5, @(F)], @[@9, @0]]));
        EXPECT("tier: a count that does not parse counts as named before, not as small", 9, pick_n(@[@[@5, @(-1)], @[@9, @1]]));
        EXPECT("tier: nine small files beside eight of a tier above, the small ones", 9, pick_n(@[@[@8, @16], @[@9, @1]]));
        EXPECT("tier: their merge makes the ninth file above, which the next start takes", 9, pick_n(@[@[@8, @16], @[@1, @9]]));
        int bounds[][2] = {{1, 0}, {7, 0}, {8, 1}, {63, 1}, {64, 2}, {511, 2}, {512, 3}, {1999, 3}, {2000, 8}, {2010, 8}};
        for (int i = 0; i < 10; i++) EXPECT("tier: its boundaries", bounds[i][1], arch_tier([NSString stringWithFormat:@"/x/00001-%d.mtl4a", bounds[i][0]]));
        EXPECT("tier: a count past an int counts as named before", 3, arch_tier(@"/x/00001-99999999999.mtl4a"));
        // many sessions by the rule alone: 5 full files, then 200 sessions of 30 files of 6 pipelines, each start compacting what it picks into a file per full period and the rest.
        // Without tiers every start took every file; here the files left below a full period stay at most 4 tiers of 8, and each pipeline is captured again about once per tier
        {
            NSMutableArray<NSNumber *> *files = [NSMutableArray arrayWithArray:@[@(F), @(F), @(F), @(F), @(F)]]; long added = 0, again = 0, biggest = 0; int most = 0, seq = 0, starts = 0, fulls = 0;
            for (int session = 0; session < 200; session++) {
                for (int i = 0; i < 30; i++) [files addObject:@6];
                added += 30 * 6;
                NSMutableArray<NSString *> *names = [NSMutableArray array];
                for (NSNumber *c in files) [names addObject:[NSString stringWithFormat:@"/x/%05d-%d.mtl4a", seq++, c.intValue]];
                g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL; arch_compact_pick(names);
                if (!g_compact_enabled) continue;
                NSMutableArray<NSNumber *> *left = [NSMutableArray array]; long taken = 0; starts++;
                for (NSUInteger i = 0; i < files.count; i++) { if (g_compact_state[i]) { taken += files[i].intValue; fulls += files[i].intValue >= F; } else [left addObject:files[i]]; }
                again += taken; biggest = MAX(biggest, taken);
                for (; taken >= F; taken -= F) [left addObject:@(F)];
                if (taken) [left addObject:@(taken)];
                files = left; int small = 0; for (NSNumber *c in files) small += c.intValue < F; most = MAX(most, small);
            }
            g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL;
            EXPECT("tier, 200 sessions: every start after a session of 30 small files compacts", 200, starts);
            EXPECT("tier, 200 sessions: no full period's file taken", 0, fulls);
            EXPECT("tier, 200 sessions: files below a full period stay at most 4 tiers of 8", 1, most <= 4 * PSO_ARCH_COMPACT);
            EXPECT("tier, 200 sessions: what play added captured again at least once, and 3 times or fewer on average", 1, again >= added && again <= 3 * added);
            EXPECT("tier, 200 sessions: the largest start below the whole archive", 1, biggest < added / 2);
        }
    }
    // a compaction: the full periods' files stay, used or not, and their bases are not captured again; the nine files of one base are compacted; the next start takes
    // every base from the files left
    {
        char cdir[] = "/tmp/t_pso_tier.XXXXXX"; if (!mkdtemp(cdir)) abort();
        NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
        g_arch_period_max = 4;
        g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL; g_compacted = 0; g_compact_periods = 0; g_compact_closed = 0; g_compact_bad = 0;
        NSMutableArray<MTLRenderPipelineDescriptor *> *full = [NSMutableArray array], *unused = [NSMutableArray array], *small = [NSMutableArray array];
        for (int k = 0; k < 2; k++) {   // two full periods: each 4th capture hands its period over
            for (int i = 0; i < 4; i++) {
                MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
                cd.vertexDescriptor.layouts[16].stride = 16 * (20 + 10 * k + i); [k ? full : unused addObject:cd];
                if (!pso_build(cd, &err)) { fprintf(stderr, "tier: no pipeline %d\n", i); failed++; }
            }
            arch_flush_wait();
        }
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [small addObject:cd];
            if (!pso_build(cd, &err)) { fprintf(stderr, "tier: no pipeline %d\n", i); failed++; }
            arch_flush_wait();
        }
        NSArray<NSString *> *names = [archive_files(@(cdir)) sortedArrayUsingSelector:@selector(compare:)];
        EXPECT("tier: two full periods' files and one file per flush", PSO_ARCH_COMPACT + 3, names.count);
        EXPECT("tier: a full period's file, in no tier", 8, names.count ? arch_tier(names[0]) : -1);
        EXPECT("tier: a flush of one pipeline, the smallest tier", 0, names.count ? arch_tier(names.lastObject) : -1);
        NSString *gone = names.count > 1 ? [@(cdir) stringByAppendingPathComponent:names[0]] : @"", *kept = names.count > 1 ? [@(cdir) stringByAppendingPathComponent:names[1]] : @"";
        NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array]; NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSString *n in names.reverseObjectEnumerator) { NSString *f = [@(cdir) stringByAppendingPathComponent:n]; [paths addObject:f]; [open addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:f] error:&err]]; }
        nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = open;   // as a new start with these files
        arch_compact_pick(paths);
        EXPECT("tier: more than PSO_ARCH_COMPACT small files: compaction", 1, g_compact_enabled);
        EXPECT("tier: the small files alone", (long)small.count, compacting_n());
        NSMutableArray<NSString *> *compacted = [NSMutableArray array]; for (NSUInteger i = 0; i < paths.count; i++) if (g_compact_state[i] == 1) [compacted addObject:paths[i]];
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
        atomic_store(&g_warm_running, 1);
        for (MTLRenderPipelineDescriptor *cd in full) pso_build(cd, &err);
        for (MTLRenderPipelineDescriptor *cd in small) pso_build(cd, &err);
        atomic_store(&g_warm_running, 0);
        EXPECT("tier: every base from the archive", arch0 + full.count + small.count, g_pb_archived);
        EXPECT("tier: none compiled", built0, g_pb_built);
        EXPECT("tier: only the small files' bases captured again", small.count, g_compacted);
        arch_flush_wait();
        EXPECT("tier: done", 0, g_compact_periods + !g_compact_closed + g_compact_bad);
        EXPECT("tier: the full period's file the prewarm took from stays", 1, [NSFileManager.defaultManager fileExistsAtPath:kept]);
        EXPECT("tier: the full period's file the run took nothing from stays too", 1, [NSFileManager.defaultManager fileExistsAtPath:gone]);
        int left = 0; for (NSString *f in compacted) left += [NSFileManager.defaultManager fileExistsAtPath:f];
        EXPECT("tier: the small files removed", 0, left);
        names = [archive_files(@(cdir)) sortedArrayUsingSelector:@selector(compare:)];
        EXPECT("tier: the two full files and a file per period", 2 + (small.count + 3) / 4, names.count);
        nvn_hm_clear(&g_pbase, hm_rel); g_compact_enabled = 0; g_compact_files = nil; g_compact_state = NULL; g_compacted = 0;
        NSMutableArray<id<MTL4Archive>> *again = [NSMutableArray array];
        for (NSString *n in names.reverseObjectEnumerator) [again addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[@(cdir) stringByAppendingPathComponent:n]] error:&err]];
        g_arch4 = again; arch0 = g_pb_archived; built0 = g_pb_built;
        for (MTLRenderPipelineDescriptor *cd in full) pso_build(cd, &err);
        for (MTLRenderPipelineDescriptor *cd in small) pso_build(cd, &err);
        EXPECT("tier: the files left hold every base the prewarm took", arch0 + full.count + small.count, g_pb_archived);
        EXPECT("tier: none compiled from them", built0, g_pb_built);
        nvn_hm_clear(&g_pbase, hm_rel); g_arch_period_max = ARCH_PERIOD_MAX;
        [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
    }
    compute_pipelines();
    // a timer's flush waits for the prewarm's end, compacting or not, and so a cold start writes a file per full period, not one per 10 s: here 3 files for 9 new bases,
    // where a file per timer flush made 9, more than PSO_ARCH_COMPACT, and the next start compacted them all
    {
        char cdir[] = "/tmp/t_pso_cold.XXXXXX"; if (!mkdtemp(cdir)) abort();
        NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
        g_arch_period_max = 4; atomic_store(&g_warm_running, 1);
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 70);
            if (!pso_build(cd, &err)) { fprintf(stderr, "cold: no pipeline %d\n", i); failed++; }
            ArchEpoch *ep = g_ep;
            dispatch_sync(g_arch_q, ^{ arch_flush(); });   // the timer's
            if (i % 4 != 3) EXPECT("cold: the timer's flush waits for the prewarm's end", 1, g_ep == ep);
        }
        atomic_store(&g_warm_running, 0); arch_flush_wait();
        NSUInteger files = archive_files(@(cdir)).count;
        EXPECT("cold: a file per full period and one for the rest", (PSO_ARCH_COMPACT + 4) / 4, files);
        nvn_hm_clear(&g_pbase, hm_rel); g_arch_period_max = ARCH_PERIOD_MAX;
        [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
    }
    mesh_pipelines();
    // a function with no Metal 4 descriptor: the MTLDevice path
    uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> l = pso_build(desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 0), &err);
    EXPECT("unregistered function: a pipeline", 1, l != nil);
    EXPECT("unregistered function: MTLDevice build", legacy + 1, g_pb_legacy);
    depth_only();
    concurrent_bases(0);
    concurrent_bases(1);
    // key normalization
    pso_key k; memset(&k, 0, sizeof k); k.blend_en = 0x5; k.dfmt = MTLPixelFormatDepth32Float; k.sfmt = MTLPixelFormatStencil8;
    for (int i = 0; i < 8; i++) { k.cfmt[i] = MTLPixelFormatRGBA8Unorm; k.blend[i].src_rgb = 5; k.blend[i].dst_rgb = 6; }   // every target present: only the blend rule acts
    pso_key_norm(&k, 0);
    EXPECT("blend of an enabled target kept", 5, k.blend[0].src_rgb);
    EXPECT("blend of an enabled target kept (2)", 6, k.blend[2].dst_rgb);
    EXPECT("blend of a disabled target dropped", 0, k.blend[1].src_rgb);
    EXPECT("blend of a disabled target dropped (7)", 0, k.blend[7].dst_rgb);
    EXPECT("depth format kept (an MTLDevice pipeline names it)", MTLPixelFormatDepth32Float, k.dfmt);
    EXPECT("stencil format kept", MTLPixelFormatStencil8, k.sfmt);
    // a function that declares function constants (fn_spec, as shader_load loads one): on the Metal 4 path it stays unspecialized, with a specialized descriptor, and a
    // pipeline is made from it; nothing is specialized through MTLLibrary, which compiles through the MTLDevice (the HUD's "Consider adopting MTL4Compiler")
    {
        id<MTLLibrary> cl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\n"
            "struct V { float4 p [[position]]; };\nvertex V cvs(uint i [[vertex_id]]) { V o; o.p = float4(is_function_constant_defined(k) && k ? 1 : 0, 0, 0, 1); return o; }\n"
            "fragment float4 cfs() { return is_function_constant_defined(k) && k ? float4(1) : float4(0); }\nfragment float4 plain() { return float4(1); }\n" options:nil error:&err];
        concurrent_functions(cl);
        NSError *se = nil; id<MTLFunction> cf = fn_spec(cl, @"cfs", [MTLFunctionConstantValues new], &se), cv = fn_spec(cl, @"cvs", [MTLFunctionConstantValues new], &se);
        EXPECT("constants: the function stays unspecialized on the Metal 4 path", 1, cf.functionConstantsDictionary.count);
        EXPECT("constants: its descriptor specializes it", 1, [fn4_of(cf) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]);
        MTLRenderPipelineDescriptor *cd = [MTLRenderPipelineDescriptor new]; cd.vertexFunction = cv; cd.fragmentFunction = cf; cd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uint64_t legacy0 = g_pb_legacy;
        EXPECT("constants: a pipeline made from it", 1, pso_build(cd, &se) != nil);
        EXPECT("constants: through the Metal 4 compiler", legacy0, g_pb_legacy);
        // a fragment function with no Metal 4 descriptor (as the DM_NVN_HILITE override) sends the pipeline to the MTLDevice: its vertex function is specialized for it
        cd.fragmentFunction = [cl newFunctionWithName:@"plain"]; se = nil;
        EXPECT("constants: an MTLDevice pipeline made from it", 1, pso_build(cd, &se) != nil);
        EXPECT("constants: that pipeline went through the MTLDevice", legacy0 + 1, g_pb_legacy);
        EXPECT("constants: the vertex function given to it is specialized once", 1, fn_dev(cv) != cv && fn_dev(cv) == fn_dev(cv));
        EXPECT("constants: a function without constants is its own", 1, fn_dev(cd.fragmentFunction) == cd.fragmentFunction);
    }
    repeated_archive_miss();
    spec_desc_ownership();
    capture_preserves_foreign_folder();
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_archive: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
