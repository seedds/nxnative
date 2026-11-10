// Pipelines that share a vertex function are built one at a time (DM_PSO_VS_SERIAL, vs_enter in nvn_mtl.m): Apple's G16X driver crashed in createVertexProgramVariant's completion block
// when several threads built pipelines of one vertex function at once. Checks: the stripe follows the library's name and the entry name (a specialized function too, one file loaded twice
// is one function); a build of the same function waits for the holder, one of another function does not, a lookup that must not compile never waits, a game thread waits 50 ms at most,
// a holder that does not return stalls its stripe once (builds go on unguarded) and the stripe guards again when it returns; threads building bases of one vertex function through pso_build
// are never inside one stripe together, and none is left holding it. `t_vs_serial off` (DM_PSO_VS_SERIAL=0): nothing is guarded.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t want_ = (uint64_t)(want), got_ = (uint64_t)(got); checks++; if (want_ != got_) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)want_, (unsigned long long)got_); failed++; } } while (0)

static MTL4RenderPipelineDescriptor *pipeline(id<MTLLibrary> lib, NSString *name, int specialized) {
    MTL4RenderPipelineDescriptor *d = [MTL4RenderPipelineDescriptor new];
    MTL4LibraryFunctionDescriptor *f = [MTL4LibraryFunctionDescriptor new]; f.library = lib; f.name = name;
    if (specialized) { MTL4SpecializedFunctionDescriptor *s = [MTL4SpecializedFunctionDescriptor new]; s.functionDescriptor = f; d.vertexFunctionDescriptor = s; }
    else d.vertexFunctionDescriptor = f;
    return d;
}
// Runs block on a queue of this QoS and returns its duration in ms
static double timed(qos_class_t qos, void (^block)(void)) {
    __block double ms = 0; dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(qos, 0), ^{ uint64_t t0 = bp_now(); block(); ms = (bp_now() - t0) / 1e6; dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER); return ms;
}
static id<MTLFunction> test_fn(id<MTLLibrary> lib, NSString *name) { id<MTLFunction> f = [lib newFunctionWithName:name]; return fn4_note(f, lib, name, nil); }

int main(int argc, char **argv) { @autoreleasepool {
    int off = argc > 1 && !strcmp(argv[1], "off");
    if (off) setenv("DM_PSO_VS_SERIAL", "0", 1);
    mtl_init(); NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nstruct I { float4 a [[attribute(0)]]; };\n"
        "vertex V vs(I in [[stage_in]], uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, in.a.w); return o; }\n"
        "vertex V vs2(I in [[stage_in]], uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, in.a.w + 1); return o; }\n"
        "struct O { float4 c0 [[color(0)]]; };\nfragment O fs() { return { float4(0.5, 0.25, 1.0, 0.5) }; }\n";
    id<MTLLibrary> a = [g_dev newLibraryWithSource:src options:nil error:&err];
    id<MTLLibrary> a2 = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// the same file loaded again\n"] options:nil error:&err];
    id<MTLLibrary> b = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// another file\n"] options:nil error:&err];
    if (!a || !a2 || !b) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    lib_tag(a, @"a.metallib"); lib_tag(a2, @"a.metallib"); lib_tag(b, @"b.metallib");

    int sa = vs_stripe_of(pipeline(a, @"vs", 0));
    EXPECT("a stripe for a library function", 1, sa >= 0 && sa < VS_STRIPES);
    EXPECT("the same function has the same stripe", sa, vs_stripe_of(pipeline(a, @"vs", 0)));
    EXPECT("one file loaded twice is one function", sa, vs_stripe_of(pipeline(a2, @"vs", 0)));
    EXPECT("a specialized function takes the stripe of the function it specializes", sa, vs_stripe_of(pipeline(a, @"vs", 1)));
    EXPECT("another entry has another stripe", 1, vs_stripe_of(pipeline(a, @"vs2", 0)) != sa);
    EXPECT("another file has another stripe", 1, vs_stripe_of(pipeline(b, @"vs", 0)) != sa);
    EXPECT("no vertex function, no stripe", -1 & 0xff, vs_stripe_of([MTL4RenderPipelineDescriptor new]) & 0xff);
    EXPECT("a pipeline of another kind has no stripe", -1 & 0xff, vs_stripe_of([MTL4ComputePipelineDescriptor new]) & 0xff);

    if (off) {   // nothing is guarded
        int go; vs_guard_t g = vs_enter(pipeline(a, @"vs", 0), 1, &go);
        EXPECT("off: no guard taken", 1, g.idx < 0 && !g.held && go);
        EXPECT("off: nothing counted", 0, atomic_load(&g_vs_guarded));
        printf("t_vs_serial off: %d checks, %d failed\n", checks, failed); return failed != 0;
    }

    int go; MTL4RenderPipelineDescriptor *da = pipeline(a, @"vs", 0);
    vs_guard_t hold = vs_enter(da, 1, &go);
    EXPECT("a free stripe is taken", 1, hold.held && hold.idx == sa && go);
    // another function is not held back
    vs_guard_t other = vs_enter(pipeline(a, @"vs2", 0), 1, &go);
    EXPECT("another vertex function builds in parallel", 1, other.held && go); vs_leave(other);
    // a lookup that must not compile never waits
    __block int skipped_go = 1; __block vs_guard_t skipped;
    double ms = timed(QOS_CLASS_UTILITY, ^{ skipped = vs_enter(da, 0, &skipped_go); });
    EXPECT("a lookup that must not compile is told to leave it", 0, skipped_go);
    EXPECT("and does not wait", 1, ms < 40);
    EXPECT("and holds nothing", 1, !skipped.held && skipped.idx < 0);
    // a game thread waits 50 ms at most, then goes on unguarded
    __block vs_guard_t gt; ms = timed(QOS_CLASS_USER_INTERACTIVE, ^{ int g_; gt = vs_enter(da, 1, &g_); });
    EXPECT("a game thread goes on unguarded", 1, !gt.held && gt.idx < 0);
    EXPECT("after about 50 ms", 1, ms >= 45 && ms < 400);
    EXPECT("the stripe is not stalled by a game thread's patience", 0, atomic_load(&g_vs_stalled[sa]));
    // a worker waits for the holder
    __block vs_guard_t w; dispatch_semaphore_t entered = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);
    uint64_t t0 = bp_now();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ int g_; w = vs_enter(da, 1, &g_); dispatch_semaphore_signal(entered); dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER); vs_leave(w); });
    usleep(150000);
    EXPECT("a worker waits for the holder", 0, dispatch_semaphore_wait(entered, DISPATCH_TIME_NOW) == 0);
    vs_leave(hold);
    EXPECT("and gets the stripe when it returns", 1, dispatch_semaphore_wait(entered, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0 && w.held);
    EXPECT("after the holder's time", 1, (bp_now() - t0) / 1e6 >= 140);
    dispatch_semaphore_signal(done); usleep(50000);
    // a holder that does not return stalls the stripe once
    g_vs_wait_ns = 100ull * 1000000;
    hold = vs_enter(da, 1, &go);
    __block vs_guard_t s1, s2; double ms1 = timed(QOS_CLASS_UTILITY, ^{ int g_; s1 = vs_enter(da, 1, &g_); }); double ms2 = timed(QOS_CLASS_UTILITY, ^{ int g_; s2 = vs_enter(da, 1, &g_); });
    EXPECT("a worker waits its time for a holder that does not return", 1, ms1 >= 95 && ms1 < 1000 && !s1.held);
    EXPECT("the stripe is marked stalled once", 1, atomic_load(&g_vs_stalls) == 1 && atomic_load(&g_vs_stalled[sa]));
    EXPECT("the next build goes on at once", 1, ms2 < 40 && !s2.held);
    vs_leave(hold);
    EXPECT("the stripe guards again when the holder returns", 0, atomic_load(&g_vs_stalled[sa]));
    hold = vs_enter(da, 1, &go);
    EXPECT("and is taken again", 1, hold.held); vs_leave(hold);
    g_vs_wait_ns = 20ull * 1000000000ull;

    // through pso_build: bases of one vertex function (they differ in their vertex layout) built by several threads at once
    pso4_init();
    EXPECT("a Metal 4 compiler", 1, g_comp4 != nil);
    if (g_comp4) {
        id<MTLFunction> vf = test_fn(a, @"vs"), ff = test_fn(a, @"fs");
        enum { THREADS = 8, EACH = 5 };
        __block _Atomic int ok = 0; dispatch_group_t grp = dispatch_group_create(); uint64_t guarded0 = atomic_load(&g_vs_guarded);
        for (int t = 0; t < THREADS; t++) dispatch_group_async(grp, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            for (int i = 0; i < EACH; i++) @autoreleasepool {
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = vf; d.fragmentFunction = ff;
                MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
                vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 16; vd.layouts[16].stride = 16 * (1 + t * EACH + i); vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[16].stepRate = 0;
                d.vertexDescriptor = vd; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                NSError *e = nil; if (pso_build(d, &e)) atomic_fetch_add(&ok, 1);
            } });
        EXPECT("the builds finish", 0, dispatch_group_wait(grp, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC)));
        EXPECT("every pipeline is built", THREADS * EACH, atomic_load(&ok));
        EXPECT("they were guarded", THREADS * EACH, atomic_load(&g_vs_guarded) - guarded0);
        EXPECT("never two inside one stripe", 1, atomic_load(&g_vs_max_in));
        int held = 0; for (int i = 0; i < VS_STRIPES; i++) held += atomic_load(&g_vs_in[i]);
        EXPECT("no stripe is left held", 0, held);
        EXPECT("no build is left pending", 0, g_pbase_pending.n);
        // a lookup that must not compile, while the function is being built elsewhere: it leaves the base to a caller that can compile
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = vf; d.fragmentFunction = ff;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 16; vd.layouts[16].stride = 16 * 999; vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[16].stepRate = 0;
        d.vertexDescriptor = vd; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        vs_guard_t busy = vs_enter(da, 1, &go); uint64_t skipped0 = atomic_load(&g_vs_skipped); NSError *e = nil;
        EXPECT("a lookup that must not compile returns nothing while the function is built", 1, pso_build_ex(d, &e, 0) == nil);
        EXPECT("it was skipped, not searched", 1, atomic_load(&g_vs_skipped) == skipped0 + 1);
        EXPECT("it left no build pending", 0, g_pbase_pending.n);
        vs_leave(busy);
        EXPECT("the same demand then builds", 1, pso_build_ex(d, &e, 1) != nil);
    }
    printf("t_vs_serial: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
