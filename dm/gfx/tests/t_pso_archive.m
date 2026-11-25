// Metal 4 pipelines (pso_build): one base per pipeline with its colour state unspecialized, made into pipelines by specialization, kept in the pipeline archive.
// Checks: pipelines that differ only in colour state share one base; a specialized pipeline draws the same pixels as one built through the MTLDevice; a flushed archive
// file serves the base again without compiling; a flush of bases without a vertex layout is a p<seq> file, where a base with a layout is not looked up; a function with no
// Metal 4 descriptor keeps the MTLDevice path; pso_key_norm drops what no pipeline depends on; compaction merges the files the prewarm took bases from into one per full period despite a
// mid-prewarm timer flush, removing them only after its last period, counting them for the prepare line, and a compaction that did not finish is not tried again on the same system build; a timer flush waits for any prewarm, so a cold start writes a file per full period; depth-only draws write depth; concurrent colour variants compile
// one base; archive flushes preserve unrelated compiler state; a function with function constants is specialized by its pipeline's descriptor, not through its library,
// and an MTLDevice pipeline gets it specialized; the descriptor for a specialization is the caller's whether the driver returns it retained or autoreleased; a compute pipeline is captured into a plain file and taken from it later, one without a Metal 4 descriptor never looked up; a native mesh pipeline is captured into a p file and taken from it later, with or without a task shader or a pixel shader, captured again by a compaction, and one without a Metal 4 descriptor built through the MTLDevice; a capture that fails builds the pipeline all the same and hands a full period over, or stops capturing after a failure in a period that captured nothing; run files are compacted by size tier from the pipeline count in their names (a tier of more than PSO_ARCH_COMPACT files and every smaller one, never a full period's, old and malformed names as the largest tier, the same across fresh processes), and a compute or task / mesh pipeline in a file left out is not captured again; a run that ends in its prewarm keeps what it compiled, through the prewarm's timer flush or through SIGTERM's graceful exit while a draw holds the graphics layer
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
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
static void archive_dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block);
#define dispatch_after archive_dispatch_after
#include "../nvn/nvn_mtl.m"
#undef dispatch_after
static void archive_cache_cleanup(void) {
    [NSFileManager.defaultManager removeItemAtPath:@(archive_cache_root) error:nil];
}
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
static CAMetalLayer *test_layer;
void *front_metal_layer(void) { return (__bridge void *)test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void hlecount_flush(void) {} void dm_rec_finish(void) {} void dm_cheats_exit(void) {} void dm_audio_suspend(int on) { (void)on; }   // what the graceful exit (hle_lifecycle.c) calls besides graphics
static int checks, failed;
// The next program of a test hand-over (--exec-seed): it starts only after the archive's write, so the folder holds the file; it says so in exec.txt (the test reads it)
static void exec_then(void *dir) {
    int files = 0; for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@((const char *)dir) error:nil]) files += [f hasSuffix:@".mtl4a"];
    NSString *path = [@((const char *)dir) stringByAppendingPathComponent:@"exec.txt"];
    if (!path) abort();
    [[NSString stringWithFormat:@"%d\n", files] writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

// Drive the real archive timer callback on its queue without waiting ten seconds for each retry.
static int test_archive_timer;
static dispatch_block_t test_timer_block;
static void archive_dispatch_after(dispatch_time_t when, dispatch_queue_t queue, dispatch_block_t block) {
    if (test_archive_timer && queue == g_arch_q) {
        EXPECT("at most one archive timer queued", 1, test_timer_block == nil);
        test_timer_block = block;
    } else {
        dispatch_after(when, queue, block);
    }
}
static void fire_archive_timer(void) {
    dispatch_sync(g_arch_q, ^{
        dispatch_block_t tick = test_timer_block;
        EXPECT("archive timer armed", 1, tick != nil);
        test_timer_block = nil;
        if (tick) tick();
    });
}

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
static NSError *compile_error(int kind) {
    return kind == 2 ? [NSError errorWithDomain:@"AGXMetalG14G" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Compilation failed due to an interrupted connection: XPC_ERROR_CONNECTION_INTERRUPTED."}]
                     : [NSError errorWithDomain:@"test" code:1 userInfo:nil];
}
// Hold one function's first library specialization while another caller requests it and an unrelated function specializes.
@interface FunctionGateLibrary : NSObject {
@public id<MTLLibrary> library; NSString *name; dispatch_semaphore_t entered, extra, release; _Atomic int calls; int fail;
}
@end
@implementation FunctionGateLibrary
- (id)forwardingTargetForSelector:(SEL)selector { return library; }
- (id<MTLFunction>)newFunctionWithName:(NSString *)entry constantValues:(MTLFunctionConstantValues *)values error:(NSError **)error {
    if (fail) { if (error) *error = compile_error(fail); return nil; }
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
@public id<MTL4Compiler> compiler; dispatch_semaphore_t entered, release; _Atomic int calls, lookup_calls; int fail, fail_spec;
}
@end
@implementation ArchiveGateCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (BOOL)refuses:(NSError **)error {
    int call = atomic_fetch_add(&calls, 1);
    if (!call) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
    if (fail && error) *error = compile_error(fail);
    return fail != 0;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    if (opts.lookupArchives.count) atomic_fetch_add(&lookup_calls, 1);
    if ([self refuses:error]) return nil;
    return [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    if ([self refuses:error]) return nil;
    return [compiler newComputePipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateBySpecializationWithDescriptor:(MTL4RenderPipelineDescriptor *)d pipeline:(id<MTLRenderPipelineState>)base error:(NSError **)error {
    if (entered) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
    if (fail_spec) { if (error) *error = compile_error(fail_spec); return nil; }
    return [compiler newRenderPipelineStateBySpecializationWithDescriptor:d pipeline:base error:error];
}
@end
// Inject service loss at the device compilation APIs without changing the real compiler service.
@interface CompileFailureDevice : NSObject { @public id<MTLDevice> device; } @end
@implementation CompileFailureDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)f error:(NSError **)error {
    if (error) *error = compile_error(2); return nil;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    if (error) *error = compile_error(2); return nil;
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:(MTLMeshRenderPipelineDescriptor *)d options:(MTLPipelineOption)o reflection:(MTLAutoreleasedRenderPipelineReflection *)r error:(NSError **)error {
    if (error) *error = compile_error(2); return nil;
}
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)s options:(MTLCompileOptions *)o error:(NSError **)error {
    if (error) *error = compile_error(2); return nil;
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
    // a recapture still queued when the prewarm ends (the harvest queue held 300 ms): the flush after the prewarm leaves the compaction open, and the recapture's own flush closes it
    g_compact_enabled = 1; g_compacted = 0; g_compact_closed = 0; g_compact_periods = 0; atomic_store(&g_warm_running, 1);
    dispatch_async(arch_harvest_q(), ^{ usleep(300000); });
    EXPECT("compute, queued recapture: a pipeline", 1, cps_new(kf, &err) != nil);
    atomic_store(&g_warm_running, 0); arch_flush();   // as warm_finish
    EXPECT("compute, queued recapture: the compaction stays open", 0, g_compact_closed);
    arch_harvest_wait(DISPATCH_TIME_FOREVER);
    EXPECT("compute, queued recapture: captured again", 1, g_compacted);
    EXPECT("compute, queued recapture: its flush closes the compaction", 1, g_compact_closed);
    arch_flush_wait(); dispatch_sync(g_arch_q, ^{});
    EXPECT("compute, queued recapture: its period written as the compaction's", 0, g_compact_periods + g_arch_new);
    // a compaction due to close when the current period is empty (its recaptures all handed over): closed all the same
    g_compact_closed = 0; g_compacted = 1;
    arch_flush(); dispatch_sync(g_arch_q, ^{});
    EXPECT("compaction, empty period: closed", 1, g_compact_closed);
    g_compact_enabled = 0; g_compacted = 0; g_compact_closed = 0; g_compact_periods = 0;
    // no Metal 4 descriptor: built through the MTLDevice, no lookup, nothing captured
    int looked = la->lookups; built = g_cps_built; pending = g_arch_new;
    id<MTLComputePipelineState> up = cps_new(uf, &err);
    EXPECT("compute without a descriptor: a pipeline", 1, up != nil);
    EXPECT("compute without a descriptor: never looked up", looked, la->lookups);
    EXPECT("compute without a descriptor: built", built + 1, g_cps_built);
    dispatch_sync(arch_harvest_q(), ^{});
    EXPECT("compute without a descriptor: not captured", pending, g_arch_new);
    EXPECT("compute without a descriptor: its dispatch writes", 1, run_kernel(up));
    // a capture still queued behind a slow one (the harvest queue held 300 ms): a waiting flush and the quit's save each take it into the period they write (arch_harvest_wait)
    for (int quit = 0; quit < 2; quit++) {
        NSString *kn = quit ? @"kq" : @"kf", *s2 = [src stringByReplacingOccurrencesOfString:@"void k(" withString:[NSString stringWithFormat:@"void %@(", kn]];
        id<MTLLibrary> l2 = [g_dev newLibraryWithSource:s2 options:nil error:&err];
        id<MTLFunction> f2 = fn4_note([l2 newFunctionWithName:kn], l2, kn, nil);
        NSUInteger files = archive_files(@(kdir)).count; built = g_cps_built;
        dispatch_async(arch_harvest_q(), ^{ usleep(300000); });
        EXPECT(quit ? "queued capture, quit: a pipeline" : "queued capture, flush: a pipeline", 1, cps_new(f2, &err) != nil && g_cps_built == built + 1);
        if (quit) mtl_arch_save(MTL_QUIT_ARCH_MS, 0); else arch_flush_wait();
        EXPECT(quit ? "queued capture, quit: written" : "queued capture, flush: written", files + 1, archive_files(@(kdir)).count);
        EXPECT(quit ? "queued capture, quit: nothing left" : "queued capture, flush: nothing left", 0, g_arch_new);
    }
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
    EXPECT("mesh: whole pipeline keeps mesh grid limit", 1, p1.maxTotalThreadgroupsPerMeshGrid > 0 && p2.maxTotalThreadgroupsPerMeshGrid > 0);
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
    EXPECT("mesh again: archived whole pipeline keeps mesh grid limit", 1, q1.maxTotalThreadgroupsPerMeshGrid > 0 && q2.maxTotalThreadgroupsPerMeshGrid > 0);
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
    id<MTLRenderPipelineState> a = pso_build(desc(f0, blend, f1, m1, 1), 0, &e), b = [g_dev newRenderPipelineStateWithDescriptor:desc(f0, blend, f1, m1, 0) error:&e];
    checks++; if (!a || !b) { fprintf(stderr, "%s: no pipeline (%s)\n", name, e.localizedDescription.UTF8String); failed++; return; }
    float pa[2][4], pb[2][4]; draw_with(a, f0, f1, pa); draw_with(b, f0, f1, pb);
    checks++; if (memcmp(pa, pb, sizeof pa)) { fprintf(stderr, "%s: the specialized pipeline drew (%g %g %g %g | %g %g %g %g), the MTLDevice one (%g %g %g %g | %g %g %g %g)\n", name,
        pa[0][0], pa[0][1], pa[0][2], pa[0][3], pa[1][0], pa[1][1], pa[1][2], pa[1][3], pb[0][0], pb[0][1], pb[0][2], pb[0][3], pb[1][0], pb[1][1], pb[1][2], pb[1][3]); failed++; }
}
static void depth_only(void) {
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatInvalid, 0, MTLPixelFormatInvalid, MTLColorWriteMaskNone, 1);
    d.fragmentFunction = nil; NSError *err = nil; uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> ps = pso_build(d, 0, &err);
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
    if (!out) abort();
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
        NSError *err = nil; EXPECT("failed capture: a capture before", 1, pso_build(pd, 0, &err) != nil && atomic_load(&g_ep->n) > 0);
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
        @autoreleasepool { NSError *err = nil; id ps = pso_build(d, 0, &err);
            @synchronized(lock) { good += ps != nil && !err; }   // a capture that fails builds the pipeline by the plain compiler all the same
        }
    });
    EXPECT("concurrent: first compiler entered", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    // An unrelated key must complete while the first remains blocked. It has the held compile's stage functions, so it is not captured beside it (fn_claim, Metal crashes on
    // that): after the wait the plain compiler builds it
    int plain0 = g_fn_plain; g_fn_wait_ms = 200;
    MTLRenderPipelineDescriptor *other = [ds[0] copy]; other.vertexDescriptor = [other.vertexDescriptor copy]; other.vertexDescriptor.layouts[16].stride = 1024;
    dispatch_group_t separate = dispatch_group_create();
    dispatch_group_async(separate, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { pso_build(other, 0, NULL); } });
    EXPECT("concurrent: unrelated base progresses", 0, dispatch_group_wait(separate, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("concurrent: the unrelated base built without the archive", 1, g_fn_plain - plain0); g_fn_wait_ms = FN_WAIT_MS;
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER); dispatch_group_wait(separate, DISPATCH_TIME_FOREVER);
    EXPECT("concurrent: all callers receive result", 8, good);
    if (!fail) EXPECT("concurrent: one compile per base, the unrelated one by the plain compiler", 1, gate->calls);
    EXPECT("concurrent: no unfinished builds", 0, g_pbase_pending.n);
    ep->comp = gate->compiler;
    if (!fail) return;
    // the full period was handed over once, and the run still captures, into a fresh period
    EXPECT("failed capture: a new period", 1, g_ep != ep);
    EXPECT("failed capture: handed over once", 1, ep->failed);
    EXPECT("failed capture: still capturing", 1, arch_capturing());
    NSError *err = nil; int n0 = atomic_load(&g_ep->n); nvn_hm_clear(&g_pbase, hm_rel);
    EXPECT("failed capture: the base captured in the new period", 1, pso_build(ds[0], 0, &err) != nil && atomic_load(&g_ep->n) == n0 + 1);
    // a capture that fails in a period that has captured nothing: not the store's size, so the run captures no more, and builds the pipeline all the same
    arch_flush_wait();
    ArchEpoch *fresh = g_ep; ArchiveGateCompiler *fg = [ArchiveGateCompiler new]; fg->compiler = fresh->comp; fg->fail = 1; fg->calls = 1; fresh->comp = (id<MTL4Compiler>)fg;
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("failed capture in a new period: built", 1, pso_build(ds[1], 0, &err) != nil && !err);
    EXPECT("failed capture in a new period: no more captures", 0, arch_capturing());
    int calls = fg->calls; nvn_hm_clear(&g_pbase, hm_rel);
    EXPECT("no more captures: built by the plain compiler", 1, pso_build(ds[2], 0, &err) != nil && fg->calls == calls && g_ep == fresh);
    // a pipeline both compilers refuse fails, with the plain compiler's error
    atomic_store(&g_arch_cap_off, 0); id<MTL4Compiler> plain = g_comp4;
    ArchiveGateCompiler *pg = [ArchiveGateCompiler new]; pg->compiler = plain; pg->fail = 1; pg->calls = 1; g_comp4 = (id<MTL4Compiler>)pg;
    atomic_store(&fresh->n, 1);   // as a period that holds a capture, so the failure is not taken for a broken capture
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("both refuse: no pipeline, the error", 1, pso_build(ds[3], 0, &err) == nil && [err.domain isEqualToString:@"test"]);
    EXPECT("both refuse: not a failed capture", 0, fresh->failed);
    atomic_store(&fresh->n, 0); g_comp4 = plain; fresh->comp = fg->compiler;
    nvn_hm_clear(&g_pbase, hm_rel); err = nil;
    EXPECT("concurrent: failed base can retry", 1, pso_build(ds[0], 0, &err) != nil);
}
// A capture the compiler service died under (arch_capture_try, err_compiler_died): its period is handed over and not written, the counts of unwritten pipelines drop by what it held,
// and the run goes on capturing into a new period that is written. A written crashed period made Ocarina of Time's every later start abort in Metal's archive lookup
static void crashed_period(void) {
    char cdir[] = "/tmp/t_pso_crashed.XXXXXX"; if (!mkdtemp(cdir)) abort();
    arch_flush_wait(); NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
    NSError *err = nil;
    MTLRenderPipelineDescriptor *a = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); a.vertexDescriptor.layouts[16].stride = 4096;
    EXPECT("crashed: a capture before", 1, pso_build(a, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->fail = 2; gate->calls = 1; ep->comp = (id<MTL4Compiler>)gate;
    MTLRenderPipelineDescriptor *b = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); b.vertexDescriptor.layouts[16].stride = 3072;
    err = nil; EXPECT("crashed: the pipeline built by the plain compiler", 1, pso_build(b, 0, &err) != nil && !err);
    EXPECT("crashed: the period marked", 1, ep->crashed);
    EXPECT("crashed: handed over", 1, g_ep != ep);
    EXPECT("crashed: still capturing", 1, arch_capturing());
    ep->comp = gate->compiler;
    arch_flush_wait();
    EXPECT("crashed: the period not written", 0, (int)archive_files(@(cdir)).count);
    EXPECT("crashed: nothing left counted as unwritten", 0, atomic_load(&g_arch_new));
    MTLRenderPipelineDescriptor *c = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); c.vertexDescriptor.layouts[16].stride = 2560;
    EXPECT("crashed: the next period captures", 1, pso_build(c, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    arch_flush_wait();
    EXPECT("crashed: the next period written", 1, (int)archive_files(@(cdir)).count);
    // the compiler service dying under a compile that is not a capture (a plain or MTLDevice pipeline): the current period is not written either
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); d.vertexDescriptor.layouts[16].stride = 2048;
    EXPECT("died elsewhere: a capture before", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    ArchEpoch *ep2 = g_ep;
    arch_died([NSError errorWithDomain:@"AGXMetalG14G" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Compilation failed due to an interrupted connection: XPC_ERROR_CONNECTION_INTERRUPTED."}]);
    arch_died([NSError errorWithDomain:@"test" code:1 userInfo:nil]);   // any other error: nothing
    EXPECT("died elsewhere: the period marked", 1, ep2->crashed);
    EXPECT("died elsewhere: handed over", 1, g_ep != ep2);
    arch_flush_wait();
    EXPECT("died elsewhere: the period not written", 1, (int)archive_files(@(cdir)).count);
    EXPECT("died elsewhere: nothing left counted as unwritten", 0, atomic_load(&g_arch_new));
    nvn_hm_clear(&g_pbase, hm_rel);
    [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[]; g_arch4p = @[];
}
// Hand over a period while its last capture is blocked. The plain fallback stays blocked until the writer has run: marking after fallback is too late, even if it succeeds.
static void crashed_flush_race(int compute) {
    char cdir[] = "/tmp/t_pso_crash_race.XXXXXX"; if (!mkdtemp(cdir)) abort();
    arch_flush_wait(); NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
    NSError *err = nil;
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); d.vertexDescriptor.layouts[16].stride = 4608 + compute * 16;
    EXPECT("crash race: a captured base", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    ArchEpoch *ep = g_ep; ArchiveGateCompiler *capture = [ArchiveGateCompiler new], *plain = [ArchiveGateCompiler new];
    capture->compiler = ep->comp; capture->fail = 2; capture->entered = dispatch_semaphore_create(0); capture->release = dispatch_semaphore_create(0);
    plain->compiler = g_comp4; plain->fail = 1; plain->entered = dispatch_semaphore_create(0); plain->release = dispatch_semaphore_create(0);
    ep->comp = (id<MTL4Compiler>)capture; g_comp4 = (id<MTL4Compiler>)plain;
    MTL4RenderPipelineDescriptor *rd = pso_desc4(d, 1, 0);
    MTL4ComputePipelineDescriptor *cd = [MTL4ComputePipelineDescriptor new]; cd.computeFunctionDescriptor = rd.vertexFunctionDescriptor;   // the fake compiler refuses before validation
    dispatch_group_t work = dispatch_group_create();
    dispatch_group_async(work, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (compute) arch_capture_cs(cd, NULL, NULL); else arch_capture(rd, NULL, 1, NULL);
    });
    EXPECT("crash race: capture entered", 0, dispatch_semaphore_wait(capture->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    arch_hand_over(ep, ARCH_FLUSH_NOW);
    EXPECT("crash race: handed over while active", 1, g_ep != ep && atomic_load(&ep->active) == 1);
    dispatch_semaphore_signal(capture->release);
    EXPECT("crash race: capture left", 0, dispatch_group_wait(ep->busy, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("crash race: marked before releasing the period", 1, atomic_load(&ep->crashed));
    dispatch_semaphore_t written = dispatch_semaphore_create(0);
    dispatch_async(g_arch_q, ^{ dispatch_semaphore_signal(written); });
    EXPECT("crash race: writer completed before fallback", 0, dispatch_semaphore_wait(written, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("crash race: nothing published", 0, archive_files(@(cdir)).count);
    EXPECT("crash race: unwritten count drained", 0, atomic_load(&g_arch_new));
    dispatch_semaphore_signal(plain->release);
    EXPECT("crash race: fallback finished", 0, dispatch_group_wait(work, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    g_comp4 = plain->compiler; ep->comp = capture->compiler;
    arch_flush_wait(); nvn_hm_clear(&g_pbase, hm_rel);
    [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[]; g_arch4p = @[];
}
// Callers may ignore errors (background prediction) or pass NULL. The shared pipeline/function compile boundary must still invalidate the archive; ordinary errors must not.
static void compiler_death_notifications(void) {
    char cdir[] = "/tmp/t_pso_notify.XXXXXX"; if (!mkdtemp(cdir)) abort();
    arch_flush_wait(); NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
    for (int kind = 1; kind <= 2; kind++) {
        NSError *err = nil; MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); d.vertexDescriptor.layouts[16].stride = 4704 + kind * 16;
        EXPECT("notify: a captured base", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
        ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = g_comp4; gate->fail_spec = kind; g_comp4 = (id<MTL4Compiler>)gate;
        EXPECT("notify: specialization refused with no error output", 1, pso_build(d, 0, NULL) == nil);
        g_comp4 = gate->compiler;
        EXPECT("notify: only compiler death invalidates", kind == 2, atomic_load(&ep->crashed));
        EXPECT("notify: only compiler death hands over", kind == 2, g_ep != ep);
        arch_flush_wait();
        EXPECT("notify: only the ordinary error's period published", 1, archive_files(@(cdir)).count);
    }
    NSError *err = nil; MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1); d.vertexDescriptor.layouts[16].stride = 4800;
    EXPECT("notify function: a captured base", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    ArchEpoch *ep = g_ep; FunctionGateLibrary *lib = [FunctionGateLibrary new]; lib->library = g_lib; lib->fail = 2;
    EXPECT("notify function: specialization refused with no error output", 1, fn_lib_spec((id<MTLLibrary>)lib, @"vs", [MTLFunctionConstantValues new], NULL) == nil);
    EXPECT("notify function: period invalidated", 1, atomic_load(&ep->crashed));
    arch_flush_wait();
    EXPECT("notify function: nothing published", 1, archive_files(@(cdir)).count);
    // The background worker ignores NSError. Its interruption may return after the writer's bounded wait: that wait must never publish a still-unclassified period.
    d.vertexDescriptor.layouts[16].stride = 4816;
    EXPECT("notify background: a captured base", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
    ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = g_comp4; gate->fail_spec = 2;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0); g_comp4 = (id<MTL4Compiler>)gate;
    dispatch_queue_t q = dispatch_queue_create("test.pso-death", DISPATCH_QUEUE_SERIAL);
    pso_key key = {0}; uint32_t gen = 0;
    pthread_mutex_lock(&g_pp_mu); int slot = pso_pend_slot(&key, 0, &gen); pthread_mutex_unlock(&g_pp_mu);
    if (slot < 0) abort();
    pso_pend_start(slot, gen, d, 0, q);
    EXPECT("notify background: specialization entered", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    arch_hand_over(ep, ARCH_FLUSH_NOW);
    dispatch_semaphore_t writer = dispatch_semaphore_create(0);
    dispatch_async(g_arch_q, ^{ dispatch_semaphore_signal(writer); });
    EXPECT("notify background: writer finishes with plain compile unresolved", 0, dispatch_semaphore_wait(writer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("notify background: no publication before the compile reports its error", 1, archive_files(@(cdir)).count);
    dispatch_semaphore_signal(gate->release); dispatch_sync(q, ^{});
    EXPECT("notify background: the worker completed without a pipeline", 1, g_pp[slot].done && !g_pp[slot].ps);
    g_comp4 = gate->compiler; g_pp[slot].used = 0;
    arch_flush_wait();
    EXPECT("notify background: handed-over period not published", 1, archive_files(@(cdir)).count);
    // Geometry and embedded pipelines share these MTLDevice boundaries; no reporter is required to notify the archive.
    CompileFailureDevice *device = [CompileFailureDevice new]; device->device = g_dev;
    id<MTLLibrary> mesh_lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "[[mesh]] void ms(mesh<V, void, 3, 1, topology::triangle> out) { out.set_primitive_count(0); }\n"
        options:nil error:&err];
    id<MTLFunction> mesh_fn = [mesh_lib newFunctionWithName:@"ms"];
    EXPECT("notify device: fixture mesh function", 1, mesh_fn != nil);
    for (int kind = 0; kind < 4; kind++) {
        d.vertexDescriptor.layouts[16].stride = 4832 + kind * 16;
        EXPECT("notify device: a captured base", 1, pso_build(d, 0, &err) != nil && atomic_load(&g_ep->n) == 1);
        ep = g_ep; g_dev = (id<MTLDevice>)device; id result;
        if (kind == 0) result = cps_new([g_lib2 newFunctionWithName:@"vs"], NULL);   // unregistered: directly through the MTLDevice
        else if (kind == 1) result = pso_build_dev(d, 0, NULL);
        else if (kind == 2) {
            MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
            m.meshFunction = mesh_fn; m.fragmentFunction = [g_lib newFunctionWithName:@"fs"];
            result = dev_mps(m, NULL);
        }
        else result = dev_lib(@"fragment void fixture() {}", NULL);
        g_dev = device->device;
        EXPECT("notify device: compile refused", 1, result == nil);
        EXPECT("notify device: period invalidated", 1, atomic_load(&ep->crashed));
        arch_flush_wait();
        EXPECT("notify device: nothing published", 1, archive_files(@(cdir)).count);
    }
    nvn_hm_clear(&g_pbase, hm_rel);
    [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[]; g_arch4p = @[];
}
static void capture_preserves_foreign_folder(void) {
    arch_flush_wait();   // Finish earlier captures: the captures below go into a new flush period
    NSError *error = nil;
    MTLRenderPipelineDescriptor *prime = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    prime.vertexDescriptor.layouts[16].stride = 1536;
    // Establish the period's working folder with a capture of its own compiler
    EXPECT("foreign capture: prime real compiler", 1,
           [g_ep->comp newRenderPipelineStateWithDescriptor:pso_desc4(prime, 1, 0) compilerTaskOptions:nil error:&error] != nil);
    char root[PATH_MAX];
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, root, sizeof root)) { EXPECT("foreign capture: cache path available", 1, 0); return; }
    ArchEpoch *ep = g_ep; ArchiveForeignCompiler *proxy = [ArchiveForeignCompiler new]; proxy->compiler = ep->comp;
    NSString *cache = [@(root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    proxy->fixture = [cache stringByAppendingPathComponent:[@"gpuarchiver-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    MTLRenderPipelineDescriptor *fresh = [prime copy]; fresh.vertexDescriptor = [prime.vertexDescriptor copy];
    fresh.vertexDescriptor.layouts[16].stride = 2048;
    ep->comp = (id<MTL4Compiler>)proxy;   // the current flush period's compiler, which the captures go through
    EXPECT("foreign capture: real pipeline", 1, pso_build(fresh, 0, &error) != nil);
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
    for (int i = 0; i < 100; i++) EXPECT("negative probe: no archived pipeline", 1, pso_build_ex(d, 0, &e, 0) == nil);
    EXPECT("negative probe: one lookup for 100 misses", 1, archive->calls);
    EXPECT("negative probe: no unfinished owner", 0, g_pbase_pending.n);
    d.vertexDescriptor.layouts[16].stride = 1552;
    EXPECT("negative probe: another layout is independent", 1, pso_build_ex(d, 0, &e, 0) == nil);
    EXPECT("negative probe: another key looked up", 2, archive->calls);
    d.vertexDescriptor.layouts[16].stride = 1536;
    EXPECT("negative probe: demand can still compile", 1, pso_build_ex(d, 0, &e, 1) != nil);
    EXPECT("negative probe: the compile does not search again", 2, archive->calls);
    unsigned after = archive->calls;
    EXPECT("negative probe: compiled base overrides miss", 1, pso_build_ex(d, 0, &e, 0) != nil);
    EXPECT("negative probe: completed base needs no archive scan", after, archive->calls);
    g_arch4 = saved; g_arch4p = savedp;
    nvn_hm_clear(&g_pbase_miss, hm_rel);
}

// An app's folder of Metal's build folders (ga_claim / ga_remove): a start with no other copy running snapshots only earlier runs' directories with their (dev, ino)
// identity; a copy started beside it lists none; once the first exits, a third start still lists none while the second runs; and background removal preserves files,
// symlinks, unrelated directories, post-snapshot directories, and any entry replaced by a new directory, file, or symlink with the same name.
static NSSet<NSString *> *ga_names(NSArray<GaDir *> *dirs) {
    NSMutableSet<NSString *> *s = [NSMutableSet setWithCapacity:dirs.count];
    for (GaDir *g in dirs) [s addObject:g->name];
    return s;
}
static void app_folder_claim(void) {
    EXPECT("app folder: a host without a bundle shares Metal's folder", 0, ga_own());
    EXPECT("app folder: Metal's folder", 1, [ga_dir() hasSuffix:@"/com.apple.gpuarchiver"]);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *app_dir = [@(archive_cache_root) stringByAppendingPathComponent:@"org.nxnative.test"];
    NSString *dir = [app_dir stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    NSString *d_a = [dir stringByAppendingPathComponent:@"gpuarchiver-aaaaaa"];
    NSString *d_a_nested = [d_a stringByAppendingPathComponent:@"nested"];
    NSString *d_b = [dir stringByAppendingPathComponent:@"gpuarchiver-bbbbbb"];
    NSString *d_rep = [dir stringByAppendingPathComponent:@"gpuarchiver-replaced"];
    NSString *d_sfile = [dir stringByAppendingPathComponent:@"gpuarchiver-swapfile"];
    NSString *d_slink = [dir stringByAppendingPathComponent:@"gpuarchiver-swaplink"];
    for (NSString *p in @[d_a_nested, d_b, d_rep, d_sfile, d_slink])
        [fm createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:nil];
    [@"old" writeToFile:[d_a_nested stringByAppendingPathComponent:@"state"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [@"old" writeToFile:[d_b stringByAppendingPathComponent:@"state"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [@"old" writeToFile:[d_rep stringByAppendingPathComponent:@"old"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [@"old" writeToFile:[d_sfile stringByAppendingPathComponent:@"old"] atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [@"old" writeToFile:[d_slink stringByAppendingPathComponent:@"old"] atomically:NO encoding:NSUTF8StringEncoding error:nil];

    NSString *other = [dir stringByAppendingPathComponent:@"unrelated"];
    NSString *other_file = [other stringByAppendingPathComponent:@"state"];
    [fm createDirectoryAtPath:other withIntermediateDirectories:NO attributes:nil error:nil];
    [@"keep" writeToFile:other_file atomically:NO encoding:NSUTF8StringEncoding error:nil];
    EXPECT("app folder: nested symlink fixture created", 0, symlink(other.fileSystemRepresentation, [d_a_nested stringByAppendingPathComponent:@"out"].fileSystemRepresentation));

    NSString *file = [dir stringByAppendingPathComponent:@"gpuarchiver-file"];
    [@"keep" writeToFile:file atomically:NO encoding:NSUTF8StringEncoding error:nil];
    NSString *link = [dir stringByAppendingPathComponent:@"gpuarchiver-link"];
    EXPECT("app folder: matching symlink fixture created", 0, symlink(other.fileSystemRepresentation, link.fileSystemRepresentation));

    int a, b, c, d;
    NSArray<GaDir *> *got = ga_claim(dir, &a);
    NSSet<NSString *> *want = [NSSet setWithObjects:@"gpuarchiver-aaaaaa", @"gpuarchiver-bbbbbb", @"gpuarchiver-replaced", @"gpuarchiver-swapfile", @"gpuarchiver-swaplink", nil];
    EXPECT("app folder: the first copy lists only earlier run directories", 1, a >= 0 && [ga_names(got) isEqualToSet:want]);
    NSString *late = [dir stringByAppendingPathComponent:@"gpuarchiver-late"], *late_file = [late stringByAppendingPathComponent:@"live"];
    EXPECT("app folder: post-snapshot directory created", 1, [fm createDirectoryAtPath:late withIntermediateDirectories:NO attributes:nil error:nil]);
    EXPECT("app folder: post-snapshot file written", 1, [@"live" writeToFile:late_file atomically:NO encoding:NSUTF8StringEncoding error:nil]);
    EXPECT("app folder: a second copy lists none", 1, ga_claim(dir, &b) == nil && b >= 0);
    close(a);   // the first copy quits
    EXPECT("app folder: a third copy lists none while the second runs", 1, ga_claim(dir, &c) == nil && c >= 0);
    close(b); close(c);
    NSArray<GaDir *> *again = ga_claim(dir, &d);
    EXPECT("app folder: with no copy running, a start lists old and late directories", 6, again.count);
    close(d);

    // Concurrent-writer mutations after snapshot: keep the old directory's inode allocated across replacement creation so the new directory gets a distinct st_ino.
    NSString *moved_rep = [app_dir stringByAppendingPathComponent:@"moved-replaced"];
    NSString *rep_live = [d_rep stringByAppendingPathComponent:@"live"];
    EXPECT("app folder: move replaced fixture aside", 1, [fm moveItemAtPath:d_rep toPath:moved_rep error:nil]);
    EXPECT("app folder: create replacement directory", 1, [fm createDirectoryAtPath:d_rep withIntermediateDirectories:NO attributes:nil error:nil]);
    EXPECT("app folder: write file in replacement directory", 1, [@"live" writeToFile:rep_live atomically:NO encoding:NSUTF8StringEncoding error:nil]);
    EXPECT("app folder: remove moved old fixture", 1, [fm removeItemAtPath:moved_rep error:nil]);

    EXPECT("app folder: remove swapfile fixture dir", 1, [fm removeItemAtPath:d_sfile error:nil]);
    EXPECT("app folder: replace with regular file", 1, [@"file" writeToFile:d_sfile atomically:NO encoding:NSUTF8StringEncoding error:nil]);

    EXPECT("app folder: remove swaplink fixture dir", 1, [fm removeItemAtPath:d_slink error:nil]);
    EXPECT("app folder: replace with symlink", 0, symlink(other.fileSystemRepresentation, d_slink.fileSystemRepresentation));

    EXPECT("app folder: removes only unchanged snapshotted directories", 2, ga_remove(dir, got));
    struct stat st;
    EXPECT("app folder: old matching directory a gone", 0, lstat(d_a.fileSystemRepresentation, &st) == 0);
    EXPECT("app folder: old matching directory b gone", 0, lstat(d_b.fileSystemRepresentation, &st) == 0);
    EXPECT("app folder: unrelated directory retained", 1, [fm fileExistsAtPath:other_file]);
    EXPECT("app folder: matching regular file retained", 1, lstat(file.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode));
    EXPECT("app folder: matching symlink retained without following it", 1, lstat(link.fileSystemRepresentation, &st) == 0 && S_ISLNK(st.st_mode));
    EXPECT("app folder: directory created after snapshot retained", 1, [fm fileExistsAtPath:late_file]);
    EXPECT("app folder: replacement directory with same name retained", 1, [fm fileExistsAtPath:rep_live]);
    EXPECT("app folder: directory swapped for regular file retained", 1, lstat(d_sfile.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode));
    EXPECT("app folder: directory swapped for symlink retained", 1, lstat(d_slink.fileSystemRepresentation, &st) == 0 && S_ISLNK(st.st_mode));
    EXPECT("app folder: no folder, no lock", 1, ga_claim(nil, &d) == nil && d < 0);
    EXPECT("app folder: no folder, nothing removed", 0, ga_remove(nil, got));
}

// Injects per-stride compile failures across both the current compiler and fresh compilers created by ep_new().
@interface StrideFaultCompiler : NSObject {
@public
    id<MTL4Compiler> compiler;
    NSUInteger transient_stride, permanent_stride, reject_stride;
    _Atomic int transient_left;
    _Atomic int permanent_calls, reject_calls;
}
@end
@implementation StrideFaultCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    NSUInteger stride = d.vertexDescriptor.layouts[16].stride;
    if (permanent_stride && stride == permanent_stride) {
        atomic_fetch_add(&permanent_calls, 1);
        if (error) *error = compile_error(2);
        return nil;
    }
    if (reject_stride && stride == reject_stride) {
        atomic_fetch_add(&reject_calls, 1);
        if (error) *error = compile_error(1);
        return nil;
    }
    if (transient_stride && stride == transient_stride) {
        int left = atomic_load(&transient_left);
        while (left > 0 && !atomic_compare_exchange_weak(&transient_left, &left, left - 1)) {}
        if (left > 0) {
            if (error) *error = compile_error(2);
            return nil;
        }
    }
    return [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
@end

@interface StrideFaultDevice : NSObject {
@public
    id<MTLDevice> device;
    StrideFaultCompiler *proto;
}
@end
@implementation StrideFaultDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (id<MTL4Compiler>)newCompilerWithDescriptor:(MTL4CompilerDescriptor *)d error:(NSError **)error {
    id<MTL4Compiler> real = [device newCompilerWithDescriptor:d error:error];
    if (!real) return nil;
    StrideFaultCompiler *wrap = [StrideFaultCompiler new];
    wrap->compiler = real;
    wrap->transient_stride = proto->transient_stride;
    wrap->permanent_stride = proto->permanent_stride;
    wrap->reject_stride = proto->reject_stride;
    atomic_store(&wrap->transient_left, atomic_load(&proto->transient_left));
    return (id<MTL4Compiler>)wrap;
}
@end

static int run_self_bundle_open(NSString *bundle, int count, int with_compute, uint64_t *mask_out, int *usable_out, int *cs_usable_out) {
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:NSProcessInfo.processInfo.arguments[0]];
    t.arguments = @[@"--bundle-open", bundle, [NSString stringWithFormat:@"%d", count], with_compute ? @"1" : @"0"];
    NSMutableDictionary *e = [NSProcessInfo.processInfo.environment mutableCopy];
    e[@"DM_PSO_BUNDLE"] = bundle;
    e[@"DM_PSO_ARCHIVE"] = @"off";
    t.environment = e;
    NSPipe *pipe = [NSPipe pipe];
    t.standardOutput = pipe;
    t.standardError = pipe;
    if (![t launchAndReturnError:nil]) return -1;
    NSData *out = [pipe.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    NSString *text = [[NSString alloc] initWithData:out encoding:NSUTF8StringEncoding] ?: @"";
    int usable = -1, cs_usable = -1; uint64_t mask = 0;
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"BUNDLE_USABLE="]) usable = [line substringFromIndex:14].intValue;
        else if ([line hasPrefix:@"BUNDLE_MASK="]) mask = strtoull([line substringFromIndex:12].UTF8String, NULL, 16);
        else if ([line hasPrefix:@"BUNDLE_CS_USABLE="]) cs_usable = [line substringFromIndex:17].intValue;
    }
    if (usable_out) *usable_out = usable;
    if (mask_out) *mask_out = mask;
    if (cs_usable_out) *cs_usable_out = cs_usable;
    if (t.terminationStatus || usable < 0) fprintf(stderr, "--bundle-open output:\n%s\n", text.UTF8String);
    return t.terminationStatus;
}

static MTLRenderPipelineDescriptor *bundle_desc(int i) {
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    d.vertexDescriptor.layouts[16].stride = 16 * (NSUInteger)(100 + i);
    return d;
}

static MTL4ComputePipelineDescriptor *bundle_compute_desc(void) {
    NSError *err = nil;
    NSString *ksrc = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void bundle_k(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i ^ 0x5au; }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:ksrc options:nil error:&err];
    if (!lib) return nil;
    lib_tag(lib, @"fixture-bundle-cs.metallib");
    id<MTLFunction> fn = fn4_note([lib newFunctionWithName:@"bundle_k"], lib, @"bundle_k", nil);
    MTL4ComputePipelineDescriptor *cd = [MTL4ComputePipelineDescriptor new];
    cd.computeFunctionDescriptor = fn4_of(fn);
    return cd;
}

static void bundle_open_verify(int count, int with_compute) {
    pso4_init();
    int usable = 0; uint64_t mask = 0;
    for (int i = 0; i < count; i++) {
        NSError *err = nil;
        MTLRenderPipelineDescriptor *d = bundle_desc(i);
        if (pso_build_ex(d, 0, &err, 0) != nil) { usable++; mask |= 1ull << i; }
    }
    int cs_usable = 0;
    if (with_compute) {
        MTL4ComputePipelineDescriptor *cd = bundle_compute_desc();
        cs_usable = cd && arch_lookup(cd, 1, NULL) != nil;
    }
    printf("BUNDLE_USABLE=%d\nBUNDLE_MASK=%llx\nBUNDLE_CS_USABLE=%d\n", usable, (unsigned long long)mask, cs_usable);
    fflush(stdout);
}

// 1. Recoverable interruption: period 2 dies partway through (2 captured + 1 in-flight fallback);
// recovery recaptures all 3 in a fresh period after the serialization gate is released and publishes a complete bundle.
static void bundle_recovery_recoverable(void) {
    char sdir[] = "/tmp/t_pso_bstage.XXXXXX", bdir[] = "/tmp/t_pso_bout.XXXXXX";
    if (!mkdtemp(sdir) || !mkdtemp(bdir)) abort();
    arch_flush_wait(); arch_pkg_reset();
    NSString *saved_dir = g_arch_dir, *saved_out = g_pre_out;
    int saved_tool = g_pre_tool, saved_max = g_arch_period_max;
    g_arch_dir = @(sdir); g_pre_out = @(bdir); g_pre_tool = 1; g_arch_period_max = 4;
    nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
    for (int i = 0; i < 12; i++) {
        NSError *err = nil;
        ArchEpoch *ep = g_ep;
        ArchiveGateCompiler *gate = nil;
        if (i == 6) {
            gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->fail = 2; gate->calls = 1;
            ep->comp = (id<MTL4Compiler>)gate;
        }
        id ps = pso_build(bundle_desc(i), 0, &err);
        if (gate) ep->comp = gate->compiler;
        EXPECT("recoverable: pipeline built", 1, ps != nil);
    }
    arch_flush_wait();
    int pub = pre_bundle_out(@(bdir), 12);
    EXPECT("recoverable: pre_bundle_out succeeded", 1, pub);
    EXPECT("recoverable: 12 requested, 12 published", 1, g_pkg_last_requested == 12 && g_pkg_last_published == 12);
    EXPECT("recoverable: 3 lost keys recovered in 1 round", 1, g_pkg_last_recovered == 3 && g_pkg_last_rounds == 1);
    EXPECT("recoverable: 0 missing and 0 rejected", 1, g_pkg_last_missing_lost == 0 && g_pkg_last_missing_died == 0 && g_pkg_last_rejected == 0);
    // Retained descriptors are released once their periods write.
    int retained = 0;
    os_unfair_lock_lock(&g_pkg_lock);
    for (ArchPkgItem *it in g_pkg_items.allValues) retained += it->desc != nil;
    os_unfair_lock_unlock(&g_pkg_lock);
    EXPECT("recoverable: retained descriptors released after publish", 0, retained);
    // Every published archive file has a matching pipelines script for other GPUs.
    NSArray<NSString *> *out_files = archive_files(@(bdir));
    NSArray<NSString *> *scripts = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(bdir) error:nil]
                                    filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4-json'"]];
    EXPECT("recoverable: scripts match archive file count", out_files.count, scripts.count);
    int usable = -1; uint64_t mask = 0;
    int rc = run_self_bundle_open(@(bdir), 12, 0, &mask, &usable, NULL);
    EXPECT("recoverable: fresh open succeeded", 0, rc);
    EXPECT("recoverable: all 12 keys usable in fresh process", 12, usable);
    EXPECT("recoverable: exact key mask usable", (1ull << 12) - 1, mask);
    arch_pkg_reset(); nvn_hm_clear(&g_pbase, hm_rel);
    g_arch_dir = saved_dir; g_pre_out = saved_out; g_pre_tool = saved_tool; g_arch_period_max = saved_max;
    g_arch4 = @[]; g_arch4p = @[];
    [NSFileManager.defaultManager removeItemAtPath:@(sdir) error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(bdir) error:nil];
}

// 2. Repeated compiler-death failure with isolation:
// base 4 enters period 2, base 5 suffers a transient double-death (unconfirmed once), base 6 always kills the compiler service.
// Recovery rescues base 4 (known-buildable) and base 5 (isolated unconfirmed), bounds retries on base 6, reports it missing, and returns incomplete (2).
static void bundle_recovery_repeated_failure(void) {
    char sdir[] = "/tmp/t_pso_brep.XXXXXX", bdir[] = "/tmp/t_pso_brep_out.XXXXXX";
    if (!mkdtemp(sdir) || !mkdtemp(bdir)) abort();
    arch_flush_wait(); arch_pkg_reset();
    NSString *saved_dir = g_arch_dir, *saved_out = g_pre_out;
    int saved_tool = g_pre_tool, saved_max = g_arch_period_max;
    id<MTLDevice> saved_dev = g_dev; id<MTL4Compiler> saved_comp4 = g_comp4;
    g_arch_dir = @(sdir); g_pre_out = @(bdir); g_pre_tool = 1; g_arch_period_max = 4;
    nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];

    StrideFaultCompiler *proto = [StrideFaultCompiler new];
    proto->transient_stride = 16 * 105;   // base 5: fails first capture + plain fallback (2 calls), succeeds on recovery
    atomic_store(&proto->transient_left, 2);
    proto->permanent_stride = 16 * 106;   // base 6: always kills MTLCompilerService

    StrideFaultDevice *fdev = [StrideFaultDevice new]; fdev->device = saved_dev; fdev->proto = proto;
    StrideFaultCompiler *fplain = [StrideFaultCompiler new];
    fplain->compiler = saved_comp4; fplain->transient_stride = proto->transient_stride; fplain->permanent_stride = proto->permanent_stride;
    atomic_store(&fplain->transient_left, 1);

    g_dev = (id<MTLDevice>)fdev;
    g_comp4 = (id<MTL4Compiler>)fplain;
    // Swap current epoch's compiler through the same fault wrapper.
    ArchEpoch *ep0 = g_ep;
    StrideFaultCompiler *fep = [StrideFaultCompiler new];
    fep->compiler = ep0->comp; fep->transient_stride = proto->transient_stride; fep->permanent_stride = proto->permanent_stride;
    atomic_store(&fep->transient_left, 1);
    ep0->comp = (id<MTL4Compiler>)fep;

    uint64_t key6 = arch_key(pso_desc4(bundle_desc(6), 1, 0));
    for (int i = 0; i < 12; i++) {
        NSError *err = nil;
        id ps = pso_build(bundle_desc(i), 0, &err);
        EXPECT("repeated: expected build outcome", (i != 5 && i != 6), ps != nil);
    }
    // After the initial pass, clear transient_left on proto so recovery's fresh compilers let base 5 succeed while base 6 keeps dying.
    atomic_store(&proto->transient_left, 0);
    arch_flush_wait();
    int pub = pre_bundle_out(@(bdir), 12);
    // Restore device and compilers before fresh-process verification.
    ep0->comp = fep->compiler;
    g_comp4 = saved_comp4;
    g_dev = saved_dev;
    arch_flush_wait();
    os_unfair_lock_lock(&g_ep_lock); g_ep = ep_new(); os_unfair_lock_unlock(&g_ep_lock);

    EXPECT("repeated: pre_bundle_out reports incomplete (2)", 2, pub);
    EXPECT("repeated: 12 requested, 11 published", 1, g_pkg_last_requested == 12 && g_pkg_last_published == 11);
    EXPECT("repeated: base 4 and base 5 recovered despite poison base 6", 2, g_pkg_last_recovered);
    EXPECT("repeated: retries bounded at max", g_arch_pkg_max_retries, g_pkg_last_rounds);
    EXPECT("repeated: exactly 1 repeated compiler-death key missing, 0 rejected", 1, g_pkg_last_missing_died == 1 && g_pkg_last_missing_lost == 0 && g_pkg_last_rejected == 0);
    int retries6 = -1, state6 = -1;
    os_unfair_lock_lock(&g_pkg_lock);
    ArchPkgItem *it6 = g_pkg_items[@(key6)];
    if (it6) { retries6 = it6->retries; state6 = it6->state; }
    os_unfair_lock_unlock(&g_pkg_lock);
    EXPECT("repeated: poison key hit retry bound as unconfirmed compiler death", 1, retries6 == g_arch_pkg_max_retries && state6 == ARCH_PKG_LOST_UNCONFIRMED);

    int usable = -1; uint64_t mask = 0;
    int rc = run_self_bundle_open(@(bdir), 12, 0, &mask, &usable, NULL);
    EXPECT("repeated: fresh open succeeded", 0, rc);
    EXPECT("repeated: all 11 innocent keys usable in fresh process", 11, usable);
    EXPECT("repeated: only base 6 missing from usable mask", ((1ull << 12) - 1) & ~(1ull << 6), mask);

    arch_pkg_reset(); nvn_hm_clear(&g_pbase, hm_rel);
    g_arch_dir = saved_dir; g_pre_out = saved_out; g_pre_tool = saved_tool; g_arch_period_max = saved_max;
    g_arch4 = @[]; g_arch4p = @[];
    [NSFileManager.defaultManager removeItemAtPath:@(sdir) error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(bdir) error:nil];
}

// 3. Permanently rejected pipeline vs compiler-death loss:
// base 12 fails with an ordinary non-compiler-death error on both capture and plain; base 6 triggers a recoverable compiler-death interruption.
// Base 12 must not be retried, remains classified as permanently rejected, and does not prevent the 12 valid pipelines from publishing cleanly.
static void bundle_recovery_rejected_vs_lost(void) {
    char sdir[] = "/tmp/t_pso_brej.XXXXXX", bdir[] = "/tmp/t_pso_brej_out.XXXXXX";
    if (!mkdtemp(sdir) || !mkdtemp(bdir)) abort();
    arch_flush_wait(); arch_pkg_reset();
    NSString *saved_dir = g_arch_dir, *saved_out = g_pre_out;
    int saved_tool = g_pre_tool, saved_max = g_arch_period_max;
    id<MTL4Compiler> saved_comp4 = g_comp4;
    g_arch_dir = @(sdir); g_pre_out = @(bdir); g_pre_tool = 1; g_arch_period_max = 4;
    nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];

    StrideFaultCompiler *fplain = [StrideFaultCompiler new];
    fplain->compiler = saved_comp4; fplain->reject_stride = 16 * 112;
    g_comp4 = (id<MTL4Compiler>)fplain;

    uint64_t key12 = arch_key(pso_desc4(bundle_desc(12), 1, 0));
    for (int i = 0; i < 13; i++) {
        NSError *err = nil;
        ArchEpoch *ep = g_ep;
        id<MTL4Compiler> orig_ep_comp = ep->comp;
        if (i == 6) {
            ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = orig_ep_comp; gate->fail = 2; gate->calls = 1;
            ep->comp = (id<MTL4Compiler>)gate;
        } else if (i == 12) {
            StrideFaultCompiler *rej = [StrideFaultCompiler new]; rej->compiler = orig_ep_comp; rej->reject_stride = 16 * 112;
            ep->comp = (id<MTL4Compiler>)rej;
        }
        id ps = pso_build(bundle_desc(i), 0, &err);
        ep->comp = orig_ep_comp;
        EXPECT("rejected vs lost: expected build outcome", i != 12, ps != nil);
    }
    arch_flush_wait();
    int pub = pre_bundle_out(@(bdir), 13);
    g_comp4 = saved_comp4;

    EXPECT("rejected vs lost: pre_bundle_out succeeds for all valid keys", 1, pub);
    EXPECT("rejected vs lost: 13 requested, 12 published, 1 rejected", 1, g_pkg_last_requested == 13 && g_pkg_last_published == 12 && g_pkg_last_rejected == 1);
    EXPECT("rejected vs lost: 3 lost keys recovered, 0 missing", 1, g_pkg_last_recovered == 3 && g_pkg_last_missing_lost == 0 && g_pkg_last_missing_died == 0);
    int retries12 = -1, state12 = -1; int has_desc12 = -1;
    os_unfair_lock_lock(&g_pkg_lock);
    ArchPkgItem *it12 = g_pkg_items[@(key12)];
    if (it12) { retries12 = it12->retries; state12 = it12->state; has_desc12 = it12->desc != nil; }
    os_unfair_lock_unlock(&g_pkg_lock);
    EXPECT("rejected vs lost: permanently rejected key never retried and drops descriptor", 1, retries12 == 0 && state12 == ARCH_PKG_REJECTED && has_desc12 == 0);

    int usable = -1; uint64_t mask = 0;
    int rc = run_self_bundle_open(@(bdir), 12, 0, &mask, &usable, NULL);
    EXPECT("rejected vs lost: fresh open succeeded", 0, rc);
    EXPECT("rejected vs lost: all 12 valid keys usable in fresh process", 12, usable);
    EXPECT("rejected vs lost: exact valid key mask usable", (1ull << 12) - 1, mask);

    arch_pkg_reset(); nvn_hm_clear(&g_pbase, hm_rel);
    g_arch_dir = saved_dir; g_pre_out = saved_out; g_pre_tool = saved_tool; g_arch_period_max = saved_max;
    g_arch4 = @[]; g_arch4p = @[];
    [NSFileManager.defaultManager removeItemAtPath:@(sdir) error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(bdir) error:nil];
}

// 4. Compute pipeline captured via arch_capture_cs in an interrupted period is also recovered and usable in a fresh process.
static void bundle_recovery_compute(void) {
    char sdir[] = "/tmp/t_pso_bcs.XXXXXX", bdir[] = "/tmp/t_pso_bcs_out.XXXXXX";
    if (!mkdtemp(sdir) || !mkdtemp(bdir)) abort();
    arch_flush_wait(); arch_pkg_reset();
    NSString *saved_dir = g_arch_dir, *saved_out = g_pre_out;
    int saved_tool = g_pre_tool, saved_max = g_arch_period_max;
    g_arch_dir = @(sdir); g_pre_out = @(bdir); g_pre_tool = 1; g_arch_period_max = 8;
    nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];

    MTL4ComputePipelineDescriptor *cd = bundle_compute_desc();
    EXPECT("compute recovery: descriptor built", 1, cd != nil);
    NSError *err = nil; int cap = 0;
    EXPECT("compute recovery: render 0 built", 1, pso_build(bundle_desc(0), 0, &err) != nil);
    EXPECT("compute recovery: render 1 built", 1, pso_build(bundle_desc(1), 0, &err) != nil);
    EXPECT("compute recovery: compute captured", 1, arch_capture_cs(cd, &err, &cap) != nil && cap == 1);
    // Kill the compiler service before this period flushes: all 3 captures (2 render + 1 compute) are discarded.
    arch_died(compile_error(2));
    arch_flush_wait();
    EXPECT("compute recovery: crashed period discarded before publish", 0, archive_files(@(sdir)).count);

    int pub = pre_bundle_out(@(bdir), 3);
    EXPECT("compute recovery: pre_bundle_out succeeded", 1, pub);
    EXPECT("compute recovery: all 3 recovered", 1, g_pkg_last_requested == 3 && g_pkg_last_published == 3 && g_pkg_last_recovered == 3 && g_pkg_last_missing_lost == 0 && g_pkg_last_missing_died == 0);
    int usable = -1, cs_usable = -1; uint64_t mask = 0;
    int rc = run_self_bundle_open(@(bdir), 2, 1, &mask, &usable, &cs_usable);
    EXPECT("compute recovery: fresh open succeeded", 0, rc);
    EXPECT("compute recovery: both render bases usable", 2, usable);
    EXPECT("compute recovery: compute pipeline usable in fresh process", 1, cs_usable);

    arch_pkg_reset(); nvn_hm_clear(&g_pbase, hm_rel);
    g_arch_dir = saved_dir; g_pre_out = saved_out; g_pre_tool = saved_tool; g_arch_period_max = saved_max;
    g_arch4 = @[]; g_arch4p = @[];
    [NSFileManager.defaultManager removeItemAtPath:@(sdir) error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(bdir) error:nil];
}

int main(int argc, char **argv) { @autoreleasepool {
    if (!mkdtemp(archive_cache_root)) abort();
    atexit(archive_cache_cleanup);
    NSString *cache_path = [@(archive_cache_root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    if (!cache_path) abort();
    if (![NSFileManager.defaultManager createDirectoryAtPath:cache_path
                               withIntermediateDirectories:NO attributes:nil error:nil]) abort();
    if (argc == 2 && !strcmp(argv[1], "--cache-only")) {
        app_folder_claim();
        printf("t_pso_archive cache cleanup: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
    char dir[] = "/tmp/t_pso_archive.XXXXXX"; if (!mkdtemp(dir)) abort();
    int bundle_open = argc == 5 && !strcmp(argv[1], "--bundle-open");
    setenv("DM_PSO_ARCHIVE", bundle_open ? "off" : argc == 3 ? argv[2] : dir, 1);
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
    if (bundle_open) {
        bundle_open_verify(atoi(argv[3]), atoi(argv[4]));
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--bundle-recovery")) {
        pso4_init();
        bundle_recovery_recoverable();
        bundle_recovery_repeated_failure();
        bundle_recovery_rejected_vs_lost();
        bundle_recovery_compute();
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive bundle recovery: %d checks, %d failed\n", checks, failed); return failed != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--compiler-death")) {
        pso4_init(); crashed_flush_race(0); crashed_flush_race(1); compiler_death_notifications();
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive compiler death: %d checks, %d failed\n", checks, failed); return failed != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--active-flush")) {
        pso4_init();
        test_archive_timer = 1;
        test_layer = [CAMetalLayer layer];
        const uint64_t idle_ns = (ARCH_FLUSH_INTERVAL_S + 1) * (uint64_t)NSEC_PER_SEC;
        MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
        EXPECT("capture before the timer", 1, pso_build(cd, 0, &err) != nil && atomic_load(&g_arch_new) == 1);
        atomic_store(&dm_present_ns, now_ns() - idle_ns);
        ser_compile_begin();
        ArchEpoch *busy = ep_enter();
        fire_archive_timer();
        EXPECT("idle timer keeps the period while its compile is pending", 1, busy == g_ep);
        atomic_store(&dm_present_ns, now_ns());
        ep_leave(busy);
        ser_compile_end();
        fire_archive_timer();
        EXPECT("compile completion and a retry cannot start a write after presentation resumes", 0, archive_files(@(dir)).count);
        EXPECT("the partial period is still pending", 1, atomic_load(&g_arch_new));
        atomic_store(&dm_present_ns, now_ns() - idle_ns);
        plain_compile_begin();
        fire_archive_timer();
        EXPECT("idle timer also defers for a plain compile", 0, archive_files(@(dir)).count);
        plain_compile_end();
        fire_archive_timer();
        EXPECT("retry wrote the archive when idle with no compiles pending", 1, archive_files(@(dir)).count);
        EXPECT("the partial period was drained", 0, atomic_load(&g_arch_new));
        EXPECT("no retry after the archive was written", 1, test_timer_block == nil);
        atomic_store(&dm_present_ns, now_ns());
        cd.vertexDescriptor.layouts[16].stride += 16;
        EXPECT("capture before explicit save", 1, pso_build(cd, 0, &err) != nil && atomic_load(&g_arch_new) == 1);
        mtl_arch_save(5000, 0);
        EXPECT("explicit save also writes during presentation", 2, archive_files(@(dir)).count);
        g_arch_period_max = 1;
        cd.vertexDescriptor.layouts[16].stride += 16;
        EXPECT("capture fills another period", 1, pso_build(cd, 0, &err) != nil);
        dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER);
        EXPECT("full period also writes during presentation", 3, archive_files(@(dir)).count);
        g_arch_period_max = ARCH_PERIOD_MAX;
        test_layer = nil;
        cd.vertexDescriptor.layouts[16].stride += 16;
        EXPECT("capture before headless timer", 1, pso_build(cd, 0, &err) != nil && atomic_load(&g_arch_new) == 1);
        atomic_store(&dm_present_ns, now_ns());
        fire_archive_timer();
        dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER);
        EXPECT("headless timer wrote the capture", 4, archive_files(@(dir)).count);
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive active flush: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--adopt-prebuilt")) {
        // Real Metal 4 end-to-end check of arch_adopt_prebuilt(): a no-layout base captured into p*.mtl4a is opened and indexed in the same process,
        // clears g_pbase_miss, serves compile=0 lookups with zero recompiles, stays out of g_arch_use (arch_sweep), and is idempotent on repeat calls.
        pso4_init();
        MTLRenderPipelineDescriptor *nd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
        nd.vertexFunction = fn4_note([g_lib newFunctionWithName:@"vs0"], g_lib, @"vs0", nil);
        nd.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
        EXPECT("adopt: misses before capture", 1, pso_build_ex(nd, 0, &err, 0) == nil);
        EXPECT("adopt: miss cached in g_pbase_miss", 1, g_pbase_miss.n == 1);
        MTL4RenderPipelineDescriptor *m4 = pso_desc4(nd, 1, 0);
        EXPECT("adopt: Metal 4 base descriptor built", 1, m4 != nil);
        EXPECT("adopt: prebuild capture succeeded", 1, arch_capture(m4, &err, 0, NULL) != nil);
        arch_flush_wait();
        EXPECT("adopt: wrote a p*.mtl4a file", 1, archive_files(@(dir)).count == 1 && [archive_files(@(dir))[0] hasPrefix:@"p"]);
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built, asks0 = g_arch_asks;
        arch_adopt_prebuilt();
        EXPECT("adopt: indexed into g_arch_ix and g_arch_ixf", 1, g_arch_ix.n == 1 && g_arch_ixf.count == 1);
        EXPECT("adopt: prior miss cleared", 0, g_pbase_miss.n);
        EXPECT("adopt: excluded from arch_sweep", 0, g_arch_use.count);
        EXPECT("adopt: compile=0 lookup finds base in same run", 1, pso_build_ex(nd, 0, &err, 0) != nil);
        EXPECT("adopt: served from archive via index (1 ask, 0 compiles)", 1, g_pb_archived == arch0 + 1 && g_pb_built == built0 && g_arch_asks == asks0 + 1);
        arch_adopt_prebuilt();
        EXPECT("adopt: idempotent on already-opened file", 1, g_arch_ixf.count);
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive adopt prebuilt: %d checks, %d failed\n", checks, failed);
        return failed != 0;
    }
    if (argc == 3 && !strncmp(argv[1], "--cold-", 7)) {   // across fresh processes: a cold start's prewarm with a timer flush after each new base, then the next start
        g_arch_period_max = 4; int seed = !strcmp(argv[1], "--cold-seed"), cut = !strcmp(argv[1], "--cold-cut-seed"), after_cut = !strcmp(argv[1], "--cold-after-cut");
        int quit = !strcmp(argv[1], "--cold-quit-seed"), after_quit = !strcmp(argv[1], "--cold-after-quit");
        // a crashed compaction's marker, left by an earlier run: the quit's write must not take it away
        // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): argc == 3 above, so argv[2] is set
        if (quit) [arch_compact_stamp() writeToFile:[@(argv[2]) stringByAppendingPathComponent:@"compacting"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        pso4_init();
        EXPECT("cold restart: no compaction", 0, g_compact_enabled);
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built, asks0 = g_arch_asks;
        atomic_store(&g_warm_running, 1);
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 70);
            EXPECT("cold restart: pipeline", 1, pso_build(cd, 0, &err) != nil);
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
    if (argc == 3 && !strncmp(argv[1], "--sweep-", 8)) {   // across fresh processes: two files of four bases; the next start takes the first file's alone, so once the prewarm and the
        // pairs were asked for (arch_sweep's two conditions), the second leaves the archive (DM_OBSOLETE: into its folder), with its index
        g_arch_period_max = 4; int seed = !strcmp(argv[1], "--sweep-seed");
        pso4_init();
        NSUInteger (^files)(void) = ^NSUInteger { return [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(argv[2]) error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]].count; };
        for (int f = 0; f < (seed ? 2 : 1); f++) {
            for (int i = 0; i < 4; i++) {
                MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
                cd.vertexDescriptor.layouts[16].stride = 16 * (i + 200 + 10 * f);
                EXPECT("sweep: pipeline", 1, pso_build(cd, 0, &err) != nil);
            }
            dispatch_sync(g_arch_q, ^{ arch_flush(); }); arch_flush_wait();
        }
        if (seed) EXPECT("sweep: two files written", 2, (int)files());
        else {
            EXPECT("sweep: two files opened", 2, (int)g_arch_use.count);
            EXPECT("sweep: every base from the archive", 4, (int)g_pb_archived);
            atomic_store(&g_sweep_warm, 1); arch_sweep();
            EXPECT("sweep: nothing goes before the pairs were asked for", 2, (int)files());
            atomic_store(&g_sweep_pairs, 1); arch_sweep();
            EXPECT("sweep: the file no base was taken from left the archive", 1, (int)files());
            arch_sweep();
            EXPECT("sweep: once", 1, (int)files());
        }
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    // Across fresh processes: a run ends in its prewarm, before any full period (Ocarina of Time's warm-up runs of 2026-11-10 without a window: 400-500 bases held, none written).
    // --warmflush-: the timer's flush in a prewarm the game plays through hands the period over once it is DM_PSO_ARCH_WARM_FLUSH_MS old, so a run killed after it (no exit write)
    // keeps them. --warmprep-: the same in the prepare step, which ends before the first frame, hands nothing over: the next start compiles them. --term-: SIGTERM while a draw is
    // still in the graphics layer (DM_TEST_STUCK_DRAW: it never leaves, as one compiling its pipeline did), so the drain never returns; the archive is written all the same.
    // --exec-: oe::ExecuteProgram's hand-over (dm_host_shutdown_then) with that draw: the next program (exec_then) starts after the write, and finds the file there. Then the
    // next start takes every base from the archive and compiles none
    if (argc == 3 && (!strncmp(argv[1], "--warmflush-", 12) || !strncmp(argv[1], "--warmprep-", 11) || !strncmp(argv[1], "--term-", 7) || !strncmp(argv[1], "--exec-", 7))) {
        int term = !strncmp(argv[1], "--term-", 7), exec = !strncmp(argv[1], "--exec-", 7), prep = !strncmp(argv[1], "--warmprep-", 11), seed = strstr(argv[1], "-seed") != NULL;
        enum { K = 5 };
        if (seed && term) { void dm_term_install(void); dm_term_install(); }
        pso4_init();
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
        atomic_store(&g_warm_running, 1); atomic_store(&g_warm_bg, !prep);
        for (int i = 0; i < K; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 120);
            EXPECT("end in the prewarm: pipeline", 1, pso_build(cd, 0, &err) != nil);
        }
        if (seed) {
            EXPECT("end in the prewarm: compiled", K, (int)(g_pb_built - built0));
            if (!term && !exec) {   // the timer's flush, once the period is old enough; then the run is killed (no exit write)
                usleep(400 * 1000); dispatch_sync(g_arch_q, ^{ arch_flush(); }); dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER);
                printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed); fflush(stdout);
                _exit(failed != 0);
            }
            if (getenv("DM_TEST_STUCK_DRAW")) { void dm_gfx_enter(void); dm_gfx_enter(); }   // a draw under way that does not return
            printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed); fflush(stdout);
            if (failed) _exit(1);
            if (exec) { void dm_host_shutdown_then(const char *, void (*)(void *), void *); dm_host_shutdown_then("test hand-over", exec_then, argv[2]); }
            kill(getpid(), SIGTERM);   // to the process, as a harness sends it: raise() aims it at this thread, where the ignored signal never reaches the GCD signal source
            for (;;) pause();
        }
        atomic_store(&g_warm_running, 0); arch_flush_wait();
        EXPECT("after an end in the prewarm: bases from the archive", prep ? 0 : K, (int)(g_pb_archived - arch0));
        EXPECT("after an end in the prewarm: bases compiled", prep ? K : 0, (int)(g_pb_built - built0));
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
        if (seed) for (MTLRenderPipelineDescriptor *cd in descs) { EXPECT("old names restart: pipeline", 1, pso_build(cd, 0, &err) != nil); arch_flush_wait(); }
        else {
            EXPECT("old names restart: the nine old names compacted once", replay ? PSO_ARCH_COMPACT + 1 : 0, g_compact_enabled ? compacting_n() : 0);
            uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
            atomic_store(&g_warm_running, 1);
            for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, 0, &err);
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
            for (int i = 0; i < 4; i++) EXPECT("tier restart: pipeline", 1, pso_build(descs[i], 0, &err) != nil);   // the 4th capture hands the period over
            arch_flush_wait();
            for (NSUInteger i = 4; i < descs.count; i++) { EXPECT("tier restart: pipeline", 1, pso_build(descs[i], 0, &err) != nil); arch_flush_wait(); }
        } else {
            EXPECT("tier restart: the nine small files compacted, not the full one", replay ? PSO_ARCH_COMPACT + 1 : 0, g_compact_enabled ? compacting_n() : 0);
            uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
            atomic_store(&g_warm_running, 1);
            for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, 0, &err);
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
            if (seed) { EXPECT("restart: pipeline", 1, pso_build(cd, 0, &err) != nil); arch_flush_wait(); }
        }
        if (!seed) {
            int fresh = getenv("DM_TEST_ARCHIVE_MIXED") != NULL, compacting = g_compact_files != nil;
            int outcomes[PSO_ARCH_COMPACT + 1] = {0}; int *results = outcomes;
            dispatch_apply(descs.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
                @autoreleasepool { results[i] = pso_build(descs[i], 0, NULL) != nil; }
            });
            for (int i = 0; i <= PSO_ARCH_COMPACT; i++) EXPECT("restart: pipeline", 1, outcomes[i]);
            if (fresh) {
                MTLRenderPipelineDescriptor *extra = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
                extra.fragmentFunction = test_fn(g_lib, @"fs2", 1);
                extra.vertexDescriptor.layouts[16].stride = 4096;
                EXPECT("restart: fresh pipeline mixed with recaptures", 1, pso_build(extra, 0, &err) != nil);
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
    if (!file) abort();
    EXPECT("archive file written", 1, [NSFileManager.defaultManager fileExistsAtPath:file]);
    EXPECT("the flush began a new period", 1, g_ep != ep0);
    EXPECT("nothing left to write", 0, g_arch_new);
    {   // its index: the one base's key, complete; a key is the descriptor's, the same for the same pipeline and another for another, none without names
        int complete = -1; NSData *keys = arch_keys_read(arch_keys_path(file), &complete);
        MTLRenderPipelineDescriptor *k1 = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll, 1), *k2 = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll, 1);
        uint64_t a = arch_key(pso_desc4(k1, 1, 0)), b = arch_key(pso_desc4(k2, 1, 0));
        EXPECT("index: written beside the file, one key", 1, keys.length == sizeof(uint64_t));
        EXPECT("index: complete", 1, complete);
        EXPECT("index: the base's key", 1, keys.length == sizeof(uint64_t) && !memcmp(keys.bytes, &a, sizeof a));
        EXPECT("key: the same pipeline, the same key", 1, a && a == b);
        k2.vertexDescriptor.layouts[16].stride = 32;
        EXPECT("key: another layout, another key", 1, arch_key(pso_desc4(k2, 1, 0)) != a);
        MTL4RenderPipelineDescriptor *u = pso_desc4(k1, 1, 0); MTL4LibraryFunctionDescriptor *uf = [MTL4LibraryFunctionDescriptor new];
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
    {   // two shader keys whose library files hold the same bytes (Cuphead's sprite shaders): Metal hands out one function object for both, and fn4_note keeps the descriptor of whichever
        // was noted last, which the prewarm's workers make differ from start to start. The key names a library by its bytes, so it is the same either way; other bytes, another key
        NSString *metal = [@(dir) stringByAppendingPathComponent:@"same.metal"], *la = [@(dir) stringByAppendingPathComponent:@"aaaa_b.metallib"],
                 *lb = [@(dir) stringByAppendingPathComponent:@"bbbb_b.metallib"], *lc = [@(dir) stringByAppendingPathComponent:@"cccc_b.metallib"];
        int (^compile)(NSString *, NSString *) = ^int(NSString *code, NSString *out) {
            if (![code writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil]) return 0;
            NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"]; task.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", out];
            if (![task launchAndReturnError:nil]) return 0;
            [task waitUntilExit]; return task.terminationStatus == 0;
        };
        int built = compile(src, la) && [NSFileManager.defaultManager copyItemAtPath:la toPath:lb error:nil] && compile([src stringByAppendingString:@"fragment O fs3() { return { float4(0.3), float4(0) }; }\n"], lc);
        EXPECT("same bytes: the libraries built", 1, built);
        id<MTLLibrary> a = built ? lib_from_file(la, &err) : nil, b = built ? lib_from_file(lb, &err) : nil, c = built ? lib_from_file(lc, &err) : nil;
        EXPECT("same bytes: two library objects", 1, a && b && a != b);
        MTLRenderPipelineDescriptor *k = desc(MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll, 1);
        k.vertexFunction = fn4_note([a newFunctionWithName:@"vs"], a, @"vs", nil); uint64_t ka = arch_key(pso_desc4(k, 1, 0));
        id<MTLFunction> fb = fn4_note([b newFunctionWithName:@"vs"], b, @"vs", nil); uint64_t kb = arch_key(pso_desc4(k, 1, 0));
        EXPECT("same bytes: Metal hands out one function", 1, fb == k.vertexFunction);   // what makes the noted library vary; the key must not depend on it either way
        EXPECT("same bytes: the same key whichever library was noted last", 1, ka && ka == kb);
        k.vertexFunction = fn4_note([c newFunctionWithName:@"vs"], c, @"vs", nil);
        EXPECT("other bytes: another key", 1, arch_key(pso_desc4(k, 1, 0)) != ka);
        for (NSString *p in @[metal, la, lb, lc]) unlink(p.fileSystemRepresentation);
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
    EXPECT("no layout: a pipeline", 1, pso_build(nd, 0, &err) != nil);
    EXPECT("no layout: its base compiled", built + 1, g_pb_built);
    arch_flush_wait();
    NSString *pfile = [@(dir) stringByAppendingPathComponent:@"p00001.mtl4a"];
    EXPECT("no layout: written to a p file", 1, [NSFileManager.defaultManager fileExistsAtPath:pfile]);
    nvn_hm_clear(&g_pbase, hm_rel);
    id<MTL4Archive> pa = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:pfile] error:&err], va = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err];
    g_arch4 = @[]; g_arch4p = @[pa, va];   // the layout base's file among the p files: not looked in
    uint64_t arch0 = g_pb_archived; built = g_pb_built;
    EXPECT("no layout again: a pipeline", 1, pso_build(nd, 0, &err) != nil);
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
            if (!pso_build(cd, 0, &err)) { fprintf(stderr, "compaction: no pipeline %d\n", i); failed++; }
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
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, 0, &err);
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
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, 0, &err);
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
                if (!pso_build(cd, 0, &err)) { fprintf(stderr, "tier: no pipeline %d\n", i); failed++; }
            }
            arch_flush_wait();
        }
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [small addObject:cd];
            if (!pso_build(cd, 0, &err)) { fprintf(stderr, "tier: no pipeline %d\n", i); failed++; }
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
        for (MTLRenderPipelineDescriptor *cd in full) pso_build(cd, 0, &err);
        for (MTLRenderPipelineDescriptor *cd in small) pso_build(cd, 0, &err);
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
        for (MTLRenderPipelineDescriptor *cd in full) pso_build(cd, 0, &err);
        for (MTLRenderPipelineDescriptor *cd in small) pso_build(cd, 0, &err);
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
            if (!pso_build(cd, 0, &err)) { fprintf(stderr, "cold: no pipeline %d\n", i); failed++; }
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
    id<MTLRenderPipelineState> l = pso_build(desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 0), 0, &err);
    EXPECT("unregistered function: a pipeline", 1, l != nil);
    EXPECT("unregistered function: MTLDevice build", legacy + 1, g_pb_legacy);
    depth_only();
    concurrent_bases(0);
    concurrent_bases(1);
    crashed_period();
    crashed_flush_race(0);
    crashed_flush_race(1);
    compiler_death_notifications();
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
        EXPECT("constants: a pipeline made from it", 1, pso_build(cd, 0, &se) != nil);
        EXPECT("constants: through the Metal 4 compiler", legacy0, g_pb_legacy);
        // a fragment function with no Metal 4 descriptor (as the DM_NVN_HILITE override) sends the pipeline to the MTLDevice: its vertex function is specialized for it
        cd.fragmentFunction = [cl newFunctionWithName:@"plain"]; se = nil;
        EXPECT("constants: an MTLDevice pipeline made from it", 1, pso_build(cd, 0, &se) != nil);
        EXPECT("constants: that pipeline went through the MTLDevice", legacy0 + 1, g_pb_legacy);
        EXPECT("constants: the vertex function given to it is specialized once", 1, fn_dev(cv) != cv && fn_dev(cv) == fn_dev(cv));
        EXPECT("constants: a function without constants is its own", 1, fn_dev(cd.fragmentFunction) == cd.fragmentFunction);
        // fn4_note(..., nil) must never publish or downgrade to an unspecialized descriptor for a function declaring constants, and concurrent fn_load never exposes an unspecialized window
        id<MTLLibrary> bl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nconstant bool dm_blend_off [[function_constant(217)]];\n"
            "fragment float4 bfs() { return is_function_constant_defined(dm_blend_off) && dm_blend_off ? float4(1) : float4(0); }\n" options:nil error:&err];
        id<MTLFunction> raw_bfs = [bl newFunctionWithName:@"bfs"];
        fn4_note(raw_bfs, bl, @"bfs", nil);
        EXPECT("constants: fn4_note(nil) refuses function with constants", 1, fn4_of(raw_bfs) == nil);
        id<MTLFunction> bf = fn_load(bl, @"bfs", &se);
        EXPECT("constants: fn_load specializes dm_blend_off function", 1, bf != nil && [fn4_of(bf) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]);
        fn4_note(raw_bfs, bl, @"bfs", nil);
        EXPECT("constants: fn4_note(nil) does not downgrade specialized descriptor", 1, [fn4_of(bf) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]);
        __block _Atomic int bad_race = 0;
        dispatch_group_t rg = dispatch_group_create();
        dispatch_queue_t rq = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        for (int t = 0; t < 4; t++) dispatch_group_async(rg, rq, ^{
            for (int i = 0; i < 2000; i++) {
                if (t == 0) {
                    if (![fn4_of(bf) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) atomic_fetch_add(&bad_race, 1);
                } else {
                    id<MTLFunction> rf = fn_load(bl, @"bfs", NULL);
                    if (!rf || ![fn4_of(rf) isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) atomic_fetch_add(&bad_race, 1);
                }
            }
        });
        dispatch_group_wait(rg, DISPATCH_TIME_FOREVER);
        EXPECT("constants: concurrent fn_load never exposes unspecialized descriptor", 0, atomic_load(&bad_race));
    }
    repeated_archive_miss();
    spec_desc_ownership();
    capture_preserves_foreign_folder();
    app_folder_claim();
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_archive: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
