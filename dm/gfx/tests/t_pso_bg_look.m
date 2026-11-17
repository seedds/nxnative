// The prediction's base lookups in the background (nvn_mtl.m pso_bg_look): with hybrid pipelines a proposed key whose base is not made gets no stand-in on the submit
// thread; a worker looks its base up (the key's own, else its pulling one), the draw then takes its stand-in from the base made, and a key with neither base has its build
// admitted by the worker. Real Metal 4 compiles of synthetic shaders, no archive, no game files.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } else printf("ok: %s\n", label); } while (0)

// Model an archive stalled behind a compiler operation. The test owns its
// release, so a synchronous prediction fails without hanging the test runner.
@interface BlockedArchive : NSObject {
@public dispatch_semaphore_t entered, release; _Atomic int calls;
} @end
@implementation BlockedArchive
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4PipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor; (void)error;
    if (!atomic_fetch_add(&calls, 1)) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
    return nil;
}
@end

// A program whose vertex shader has a pulling entry (pull: its own function, no stage inputs) or none
static NvProg *program(id<MTLLibrary> lib, const char *tag, int pull) {
    NvProg *p = [NvProg new];
    p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    snprintf(p->sh[0]->key, sizeof p->sh[0]->key, "%s-vs", tag); snprintf(p->sh[1]->key, sizeof p->sh[1]->key, "%s-fs", tag); p->sh[1]->stage = 1;
    p->sh[0]->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    p->sh[1]->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    p->sh[1]->rf.color_known = 1; p->sh[1]->rf.color_mask = 1;
    if (pull) { p->sh[0]->has_pull = 1; p->sh[0]->pfn = fn4_note([lib newFunctionWithName:@"vp"], lib, @"vp", nil); p->sh[0]->pfn_state = 1; }
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
static uint8_t g_cmd[1024];
static void one_draw(void) {
    memset(g_cmd, 0, sizeof g_cmd); p_draw draw = {.prim = NVN_DRAW_PRIMITIVE_TRIANGLES, .count = 3};
    unsigned size = (unsigned)((8 + sizeof draw + 7) & ~7u); op_hdr h = {OP_DRAW, size}, end = {OP_END, 8};
    memcpy(g_cmd, &h, 8); memcpy(g_cmd + 8, &draw, sizeof draw); memcpy(g_cmd + size, &end, 8);
}
static void predict(prog_t *pg) {
    state_reset(); S.prog = pg; memset(&g_pass, 0, sizeof g_pass); g_pass.cfmt[0] = 70; g_pass.samples = 1;
    one_draw(); g_exec_next = g_cmd; pso_predict_rest_(); g_exec_next = NULL;
}
static int wait_lookups(void) {   // the workers finish what was handed over (5 s at most)
    for (int i = 0; i < 500; i++) { if (g_bgl_done == g_bgl_n) return 1; usleep(10000); }
    return 0;
}

int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_NVN_PREDICT", "0", 1); setenv("DM_PSO_MTL4", "1", 1); unsetenv("DM_PSO_HYBRID");
    mtl_init();
    if (!hybrid_on()) { printf("no Metal 4 bases on this system: nothing to check\n"); return 0; }


    // 1. A key whose pulling base is made: the prediction makes no stand-in on the submit thread; the worker finds the pulling base, admits no build; the draw's stand-in
    //    is then a specialization of that base, with no compile
    {   NvProg *p = program(library(.25f), "pull", 1); static prog_t pg = {.serial = 990100}; serial_put(pg.serial, p);
        pso_key k; state_reset(); S.prog = &pg; pso_key_fill(&k, p->sh[0], p->sh[1], (uint32_t[8]){70}, 0, 0, 1, 0);
        NSError *e = nil; CHECK(pso_build(pso_desc_pull(&k, p->sh[0], p->sh[1]), !k.border, &e) != nil, "the pulling base builds (the prebuild's part)");
        uint64_t made = g_hy_made, n0 = g_bgl_n, found = g_bgl_found, admit = g_pp_admit, kicked = g_pp_kicked;
        predict(&pg);
        CHECK(hm_get_id(&g_pso_hy, &k) == nil && g_hy_made == made, "the prediction makes no stand-in on the submit thread");
        CHECK(g_bgl_n == n0 + 1, "it hands the key's base lookup to the background");
        CHECK(wait_lookups(), "the worker finishes");
        CHECK(g_bgl_found == found + 1 && g_pp_admit == admit && g_pp_kicked == kicked, "the worker finds the pulling base and admits no build");
        uint64_t built = g_pb_built;
        CHECK(hybrid_get(&k, p->sh[0], p->sh[1]) != nil && g_pb_built == built, "the draw takes its stand-in from the base made, with no compile");
        uint64_t n1 = g_bgl_n; predict(&pg);
        CHECK(g_bgl_n == n1, "a second prediction hands the same base over no more");
    }
    // 2. A key with no base made and no pulling base: the worker finds neither, so its draw would wait: it admits the key's build, which can be claimed
    {   NvProg *p = program(library(.5f), "cold", 1); static prog_t pg = {.serial = 990101}; serial_put(pg.serial, p);
        pso_key k; state_reset(); S.prog = &pg; pso_key_fill(&k, p->sh[0], p->sh[1], (uint32_t[8]){70}, 0, 0, 1, 0);
        uint64_t found = g_bgl_found, admit = g_pp_admit;
        predict(&pg);
        CHECK(hm_get_id(&g_pso_hy, &k) == nil, "no stand-in is made for it");
        CHECK(wait_lookups(), "the worker finishes");
        CHECK(g_bgl_found == found && g_pp_admit == admit + 1, "it finds no base and admits the key's build");
        CHECK(pso_pend_take(&k) != nil, "the admitted build can be claimed");
    }
    // 3. A key whose own base is made by the worker's lookup... of another key that shares it: the variant's base is the same, so it is not handed over again
    {   NvProg *p = program(library(.75f), "share", 0); static prog_t pg = {.serial = 990102}; serial_put(pg.serial, p);
        uint64_t n0 = g_bgl_n;
        predict(&pg);
        CHECK(g_bgl_n == n0 + 1, "a key with no pulling entry hands its base lookup over too");
        CHECK(wait_lookups(), "the worker finishes");
        state_reset(); S.prog = &pg; memset(&g_pass, 0, sizeof g_pass); g_pass.cfmt[0] = 70; g_pass.samples = 1; g_pass.dfmt = 252;   // the same draw, a depth attachment
        one_draw(); g_exec_next = g_cmd; uint64_t n1 = g_bgl_n; pso_predict_rest_(); g_exec_next = NULL;
        CHECK(g_bgl_n == n1, "a key that shares a base already handed over is not handed over again");
    }
    // 4. A slow archive lookup never holds the prediction's submit thread.
    {   NvProg *p = program(library(.875f), "blocked", 1); static prog_t pg = {.serial = 990103}; serial_put(pg.serial, p);
        BlockedArchive *archive = [BlockedArchive new]; archive->entered = dispatch_semaphore_create(0); archive->release = dispatch_semaphore_create(0);
        NSArray *saved = g_arch4p; g_arch4p = @[(id<MTL4Archive>)archive];
        dispatch_semaphore_t returned = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { predict(&pg); dispatch_semaphore_signal(returned); } });
        CHECK(!dispatch_semaphore_wait(archive->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "the archive lookup reaches the closed gate");
        long blocked = dispatch_semaphore_wait(returned, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2));
        CHECK(!blocked, "the prediction returns while the archive lookup is blocked");
        dispatch_semaphore_signal(archive->release);
        if (blocked) dispatch_semaphore_wait(returned, DISPATCH_TIME_FOREVER);
        CHECK(wait_lookups(), "the released lookup completes");
        g_arch4p = saved;
    }
    printf("background base lookups: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
