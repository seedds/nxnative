// NVN2's DLSS on MetalFX (nvn_mtl.m dlss_eval) with Metal encoding on its own thread and without: MetalFX's temporal scaler encodes into the replay's real command buffer
// (rec_cb_block), never the recording's stand-in, and fills a 1920x1080 output from a flat 960x540 colour; two evaluations through the same scaler before the submission ends
// keep each one's own output. A network that takes new textures after an evaluation in the same submission replaces its scaler, and the submission still completes: the
// replaced scaler lives until then (a Metal 4 command buffer retains nothing; released at once, the commit messages a freed object, which the Makefile's NSZombieEnabled
// turns into a crash every time: "-[_MTL4MachineLearningPipelineState executable]: message sent to deallocated instance").
// An input between the network's optimal size and its output (Xenoblade Chronicles 2's Switch 2 program: 1024x576 into its 1536x864 network, inside the range GetOptimalSettings
// reports) is scaled, and a smaller input in the same textures (a dynamic resolution) goes through the same scaler and fills the output too
// The scaler comes after the work before it and the next draw pass after the scaler (mfx_order): a slow pass that ends with no barrier writes the colour just before each
// evaluation, and a draw pass copies the output just after it; the Makefile's DM_NVN_BARRIER=game run makes that pass skip its consumer barrier unless the scaler cleared the
// relaxed end
#include "../nvn/nvn_mtl.m"
#include "t_mfx_passes.h"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1, .flags = NVN_TEXTURE_FLAGS_IMAGE};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static void fill(id<MTLTexture> t, MTLClearColor c, int depth) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 0; }
    else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int flat(id<MTLTexture> t, const float want[3]) {   // the middle half of the picture (the scaler's edges may differ), within HDR rounding
    NSUInteger n = t.width * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "readback buffer did not allocate\n"); return 0; }
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 8 destinationBytesPerImage:n * 8];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const __fp16 *p = b.contents;
    for (NSUInteger y = t.height / 4; y < t.height * 3 / 4; y++) for (NSUInteger x = t.width / 4; x < t.width * 3 / 4; x++)
        for (int c = 0; c < 3; c++) if (!(fabsf((float)p[4 * (y * t.width + x) + c] - want[c]) <= 0.03f)) {
            fprintf(stderr, "texel %lu,%lu channel %d: %g, expected %g\n", (unsigned long)x, (unsigned long)y, c, (float)p[4 * (y * t.width + x) + c], want[c]); return 0; }
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t col = texture(90, 960, 540, NVN_FORMAT_RGBA16F), col2 = texture(91, 960, 540, NVN_FORMAT_RGBA16F), dep = texture(92, 960, 540, NVN_FORMAT_DEPTH32F),
          mot = texture(93, 960, 540, NVN_FORMAT_RG16F), out = texture(94, 1920, 1080, NVN_FORMAT_RGBA16F), out2 = texture(95, 1920, 1080, NVN_FORMAT_RGBA16F);
    mtl_tex_init(&col); mtl_tex_init(&col2); mtl_tex_init(&dep); mtl_tex_init(&mot); mtl_tex_init(&out); mtl_tex_init(&out2);
    const float a[3] = {0.5f, 0.25f, 0.75f}, b[3] = {0.125f, 0.625f, 0.25f};
    fill(tex_mtl(&col), MTLClearColorMake(a[0], a[1], a[2], 1), 0); fill(tex_mtl(&col2), MTLClearColorMake(b[0], b[1], b[2], 1), 0);
    fill(tex_mtl(&dep), MTLClearColorMake(0, 0, 0, 0), 1); fill(tex_mtl(&mot), MTLClearColorMake(0, 0, 0, 0), 0);
    p_dlss c = {.tex = {&col, &dep, &mot, &out}, .net = 1, .iw = 960, .ih = 540, .ow = 1920, .oh = 1080, .mv = {1, 1}, .pre_exposure = 1, .flags = DLSS_F_DEPTH_INVERTED, .reset = 1};
    p_dlss c2 = c; c2.tex[DLSS_COLOR] = &col2; c2.tex[DLSS_OUTPUT] = &out2;
    for (int frame = 0; frame < 4; frame++) {   // the temporal scaler settles over a few frames of the same input
        mtl_sub_begin(); dlss_eval(&c); dlss_eval(&c2);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
        c.reset = c2.reset = 0;
    }
    CHECK(g_dlss[0].sc != nil, "MetalFX made a temporal scaler for these formats");
    tex_t col3 = texture(96, 960, 486, NVN_FORMAT_RGBA16F), dep3 = texture(97, 960, 486, NVN_FORMAT_DEPTH32F), mot3 = texture(98, 960, 486, NVN_FORMAT_RG16F), out3 = texture(99, 1920, 972, NVN_FORMAT_RGBA16F);
    mtl_tex_init(&col3); mtl_tex_init(&dep3); mtl_tex_init(&mot3); mtl_tex_init(&out3);
    fill(tex_mtl(&col3), MTLClearColorMake(b[0], b[1], b[2], 1), 0); fill(tex_mtl(&dep3), MTLClearColorMake(0, 0, 0, 0), 1); fill(tex_mtl(&mot3), MTLClearColorMake(0, 0, 0, 0), 0);
    p_dlss c3 = c; c3.net = c.net = 2; c.reset = 1; c3.tex[DLSS_COLOR] = &col3; c3.tex[DLSS_DEPTH] = &dep3; c3.tex[DLSS_MOTION] = &mot3; c3.tex[DLSS_OUTPUT] = &out3; c3.iw = 960; c3.ih = 486; c3.ow = 1920; c3.oh = 972;
    // A new network evaluated at 1920x1080, then at 1920x972 (Fast Fusion's second size) in the same submission: its first scaler is replaced before its first commit. The
    // encoding thread has made the first evaluation's calls by then (rec_drain, as a game's frame of records hands them over in pieces), so the recording holds the scaler no longer
    mtl_sub_begin(); dlss_eval(&c);
    const void *first = (__bridge void *)g_dlss[1].sc;   // its address only: a reference here would keep it alive
    rec_drain(); dlss_eval(&c3);
    CHECK(first && g_dlss[1].sc && (__bridge void *)g_dlss[1].sc != first, "new sizes replace the network's scaler");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission that encoded the replaced scaler completes");
    CHECK(flat(tex_mtl(&out), a), "the first evaluation's output holds its own colour");
    CHECK(flat(tex_mtl(&out2), b), "the second evaluation through the same scaler holds its own");
    tex_t col4 = texture(100, 1024, 576, NVN_FORMAT_RGBA16F), dep4 = texture(101, 1024, 576, NVN_FORMAT_DEPTH32F), mot4 = texture(102, 1024, 576, NVN_FORMAT_RG16F), out4 = texture(103, 1536, 864, NVN_FORMAT_RGBA16F);
    mtl_tex_init(&col4); mtl_tex_init(&dep4); mtl_tex_init(&mot4); mtl_tex_init(&out4);
    fill(tex_mtl(&col4), MTLClearColorMake(a[0], a[1], a[2], 1), 0); fill(tex_mtl(&dep4), MTLClearColorMake(0, 0, 0, 0), 1); fill(tex_mtl(&mot4), MTLClearColorMake(0, 0, 0, 0), 0);
    p_dlss c4 = {.tex = {&col4, &dep4, &mot4, &out4}, .net = 3, .iw = 1024, .ih = 576, .ow = 1536, .oh = 864, .mv = {1, 1}, .pre_exposure = 1, .flags = DLSS_F_DEPTH_INVERTED, .reset = 1};
    for (int frame = 0; frame < 4; frame++) {
        mtl_sub_begin(); dlss_eval(&c4);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the 1.5x submission completes");
        c4.reset = 0;
    }
    dlss_scaler *s4 = NULL; for (int i = 0; i < 4; i++) if (g_dlss[i].net == 3) s4 = &g_dlss[i];
    CHECK(s4 && s4->sc != nil, "MetalFX made a temporal scaler for 1024x576 into 1536x864");
    CHECK(flat(tex_mtl(&out4), a), "the 1.5x evaluation fills its output");
    // The same network at 768x432 in the same textures, as a dynamic resolution renders: no new scaler, and the output is whole (MetalFX scales the content, not the textures)
    const void *sc4 = s4 ? (__bridge void *)s4->sc : NULL;
    fill(tex_mtl(&col4), MTLClearColorMake(b[0], b[1], b[2], 1), 0);
    c4.iw = 768; c4.ih = 432; c4.reset = 1;
    for (int frame = 0; frame < 4; frame++) {
        mtl_sub_begin(); dlss_eval(&c4);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the 2x submission in the same textures completes");
        c4.reset = 0;
    }
    CHECK(s4 && sc4 && (__bridge void *)s4->sc == sc4, "a smaller input in the same textures keeps the network's scaler");
    CHECK(flat(tex_mtl(&out4), b), "the 2x evaluation in the same textures fills its output");
    // The scaler after the work before it, and the next pass after the scaler (mfx_order). Before: a slow pass writes the colour just before each evaluation and ends with no
    // barrier, as a scaler's own encoders end (the passes the host ends carry barriers of their own); behind an encoder of barriers alone the scaler read the colour before it in
    // most rounds. After: a draw pass copies the output right after the evaluation; with DM_NVN_BARRIER=game (a run of the Makefile's) it skipped its consumer barrier and read the
    // output before the scaler wrote it, in every round
    tex_t cp = texture(104, 1920, 1080, NVN_FORMAT_RGBA16F); mtl_tex_init(&cp);
    float rc[2][4] = {{a[0], a[1], a[2], 1}, {b[0], b[1], b[2], 1}}; int drawn = 0, stale_in = 0, stale_out = 0;
    for (int round = 0; round < 8; round++) {
        mtl_sub_begin(); drawn += slow_draw(tex_mtl(&col), rc[round & 1], 200000, 1); c.reset = 1; dlss_eval(&c); drawn += copy_draw(tex_mtl(&out), tex_mtl(&cp));
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission with passes around the evaluation completes");
        stale_in += !flat(tex_mtl(&out), rc[round & 1]); stale_out += !flat(tex_mtl(&cp), rc[round & 1]);
    }
    CHECK(drawn == 16, "the passes around the evaluation are drawn");
    CHECK(!stale_in, "the scaler reads the colour the pass just before it wrote, in every round");
    CHECK(!stale_out, "the pass after the scaler reads its output, in every round");
    printf("t_dlss_mfx (encoding thread %s): %d checks, %d failed\n", rec_on() ? "on" : "off", checks, failed);
    return failed != 0;
}}
