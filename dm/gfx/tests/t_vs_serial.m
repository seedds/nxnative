// The AGX vertex-variant guard (vs_stripe_of, DM_PSO_VS_SERIAL): pipelines whose vertex function is the same library function (by the library's name, lib_tag,
// and the entry name) share one lock stripe, through a specialized function descriptor too; a library with another file name, or the same entry name in another file, does
// not; a build that compiles waits for the stripe and counts the wait; a draw's lookup without compiling (pso_build_ex compile 0) never waits: it returns no pipeline,
// counts the lookup it left, and leaves no build pending, so a later lookup still looks. With the stripe free the pipeline builds. A holder that does not return (issue #294)
// holds a build until it has held the stripe for g_vs_wait_max_ms: the stripe is then stalled, and later builds and lookups go on without waiting. A game thread
// (user-interactive QoS) waits g_vs_wait_draw_ms at most and does not mark the stripe. No archive (DM_PSO_ARCHIVE=off)
#include "../nvn/nvn_mtl.m"
#include <stdlib.h>
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static NSString *const src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
    "vertex V main0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n"
    "fragment float4 fs() { return float4(1); }\nfragment float4 fs2() { return float4(0.5); }\nfragment float4 fs3() { return float4(0.25); }\n";
static id<MTLLibrary> lib(NSString *file, NSString *extra) {
    NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:[src stringByAppendingString:extra] options:nil error:&e];
    if (!l) { fprintf(stderr, "library: %s\n", e.localizedDescription.UTF8String); exit(1); }
    return lib_tag(l, file);
}
static MTL4RenderPipelineDescriptor *desc4(id<MTLLibrary> l, NSString *name, int specialized) {
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = l; ld.name = name;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    if (specialized) { MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = [MTLFunctionConstantValues new]; m.vertexFunctionDescriptor = sd; }
    else m.vertexFunctionDescriptor = ld;
    return m;
}
static MTLRenderPipelineDescriptor *desc(id<MTLLibrary> l, NSString *fs) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = fn4_note([l newFunctionWithName:@"main0"], l, @"main0", nil); d.fragmentFunction = fn4_note([l newFunctionWithName:fs], l, fs, nil);
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    return d;
}
int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE", "off", 1);
    mtl_init();
    if (!pso4_compiler()) { fprintf(stderr, "t_vs_serial: no Metal 4 compiler, skipped\n"); return 0; }
    id<MTLLibrary> a = lib(@"aaaa_b.metallib", @""), a2 = lib(@"aaaa_b.metallib", @"// loaded again\n"), b = lib(@"bbbb_b.metallib", @"// another shader\n");
    int la = vs_stripe_of(desc4(a, @"main0", 0));
    EXPECT("a stripe", 1, la >= 0);
    EXPECT("the same file loaded again: the same stripe", 1, la == vs_stripe_of(desc4(a2, @"main0", 0)));
    EXPECT("through a specialized descriptor: the same stripe", 1, la == vs_stripe_of(desc4(a, @"main0", 1)));
    EXPECT("main0 of another file: another stripe", 1, la != vs_stripe_of(desc4(b, @"main0", 0)));
    EXPECT("no vertex function: no stripe", 1, vs_stripe_of([MTL4RenderPipelineDescriptor new]) < 0);

    // Hold the stripe of a's vertex function, as a build in flight would
    MTLRenderPipelineDescriptor *d = desc(a, @"fs"), *d2 = desc(a, @"fs2");
    int l = vs_stripe_of(pso_desc4(d, 1));
    EXPECT("the pipeline's stripe is a's", 1, l == la);
    if (l < 0 || l >= VS_STRIPES) { fprintf(stderr, "t_vs_serial: no stripe, %d checks, %d failed\n", checks, failed); return 1; }
    EXPECT("held", 1, vs_lock(l));
    uint64_t busy0 = g_vs_busy, wait0 = g_vs_wait_n;
    __block id<MTLRenderPipelineState> r0 = (id)NSNull.null;
    dispatch_semaphore_t s0 = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; r0 = pso_build_ex(d, &e, 0); dispatch_semaphore_signal(s0); } });
    EXPECT("compile 0: does not wait for the stripe", 0, dispatch_semaphore_wait(s0, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    EXPECT("compile 0: no pipeline", 1, r0 == nil);
    EXPECT("compile 0: the lookup left counted", 1, g_vs_busy - busy0);
    pbase_key bk; pbase_key_of(d, &bk);
    os_unfair_lock_lock(&g_pbase_lock); id pend = hm_get_id(&g_pbase_pending, &bk); os_unfair_lock_unlock(&g_pbase_lock);
    EXPECT("compile 0: no build left pending", 1, pend == nil);

    __block id<MTLRenderPipelineState> r1 = nil, r2 = nil;
    dispatch_semaphore_t s1 = dispatch_semaphore_create(0), s2 = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; r1 = pso_build_ex(d, &e, 1); dispatch_semaphore_signal(s1); } });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; r2 = pso_build_ex(d2, &e, 1); dispatch_semaphore_signal(s2); } });
    EXPECT("compile 1: waits for the stripe", 1, dispatch_semaphore_wait(s1, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) != 0);
    EXPECT("compile 1, another fragment function of the same vertex function: waits too", 1, dispatch_semaphore_wait(s2, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0);
    // A pipeline of another vertex function does not wait (unless its file hashes to the same stripe, which b does not: checked above)
    MTLRenderPipelineDescriptor *db = desc(b, @"fs");
    __block id<MTLRenderPipelineState> rb = nil; dispatch_semaphore_t sb = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; rb = pso_build_ex(db, &e, 1); dispatch_semaphore_signal(sb); } });
    EXPECT("another vertex function: built while the stripe is held", 0, dispatch_semaphore_wait(sb, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("another vertex function: a pipeline", 1, rb != nil);
    vs_unlock(l, 1);
    EXPECT("compile 1: built once the stripe is free", 0, dispatch_semaphore_wait(s1, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("compile 1, the other: built", 0, dispatch_semaphore_wait(s2, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("compile 1: a pipeline", 1, r1 != nil && r2 != nil);
    EXPECT("compile 1: the waits counted", 1, g_vs_wait_n - wait0 >= 2);
    NSError *e = nil;
    EXPECT("compile 0 after the build: the base is there", 1, pso_build_ex(d, &e, 0) != nil);
    // A whole pipeline (PSO_WHOLE: no base, compiled through the Metal 4 compiler without the archive) of the same vertex function waits for the stripe too: the prewarm's
    // whole builds (pso_build_ex+0x238 from warm_task) are where iss_uf and sc_fs2 crashed, while other workers took bases of shared vertex functions from the archive
    MTLRenderPipelineDescriptor *dw = desc(a, @"fs3"); dw.label = PSO_WHOLE;
    EXPECT("whole: the same stripe as the bases", 1, vs_stripe_of(pso_desc4(dw, 0)) == l);
    EXPECT("held for the whole pipeline", 1, vs_lock(l));
    EXPECT("whole, compile 0: no pipeline, no wait", 1, pso_build_ex(dw, &e, 0) == nil);
    uint64_t wait1 = g_vs_wait_n;
    __block id<MTLRenderPipelineState> rwh = nil; dispatch_semaphore_t swh = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; rwh = pso_build_ex(dw, &e, 1); dispatch_semaphore_signal(swh); } });
    EXPECT("whole, compile 1: waits for the stripe", 1, dispatch_semaphore_wait(swh, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) != 0);
    vs_unlock(l, 1);
    EXPECT("whole: built once the stripe is free", 0, dispatch_semaphore_wait(swh, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("whole: a pipeline", 1, rwh != nil);
    EXPECT("whole: the wait counted", 1, g_vs_wait_n - wait1 >= 1);
    char line[2048]; mtl_pso_line(line, sizeof line);
    EXPECT("the pipeline line names the waits", 1, strstr(line, "pipelines that waited for one of the same vertex function") != NULL);
    // A game thread (user-interactive QoS) waits g_vs_wait_draw_ms at most, builds unguarded, and leaves the stripe unmarked
    g_vs_wait_max_ms = 400; g_vs_wait_draw_ms = 50;
    MTLRenderPipelineDescriptor *s1d = desc(a2, @"fs"), *s2d = desc(a2, @"fs2"), *s3d = desc(a, @"fs3");   // a's stripe, bases not built yet (other function objects)
    EXPECT("held again", 1, vs_lock(l));
    uint64_t sh0 = g_vs_short_n, st0 = g_vs_stall_n;
    __block uint64_t ug = 0; __block id<MTLRenderPipelineState> ru = nil; dispatch_semaphore_t su = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ @autoreleasepool { NSError *e = nil; uint64_t t = bp_now(); ru = pso_build_ex(s3d, &e, 1); ug = bp_now() - t; dispatch_semaphore_signal(su); } });
    EXPECT("game thread: built while the stripe is held", 0, dispatch_semaphore_wait(su, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("game thread: a pipeline", 1, ru != nil);
    EXPECT("game thread: waited about g_vs_wait_draw_ms, not g_vs_wait_max_ms", 1, ug >= 40 * 1000000ull && ug < 350 * 1000000ull);
    EXPECT("game thread: counted as cut short", 1, g_vs_short_n - sh0);
    EXPECT("game thread: the stripe not stalled", 1, !g_vs_stalled[l] && g_vs_stall_n == st0);
    // A holder that does not return: a worker waits until the holder has held the stripe g_vs_wait_max_ms, then the stripe is stalled and the next ones do not wait
    __block uint64_t wg = 0; __block id<MTLRenderPipelineState> rw = nil; dispatch_semaphore_t sw = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; uint64_t t = bp_now(); rw = pso_build_ex(s1d, &e, 1); wg = bp_now() - t; dispatch_semaphore_signal(sw); } });
    EXPECT("stall: the worker gives up waiting and builds", 0, dispatch_semaphore_wait(sw, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)));
    EXPECT("stall: a pipeline", 1, rw != nil);
    EXPECT("stall: it waited for the holder's g_vs_wait_max_ms", 1, wg >= 150 * 1000000ull);
    EXPECT("stall: counted once", 1, g_vs_stall_n - st0);
    __block uint64_t ng = 0; dispatch_semaphore_t sn = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; uint64_t t = bp_now(); id ps = pso_build_ex(s2d, &e, 1); ng = ps ? bp_now() - t : ~0ull; dispatch_semaphore_signal(sn); } });
    dispatch_semaphore_wait(sn, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
    EXPECT("stall: the next build does not wait for the stripe", 1, ng != ~0ull && g_vs_stall_n - st0 == 1);
    int own = 0;
    EXPECT("stall: a lookup goes on", 1, vs_try(l, &own) && !own);
    vs_unlock(l, 1);
    EXPECT("released: the stripe is taken again, not stalled", 1, vs_lock(l) && !g_vs_stalled[l]);
    vs_unlock(l, 1);
    // Queued behind short builds, not a stall: two workers hold it 150 ms each in turn (g_vs_wait_max_ms 400): a third waits ~300 ms and takes it
    uint64_t st1 = g_vs_stall_n; dispatch_group_t q = dispatch_group_create();
    for (int k = 0; k < 2; k++) dispatch_group_async(q, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ int o = vs_lock(l); usleep(150000); vs_unlock(l, o); });
    usleep(20000);
    __block int got = -1; dispatch_group_async(q, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ usleep(10000); got = vs_lock(l); vs_unlock(l, got); });
    dispatch_group_wait(q, DISPATCH_TIME_FOREVER);
    EXPECT("queued: took the stripe, no stall", 1, got == 1 && g_vs_stall_n == st1);
    fprintf(stderr, "t_vs_serial: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
