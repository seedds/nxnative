// A game's upscaler drawn by MetalFX (DM_UPSCALER=metalfx; nvn_mtl.m up_mfx_encode): one of the game's upscaler shaders reading a 960x540 texture and writing a
// 1920x1080 image is replaced by the spatial scaler, which fills the whole image with the texture's colour; equal sizes, another shader, and DM_UPSCALER unset
// (the last run) leave the dispatch to the shader. Runs under any profile: the named shader is the profile's first upscaler (DM_GAME_UPSCALER_KEYS), and a profile with
// none swaps no shader; the MetalFX draw itself (up_mfx_encode) does not look at the key. The colour (up_mfx_color) comes from synthetic code holding the curves' constants: a variant with sRGB input
// comes out decoded to linear, and one whose output stays encoded is left to its shader
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static void fill(id<MTLTexture> t, MTLClearColor c) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c;
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int matches(id<MTLTexture> t, const float inside[4], MTLRegion region, const float outside[4]) {   // allow HDR scaler rounding on each channel
    NSUInteger n = t.width * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "readback buffer did not allocate\n"); return 0; }
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 8 destinationBytesPerImage:n * 8];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const __fp16 *p = b.contents;
    for (NSUInteger i = 0; i < n; i++) {
        NSUInteger x = i % t.width, y = i / t.width;
        const float *want = x >= region.origin.x && x < region.origin.x + region.size.width &&
                            y >= region.origin.y && y < region.origin.y + region.size.height ? inside : outside;
        for (int c = 0; c < 4; c++) if (!(fabsf((float)p[4*i+c] - want[c]) <= fmaxf(0.01f, 0.03f * fabsf(want[c])))) {
            fprintf(stderr, "texel %lu,%lu channel %d: %g, expected %g\n", (unsigned long)x, (unsigned long)y, c, (float)p[4*i+c], want[c]);
            return 0;
        }
    }
    return 1;
}
// Code holding each constant 12 times, among others: the shape of a real variant's curves
static int color_of(const float *f32, int n32, const float *f16, int n16) {
    uint8_t code[4096]; size_t o = 0;
    for (int i = 0; i < (int)sizeof code; i++) code[i] = (uint8_t)(i * 37 + 11);
    for (int r = 0; r < 12; r++) {
        for (int i = 0; i < n32; i++) { memcpy(code + o, &f32[i], 4); o += 16; }
        for (int i = 0; i < n16; i++) { __fp16 h = f16[i]; memcpy(code + o, &h, 2); o += 16; }
    }
    return up_mfx_color(code, sizeof code);
}
static NvShader *upscaler(const char *key) {
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_LINEAR;
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
int main(void) { @autoreleasepool {
    mtl_init();
    const float enc[2] = {0.0031308f, 1 / 2.4f}, dec[2] = {0.04045f, 2.4f}, both[4] = {0.0031308f, 1 / 2.4f, 0.04045f, 2.4f}, dec16[3] = {0.040435791015625f, 2.3984375f, 0.07745361328125f}, enc16[1] = {0.41650390625f};
    float once[1] = {2.4f}; uint8_t rare[256] = {0}; memcpy(rare, once, 4);
    CHECK(color_of(both, 4, NULL, 0) == UP_COLOR_LINEAR, "an encode and a decode in floats: linear input and output (Bananza)");
    CHECK(color_of(NULL, 0, dec16, 3) == UP_COLOR_SRGB, "only a decode, in half floats: sRGB input, linear output (Splatoon Raiders)");
    CHECK(color_of(enc, 2, NULL, 0) == UP_COLOR_KEEP, "an encode with no decode: the output stays encoded, the shader runs");
    CHECK(color_of(NULL, 0, NULL, 0) == UP_COLOR_KEEP, "no curve: the output stays encoded, the shader runs");
    CHECK(color_of(dec, 2, NULL, 0) == UP_COLOR_KEEP, "a shape no pack has shown (a decode in floats, no encode): the shader runs");
    CHECK(color_of(NULL, 0, (const float[4]){0.040435791015625f, 2.3984375f, 0.07745361328125f, enc16[0]}, 4) == UP_COLOR_KEEP, "a half float encode must not read as encoded input: the shader runs");
    CHECK(up_mfx_color(rare, sizeof rare) == UP_COLOR_KEEP, "a constant used once is chance, not a curve");
    const char *up = getenv("DM_UPSCALER"); int on = up && !strcmp(up, "metalfx");
    tex_t in = texture(77, 960, 540, NVN_FORMAT_RGBA16F), out = texture(78, 1920, 1080, NVN_FORMAT_RGBA16F), same = texture(79, 960, 540, NVN_FORMAT_RGBA16F);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; same.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL); mtl_reg_image(703, &same, NULL);
    const float colour[4] = {0.5f, 0.25f, 0.75f, 1.0f}, black[4] = {0, 0, 0, 1};
    fill(tex_mtl(&in), MTLClearColorMake(colour[0], colour[1], colour[2], colour[3])); fill(tex_mtl(&out), MTLClearColorMake(0, 0, 0, 1));
    char key[24] = "7629d857a0a93833_b"; if (DM_GAME_UPSCALER_N) snprintf(key, sizeof key, "%.16s_b", DM_GAME_UPSCALER_KEYS);   // the profile's first upscaler, else Bananza's (not named here)
    NvShader *named = upscaler(key), *other = upscaler("0123456789abcdef_b");
    CHECK(up_mfx_wanted(named) == (on && DM_GAME_UPSCALER_N), "a shader the profile names is swapped exactly when DM_UPSCALER=metalfx (none in a profile without upscalers)");
    CHECK(!up_mfx_wanted(other), "a shader the profile does not name is never swapped");
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }   // c[24]: input and output rectangles (origin, inclusive last texel)
    mtl_sub_begin(); int drawn = on && up_mfx_encode(named);
    CHECK(drawn == on, "the named shader's dispatch is drawn by MetalFX");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> written = tex_by_id(702); os_unfair_lock_unlock(&g_rmu);
    CHECK(matches(written, on ? colour : black, MTLRegionMake2D(0, 0, 1920, 1080), black), on ? "MetalFX filled the whole 1920x1080 image with the texture's colour" : "without the switch the image is untouched");
    if (on) {
        tex_t packed = texture(80, 976, 548, NVN_FORMAT_R11G11B10F);
        mtl_reg_tex(704, &packed, NULL);
        const float hdr[4] = {2.0f, 0.5f, 0.25f, 1.0f};
        fill(tex_mtl(&packed), MTLClearColorMake(hdr[0], hdr[1], hdr[2], hdr[3]));
        fill(written, MTLClearColorMake(0, 0, 0, 1));
        const uint32_t crop[8] = {7, 3, 959, 539, 13, 9, 1899, 1059};
        memcpy(S.push[1], crop, sizeof crop);
        S.tex[5][0] = 704;
        mtl_sub_begin();
        CHECK(up_mfx_encode(named), "MetalFX takes the packed HDR subrectangle");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the converted submission completes");
        CHECK(matches(written, hdr, MTLRegionMake2D(13, 9, 1900, 1060), black), "the HDR output fills only its destination rectangle");
        tex_t packed_out = texture(81, 1920, 1080, NVN_FORMAT_R11G11B10F);
        packed_out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
        view_t view = {.set_swizzle = 1, .swz = {MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne}};
        mtl_reg_image(705, &packed_out, &view);
        S.img[5][1] = 705;
        id<MTLTexture> backing = tex_mtl(&packed_out);
        uint64_t upgrades = g_pfv_upgrades;
        for (int frame = 0; frame < 32; frame++) {
            mtl_sub_begin();
            CHECK(up_mfx_encode(named), "MetalFX writes the image behind a swizzled sampled view");
            CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "packed output submission completes");
        }
        CHECK(tex_mtl(&packed_out) == backing && g_pfv_upgrades == upgrades, "repeated scales reuse the original image backing");
        // sRGB input and linear output (Splatoon Raiders): the same sizes as above, so a scaler of their own, and the image holds the colour decoded
        S.img[5][1] = 702; S.tex[5][0] = 701; { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }
        named->up_color = UP_COLOR_SRGB;
        const float decoded[4] = {0.2140f, 0.0509f, 0.5225f, 1.0f};   // sRGB 0.5, 0.25, 0.75
        fill(written, MTLClearColorMake(0, 0, 0, 1)); mtl_sub_begin();
        CHECK(up_mfx_encode(named), "MetalFX takes sRGB-encoded input");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the sRGB submission completes");
        CHECK(matches(written, decoded, MTLRegionMake2D(0, 0, 1920, 1080), black), "the sRGB input comes out decoded to linear");
        fill(tex_mtl(&packed), MTLClearColorMake(colour[0], colour[1], colour[2], colour[3])); fill(written, MTLClearColorMake(0, 0, 0, 1));
        { const uint32_t r[8] = {7, 3, 959, 539, 13, 9, 1899, 1059}; memcpy(S.push[1], r, sizeof r); } S.tex[5][0] = 704; mtl_sub_begin();
        CHECK(up_mfx_encode(named), "MetalFX takes a packed sRGB-encoded subrectangle");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the packed sRGB submission completes");
        CHECK(matches(written, decoded, MTLRegionMake2D(13, 9, 1900, 1060), black), "the packed sRGB input comes out decoded, in its destination rectangle only");
        named->up_color = UP_COLOR_LINEAR;
        fill(tex_mtl(&in), MTLClearColorMake(colour[0], colour[1], colour[2], colour[3])); fill(written, MTLClearColorMake(0, 0, 0, 1));
        { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); } S.tex[5][0] = 701; mtl_sub_begin();
        CHECK(up_mfx_encode(named), "linear input after sRGB at the same sizes");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the second linear submission completes");
        CHECK(matches(written, colour, MTLRegionMake2D(0, 0, 1920, 1080), black), "the same sizes in linear colour are not decoded: each colour has its own scaler");
        NvShader *encoded_out = upscaler(key); encoded_out->up_color = UP_COLOR_KEEP;
        CHECK(!up_mfx_wanted(encoded_out), "a named shader whose output stays encoded is left to its shader");
        S.tex[5][0] = 701;
        S.img[5][1] = 703; { const uint32_t same_r[8] = {0, 0, 959, 539, 0, 0, 959, 539}; memcpy(S.push[1], same_r, sizeof same_r); } mtl_sub_begin(); uint64_t kept = g_up_mfx_kept;
        CHECK(!up_mfx_encode(named) && g_up_mfx_kept == kept + 1, "an image no larger than the texture leaves the dispatch to the shader");
        mtl_wait(mtl_sub_commit(), 5000000000ull);
    }
    printf("t_upscaler_mfx (%s): %d checks, %d failed\n", on ? "metalfx" : "game", checks, failed);
    return failed != 0;
}}
