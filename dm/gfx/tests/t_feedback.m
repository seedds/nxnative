// Feedback snapshots and their per-draw argument substitution include separate texture slots; a snapshot is reused until the game's next barrier,
// and a draw that only samples an attachment nothing in its pass has written reads it directly.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> shaped_target(tex_t *t, uint64_t serial, int format, int width, int height) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format,
        .width=width, .height=height, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
static id<MTLTexture> target(tex_t *t, uint64_t serial) { return shaped_target(t, serial, NVN_FORMAT_RGBA8, 4, 4); }
static void game_barrier(void) {
    const struct { op_hdr barrier; int32_t bits, pad; op_hdr end; } commands = {
        {OP_BARRIER, 16}, NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_INVALIDATE_TEXTURE_BIT, 0, {OP_END, 8}
    };
    mtl_exec((const uint8_t *)&commands);
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
    game_barrier();
    clear(MTLClearColorMake(0, 0, 1, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 2, "a game barrier makes the next writing draw take a fresh snapshot");
    rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    printf("feedback reuse: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
}
// Queue every readback before waiting: recycling a stale allocation must not change an
// earlier reader, including readers in the preceding still-unwaited submission.
static void pressure_case(NvProg *pr, id<MTLRenderPipelineState> pipeline) {
    enum { N = 12, READS = 40 };
    int before = failed; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t sources[N], output; id<MTLTexture> textures[N];
    for (int i = 0; i < N; i++) {
        int shape = i < 8 ? i % 4 : (i + 1) % 4;
        textures[i] = shaped_target(&sources[i], 99800 + i, shape & 1 ? NVN_FORMAT_BGRA8 : NVN_FORMAT_RGBA8,
                                    shape & 2 ? 8 : 4, shape & 1 ? 8 : 4);
        view_t view = {0};
        if (i % 3 == 0) { view.set_swizzle = 1; const uint8_t sw[] = {MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha}; memcpy(view.swz, sw, 4); }
        if (i == 9) { view.set_format = 1; view.format = NVN_FORMAT_RGBA8_SRGB; }
        mtl_reg_tex(700 + i, &sources[i], view.set_swizzle || view.set_format ? &view : NULL);
    }
    id<MTLTexture> destination = target(&output, 99820);
    id<MTLBuffer> result = [g_dev newBufferWithLength:READS * 1024 options:MTLResourceStorageModeShared]; res_add(result);
    uint8_t expected[READS][4]; int nr = 0;
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1;
    uint64_t copies = g_fb_count, want_copies = 0;
    for (int phase = 0; phase < 4; phase++) {
        if (phase == 1 || phase == 2) game_barrier();
        if (phase == 3) {
            mtl_sub_commit(); mtl_sub_begin();   // same barrier, different submission: contents must not be reused
        }
        int first = phase == 0 ? 0 : 8, end = phase == 0 ? 8 : N;
        for (int i = first; i < end; i++) {
            uint8_t color[4] = {(uint8_t)((i & 1) ? 255 : 0), (uint8_t)((i & 2) ? 255 : 0), (uint8_t)((phase & 1) ? 255 : 0), 255};
            if (i == 9) color[0] = 128;   // sRGB readback must decode mid-grey, not merely preserve 0/1 endpoints
            uint8_t sampled[4]; memcpy(sampled, color, 4);
            if (i % 3 == 0) { sampled[0] = color[2]; sampled[2] = color[0]; }
            if (i == 9) sampled[2] = 55;   // BGR puts linearized sRGB red (128 -> 55) in blue
            R.rt.color[0] = &sources[i]; S.tex[1][0] = 700 + i;
            clear(MTLClearColorMake(color[0] / 255.0, color[1] / 255.0, color[2] / 255.0, 1));
            fb_prepare(pr); want_copies++;
            CHECK(g_fb_n == 1 && g_fb_count == copies + want_copies, "a new key, barrier or submission takes exactly one snapshot copy");
            id<MTLTexture> snapshot = g_fb_use[0].parentTexture ? g_fb_use[0].parentTexture : g_fb_use[0];
            CHECK(snapshot != textures[i], "mixed-shape feedback substitutes a distinct texture");
            clear(MTLClearColorMake(1 - color[0] / 255.0, 1 - color[1] / 255.0, 1 - color[2] / 255.0, 1)); rp_end();
            R.rt.color[0] = &output; sample(fs, pipeline);
            memcpy(expected[nr], sampled, 4); readback(destination, result, nr++ * 1024);
            R.rt.color[0] = &sources[i]; fb_prepare(pr);
            id<MTLTexture> reused = g_fb_use[0].parentTexture ? g_fb_use[0].parentTexture : g_fb_use[0];
            CHECK(g_fb_n == 1 && reused == snapshot && g_fb_count == copies + want_copies, "a valid key preserves its pixels without another copy");
            R.rt.color[0] = &output; sample(fs, pipeline);
            memcpy(expected[nr], sampled, 4); readback(destination, result, nr++ * 1024);
        }
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "mixed-shape GPU submissions completed");
    CHECK(!mtl_last_commit_error(), "mixed-shape GPU submissions have no error");
    for (int i = 0; i < nr; i++) pixels(result, i * 1024, expected[i], "mixed-shape snapshots keep the correct pixels across eviction, barriers and submissions");
    printf("feedback pressure: %s\n", before == failed ? "PASS" : "FAIL");
    for (int i = 0; i < N; i++) mtl_tex_release(&sources[i]);
    mtl_tex_release(&output);
}
// Stale matches jump 0 -> 4 -> 2 before two shapes miss. Without draw pinning the
// fourth miss destroys the first attachment's snapshot before arguments are encoded.
static void multiattachment_case(NvProg *seed_pr, id<MTLRenderPipelineState> seed_pipeline, NvProg *pr, id<MTLRenderPipelineState> pipeline) {
    int before = failed; NvShader *fs = pr->sh[1];
    for (int i = 0; i < 8; i++) {
        if (g_fbc[i].snap) defer_obj_release(g_fbc[i].snap);
        g_fbc[i] = (fb_cache_t){0};
    }
    g_fbc_next = 0;
    const int widths[] = {4, 5, 6, 7, 8, 5, 5, 7, 8, 6, 9, 10};
    const MTLClearColor colors[] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {0, 0, 0, 1}};
    tex_t sources[12], output;
    for (int i = 0; i < 12; i++) { shaped_target(&sources[i], 99840 + i, NVN_FORMAT_RGBA8, widths[i], 4); mtl_reg_tex(720 + i, &sources[i], NULL); }
    id<MTLTexture> destination = target(&output, 99860);
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); R.has_rt = 1; R.rt.n = 1;
    for (int i = 0; i < 8; i++) {
        R.rt.color[0] = &sources[i]; S.tex[1][0] = 720 + i;
        clear(MTLClearColorMake(1, 1, 1, 1)); fb_prepare(seed_pr);
        R.rt.color[0] = &output; sample(seed_pr->sh[1], seed_pipeline);
    }
    game_barrier();
    for (int i = 0; i < 4; i++) { R.rt.color[0] = &sources[8 + i]; clear(colors[i]); rp_end(); }
    R.rt.n = 4;
    for (int i = 0; i < 4; i++) { R.rt.color[i] = &sources[8 + i]; S.tex[1][i] = 728 + i; }
    uint64_t copies = g_fb_count;
    fb_prepare(pr);
    CHECK(g_fb_n == 4 && g_fb_count == copies + 4, "one four-attachment draw takes exactly four copies");
    // Poison originals but keep the draw's substitutions; all four weighted colours must survive.
    R.rt.n = 1;
    for (int i = 0; i < 4; i++) { R.rt.color[0] = &sources[8 + i]; clear(MTLClearColorMake(1, 0, 1, 1)); rp_end(); }
    for (int i = 1; i < 4; i++) R.rt.color[i] = NULL;
    R.rt.color[0] = &output; sample(fs, pipeline); readback(destination, result, 0);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "multiattachment GPU submission completed");
    CHECK(!mtl_last_commit_error(), "multiattachment GPU submission has no error");
    pixels(result, 0, (const uint8_t[]){17, 34, 68, 255}, "all four pre-overwrite snapshots survive stale-shape jumps and fallback replacement");
    printf("feedback multiattachment: %s\n", before == failed ? "PASS" : "FAIL");
    for (int i = 0; i < 12; i++) mtl_tex_release(&sources[i]);
    mtl_tex_release(&output);
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "struct A {texture2d<float, access::read> source[[id(0)]];};\n"
        "fragment float4 f(float4 p[[position]], constant A&a[[buffer(0)]]) {return a.source.read(uint2(p.xy));}\n"
        "struct A4 {texture2d<float, access::read> a[[id(0)]]; texture2d<float, access::read> b[[id(1)]]; texture2d<float, access::read> c[[id(2)]]; texture2d<float, access::read> d[[id(3)]];};\n"
        "fragment float4 f4(float4 p[[position]], constant A4&a[[buffer(0)]]) {uint2 q=uint2(p.xy); return (a.a.read(q)+2*a.b.read(q)+4*a.c.read(q)+8*a.d.read(q))/15;}\n"
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
    const int slots[] = {0, 128, 255};
    for (int i = 0; i < 3; i++) slot_case(pr, pipeline, slots[i], i);
    reuse_case(pr);
    pressure_case(pr, pipeline);
    NvProg *pr4 = [NvProg new]; NvShader *fs4 = [NvShader new]; pr4->sh[1] = fs4;
    fs4->stage = 1; strcpy(fs4->key, "fedcba9876543210"); fs4->fn = [lib newFunctionWithName:@"f4"];
    fs4->ae = [fs4->fn newArgumentEncoderWithBufferIndex:0]; fs4->arg_len = fs4->ae.encodedLength; fs4->rf.nb = 4;
    for (int i = 0; i < 4; i++) fs4->rf.b[i] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=i, .aid=i, .dtt=MTLTextureType2D};
    d.fragmentFunction = fs4->fn;
    id<MTLRenderPipelineState> pipeline4 = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pipeline4 != nil, "multiattachment consumer pipeline compiles");
    if (pipeline4) multiattachment_case(pr, pipeline, pr4, pipeline4);
    printf("feedback snapshot: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
