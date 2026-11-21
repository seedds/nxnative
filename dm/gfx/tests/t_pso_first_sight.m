// A draw's first look for a pipeline it has never drawn with (pso_get in nvn_mtl.m, hybrid pipelines on, DM_PSO_LOOKUP_BG=1): the replay thread takes a base only if it is made, never searches the
// archives or compiles for the first look, and skips the draw (pso_get returns nil) until the background has a pipeline or a stand-in, for at most PSO_SKIP_NS; after that it builds the
// pipeline itself as before. `--inline` is the default, DM_PSO_LOOKUP_BG unset: the first look gets a pipeline at once. Real Metal 4 compiles of synthetic shaders, no archive.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

static NvProg *program(id<MTLLibrary> lib, const char *tag) {   // a program whose vertex shader has a pulling entry
    NvProg *p = [NvProg new];
    p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    snprintf(p->sh[0]->key, sizeof p->sh[0]->key, "%s-vs", tag); snprintf(p->sh[1]->key, sizeof p->sh[1]->key, "%s-fs", tag); p->sh[1]->stage = 1;
    p->sh[0]->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    p->sh[1]->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    p->sh[1]->rf.color_known = 1; p->sh[1]->rf.color_mask = 1;
    p->sh[0]->has_pull = 1; p->sh[0]->pfn = fn4_note([lib newFunctionWithName:@"vp"], lib, @"vp", nil); p->sh[0]->pfn_state = 1;
    return p;
}
static id<MTLLibrary> library(float z) {
    NSError *error = nil;
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n],%f,1); }\n"
        "vertex float4 vp(uint n[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n].yx,%f,1); }\n"
        "fragment float4 f() { return float4(1,0,0,1); }\n", z, z];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); exit(1); }
    return lib;
}
static pso_key key_of(NvProg *p) { pso_key k; state_reset(); memset(&g_pass, 0, sizeof g_pass); g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; g_pass.samples = 1; S.chmask.mask = 15; pso_key_fill(&k, p->sh[0], p->sh[1], g_pass.cfmt, 0, 0, 1, 0); return k; }
static int pending(const pso_key *key) {   // queued in the build table and not started
    pthread_mutex_lock(&g_pp_mu); int i = pso_pend_find(key); int yes = i >= 0 && !g_pp[i].done && !g_pp[i].run && g_pp[i].d != nil; pthread_mutex_unlock(&g_pp_mu); return yes;
}

int main(int argc, char **argv) { @autoreleasepool {
    alarm(90);
    int inl = argc == 2 && !strcmp(argv[1], "--inline");
    if (argc > 2 || (argc == 2 && !inl)) return 2;
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_NVN_PREDICT", "0", 1); setenv("DM_PSO_MTL4", "1", 1); setenv("DM_PSO_HYBRID", "1", 1);
    if (inl) unsetenv("DM_PSO_LOOKUP_BG"); else setenv("DM_PSO_LOOKUP_BG", "1", 1);
    mtl_init(); pso4_init();
    if (!g_comp4) { printf("no Metal 4 bases on this system: nothing to check\n"); return 0; }
    CHECK(hybrid_on(), "hybrid pipelines are on");
    // The background builds stay queued until the test lets them run, so no draw's pipeline arrives meanwhile
    dispatch_queue_t queues[8]; int nq = 0;
    for (int i = 0; i < 9; i++) { dispatch_queue_t q = pp_bg_queue(); int seen = 0; for (int j = 0; j < nq; j++) seen |= q == queues[j]; if (seen) break; if (nq == 8) return 2; queues[nq++] = q; dispatch_suspend(q); }

    NvProg *p = program(library(.5f), "fs1"); pso_key k = key_of(p);
    uint64_t built = g_pb_built, comp = g_pt_comp_ns, skipped = atomic_load(&g_hy_skipped);
    id<MTLRenderPipelineState> a = pso_get(p, nil);
    if (inl) {
        CHECK(a != nil, "default: the first look gets its pipeline at once");
        CHECK(atomic_load(&g_hy_skipped) == skipped, "default: nothing is skipped");
    } else {
        CHECK(a == nil, "the first look of a pipeline with no base made is skipped");
        CHECK(g_pb_built == built && g_pt_comp_ns == comp, "no base is compiled on the calling thread");
        CHECK(atomic_load(&g_hy_skipped) == skipped + 1, "the skip is counted");
        CHECK(pending(&k), "the pipeline's own build is queued in the background");
        CHECK(pso_get(p, nil) == nil && atomic_load(&g_hy_skipped) == skipped + 2, "the next draw of it is skipped as well");
        usleep((PSO_SKIP_NS / 1000) + 50000);
        id<MTLRenderPipelineState> b = pso_get(p, nil);
        CHECK(b != nil, "after the grace the draw builds its pipeline itself");
        CHECK(atomic_load(&g_hy_skip_giveup) == 1, "and the give-up is counted");
        CHECK(pso_get(p, nil) == b, "the pipeline is kept");
    }
    for (int i = 0; i < nq; i++) dispatch_resume(queues[i]);
    for (int i = 0; i < nq; i++) dispatch_sync(queues[i], ^{});

    // With the background running: skipped once, then the pipeline (or its stand-in) comes without the draw having waited for a compile
    NvProg *p2 = program(library(.75f), "fs2"); pso_key k2 = key_of(p2); (void)k2;
    built = g_pb_built; uint64_t giveup = atomic_load(&g_hy_skip_giveup);
    id<MTLRenderPipelineState> c = pso_get(p2, nil);
    if (!inl) CHECK(c == nil, "a second never-seen pipeline is skipped at its first look");
    for (int i = 0; i < nq; i++) dispatch_sync(queues[i], ^{});
    uint64_t t0 = bp_now(); id<MTLRenderPipelineState> d = nil;
    while (!d && (bp_now() - t0) / 1e9 < 30) { d = pso_get(p2, nil); if (!d) usleep(10000); }
    CHECK(d != nil, "the background's pipeline or stand-in arrives");
    CHECK(atomic_load(&g_hy_skip_giveup) == giveup, "within the grace, so no draw built it itself");
    printf("t_pso_first_sight%s: %d checks, %d failed\n", inl ? " --inline" : "", checks, failed);
    return failed != 0;
} }
