// DM_NVN_CUT_FIRST (cut_first_check in nvn_mtl.m): at the start of a submission, while the GPU has nothing of ours to run, segments are cut inside a pass every DM_NVN_CUT_FIRST_DRAWS draws.
// Checks: no cut before the draw count; none while the previous submission still runs, and none later in that submission; on an idle GPU the pass ends and a command buffer is committed, and
// the reopened pass loads what the first part drew (a cleared green target stays green); a second cut only once the first segment has run (none while it is in flight); at most
// DM_NVN_CUT_FIRST_MAX cuts a submission; none after a cut at a pass boundary (sub_cut); a pass whose attachments are larger than DM_NVN_CUT_FIRST_MB is not cut, counted once.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> target(tex_t *t, uint64_t serial, uint32_t size) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=size, .height=size, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
static void open_cleared(tex_t *t, MTLClearColor c) {   // a submission whose first pass clears t: the pass stays open, as while its draws are encoded
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = t;
    R.clear_c[0] = 1; R.cc[0] = c;
    CHECK(rp_begin() != nil, "first pass opens with its clear");
}
static void finish(void) { if (g_enc) rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed"); }
static uint64_t cuts(void) { uint64_t c, b, u; mtl_cut_first_stats(&c, &b, &u); return c; }
static void draws(int n) { g_stat_draw += (uint64_t)n; cut_first_check(); }   // n draws encoded, then the check a next draw makes

int main(void) { @autoreleasepool {
    setenv("DM_NVN_CUT_FIRST", "1", 1); setenv("DM_NVN_CUT_FIRST_DRAWS", "48", 1); setenv("DM_NVN_CUT_FIRST_MAX", "3", 1);
    setenv("DM_NVN_CUT_FIRST_MB", "0.001", 1);   // 1,048 bytes: an 8x8 RGBA8 target (256) is cut, a 32x32 one (4,096) is not
    mtl_init();
    tex_t small, big; id<MTLTexture> ts = target(&small, 99900, 8); target(&big, 99901, 32);
    uint64_t c, nbig, busy;
    open_cleared(&small, MTLClearColorMake(0, 0, 0, 1)); finish();   // one submission first, so there is a fence to be short of
    uint64_t c0 = cuts();

    // Too few draws, then the previous submission still running: no cut, and none later in this submission
    open_cleared(&small, MTLClearColorMake(1, 0, 0, 1));
    id cb0 = g_cb;
    draws(10);
    CHECK(g_enc != nil && g_cb == cb0, "10 draws: the pass stays open, nothing committed");
    uint64_t done0 = atomic_load(&g_done); atomic_store(&g_done, g_fence - 1);   // the previous submission not finished
    draws(48);
    CHECK(g_enc != nil && g_cb == cb0, "previous submission running: no cut");
    atomic_store(&g_done, done0);
    draws(48);
    CHECK(g_enc != nil && g_cb == cb0, "idle later in the same submission: still no cut (it started busy)");
    mtl_cut_first_stats(&c, &nbig, &busy);
    CHECK(c == c0 && busy == 1, "counted as busy, not cut");
    finish();

    // Idle at the start: cut, the draw reopens the pass loading the target; more cuts only while the GPU runs dry, up to the maximum
    open_cleared(&small, MTLClearColorMake(0, 1, 0, 1));
    cb0 = g_cb; uint64_t seg0 = g_seg, passes0 = g_stat_pass;
    atomic_fetch_add(&g_seg_inflight, 1);   // the previous submission's feedback handler not run yet although its fence is done (g_done): still idle
    draws(48);
    atomic_fetch_sub(&g_seg_inflight, 1);
    CHECK(g_enc == nil, "idle GPU, 48 draws: the pass ends");
    CHECK(g_cb != cb0 && g_seg == seg0 + 1 && cuts() == c0 + 1, "and the segment is committed");
    CHECK(g_cut_reopen == 1, "the reopening draw is marked: the reorders of the pass's first draw are not tried again");
    CHECK(rp_begin() != nil && g_stat_pass == passes0 + 1, "the next draw reopens the pass");
    CHECK(g_cut_reopen == 0, "the reopened pass clears the mark");
    atomic_fetch_add(&g_seg_inflight, 1);   // that segment still running
    draws(48);
    CHECK(g_enc != nil && cuts() == c0 + 1, "the first segment in flight: no second cut");
    atomic_fetch_sub(&g_seg_inflight, 1);
    while (atomic_load(&g_seg_inflight)) usleep(1000);   // the first segment's feedback
    draws(1);
    CHECK(g_enc == nil && cuts() == c0 + 2, "it ran out: a second cut");
    CHECK(rp_begin() != nil, "reopened again");
    while (atomic_load(&g_seg_inflight)) usleep(1000);
    draws(48);
    CHECK(g_enc == nil && cuts() == c0 + 3, "a third cut (DM_NVN_CUT_FIRST_MAX 3)");
    CHECK(rp_begin() != nil, "reopened a third time");
    while (atomic_load(&g_seg_inflight)) usleep(1000);
    draws(48);
    CHECK(g_enc != nil && cuts() == c0 + 3, "no fourth cut: the maximum");
    finish();
    g_cut_reopen = 1; mtl_sub_begin(); CHECK(g_cut_reopen == 0, "a new submission clears a mark no pass consumed"); if (g_enc) rp_end(); mtl_wait(mtl_sub_commit(), 5000000000ull);
    uint8_t px[4] = {0}; [ts getBytes:px bytesPerRow:8 * 4 fromRegion:MTLRegionMake2D(3, 3, 1, 1) mipmapLevel:0];
    CHECK(px[0] == 0 && px[1] == 255 && px[3] == 255, "three splits later the target still holds the first part's green (stored and loaded each time, not cleared again or dropped)");

    // A cut at a pass boundary (sub_cut) ends the mid-pass cuts of the submission
    open_cleared(&small, MTLClearColorMake(0, 0, 0, 1));
    rp_end(); while (atomic_load(&g_seg_inflight)) usleep(1000); g_stat_draw += (uint64_t)cut_draws(); sub_cut();
    CHECK(g_cut_first_seg == 1, "a boundary commit");
    CHECK(rp_begin() != nil, "the next pass opens");
    while (atomic_load(&g_seg_inflight)) usleep(1000);
    uint64_t cb = cuts(); draws(48);
    CHECK(g_enc != nil && cuts() == cb, "no mid-pass cut after it");
    finish();

    // Attachments over DM_NVN_CUT_FIRST_MB: no cut, counted once for the pass
    open_cleared(&big, MTLClearColorMake(0, 0, 1, 1));
    cb0 = g_cb; cb = cuts();
    draws(48); draws(48);
    CHECK(g_enc != nil && g_cb == cb0 && cuts() == cb, "a 32x32 RGBA8 pass over the 0.001 MB limit is not cut");
    mtl_cut_first_stats(&c, &nbig, &busy);
    CHECK(nbig == 1, "counted once as too big");
    finish();

    printf("cut first: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
