// Feedback snapshots and their per-draw argument substitution include separate texture slots; a snapshot is reused until the game's next barrier,
// and a draw that only samples an attachment nothing in its pass has written reads it directly. With the directory test_dmsh_fetch.py --fixture writes, a draw that samples the
// attachment at its own pixel through a projective sample reads it from the tile (fb_tile_plan): a snapshot while its pipeline builds, then none after a barrier, the pass stays
// open, and the GPU reads what an earlier draw of the pass wrote; not with a viewport smaller than the target.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> target(tex_t *t, uint64_t serial) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8,
        .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
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
    mtl_reg_tex(tid, &source, NULL);   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the texture (serial_put's CFBridgingRetain) until mtl_tex_release below, not a leak
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
static id<MTLLibrary> lib_file(NSString *path) {
    NSError *e = nil; NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&e];
    id<MTLLibrary> l = src ? [g_dev newLibraryWithSource:src options:nil error:&e] : nil;
    if (!l) fprintf(stderr, "%s: %s\n", path.UTF8String, e.localizedDescription.UTF8String);
    return l;
}
// Breath of the Wild's decals: the G-buffer target cleared red, an earlier draw of the pass writes green into its left half, then a draw that samples the target at its screen varying
// writes the sample's channels rotated (green from blue, blue from red, red from green): red where it read green, blue where it read red
static void tile_case(const char *dir) {
    int before = failed, tid = 660; NSString *d = @(dir);
    g_msl_dir = d;   // bf_fn loads <key>_bf.metallib from here
    NSData *fj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"facts.json"]];
    NSDictionary *facts = fj ? [NSJSONSerialization JSONObjectWithData:fj options:0 error:nil] : nil;
    id<MTLLibrary> vl = lib_file([d stringByAppendingPathComponent:@"fb_vs.metal"]), fl = lib_file([d stringByAppendingPathComponent:@"fb_fs.metal"]);
    CHECK(facts && vl && fl, "the fixture's shaders compile");
    if (!facts || !vl || !fl) return;
    NvProg *pr = [NvProg new]; NvShader *vs = [NvShader new], *fs = [NvShader new]; pr->sh[0] = vs; pr->sh[1] = fs;
    vs->stage = 0; strcpy(vs->key, "0123456789abcdef_b"); vs->fn = [vl newFunctionWithName:@"main0"]; vs->rf.pos_attr = -1;
    for (NSNumber *k in facts[@"screen"]) vs->rf.scr_locn |= 1u << k.intValue;
    fs->stage = 1; strcpy(fs->key, "fedcba9876543210_b"); fs->fn = [fl newFunctionWithName:@"main0"];
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D};
    for (NSDictionary *c in facts[@"calls"]) { int i = fs->rf.nfc++; fs->rf.fc_reg[i] = (uint8_t)[c[@"tex_reg"] intValue]; fs->rf.fc_locn[i] = (uint8_t)[c[@"varying_locn"] intValue]; if ([c[@"proj"] intValue]) fs->rf.fc_proj |= 1u << i; }
    for (NSDictionary *o in facts[@"outputs"]) fs->rf.fo_color[fs->rf.nfo++] = (uint8_t)[o[@"color"] intValue];
    fs->rf.color_known = 1; fs->rf.color_mask = 1;
    CHECK((vs->rf.scr_locn >> 4 & 1) && fs->rf.nfc == 1 && fs->rf.fc_proj == 1 && fs->rf.fc_locn[0] == 4, "the fixture's facts: screen varying 4 (and 5), one projective sample of 4");
    NSError *e = nil;
    id<MTLLibrary> gl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(0, 1, 0, 1); }\n" options:nil error:&e];
    MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new];
    gd.vertexFunction = [gl newFunctionWithName:@"v"]; gd.fragmentFunction = [gl newFunctionWithName:@"f"]; gd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> green = gl ? [g_dev newRenderPipelineStateWithDescriptor:gd error:&e] : nil;
    CHECK(green != nil, "the green pipeline compiles");
    tex_t source; id<MTLTexture> attachment = target(&source, 99795);
    mtl_reg_tex(tid, &source, NULL);
    id<MTLBuffer> zero = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(zero);   // the variant's argument buffer: the tile read leaves its texture and sampler unread
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid; S.chmask.mask = 0xffffffffu;
    S.has_vp = 1; S.vp[0][0] = 0; S.vp[0][1] = 0; S.vp[0][2] = 4; S.vp[0][3] = 4;
    uint64_t copies = g_fb_count, waits = g_fb_tile_wait, tiles = g_fb_tile_draws;
    clear(MTLClearColorMake(1, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1 && g_fb_tile_wait == waits + 1 && g_fb_tseq != g_draw_seq, "while its pipeline builds, the draw takes a snapshot");
    for (int i = 0; i < 1000; i++) { os_unfair_lock_lock(&g_fb_tp_lock); int done = g_fb_tp_n && g_fb_tp[0].done; os_unfair_lock_unlock(&g_fb_tp_lock); if (done) break; usleep(10000); }
    CHECK(g_fb_tp_n == 1 && g_fb_tp[0].done && g_fb_tp[0].ps, "the tile-reading pipeline is built in the background");
    g_fb_bar++;   // a game barrier: a snapshot would have to be taken again
    clear(MTLClearColorMake(1, 0, 0, 1));
    id first = g_enc;
    enc_r_pso(g_enc, green); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 2, 4});
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    fb_prepare(pr);
    CHECK(g_enc == first && g_fb_n == 0 && g_fb_count == copies + 1 && g_fb_tseq == g_draw_seq && g_fb_tfn && g_fb_tile_draws == tiles + 1 && g_fb_tdesc.fbmask == 1 && g_fb_tdesc.fx[0] == 0,
          "after a barrier the draw reads the tile: no snapshot, the pass stays open");
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> ps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(ps == g_fb_tp[0].ps, "the draw's pipeline is the one built in the background");
    if (ps) { enc_r_pso(g_enc, ps); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, zero.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024);
    S.vp[0][2] = 2; g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_tseq != g_draw_seq, "with a viewport smaller than the target the draw takes a snapshot");
    rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    int left = 1, right = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = (const uint8_t *)result.contents + y * 256 + x * 4;
        if (x < 2) left &= p[0] == 255 && p[1] == 0 && p[2] == 0; else right &= p[0] == 0 && p[1] == 0 && p[2] == 255; }
    CHECK(left, "where the earlier draw of the pass wrote green, the tile read gives green");
    CHECK(right, "elsewhere it gives the cleared red");
    printf("feedback tile read: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
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
    const int slots[] = {0, 128, 255};
    for (int i = 0; i < 3; i++) slot_case(pr, pipeline, slots[i], i);
    reuse_case(pr);
    if (argc == 2) tile_case(argv[1]);
    printf("feedback snapshot: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
