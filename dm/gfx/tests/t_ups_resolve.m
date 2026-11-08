// Resolution upscaling: the game's own spatial upscaler resolved by the host (DM_UPSCALE_GAME; nvn_mtl.m ups_bypass_encode). An upscaler-shaped compute shader (one sampled
// texture, one written image) with FSR's EASU constants in its constant buffer, dispatched with a 1600x900 sRGB scene the host made at 2x (3200x1800), of which the constants
// name a 1200x675 viewport, and a 1920x1080 output image, has its output written by the host: the viewport filtered down, so a viewport of two flat colours comes out as the same
// two colours away from their edge, with every filter (box, tent, Lanczos-2), nothing of the scene outside the viewport reaches the output, and the sRGB encoding survives the
// round trip (the output is read back as bytes). DM_UPSCALE_GAME=keep, constants that are not EASU's, an output of the scene's own size, and a run without DM_UPSCALE (the last
// run) leave the dispatch to the game's kernel, as does a Switch 2 game's run without DM_UPSCALE_SWITCH2=1 (T_UPS_SWITCH2=1 plays one: the factor stays 1, ups_init). Runs under
// any profile: the shape, the constants and the sizes decide, not the shader's key
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
int dm_sdk_ounce(void) { const char *e = getenv("T_UPS_SWITCH2"); return e && !strcmp(e, "1"); }   // the sdk's console (loader.c; hle_nvn.c's weak stand-in says Switch 1)
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
// A single-level 2D texture of the game's size w x h with NVN flags (COMPRESSIBLE makes a render target the host scales; IMAGE alone is data, left at the game's size), initialized
// in the caller's object as a game's is: mtl_tex_init records that object's address (g_tex_live), and mtl_tex_release tells the texture's own object from a copy by it
static void texture(tex_t *t, uint64_t serial, unsigned w, unsigned h, int format, uint32_t flags) {
    *t = (tex_t){.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1, .flags = flags};
    nvn_tex_layout(&t->d, &t->L); mtl_tex_init(t);
}
static void fill(id<MTLTexture> t, MTLClearColor c) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c;
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
// The texels of an RGBA8 texture, as stored (sRGB-encoded bytes for an sRGB format)
static NSData *bytes_of(id<MTLTexture> t) {
    NSUInteger n = t.width * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "readback buffer did not allocate\n"); return [NSData data]; }   // an empty readback fails the checks that read it
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 4 destinationBytesPerImage:n * 4];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return [NSData dataWithBytes:b.contents length:n * 4];
}
// Whether every texel of the w x h output left of column edge - margin holds `left` and every one right of edge + margin holds `right` (bytes, within tol): the resolve of a
// scene whose left half is one flat colour and right half another keeps both flat and the edge where it was (at most `margin` output texels wide: the filter's radius)
static int halves(id<MTLTexture> t, const uint8_t left[4], const uint8_t right[4], NSUInteger edge, NSUInteger margin, int tol, const char *what) {
    NSData *d = bytes_of(t); const uint8_t *p = d.bytes;
    if (d.length < t.width * t.height * 4) return 0;
    for (NSUInteger y = 0; y < t.height; y++) for (NSUInteger x = 0; x < t.width; x++) {
        if (x + margin >= edge && x < edge + margin) continue;
        const uint8_t *want = x < edge ? left : right, *got = p + (y * t.width + x) * 4;
        for (int c = 0; c < 4; c++) if (abs((int)got[c] - (int)want[c]) > tol) {
            fprintf(stderr, "%s: texel %lu,%lu channel %d: %d, expected %d\n", what, (unsigned long)x, (unsigned long)y, c, got[c], want[c]); return 0; }
    }
    return 1;
}
static NvShader *upscaler(void) {   // the shape of KTGL's FSR-style pass: a constant buffer, one sampled texture with its sampler, one written image
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", "0123456789abcdef_b");
    s->rf.nb = 4; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_CBUF}; s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[2] = (nvn_rbind){.kind = NVN_RB_SAMP}; s->rf.b[3] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0, .rw = 1};
    return s;
}
int main(void) { @autoreleasepool {
    mtl_init();
    int scaled = ups_factor() > 1;   // DM_UPSCALE=2 in the runs that resolve; unset in the last one
    if (dm_sdk_ounce()) { const char *o = getenv("DM_UPSCALE_SWITCH2");
        CHECK(scaled == (o && !strcmp(o, "1")), "a Switch 2 game keeps its own resolution at DM_UPSCALE=2 unless DM_UPSCALE_SWITCH2=1"); }
    // The scene: a 1600x900 sRGB render target, 3200x1800 at 2x; its left half one colour, its right half another. The output: a 1920x1080 sRGB image (IMAGE alone: unscaled)
    tex_t scene, out, same, patch;
    texture(&scene, 77, 1600, 900, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE); texture(&out, 78, 1920, 1080, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_IMAGE);
    texture(&same, 79, 1600, 900, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_IMAGE); texture(&patch, 80, 4, 4, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    mtl_reg_tex(701, &scene, NULL); mtl_reg_image(702, &out, NULL); mtl_reg_image(703, &same, NULL);
    id<MTLTexture> st = tex_mtl(&scene), ot = tex_mtl(&out), pt = tex_mtl(&patch);
    CHECK(st.width == (scaled ? 3200u : 1600u) && ot.width == 1920 && ot.height == 1080, scaled ? "the scene is made at 2x and the output at the game's size" : "at 1x both are at the game's size");
    // The viewport the constants name: 1200x675 of the 1600x900 scene (2400x1350 of the scaled one), the rest of the texture black, which must never reach the output
    const unsigned vw = 1200, vh = 675, sx = scaled ? 2400 : 1200, sy = scaled ? 1350 : 675;
    const MTLClearColor a = MTLClearColorMake(0.5, 0.25, 0.75, 1), b = MTLClearColorMake(0.1, 0.8, 0.3, 1);
    tex_t patch_a; texture(&patch_a, 81, 4, 4, NVN_FORMAT_RGBA8_SRGB, NVN_TEXTURE_FLAGS_COMPRESSIBLE); id<MTLTexture> pat = tex_mtl(&patch_a);
    fill(st, MTLClearColorMake(0, 0, 0, 1)); fill(pat, a); fill(pt, b);
    // The viewport in colour a and its left half in colour b, from 4x4 patches (nearest draws; scaling copies as the host's own, in the scene's texels)
    mtl_sub_begin();
    { NVNcopyRegion sr = {0, 0, 0, 4, 4, 1}, dr = {0, 0, 0, (int)sx, (int)sy, 1}, dl = {0, 0, 0, (int)sx / 2, (int)sy, 1};
      CHECK(copy_t2t_draw(pat, st, 0, 0, 0, 0, &sr, &dr, 0, 0) && copy_t2t_draw(pt, st, 0, 0, 0, 0, &sr, &dl, 0, 0), "the viewport and its left half are drawn"); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the scene's submission completes");
    uint8_t ea[4], eb[4];   // the two colours as the sRGB scene stores them: what the output must hold again after the decode, the filter and the encode
    { NSData *d = bytes_of(st); const uint8_t *p = d.bytes; CHECK(d.length >= sx * 4, "the scene reads back"); if (d.length >= sx * 4) { memcpy(eb, p, 4); memcpy(ea, p + (sx - 1) * 4, 4); } else memset(ea, 0, 4), memset(eb, 0, 4); }
    // The constant buffer, in guest memory: FSR's EASU constants for the viewport, the texture and the output (nvn_ups_fsr_rect)
    float *con = NULL; if (posix_memalign((void **)&con, (size_t)getpagesize(), (size_t)getpagesize())) abort(); memset(con, 0, (size_t)getpagesize()); mtl_pool_add(con, (uint64_t)getpagesize());
    con[0] = (float)vw / 1920; con[1] = (float)vh / 1080; con[2] = 0.5f * con[0] - 0.5f; con[3] = 0.5f * con[1] - 0.5f;
    con[4] = 1.0f / 1600; con[5] = 1.0f / 900; con[6] = con[4]; con[7] = -con[5]; con[8] = -con[4]; con[9] = 2 * con[5]; con[10] = con[4]; con[11] = 2 * con[5]; con[13] = 4 * con[5];
    NvShader *sh = upscaler();
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][0] = 702; S.ubo[5][0] = (p_bind_buf){.stage = 5, .index = 0, .addr = (uint64_t)(uintptr_t)con, .size = 256};
    const char *filters[3] = {"box", "tent", "lanczos"}; const NSUInteger margins[3] = {2, 3, 5};   // output texels either side of the edge the filter may blend (radius 0.5, 1 or 2 footprints of 1.67 texels)
    for (int f = 0; f < 3; f++) {
        setenv("DM_UPSCALE_GAME_FILTER", filters[f], 1);
        fill(ot, MTLClearColorMake(0, 0, 0, 1));
        mtl_sub_begin(); int done = ups_bypass_encode(sh);
        CHECK(done == scaled, scaled ? "the upscaler's dispatch is resolved by the host" : "without DM_UPSCALE the dispatch is left to the game's kernel");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the resolve's submission completes");
        if (scaled) { char what[64]; snprintf(what, sizeof what, "%s filter", filters[f]);
            CHECK(halves(ot, eb, ea, 960, margins[f], 2, what), "both flat colours come out flat on their side of the edge, sRGB-encoded as the scene stores them"); }
        else { uint8_t black[4] = {0, 0, 0, 255}; CHECK(halves(ot, black, black, 960, 0, 0, "untouched"), "the output is untouched at 1x"); }
    }
    unsetenv("DM_UPSCALE_GAME_FILTER");
    if (scaled) {
        S.ubo[5][0].size = 0; uint64_t kept0 = g_ups_byp_kept;   // no constant buffer read: the rectangle is unknown
        mtl_sub_begin(); CHECK(!ups_bypass_encode(sh) && g_ups_byp_kept == kept0 + 1, "without EASU's constants the rectangle is unknown: the game's kernel runs, counted as kept"); mtl_wait(mtl_sub_commit(), 5000000000ull);
        S.ubo[5][0].size = 256; con[2] = 0;   // the half term broken: not EASU's constants
        mtl_sub_begin(); CHECK(!ups_bypass_encode(sh), "constants that are not EASU's leave the dispatch alone"); mtl_wait(mtl_sub_commit(), 5000000000ull);
        con[2] = 0.5f * con[0] - 0.5f;
        setenv("DM_UPSCALE_GAME", "keep", 1);
        mtl_sub_begin(); CHECK(!ups_bypass_encode(sh), "DM_UPSCALE_GAME=keep leaves the dispatch to the game's kernel"); mtl_wait(mtl_sub_commit(), 5000000000ull);
        unsetenv("DM_UPSCALE_GAME");
        S.img[5][0] = 703; uint64_t kept = g_ups_byp_kept; con[0] = con[1] = 1; con[2] = con[3] = 0;   // the constants name the whole 1600x900 scene into a 1600x900 output
        mtl_sub_begin(); CHECK(!ups_bypass_encode(sh) && g_ups_byp_kept == kept + 1, "an output of the scene's own size is a per-pixel pass: left to the game's kernel, counted as kept"); mtl_wait(mtl_sub_commit(), 5000000000ull);
        con[0] = (float)vw / 1920; con[1] = (float)vh / 1080; con[2] = 0.5f * con[0] - 0.5f; con[3] = 0.5f * con[1] - 0.5f;
        S.img[5][0] = 702; S.tex[5][0] = 0;
        mtl_sub_begin(); CHECK(!ups_bypass_encode(sh), "an unbound scene slot leaves the dispatch alone"); mtl_wait(mtl_sub_commit(), 5000000000ull);
        CHECK(sh->up_byp == 1, "the shape is decided once per shader");
        NvShader *two = upscaler(); two->rf.nb = 5; two->rf.b[4] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1};
        S.tex[5][0] = 701; S.tex[5][1] = 701;
        mtl_sub_begin(); CHECK(!ups_bypass_encode(two) && two->up_byp == 2, "a second sampled texture is not the upscaler's shape"); mtl_wait(mtl_sub_commit(), 5000000000ull);
    }
    printf("t_ups_resolve (%s%s): %d checks, %d failed\n", scaled ? "2x" : "1x", dm_sdk_ounce() ? ", Switch 2" : "", checks, failed);
    return failed != 0;
}}
