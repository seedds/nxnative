// A compile 0 call of pso_build_ex (nvn_mtl.m) for a base another thread holds: it waits for that thread's archive lookup, which takes milliseconds, and takes the base
// it found; it never waits for that thread's compile. Before, it returned nil at once, so a draw that came while a background worker looked up its pulling base found no
// stand-in and compiled its own base on the submit thread instead (Ocarina of Time on an iPhone 17, first visits: one draw waited 0.3-1.8 s).
// The other thread is played here by hand: a job planted in g_pbase_pending, finished from a dispatch_after block. Real Metal 4 compiles of synthetic shaders, no archive.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

static NvProg *program(float z) {
    NSError *error = nil;
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n],%f,1); }\n"
        "fragment float4 f() { return float4(1,0,0,1); }\n", z];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); exit(1); }
    NvProg *p = [NvProg new];
    p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    snprintf(p->sh[0]->key, sizeof p->sh[0]->key, "lw%.2f-vs", z); snprintf(p->sh[1]->key, sizeof p->sh[1]->key, "lw%.2f-fs", z); p->sh[1]->stage = 1;
    p->sh[0]->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    p->sh[1]->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    p->sh[1]->rf.color_known = 1; p->sh[1]->rf.color_mask = 1;
    return p;
}
// The descriptor of p's pipeline for one colour attachment, and its base's key
static MTLRenderPipelineDescriptor *desc(NvProg *p, pso_key *k, pbase_key *bk) {
    state_reset(); pso_key_fill(k, p->sh[0], p->sh[1], (uint32_t[8]){70}, 0, 0, 1, 0);
    MTLRenderPipelineDescriptor *d = pso_desc(k, p->sh[0], p->sh[1], nil, 0);
    pbase_key_of(d, !k->border, bk);
    return d;
}
// A job for bk as another thread's would be while it looks in the archive
static PbaseBuild *plant(const pbase_key *bk) {
    PbaseBuild *job = [PbaseBuild new]; job->done = dispatch_group_create(); job->looked = dispatch_group_create();
    dispatch_group_enter(job->done); dispatch_group_enter(job->looked);
    os_unfair_lock_lock(&g_pbase_lock); hm_put_id(&g_pbase_pending, bk, job); os_unfair_lock_unlock(&g_pbase_lock);
    return job;
}
static void finish(PbaseBuild *job, const pbase_key *bk, id<MTLRenderPipelineState> base) {   // what the owner does at the end
    os_unfair_lock_lock(&g_pbase_lock);
    job->ps = base; if (base) hm_put_id(&g_pbase, bk, base);
    hm_rel(nvn_hm_del(&g_pbase_pending, bk)); dispatch_group_leave(job->done);
    os_unfair_lock_unlock(&g_pbase_lock);
}

int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_NVN_PREDICT", "0", 1); setenv("DM_PSO_MTL4", "1", 1);
    mtl_init(); pso4_init();
    if (!g_comp4) { printf("no Metal 4 bases on this system: nothing to check\n"); return 0; }
    dispatch_queue_t other = dispatch_queue_create("t.owner", DISPATCH_QUEUE_SERIAL);

    // 1. The other thread's lookup finds the base after 200 ms: the compile 0 call waits for it and specializes it, with no compile of its own
    {   NvProg *p = program(.25f); pso_key k; pbase_key bk; MTLRenderPipelineDescriptor *d = desc(p, &k, &bk);
        NSError *e = nil; id<MTLRenderPipelineState> base = nil;
        { MTL4RenderPipelineDescriptor *b = pso_desc4(d, 1, !k.border); base = [g_comp4 newRenderPipelineStateWithDescriptor:b compilerTaskOptions:nil error:&e]; }
        CHECK(base != nil, "the base builds");
        PbaseBuild *job = plant(&bk);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), other, ^{ job->found = base; dispatch_group_leave(job->looked); finish(job, &bk, base); });
        uint64_t built = g_pb_built, t0 = bp_now();
        id<MTLRenderPipelineState> ps = pso_build_ex(d, !k.border, &e, 0);
        double ms = (bp_now() - t0) / 1e6;
        CHECK(ps != nil, "a compile 0 call for a base being looked up takes the base the lookup found");
        CHECK(ms >= 150, "it waited for the lookup");
        CHECK(g_pb_built == built, "it compiled nothing");
        dispatch_sync(other, ^{});
    }
    // 2. The other thread's lookup finds nothing after 100 ms and then compiles for 2 s: the compile 0 call returns nil after the lookup, not after the compile
    {   NvProg *p = program(.5f); pso_key k; pbase_key bk; MTLRenderPipelineDescriptor *d = desc(p, &k, &bk);
        PbaseBuild *job = plant(&bk);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), other, ^{ dispatch_group_leave(job->looked); usleep(2000000); finish(job, &bk, nil); });
        NSError *e = nil; uint64_t t0 = bp_now();
        id<MTLRenderPipelineState> ps = pso_build_ex(d, !k.border, &e, 0);
        double ms = (bp_now() - t0) / 1e6;
        CHECK(ps == nil, "a compile 0 call whose base the lookup did not find returns nil");
        CHECK(ms < 1000, "it does not wait for the other thread's compile");
        dispatch_sync(other, ^{});
    }
    // 3. A compile 0 owner (no other thread): an archive miss still returns nil and marks the base missing; a later compile 0 call returns nil at once
    {   NvProg *p = program(.75f); pso_key k; pbase_key bk; MTLRenderPipelineDescriptor *d = desc(p, &k, &bk);
        NSError *e = nil;
        CHECK(pso_build_ex(d, !k.border, &e, 0) == nil, "a compile 0 owner with no archive finds nothing");
        os_unfair_lock_lock(&g_pbase_lock); int miss = hm_get_id(&g_pbase_miss, &bk) != nil, pend = hm_get_id(&g_pbase_pending, &bk) != nil; os_unfair_lock_unlock(&g_pbase_lock);
        CHECK(miss && !pend, "the base is marked missing and its job is gone");
        CHECK(pso_build_ex(d, !k.border, &e, 1) != nil, "a compiling call then builds it");
    }
    printf("lookup waits: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
