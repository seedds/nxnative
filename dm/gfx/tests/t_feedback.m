// Feedback snapshots and their per-draw argument substitution include separate texture slots and read-only image slots; a snapshot is reused until the game's next barrier,
// and a draw that only samples an attachment nothing in its pass has written reads it directly. An image load of one level of a texture is matched on that level.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> target_fmt(tex_t *t, uint64_t serial, int format, int levels) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format,
        .width=4, .height=4, .depth=1, .levels=levels, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
static id<MTLTexture> target(tex_t *t, uint64_t serial) { return target_fmt(t, serial, NVN_FORMAT_RGBA8, 1); }
static int fb_env_mode(void) {
    const char *e = getenv("DM_NVN_FB_SNAP");
    return e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "all") ? 2 : 1;
}
static void clear(MTLClearColor color) {
    R.clear_c[0] = 1; R.cc[0] = color;
    CHECK(rp_begin() != nil, "production render pass opens");
}
static void sample(NvShader *fs, id<MTLRenderPipelineState> pipeline) {
    clear(MTLClearColorMake(0, 0, 0, 1));
    enc_r_pso(g_enc, pipeline);
    enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1});
    CHECK(encode_args((id<MTLCommandEncoder>)g_enc, fs, 1, 0), "production fragment arguments encoded");
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    rp_end();
}
static void readback(id<MTLTexture> t, id<MTLBuffer> b, unsigned offset) {
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), b, offset, 256, 1024);
}
static void pixels(id<MTLBuffer> b, unsigned offset, const uint8_t want[4], const char *label) {
    int valid = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        valid &= !memcmp((const uint8_t *)b.contents + offset + y * 256 + x * 4, want, 4);
    CHECK(valid, label);
}
static void slot_case(NvProg *pr, id<MTLRenderPipelineState> pipeline, int slot, int index) {
    int before = failed, tid = 600 + index;
    NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = slot;
    tex_t source, output;
    id<MTLTexture> original = target(&source, 99700 + index * 2);
    id<MTLTexture> destination = target(&output, 99701 + index * 2);
    mtl_reg_tex(tid, &source, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][slot] = tid;
    clear(MTLClearColorMake(1, 0, 0, 1));
    fb_prepare(pr);
    CHECK(!g_enc, "snapshot closes the preceding render pass");
    CHECK(g_fb_n == 1, "feedback attachment receives a snapshot");
    CHECK(g_fb_n == 1 && g_fb_src[0] == original && g_fb_use[0] != original,
          "snapshot substitutes a distinct texture for the registered source");
    rp_end();
    // Overwrite the attachment after the snapshot, then render the sampled value
    // into a distinct target. This makes a missing snapshot fail deterministically
    // even on GPUs that happen to tolerate sampling an attached render target.
    clear(MTLClearColorMake(0, 1, 0, 1)); rp_end();
    R.rt.color[0] = &output;
    sample(fs, pipeline);
    uint64_t bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
    CHECK(g_fb_n == 1 && bound == g_fb_rid[0] && bound != original.gpuResourceID._impl,
          "production argument buffer contains the snapshot resource ID");
    readback(destination, result, 0);
    // With a different attachment the following draw restores the original binding.
    fb_prepare(pr);
    CHECK(g_fb_n == 0, "following draw needs no snapshot");
    sample(fs, pipeline);
    bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
    CHECK(bound == original.gpuResourceID._impl, "following draw restores the original resource ID");
    readback(destination, result, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    pixels(result, 0, (const uint8_t[]){255, 0, 0, 255}, "first draw reads the red pre-overwrite snapshot");
    pixels(result, 1024, (const uint8_t[]){0, 255, 0, 255}, "following draw reads the green original attachment");
    printf("feedback slot %d: %s\n", slot, before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_tex_release(&output);
}
// The rule without a listed shader: copy once, reuse until a barrier, end the pass when an earlier draw of it read the attachment directly.
static void reuse_case(NvProg *pr) {
    int before = failed, tid = 650; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t source; target(&source, 99790);
    mtl_reg_tex(tid, &source, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid;
    S.chmask.mask = 0; clear(MTLClearColorMake(1, 0, 0, 1));
    uint64_t copies = g_fb_count;
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a draw that writes nothing reads an unwritten attachment directly");
    S.chmask.mask = 0xffffffffu;
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1, "a writing draw takes a snapshot");
    id<MTLTexture> first = g_fb_use[0];
    clear(MTLClearColorMake(0, 1, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && g_enc && g_fb_count == copies + 1 && g_fb_use[0] == first, "the next writing draw reuses the snapshot without ending the pass");
    S.chmask.mask = 0; rp_end(); clear(MTLClearColorMake(0, 1, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a reading draw first in a pass reads directly");
    S.chmask.mask = 0xffffffffu;
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1, "a writing draw after a direct read ends the pass and reuses the snapshot");
    g_fb_bar++;
    clear(MTLClearColorMake(0, 0, 1, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 2, "a game barrier makes the next writing draw take a fresh snapshot");
    rp_end();
    // The depth attachment read, then written in the same pass (only serials matter to the rule: the attachment is named after the pass opens)
    { tex_t output, depth; target(&output, 99791); target(&depth, 99792); mtl_reg_tex(651, &depth, NULL);   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns both textures (serial_put's CFBridgingRetain) until mtl_tex_release below, not a leak
      R.rt.color[0] = &output; R.rt.depth = NULL; S.tex[1][0] = 651; S.chmask.mask = 0xffffffffu; S.ds.dt = 1; S.ds.dw = 0;
      clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;
      fb_prepare(pr);
      CHECK(g_enc != nil, "a draw that reads the depth attachment stays in its pass");
      fb_prepare(pr);
      CHECK(g_enc != nil, "so does the next one, which writes no depth");
      S.ds.dw = 1; fb_prepare(pr);
      CHECK(!g_enc, "a draw writing depth after a read of it ends the pass");
      R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); rp_end();   // that draw read it too, and began the pass it would open: the next pass is a clean one
      clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth; S.tex[1][0] = 0;
      fb_prepare(pr);
      CHECK(g_enc != nil, "a draw writing depth with no read of it in its pass stays");
      R.rt.depth = NULL; S.ds.dt = S.ds.dw = 0; rp_end(); mtl_tex_release(&output); mtl_tex_release(&depth); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    printf("feedback reuse: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
}
// One R32Uint attachment. A finished pass stores 0x11. The next pass clears to 0x22 and a draw writes 0x33, then (after a game barrier) a fragment shader loads the same texel through a read-only image and writes the loaded value back. Switch semantics are 0x33, the
// earlier draw; an open Metal pass would load 0x11, the content from before the pass. DM_NVN_FB_SNAP=0 leaves the pass open and does not draw that load.
static void image_case(NvProg *writer, NvProg *reader, id<MTLRenderPipelineState> old_p, id<MTLRenderPipelineState> new_p, id<MTLRenderPipelineState> read_p) {
    int before = failed, mode = fb_env_mode(), tid = 760;
    NvShader *rs = reader->sh[1];
    tex_t source; target_fmt(&source, 99820, NVN_FORMAT_R32UI, 2);
    view_t level1 = {0}; level1.set_levels = 1; level1.base_level = 1; level1.nlevels = 1;
    mtl_reg_image(tid, &source, &level1);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.img[1][0] = tid;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0x22, 0, 0, 1); CHECK(rp_begin() != nil, "level pass opens");
    fb_prepare(reader);
    CHECK(g_fb_n == 0, "an image of level 1 is not feedback of the level 0 attachment");
    rp_end();
    R.rt.has_cview[0] = 1; R.rt.cview[0] = level1;
    R.clear_c[0] = 1; CHECK(rp_begin() != nil, "level 1 pass opens");
    fb_prepare(reader);
    CHECK(g_fb_mode == mode, "snapshot mode follows DM_NVN_FB_SNAP");
    CHECK(g_fb_n == (mode == 1 || mode == 2), "an image view of the attached level is snapshotted in the copying modes");
    rp_end();
    rs->rf.b[0].dpf = NVN_DPF_SINT; R.rt.has_cview[0] = 0; memset(&R.rt.cview[0], 0, sizeof R.rt.cview[0]);
    mtl_reg_image(tid, &source, NULL);
    R.clear_c[0] = 1; CHECK(rp_begin() != nil, "class pass opens");
    fb_prepare(reader);
    CHECK(g_fb_n == 0, "an image whose pixel class needs a reinterpreting view is not snapshotted");
    rp_end();
    rs->rf.b[0].dpf = NVN_DPF_UINT;
    // The value test is a one-level texture. The two-level one above would be left as a mip-chain view and never snapshotted.
    tex_t pixels; id<MTLTexture> mask = target_fmt(&pixels, 99821, NVN_FORMAT_R32UI, 1);
    mtl_reg_image(761, &pixels, NULL); S.img[1][0] = 761; R.rt.color[0] = &pixels; R.rt.has_cview[0] = 0;
    if (mode == 1) {
        R.clear_c[0] = 0; CHECK(rp_begin() != nil, "direct-read pass opens");
        S.chmask.mask = 0; fb_prepare(reader);
        CHECK(g_fb_n == 0 && (g_fb_rmask & 1), "a read-only image of an unwritten attachment reads it directly and is remembered");
        S.chmask.mask = 0xffffffffu; fb_prepare(writer);
        CHECK(!g_enc, "a later writer starts a new pass after that image read");
    }
    // The value the pass must not return: stored, then left behind by the next pass's clear and draw.
    S.chmask.mask = 0xffffffffu;
    R.clear_c[0] = 0; CHECK(rp_begin() != nil, "stale pass opens");
    enc_r_pso(g_enc, old_p); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); rp_end();
    if (mode != 0) {
        R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0x22, 0, 0, 1); CHECK(rp_begin() != nil, "feedback pass opens on its clear");
        fb_prepare(writer);
        enc_r_pso(g_enc, new_p); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
        g_fb_bar++;   // the game's barrier between the mark and the load
        fb_prepare(reader);
        int closed = g_enc == nil, snaps = g_fb_n;
        CHECK(closed, "the image load closes the pass that holds the earlier draw");
        CHECK(snaps == 1, "the image load binds one snapshot");
        R.clear_c[0] = 0; CHECK(rp_begin() != nil, "the load's pass opens on the stored attachment");
        CHECK(encode_args((id<MTLCommandEncoder>)g_enc, rs, 1, 0), "image arguments encoded");
        uint64_t bound = *(uint64_t *)((uint8_t *)rs->ab_buf.contents + rs->ab_off);
        CHECK(snaps == 1 && bound == g_fb_rid[0] && bound != mask.gpuResourceID._impl, "the argument buffer holds the snapshot");
        enc_r_pso(g_enc, read_p); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); rp_end();
        id<MTLBuffer> result = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(result);
        readback(mask, result, 0);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
        CHECK(!mtl_last_commit_error(), "GPU submission has no error");
        int valid = 1; uint32_t got = *(const uint32_t *)result.contents;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) valid &= *(const uint32_t *)((const uint8_t *)result.contents + y * 256 + x * 4) == 0x33;
        if (!valid) fprintf(stderr, "image load saw %#x, want 0x33 (0x11 is the previous pass)\n", got);
        CHECK(valid, "the image load sees the earlier draw, not the content from before the pass");
    } else {
        fb_prepare(reader);
        CHECK(g_fb_n == 0 && g_enc == nil, "snapshots off do not substitute and do not open a pass");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
        CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    }
    printf("feedback image (mode %d): %s\n", mode, before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_tex_release(&pixels);
}
int main(int argc, char **argv) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "struct A {texture2d<float, access::read> source[[id(0)]];};\n"
        "fragment float4 f(float4 p[[position]], constant A&a[[buffer(0)]]) {return a.source.read(uint2(p.xy));}\n"
        options:nil error:&error];
    CHECK(lib != nil, "synthetic shader compiles");
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "0123456789abcdef"); fs->fn = [lib newFunctionWithName:@"f"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .aid=0, .dtt=MTLTextureType2D};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"v"]; d.fragmentFunction = fs->fn;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pipeline != nil, "synthetic render pipeline compiles");
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    int image_only = argc > 1 && !strcmp(argv[1], "--image");
    if (!image_only) {
        const int slots[] = {0, 128, 255};
        for (int i = 0; i < 3; i++) slot_case(pr, pipeline, slots[i], i);
        reuse_case(pr);
    }
    id<MTLLibrary> ilib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment uint4 fold(float4 p[[position]]) { return uint4(0x11u, 0, 0, 0); }\n"
        "fragment uint4 fnew(float4 p[[position]]) { return uint4(0x33u, 0, 0, 0); }\n"
        "struct A { texture2d<uint> img[[id(0)]]; };\n"
        "fragment uint4 fread(float4 p[[position]], constant A& a[[buffer(0)]]) { return uint4(a.img.read(uint2(p.xy)).x, 0, 0, 0); }\n"
        options:nil error:&error];
    CHECK(ilib != nil, "image shader compiles");
    if (!ilib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *writer = [NvProg new]; NvShader *ws = [NvShader new]; writer->sh[1] = ws; ws->stage = 1; strcpy(ws->key, "imagewriter");
    NvProg *reader = [NvProg new]; NvShader *rs = [NvShader new]; reader->sh[1] = rs; rs->stage = 1; strcpy(rs->key, "15816d836842bfb5");
    rs->fn = [ilib newFunctionWithName:@"fread"]; rs->ae = [rs->fn newArgumentEncoderWithBufferIndex:0]; rs->arg_len = rs->ae.encodedLength;
    rs->rf.nb = 1; rs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .aid=0, .dtt=MTLTextureType2D, .dpf=NVN_DPF_UINT};
    MTLRenderPipelineDescriptor *idsc = [MTLRenderPipelineDescriptor new];
    idsc.vertexFunction = [ilib newFunctionWithName:@"v"]; idsc.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    idsc.fragmentFunction = [ilib newFunctionWithName:@"fold"];
    id<MTLRenderPipelineState> old_p = [g_dev newRenderPipelineStateWithDescriptor:idsc error:&error];
    idsc.fragmentFunction = [ilib newFunctionWithName:@"fnew"];
    id<MTLRenderPipelineState> new_p = [g_dev newRenderPipelineStateWithDescriptor:idsc error:&error];
    idsc.fragmentFunction = rs->fn;
    id<MTLRenderPipelineState> read_p = [g_dev newRenderPipelineStateWithDescriptor:idsc error:&error];
    CHECK(old_p && new_p && read_p, "image pipelines compile");
    if (old_p && new_p && read_p) image_case(writer, reader, old_p, new_p, read_p);
    printf("feedback snapshot: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
