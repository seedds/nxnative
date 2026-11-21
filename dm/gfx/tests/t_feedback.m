// Feedback snapshots and their per-draw argument substitution include separate texture slots and read-only fragment images; a snapshot is reused until the game's next barrier,
// and a draw that only samples an attachment nothing in its pass has written reads it directly. With the directory test_dmsh_fetch.py --fixture writes (folded readable-tile library, entry main0_f), a draw that samples the
// attachment at its own pixel through a projective sample reads it from the tile (fb_tile_plan): a snapshot while its pipeline builds, then none after a barrier, the pass stays
// open, and the GPU reads what an earlier draw of the pass wrote; not with a viewport smaller than the target. The same for a sample at the draw's pixel position times a uniform
// 1 / the target's size, set by an inline update (Splatoon Raiders' ink stamps), on an RG8 attachment sampled through (red, green, zero, one): the tile read fills the missing
// channels as a sample does.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> target_format(tex_t *t, uint64_t serial, int format, int levels) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format,
        .width=4, .height=4, .depth=1, .levels=levels, .samples=1, .flags=format == NVN_FORMAT_R32UI ? NVN_TEXTURE_FLAGS_IMAGE : 0};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    if (format == NVN_FORMAT_R32UI) d.usage |= MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
static id<MTLTexture> target(tex_t *t, uint64_t serial) {
    return target_format(t, serial, NVN_FORMAT_RGBA8, 1);
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
static uint32_t *g_ubo_words;   // a pool page: the uniform buffer that holds the handle of the TEXU case
// slot: a texture or image slot, or for a TEXU binding the uniform-buffer slot whose word 0 holds the handle (encode_args and fb_prepare read it through ubo_bind_handle)
static void slot_case(NvProg *pr, id<MTLRenderPipelineState> pipeline, int slot, int index) {
    int before = failed, tid = 600 + index;
    NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = slot;
    int image = fs->rf.b[0].kind == NVN_RB_IMG, texu = fs->rf.b[0].kind == NVN_RB_TEXU;
    tex_t source, output;
    id<MTLTexture> original = target_format(&source, 99700 + index * 2, image ? NVN_FORMAT_R32UI : NVN_FORMAT_RGBA8, 1);
    id<MTLTexture> destination = target(&output, 99701 + index * 2);
    mtl_reg_tex(tid, &source, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source;
    if (image) S.img[1][slot] = tid;
    else if (texu) { g_ubo_words[0] = (uint32_t)tid; S.ubo[1][slot] = (p_bind_buf){.addr=(uint64_t)(uintptr_t)g_ubo_words, .size=16}; }
    else S.tex[1][slot] = tid;
    clear(MTLClearColorMake(image ? 17 : 1, 0, 0, 1));
    if (image) {
        fs->rf.b[0].rw = 1; fb_prepare(pr);
        CHECK(!g_fb_n && g_enc, "writes stay on the original attachment");
        fs->rf.b[0].rw = 0; fs->rf.b[0].atom = 1; fb_prepare(pr);
        CHECK(!g_fb_n && g_enc, "atomics stay on the original attachment");
        fs->rf.b[0].atom = 0;
    }
    fb_prepare(pr);
    CHECK(!g_enc, "snapshot closes the preceding render pass");
    CHECK(g_fb_n == 1, "feedback attachment receives a snapshot");
    CHECK(g_fb_n == 1 && g_fb_src[0] == original && g_fb_use[0] != original,
          "snapshot substitutes a distinct texture for the registered source");
    rp_end();
    // Overwrite the attachment after the snapshot, then render the sampled value
    // into a distinct target. This makes a missing snapshot fail deterministically
    // even on GPUs that happen to tolerate sampling an attached render target.
    clear(MTLClearColorMake(image ? 29 : 0, 1, 0, 1)); rp_end();
    R.rt.color[0] = &output;
    sample(fs, pipeline);
    uint64_t bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
    CHECK(g_fb_n == 1 && bound == g_fb_rid[0] && bound != original.gpuResourceID._impl,
          "production argument buffer contains the snapshot resource ID");
    readback(destination, result, 0);
    if (image) {
        // A different slot can write the same resource: a snapshot already available for a reader must never redirect a writer.
        fs->rf.b[0].rw = 1; args_dirty_all();
        sample(fs, pipeline);
        bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
        CHECK(bound == original.gpuResourceID._impl, "a writable image bypasses an available snapshot");
        fs->rf.b[0].rw = 0; args_dirty_all();
    }
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
    printf("feedback %s %d: %s\n", texu ? "handle in uniform slot" : "slot", slot, before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_tex_release(&output);
}
// A registered image view names its own level. A shader that needs a different pixel class keeps its normal image binding.
static void image_view_case(NvProg *pr) {
    const int tid = 661;
    tex_t source;
    id<MTLTexture> original = target_format(&source, 99796, NVN_FORMAT_R32UI, 2);
    view_t level1 = {.set_levels=1, .base_level=1, .nlevels=1};
    mtl_reg_image(tid, &source, &level1);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    nvn_rbind *b = &pr->sh[1]->rf.b[0];
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.img[1][b->reg] = tid;
    clear(MTLClearColorMake(11, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a level-1 image does not snapshot a level-0 attachment");
    rp_end();
    R.rt.has_cview[0] = 1; R.rt.cview[0] = level1;
    clear(MTLClearColorMake(17, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_src[0].parentTexture == original &&
          g_fb_src[0].parentRelativeLevel == 1 && g_fb_use[0].width == 2,
          "a level-1 image snapshots its level-1 attachment");
    rp_end();
    b->dpf = NVN_DPF_SINT;
    clear(MTLClearColorMake(29, 0, 0, 1));
    os_unfair_lock_lock(&g_rmu);
    int real; uint64_t rid; id<MTLTexture> signed_view = img_binding(tid, b, &rid, &real);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(real && signed_view.pixelFormat == MTLPixelFormatR32Sint, "the image binding can reinterpret R32Uint as SINT");
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a SINT-class read of the R32Uint attachment keeps its reinterpreted image binding");
    rp_end(); b->dpf = NVN_DPF_UINT;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "image view submission completed without error");
    CHECK(serial_get(source.serial) == original, "the serial table still holds the image target");
    mtl_tex_release(&source);
    CHECK(serial_get(source.serial) == nil, "releasing the image target removes its serial entry");
}
// Mip-chain passes: a draw rendering one level of a texture whose view it samples reads by the levels its sampler's clamp lets it fetch (fb_reach). A clamp to one level other than the
// attachment's reads that level alone, on either side (an upsample: level n from n + 1); a clamp from below the attachment's, or no mip filter, reads the levels below (a downsample);
// a clamp starting at the attachment's level (a pin there, a range or a fractional LOD from there up) and the view's base level rendered with no level pinned read a snapshot of the
// whole view; a range wholly above the attachment's reads the view as it is
static void mip_chain_case(NvProg *pr) {
    const int tid = 671, sid = 672; NvShader *fs = pr->sh[1]; nvn_rbind keep[2] = {fs->rf.b[0], fs->rf.b[1]}; int keep_nb = fs->rf.nb;
    fs->rf.nb = 2; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D}; fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_SAMPT, .reg=0, .aid=1};
    tex_t chain; id<MTLTexture> original = target_format(&chain, 99800, NVN_FORMAT_RGBA8, 3);   // 4x4, 2x2, 1x1
    mtl_reg_tex(tid, &chain, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &chain; R.rt.has_cview[0] = 1; S.tex[1][0] = tid | (uint64_t)sid << 20;
    enum { NONE, VIEW, SNAP };
    struct { int level; float lod_min, lod_max; int minf, want, want_level, want_levels; const char *label; } c[] = {
        {1, 2, 2, 5, VIEW, 2, 1, "an upsample's sampler pins the level above: that level alone"},
        {1, 0, 0, 5, VIEW, 0, 1, "a sampler pinning the level below: that level alone"},
        {1, -1000, 1000, 5, VIEW, 0, 1, "a sampler that clamps nothing: the levels below"},
        {1, 2, 2, 1, VIEW, 0, 1, "no mip filter: the levels below"},
        {0, 1, 1, 5, VIEW, 1, 1, "rendering the base level, the sampler pins level 1: that level alone"},
        {0, 1, 2, 5, NONE, 0, 0, "rendering the base level, a range above it: the view as it is"},
        {0, -1000, 1000, 5, SNAP, 0, 3, "rendering the base level, a clamp from it up: a snapshot of the view"},
        {1, 1, 1, 5, SNAP, 0, 3, "a sampler pinning the attachment's level: a snapshot of the view"},
        {1, 1, 2, 5, SNAP, 0, 3, "a range from the attachment's level up: a snapshot of the view"},
        {1, 1.5f, 1.5f, 3, SNAP, 0, 3, "a fractional pin above the attachment's level: a snapshot of the view"},
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        samp_par sp = {.minf = (uint8_t)c[i].minf, .magf = 1, .ws = 2, .wt = 2, .wr = 2, .lod_min = c[i].lod_min, .lod_max = c[i].lod_max, .aniso = 1}; mtl_reg_samp(sid, &sp);
        R.rt.cview[0] = (view_t){.set_levels=1, .base_level=c[i].level, .nlevels=1};
        clear(MTLClearColorMake(0, 0, 0, 1)); fb_prepare(pr);
        id<MTLTexture> use = g_fb_n ? g_fb_use[0] : nil;
        printf("%s: %d views, level %lu+%lu\n", c[i].label, g_fb_n, (unsigned long)use.parentRelativeLevel, (unsigned long)use.mipmapLevelCount);
        int ok = c[i].want == NONE ? g_fb_n == 0
               : c[i].want == VIEW ? g_fb_n == 1 && use.parentTexture == original && (int)use.parentRelativeLevel == c[i].want_level && (int)use.mipmapLevelCount == c[i].want_levels
               : g_fb_n == 1 && use != original && use.parentTexture != original && (int)use.mipmapLevelCount == c[i].want_levels && use.width == 4;
        CHECK(ok, c[i].label);
        rp_end(); g_fb_bar++;
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "mip-chain submission completed without error");
    fs->rf.nb = keep_nb; fs->rf.b[0] = keep[0]; fs->rf.b[1] = keep[1];
    mtl_tex_release(&chain);
}
// The same clamps read by a draw, with the attachment's level overwritten after its snapshot: each reads the levels its clamp names as they were before this pass
// (red, green, blue for levels 0, 1, 2); the clamps that need no snapshot take none
static void mip_pixels_case(id<MTLLibrary> lib) {
    const int tid = 673, sid = 674;
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "1122334455667788"); fs->fn = [lib newFunctionWithName:@"fmp"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.nb = 2; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D}; fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_SAMPT, .reg=0, .aid=1};
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"v"]; pd.fragmentFunction = fs->fn; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *error = nil; id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    CHECK(pipeline != nil, "mip-chain sampling pipeline compiles"); if (!pipeline) return;
    tex_t chain, output; id<MTLTexture> original = target_format(&chain, 99802, NVN_FORMAT_RGBA8, 3); id<MTLTexture> destination = target(&output, 99803);
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    mtl_reg_tex(tid, &chain, NULL);
    static const uint8_t colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    struct { int level; float lo, hi; int direct; uint8_t want[4]; const char *label; } c[] = {   // the shader asks for LOD 2; the clamp decides
        {1, 2, 2, 1, {0, 0, 255, 255}, "a pin above the attachment reads that level, no snapshot"},
        {1, 0, 0, 1, {255, 0, 0, 255}, "a pin below the attachment reads that level, no snapshot"},
        {0, 1, 1, 1, {0, 255, 0, 255}, "a pin above the base attachment reads that level, no snapshot"},
        {0, 1, 2, 1, {0, 0, 255, 255}, "a range above the attachment reads the view as it is, no snapshot"},
        {1, 1, 1, 0, {0, 255, 0, 255}, "a pin at the attachment reads its level as it was before the pass"},
        {1, 1, 2, 0, {0, 0, 255, 255}, "a range from the attachment up reads the level the shader asks for"},
        {1, 1.5f, 1.5f, 0, {0, 128, 128, 255}, "a fractional pin blends the attachment's level as it was and the one above"},
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        for (int m = 0; m < 3; m++) { uint8_t data[4 * 4 * 4]; int width = 4 >> m;
            for (int q = 0; q < width * width; q++) memcpy(data + q * 4, colors[m], 4);
            [original replaceRegion:MTLRegionMake2D(0, 0, width, width) mipmapLevel:m withBytes:data bytesPerRow:width * 4]; }
        samp_par sp = {.minf = 5, .magf = 1, .ws = 2, .wt = 2, .wr = 2, .lod_min = c[i].lo, .lod_max = c[i].hi, .aniso = 1}; mtl_reg_samp(sid, &sp);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); g_fb_bar++;
        R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &chain; R.rt.has_cview[0] = 1; R.rt.cview[0] = (view_t){.set_levels=1, .base_level=c[i].level, .nlevels=1};
        S.tex[1][0] = tid | (uint64_t)sid << 20;
        const uint8_t *k = colors[c[i].level]; clear(MTLClearColorMake(k[0] / 255., k[1] / 255., k[2] / 255., 1));
        uint64_t copies = g_fb_count;
        fb_prepare(pr); rp_end();
        clear(MTLClearColorMake(1, 0, 1, 1)); rp_end();   // the pass's write to the attachment's level after the snapshot
        R.rt.color[0] = &output; R.rt.has_cview[0] = 0;
        sample(fs, pipeline); readback(destination, result, 0);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "mip-chain sampling submission completed without error");
        int valid = 1;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) for (int q = 0; q < 4; q++) valid &= abs((int)((const uint8_t *)result.contents)[y * 256 + x * 4 + q] - c[i].want[q]) <= 1;
        CHECK(valid, c[i].label);
        CHECK(c[i].direct ? g_fb_count == copies : g_fb_count == copies + 1, c[i].direct ? "no snapshot" : "one snapshot");
        printf("feedback mip clamp %g..%g at level %d: %s\n", c[i].lo, c[i].hi, c[i].level, valid ? "PASS" : "FAIL");
    }
    S.tex[1][0] = 0; mtl_tex_release(&chain); mtl_tex_release(&output);
}
// A pass rendering the base level of the view it samples through a sampler not known here (no combined handle's): a snapshot of the whole view, its levels and layers (2D, a 2D-array
// view of layers 1-2, a cube), keyed by serial, base level and layer, so a contracted view of the same key takes a snapshot of its own shape and the full one again its own
static void base_level_case(id<MTLLibrary> lib, int first, int sampled, int index, int shape) {
    int before = failed, tid = 780 + index, layers = shape == 2 ? 6 : shape == 1 ? 3 : 1, layer = shape == 2 ? 4 : shape == 1 ? 2 : 0;
    tex_t source = {.magic=MAGIC('T','E','X',0), .serial=99900 + index}, output;
    source.d = (nvn_texdesc){.target=shape == 2 ? NVN_TEXTURE_TARGET_CUBEMAP : shape == 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8,
        .width=16, .height=16, .depth=layers, .levels=4, .samples=1};
    MTLTextureDescriptor *td = tex_desc(&source, 0); td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td];
    CHECK(texture != nil, "base-level feedback target created"); if (!texture) return;
    res_add(texture); mtl_tex_init(&source); serial_put(source.serial, texture);
    static const uint8_t colors[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    for (int m = 0; m < 4; m++) { uint8_t data[16 * 16 * 4]; int width = 16 >> m;
        for (int q = 0; q < layers; q++) {
            for (int p = 0; p < width * width; p++) memcpy(data + p * 4, q == layer ? colors[m] : (const uint8_t[]){0, 0, 0, 255}, 4);
            [texture replaceRegion:MTLRegionMake2D(0, 0, width, width) mipmapLevel:m slice:q withBytes:data bytesPerRow:width * 4 bytesPerImage:width * width * 4]; } }
    view_t view = {.set_levels=1, .base_level=first, .nlevels=4 - first};
    if (shape == 1) { view.set_layers = 1; view.min_layer = 1; view.nlayers = 2; }
    mtl_reg_tex(tid, &source, &view);
    id<MTLTexture> destination = target(&output, 99910 + index);
    id<MTLBuffer> result = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(result);
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "aabbccddeeff0011");
    fs->fn = [lib newFunctionWithName:shape == 2 ? @"fmc2" : shape == 1 ? @"fma2" : sampled == 1 ? @"fm1" : @"fm2"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .aid=0, .dtt=shape == 2 ? MTLTextureTypeCube : shape == 1 ? MTLTextureType2DArray : MTLTextureType2D};
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"v"]; pd.fragmentFunction = fs->fn; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *error = nil; id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    CHECK(pipeline != nil, "base-level feedback pipeline compiles"); if (!pipeline) return;
    view_t attached = {.set_levels=1, .base_level=first, .nlevels=1, .set_layers=1, .min_layer=layer, .nlayers=1};
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); g_fb_bar++;
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid; R.rt.has_cview[0] = 1; R.rt.cview[0] = attached;
    const uint8_t *k = colors[first]; clear(MTLClearColorMake(k[0] / 255., k[1] / 255., k[2] / 255., 1));
    fb_prepare(pr); CHECK(g_fb_n == 1 && g_fb_use[0].mipmapLevelCount == 4u - first && g_fb_use[0].textureType == (MTLTextureType)fs->rf.b[0].dtt, "the draw samples a snapshot of the whole view, as the type the shader declares"); rp_end();
    view_t one = view; one.nlevels = 1;   // the same serial, base level and layer, one level
    mtl_reg_tex(tid, &source, &one); fb_prepare(pr); rp_end();
    R.rt.color[0] = &output; R.rt.has_cview[0] = 0;
    sample(fs, pipeline); readback(destination, result, 0);
    R.rt.color[0] = &source; R.rt.has_cview[0] = 1; R.rt.cview[0] = attached;
    mtl_reg_tex(tid, &source, &view); fb_prepare(pr); rp_end();
    for (int m = 0; m < 4; m++) for (int q = 0; q < layers; q++) {   // the pass and later ones overwrite every level and layer
        R.rt.cview[0].base_level = m; R.rt.cview[0].min_layer = q; clear(MTLClearColorMake(1, 0, 1, 1)); rp_end(); }
    R.rt.color[0] = &output; R.rt.has_cview[0] = 0;
    sample(fs, pipeline); readback(destination, result, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "base-level feedback submission completed without error");
    pixels(result, 0, colors[first], "a one-level view of the same key reads its own snapshot");
    pixels(result, 1024, colors[first + sampled], "the whole view's snapshot holds the level the shader asks for as it was");
    printf("feedback base level, shape %d, view base %d, sampled %d: %s\n", shape, first, sampled, before == failed ? "PASS" : "FAIL");
    S.tex[1][0] = 0; mtl_tex_release(&source); mtl_tex_release(&output);
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
    { tex_t output, depth; target(&output, 99791); target(&depth, 99792); mtl_reg_tex(651, &depth, NULL);
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
    g_msl_dir = d;   // bf_fn loads <key>_bf.metallib from here and specializes @"main0_f"
    NSData *fj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"facts.json"]];
    NSDictionary *facts = fj ? [NSJSONSerialization JSONObjectWithData:fj options:0 error:nil] : nil;
    id<MTLLibrary> vl = lib_file([d stringByAppendingPathComponent:@"fb_vs.metal"]), fl = lib_file([d stringByAppendingPathComponent:@"fb_fs.metal"]);
    CHECK(facts && vl && fl, "the fixture's shaders compile");
    if (!facts || !vl || !fl) return;
    NSError *bfe = nil;
    id<MTLLibrary> bfl = lib_from_file([d stringByAppendingPathComponent:@"fedcba9876543210_bf.metallib"], &bfe);
    CHECK(bfl && [bfl.functionNames containsObject:@"main0_f"], "the folded tile-read library exposes main0_f");
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
    {   // bf_fn's numeric memo: the same shader, redirections, mask and map give the function the draw got; another mask is another specialization, not the remembered one
        int fx[32] = {0}; for (int i = 0; i < 32; i++) fx[i] = g_fb_tdesc.fx[i]; uint8_t lm[8]; memcpy(lm, g_fb_tlmap, 8); // NOLINT(bugprone-signed-char-misuse): these numeric int8_t redirects include the sentinel -1
        id<MTLFunction> a = bf_fn(fs, fx, g_fb_tdesc.fbmask, lm), b = bf_fn(fs, fx, g_fb_tdesc.fbmask, lm), c = bf_fn(fs, fx, (uint8_t)(g_fb_tdesc.fbmask | 2), lm);
        CHECK(a && a == g_fb_tfn && b == a && c != a, "the readable-tile function is remembered by its numbers, and a different attachment mask is not taken for it");
    }
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> ps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(ps == g_fb_tp[0].ps, "the draw's pipeline is the one built in the background");
    if (ps) { enc_r_pso(g_enc, ps); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, zero.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024);
    // An image and a sampled texture can share a slot number. Only the sampled binding reads the tile.
    fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_IMG, .reg=0, .aid=1, .dtt=MTLTextureType2D};
    fs->rf.nb = 2; S.img[1][0] = tid; g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_tseq == g_draw_seq && g_fb_n == 1 && g_fb_src[0] == attachment && g_fb_count == copies + 2,
          "sampled slot 0 reads the tile while image slot 0 snapshots the same attachment");
    rp_end(); fs->rf.nb = 1; S.img[1][0] = 0;
    // A draw that opens its pass: the first learns the formats of the pass that opens for it and takes a snapshot, the next plans its tile read with no pass open and the pass loads what it reads
    id<MTLBuffer> result2 = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result2);
    g_fb_bar++; clear(MTLClearColorMake(1, 0, 0, 1));
    enc_r_pso(g_enc, green); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 2, 4}); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    rp_end();
    uint64_t ecopies = g_fb_count, eok = g_fb_early_n;
    S.chmask.mask = 0; fb_prepare(pr); S.chmask.mask = 0xffffffffu;
    CHECK(!g_fb_n && g_fb_count == ecopies && g_fb_learn_seq != g_draw_seq && g_fb_early_seq != g_draw_seq, "a draw that opens its pass and writes no attachment it samples learns nothing");
    fb_prepare(pr);
    CHECK(!g_enc && g_fb_n == 1 && g_fb_count == ecopies + 1 && g_fb_learn_seq == g_draw_seq && g_fb_tseq != g_draw_seq, "the first draw that opens its pass takes a snapshot");
    CHECK(rp_begin_ex(0) != nil && fb_early_after() && g_fb_el[g_fb_early_key & 63].ok && g_fb_el[g_fb_early_key & 63].pi.cfmt[0] == MTLPixelFormatRGBA8Unorm && g_fb_el[g_fb_early_key & 63].pi.tw == 4, "and the pass that opened is noted");
    rp_end(); g_fb_bar++;
    fb_prepare(pr);
    CHECK(!g_enc && g_fb_n == 0 && g_fb_count == ecopies + 1 && g_fb_early_seq == g_draw_seq && g_fb_tseq == g_draw_seq && g_fb_tfn, "the next one plans its tile read with no pass open: no snapshot");
    CHECK(rp_begin_ex(0) != nil && fb_early_after() && g_fb_early_n == eok + 1, "the pass that opens is the one it planned for");
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> eps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(eps == g_fb_tp[0].ps, "its pipeline is the one built for the open-pass case");
    udc_note_draw(pr, 1);   // the draw path's note that this draw writes attachment 0: a pass that loaded it and wrote nothing stores nothing
    if (eps && g_enc) { enc_r_pso(g_enc, eps); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, zero.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result2, 0, 256, 1024);
    g_fb_bar++; fb_prepare(pr);
    CHECK(g_fb_early_seq == g_draw_seq, "planned again");
    g_fb_el[g_fb_early_key & 63].pi.dfmt = 1;   // as if the pass it planned for had a depth attachment
    CHECK(rp_begin_ex(0) != nil && !fb_early_after() && g_fb_el[g_fb_early_key & 63].fails == 1 && !g_fb_el[g_fb_early_key & 63].pi.dfmt, "a pass other than the planned one is refused, counted and noted");
    rp_end(); ecopies = g_fb_count; g_fb_noearly = 1; fb_prepare(pr); g_fb_noearly = 0;
    CHECK(!g_enc && g_fb_n == 1 && g_fb_count == ecopies + 1 && g_fb_tseq != g_draw_seq, "and the draw goes back to a snapshot");
    g_fb_el[g_fb_early_key & 63].fails = 3; g_fb_bar++; ecopies = g_fb_count; fb_prepare(pr);
    CHECK(g_fb_n == 1 && g_fb_count == ecopies + 1 && g_fb_early_seq != g_draw_seq, "after three such passes it plans no more");
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
    left = right = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = (const uint8_t *)result2.contents + y * 256 + x * 4;
        if (x < 2) left &= p[0] == 255 && p[1] == 0 && p[2] == 0; else right &= p[0] == 0 && p[1] == 0 && p[2] == 255; }
    CHECK(left && right, "the draw that opened its pass read what the pass loaded: green on the left, red on the right");
    printf("feedback tile read: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
}
// The channel mappings a tile read can stand for: the format's channels in place, the missing ones as they are or as the 0, 0, 1 a sample fills in
static void swizzle_case(void) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:4 height:4 mipmapped:NO]; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> rg = [g_dev newTextureWithDescriptor:d]; d.pixelFormat = MTLPixelFormatRGBA8Unorm; id<MTLTexture> rgba = [g_dev newTextureWithDescriptor:d];
    #define SW(t, r, g, b, a) [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a)]
    CHECK(sw_tile(rg) && sw_tile(SW(rg, Red, Green, Zero, One)) && sw_tile(SW(rg, Red, Green, Blue, One)), "an RG8 view through (red, green, zero, one) reads like the tile");
    CHECK(!sw_tile(SW(rg, Red, Green, One, One)) && !sw_tile(SW(rg, Green, Red, Zero, One)) && !sw_tile(SW(rgba, Red, Green, Blue, One)), "not with other channels, swapped ones or alpha 1 of RGBA8");
    #undef SW
}
// Splatoon Raiders' ink stamps (GitHub #1063): the RG8 map cleared red, an earlier draw of the pass writes green into its left half, then a draw that samples the map at gl_FragCoord
// times c[6].xy writes red where it read green, black where it read red, and green where a missing channel read other than 0, 0, 1. Memory holds a scale of 1/8; the inline update
// of this submission sets 1/4, the map's size: the draw is proven only once that update is queued
static void fpos_case(const char *dir) {
    int before = failed, tid = 661; NSString *d = @(dir);
    g_msl_dir = d;
    NSData *fj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"pos.json"]];
    NSDictionary *facts = fj ? [NSJSONSerialization JSONObjectWithData:fj options:0 error:nil] : nil;
    id<MTLLibrary> vl = lib_file([d stringByAppendingPathComponent:@"fb_vs.metal"]), fl = lib_file([d stringByAppendingPathComponent:@"fb_pos.metal"]);
    CHECK(facts && vl && fl, "the pixel-position fixture compiles");
    if (!facts || !vl || !fl) return;
    NSError *bfe = nil;
    id<MTLLibrary> bfl = lib_from_file([d stringByAppendingPathComponent:@"fedcba9876543211_bf.metallib"], &bfe);
    CHECK(bfl && [bfl.functionNames containsObject:@"main0_f"], "the folded pixel-position library exposes main0_f");
    NvProg *pr = [NvProg new]; NvShader *vs = [NvShader new], *fs = [NvShader new]; pr->sh[0] = vs; pr->sh[1] = fs;
    vs->stage = 0; strcpy(vs->key, "0123456789abcdee_b"); vs->fn = [vl newFunctionWithName:@"main0"]; vs->rf.pos_attr = -1;
    fs->stage = 1; strcpy(fs->key, "fedcba9876543211_b"); fs->fn = [fl newFunctionWithName:@"main0"];
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D};
    for (NSDictionary *c in facts[@"calls"]) { int i = fs->rf.nfc++; NSArray *f = c[@"frag_pos"]; fs->rf.fc_reg[i] = (uint8_t)[c[@"tex_reg"] intValue]; fs->rf.fc_locn[i] = 0xff;
        if (f.count == 2) { fs->rf.fc_fpos |= 1u << i; for (int q = 0; q < 2; q++) fs->rf.fc_scale[i][q] = (uint32_t)([f[q][0] intValue] << 16 | [f[q][1] intValue] << 2 | [f[q][2] intValue]); } }
    for (NSDictionary *o in facts[@"outputs"]) fs->rf.fo_color[fs->rf.nfo++] = (uint8_t)[o[@"color"] intValue];
    fs->rf.color_known = 1; fs->rf.color_mask = 1;
    CHECK(fs->rf.nfc == 1 && fs->rf.fc_fpos == 1 && fs->rf.fc_scale[0][0] == (6 << 2) && fs->rf.fc_scale[0][1] == (6 << 2 | 1), "the fixture's facts: one sample at the pixel position times c[6].xy");
    NSError *e = nil;
    id<MTLLibrary> gl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(0, 1, 0, 1); }\n" options:nil error:&e];
    MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new];
    gd.vertexFunction = [gl newFunctionWithName:@"v"]; gd.fragmentFunction = [gl newFunctionWithName:@"f"]; gd.colorAttachments[0].pixelFormat = MTLPixelFormatRG8Unorm;
    id<MTLRenderPipelineState> green = gl ? [g_dev newRenderPipelineStateWithDescriptor:gd error:&e] : nil;
    CHECK(green != nil, "the green pipeline compiles");
    tex_t source; id<MTLTexture> attachment = target_format(&source, 99796, NVN_FORMAT_RG8, 1);
    mtl_reg_tex(tid, &source, NULL);
    { void *tv = NULL; tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; MTLTextureSwizzleChannels w = t.swizzle;
      printf("feedback pixel position: the RG8 map is sampled through channel mapping %d %d %d %d\n", (int)w.red, (int)w.green, (int)w.blue, (int)w.alpha); }
    float *mem = NULL; if (posix_memalign((void **)&mem, (size_t)getpagesize(), (size_t)getpagesize())) abort(); memset(mem, 0, (size_t)getpagesize());
    mem[24] = mem[25] = 0.125f; mtl_pool_add(mem, (uint64_t)getpagesize());   // c[6].xy in memory: 1/8
    id<MTLBuffer> ub = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(ub); ((float *)ub.contents)[20] = 1;   // the support buffer's render scale (_m5[0].x), and c[6] read by the GPU
    ((float *)ub.contents)[24] = ((float *)ub.contents)[25] = 0.25f;
    id<MTLArgumentEncoder> ae = [fs->fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [g_dev newBufferWithLength:MAX(ae.encodedLength, 256ul) options:MTLResourceStorageModeShared]; res_add(args);
    [ae setArgumentBuffer:args offset:0]; [ae setBuffer:ub offset:0 atIndex:2]; [ae setBuffer:ub offset:0 atIndex:3]; [ae setBuffer:ub offset:0 atIndex:4];
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid; S.chmask.mask = 0xffffffffu;
    S.ubo[1][0] = (p_bind_buf){.stage=1, .index=0, .addr=(uint64_t)mem, .size=256};
    S.has_vp = 1; S.vp[0][0] = 0; S.vp[0][1] = 0; S.vp[0][2] = 2; S.vp[0][3] = 2;   // a viewport smaller than the target does not matter to gl_FragCoord
    uint64_t copies = g_fb_count, waits = g_fb_tile_wait, tiles = g_fb_tile_draws;
    clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1 && g_fb_tile_wait == waits, "with a scale of 1/8 the draw is not its pixel's: a snapshot, no tile-reading pipeline");
    float quarter[2] = {0.25f, 0.25f}; mtl_sub_update(quarter, (uint64_t)(mem + 24), 8);
    g_fb_bar++; clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 2 && g_fb_tile_wait == waits + 1, "the inline update's 1/4 proves it; while the pipeline builds, a snapshot");
    for (int i = 0; i < 1000; i++) { os_unfair_lock_lock(&g_fb_tp_lock); int done = 1; for (int k = 0; k < g_fb_tp_n; k++) done &= g_fb_tp[k].done; os_unfair_lock_unlock(&g_fb_tp_lock); if (done) break; usleep(10000); }
    g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1));
    id first = g_enc;
    enc_r_pso(g_enc, green); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 2, 4});
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    fb_prepare(pr);
    CHECK(g_enc == first && g_fb_n == 0 && g_fb_tseq == g_draw_seq && g_fb_tfn && g_fb_tile_draws == tiles + 1 && g_fb_tdesc.fbmask == 1 && g_fb_tdesc.fx[0] == 0,
          "after a barrier the draw reads the tile: no snapshot, the pass stays open");
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> ps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(ps != nil, "the tile-reading pipeline is there");
    if (ps) { enc_r_pso(g_enc, ps); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, args.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    int left = 1, right = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = (const uint8_t *)result.contents + y * 256 + x * 2;
        if (x < 2) left &= p[0] == 255 && p[1] == 0; else right &= p[0] == 0 && p[1] == 0; }
    CHECK(left, "where the earlier draw wrote green, the tile read gives green, and blue 0 and alpha 1");
    CHECK(right, "elsewhere the cleared red, with blue 0 and alpha 1");
    printf("feedback pixel-position tile read: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_pool_del(mem); mtl_drain(); free(mem);
}
// A frame that snapshots more keys than the cache has entries, in several sizes (Tears of the Kingdom: 4-5 a frame), makes no texture once each size has one: a new key takes an entry
// whose texture has its format and size. Taking the entries in turn made one for nearly every snapshot (GitHub #931). Entries a later draw can still reuse are taken last
static void shape_case(void) {
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
    static const NSUInteger size[5][2] = {{64, 32}, {32, 16}, {16, 8}, {64, 32}, {8, 4}};
    NSMutableSet *made = [NSMutableSet set]; int fresh, bad = 0;
    for (int frame = 0; frame < 6; frame++) {
        g_fb_bar++;   // the game's barrier before each snapshot draw, as in Tears of the Kingdom's shrines
        for (int k = 0; k < 5; k++, g_fb_bar++) {
            g_draw_seq++;   // each snapshot a draw of its own: an entry handed to the current draw is not taken for another key
            fb_cache_t *e = fb_snap(1000 + frame * 5 + k, 0, 0, MTLPixelFormatRGBA8Unorm, size[k][0], size[k][1], 0, &fresh);
            bad += !e || !fresh || e->snap.width != size[k][0] || e->snap.height != size[k][1];
            if (e) [made addObject:[NSValue valueWithPointer:(__bridge void *)e->snap]];
        }
    }
    CHECK(!bad, "every new key gets a texture of its size");
    CHECK(made.count == 4, "30 snapshots of 4 sizes over 6 frames make one texture per size, not one each");
    g_draw_seq++; fb_cache_t *a = fb_snap(1, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh);
    g_draw_seq++; fb_cache_t *b = fb_snap(2, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh);
    CHECK(a != b, "a live snapshot is not taken for another key while a stale entry is free");
    fb_cache_t *a2 = fb_snap(1, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh);
    CHECK(a2 == a && !fresh, "the live snapshot is reused until the next barrier");
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
}
// One draw samples two attachments of one format and size while every snapshot entry is live (eight keys since the last barrier, in this submission) and only one entry has that shape:
// the first attachment's, cached from an earlier draw or copied for this one. Each binding must get a snapshot of its own; the second key took the first one's entry and copied its
// attachment over it, so both bindings sampled the second attachment (PR #813)
static void same_draw_case(int cached_first) {
    int before = failed, tid = 760 + cached_first * 2, fresh;
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs; fs->stage = 1; strcpy(fs->key, "fedcba9876543210");
    fs->rf.nb = 2; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D}; fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=1, .aid=1, .dtt=MTLTextureType2D};
    tex_t a, b; id<MTLTexture> ta = target(&a, 99850 + cached_first * 2), tb = target(&b, 99851 + cached_first * 2);
    mtl_reg_tex(tid, &a, NULL); mtl_reg_tex(tid + 1, &b, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1;
    R.rt.color[0] = &a; clear(MTLClearColorMake(1, 0, 0, 1)); rp_end();
    R.rt.color[0] = &b; clear(MTLClearColorMake(0, 1, 0, 1)); rp_end();
    for (int i = 0; i < 8; i++) {   // earlier draws of this submission: the first attachment's snapshot (or another key's of its shape), then seven other shapes
        g_draw_seq++;
        fb_cache_t *e = fb_snap(i == 0 && cached_first ? a.serial : 5000 + (uint64_t)i, 0, 0, ta.pixelFormat, 4 + (NSUInteger)i, 4, 0, &fresh);
        CHECK(e && fresh, "an earlier draw takes a snapshot entry");
        if (e && i == 0 && cached_first) enc_b_copy_texture(blit(), ta, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), e->snap, 0, 0, MTLOriginMake(0, 0, 0));
    }
    R.rt.n = 2; R.rt.color[0] = &a; R.rt.color[1] = &b; S.tex[1][0] = tid; S.tex[1][1] = tid + 1; S.chmask.mask = 0xffffffffu;
    uint64_t copies = g_fb_count;
    fb_prepare(pr);
    CHECK(g_fb_n == 2 && g_fb_src[0] == ta && g_fb_src[1] == tb, "the draw samples snapshots of both attachments");
    CHECK(g_fb_count == copies + 2 - (uint64_t)cached_first, cached_first ? "the cached snapshot is reused and the other copied" : "both are copied");
    CHECK(g_fb_n == 2 && g_fb_use[0] != g_fb_use[1], "each attachment gets a snapshot of its own");
    if (g_fb_n == 2) { readback(g_fb_use[0], result, 0); readback(g_fb_use[1], result, 1024); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    pixels(result, 0, (const uint8_t[]){255, 0, 0, 255}, "the first attachment's snapshot holds its red");
    pixels(result, 1024, (const uint8_t[]){0, 255, 0, 255}, "the second attachment's snapshot holds its green");
    printf("feedback snapshots of one draw (%s first): %s\n", cached_first ? "cached" : "fresh", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&a); mtl_tex_release(&b);
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
}
// A pool shader may name any registered texture, the depth attachment too, so it counts as reading the attachment (and a later draw writing depth ends the pass) only once a shader has read that
// texture through a pool, under whichever object: Ocarina of Time keeps its depth textures in the pool every material shader reads, and read none of them there, at about 100 extra passes a frame
static void pool_depth_case(void) {
    tex_t colour, depth, same, other;
    target(&colour, 99800);
    depth = (tex_t){.magic=MAGIC('T','E','X',0), .serial=99801};
    depth.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_DEPTH32F, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    id<MTLTexture> dt = [g_dev newTextureWithDescriptor:tex_desc(&depth, 0)]; res_add(dt); mtl_tex_init(&depth); serial_put(depth.serial, dt);
    static pool_t pool; depth.pool = &pool; depth.off = 0x1000;
    same = depth; same.serial = 99802; same.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;   // a second object over the same storage, one Metal texture (alias_key)
    other = depth; other.serial = 99803; other.off = 0x2000;
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs; fs->rf.pool = 1; fs->rf.bindless = 1;
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour; R.rt.depth = &depth; S.ds.dt = S.ds.dw = 1;
    clear(MTLClearColorMake(0, 0, 0, 1));
    fb_prepare(pr); fb_prepare(pr);
    CHECK(g_enc, "a pool shader that has read no depth texture leaves the pass open");
    mtl_reg_tex(710, &other, NULL); pool_ds_note(710);
    fb_prepare(pr); fb_prepare(pr);
    CHECK(g_enc, "nor does reading another depth texture end it");
    CHECK(pool_ds_read(&other) && !pool_ds_read(&depth), "the set answers for the texture read");
    mtl_reg_tex(711, &same, NULL); pool_ds_note(711);
    CHECK(pool_ds_read(&depth), "another object over the same storage counts as the attachment");
    fb_prepare(pr);
    CHECK(g_enc && g_fb_dread, "a draw that may read it marks the pass");
    fb_prepare(pr);
    CHECK(!g_enc, "and the next draw writing depth ends it, as a tile-based store would show it the write");
    rp_end(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "that submission completed");
}
// A replaced pool table comes back only once the submission that may read it has completed, and only at its own size; the readback cache makes room with its oldest buffers instead of turning new sizes away
static void reuse_tables_case(void) {
    uint64_t done0 = g_done, fence0 = g_fence, made0 = g_pool_made, reused0 = g_pool_reused;
    id<MTLBuffer> a = pool_table_take(1000);
    CHECK(a && a.length == 0x10000 && g_pool_made == made0 + 1, "a new table, its size rounded up to 64 KB");
    pool_table_give(a, -1, 0);
    id<MTLBuffer> b = pool_table_take(1000);
    CHECK(b != a && g_pool_made == made0 + 2, "not while the submission that may read it runs");
    g_done = g_fence + 1;
    id<MTLBuffer> c = pool_table_take(1000);
    CHECK(c == a && g_pool_reused == reused0 + 1, "the same buffer once its fence has passed");
    id<MTLBuffer> d = pool_table_take(0x10001);
    CHECK(d != a && d.length == 0x20000, "a larger table is its own buffer");
    pool_table_give(b, -1, 0); pool_table_give(c, -1, 0); pool_table_give(d, -1, 0);
    g_done = done0; g_fence = fence0;
}
// A cut with earlier work still running stays a boundary for the game's timestamps, and commits only when the GPU has run out; a report after it takes that boundary's time
static void cut_boundary_case(void) {
    tex_t colour; target(&colour, 99810);
    id<MTLBuffer> rep = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(rep);
    CHECK(ts_on(), "segment mode has a timestamp heap");
    // nothing of the cases before this one may still be on its way: a submission the encode thread kept open for the next one (merged submissions) reports back a moment later than
    // one committed at once, and the case counts g_seg_inflight itself
    rec_drain(); for (int i = 0; i < 20000 && atomic_load(&g_seg_inflight); i++) usleep(100);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour;
    clear(MTLClearColorMake(0, 0, 0, 1)); rp_end();
    id cb0 = g_cb;
    mtl_sub_counter((uint64_t)rep.contents, 0x10);
    atomic_fetch_add(&g_seg_inflight, 1);
    g_stat_draw += (uint64_t)cut_draws(); sub_cut();
    CHECK(g_cb == cb0 && g_seg == 1, "with work running: a boundary, no commit");
    CHECK(g_sclk && g_sclk->nb == 2, "and its timestamp slot");
    clear(MTLClearColorMake(0, 1, 0, 1)); rp_end();
    mtl_sub_counter((uint64_t)rep.contents + 16, 0x10);
    atomic_fetch_sub(&g_seg_inflight, 1);
    g_stat_draw += (uint64_t)cut_draws(); sub_cut();
    CHECK(g_cb != cb0 && g_seg == 2, "with nothing running: a commit");
    clear(MTLClearColorMake(0, 0, 1, 1)); rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the segments completed");
    uint64_t r0[2], r1[2]; memcpy(r0, rep.contents, 16); memcpy(r1, (uint8_t *)rep.contents + 16, 16);
    CHECK(r0[1] && r1[1] > r0[1], "two reports in one command buffer read two times");
}

// With Metal encoding on its own thread, submissions that reach it while the GPU still runs an earlier command buffer are held and go to the queue in one commit call (nvn_rec.h,
// held submissions): their notes and signals follow it, and each one's reports still read its own part of the GPU time. Anything else for the queue recorded inside a later
// submission (an upload batch, here a queue block) goes after the held ones and before that submission's own command buffer, as when each was committed on its own: the first
// version continued the next submission in the held command buffer, and an upload recorded inside it went to the GPU after the draws recorded before it. The encode thread is held
// by a queue block until the submissions are recorded, and "an earlier command buffer runs" is the counter raised by hand
static void merge_case(void) {
    if (!rec_on()) return;
    tex_t colour; target(&colour, 99820);
    id<MTLBuffer> rep = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(rep);
    rec_drain(); for (int i = 0; i < 20000 && (atomic_load(&g_rx_fly) || atomic_load(&g_seg_inflight)); i++) usleep(100);
    uint64_t cbs0 = g_rx_cbs, merged0 = g_rx_merged;
    dispatch_semaphore_t gate = dispatch_semaphore_create(0);
    atomic_fetch_add(&g_rx_busy_hold, 1);
    rec_queue_block(^{ dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); }); rec_publish();
    uint64_t f[3];
    for (int k = 0; k < 3; k++) {
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour;
        clear(MTLClearColorMake(k == 0, k == 1, k == 2, 1)); rp_end();
        mtl_sub_counter((uint64_t)rep.contents + 16 * (uint64_t)k, 0);   // a timestamp at the end of its (only) segment: its command buffer's end, or its own part of a shared one
        f[k] = mtl_sub_commit();
    }
    dispatch_semaphore_signal(gate);
    // (polled, not waited for: a waiter that wakes on the queue's signal completes the records itself, with its own clock for every report: post_early)
    for (int i = 0; i < 50000 && g_done < f[2]; i++) usleep(100);
    CHECK(g_done >= f[2], "three submissions behind running work complete");
    atomic_fetch_sub(&g_rx_busy_hold, 1);
    rec_drain();
    CHECK(g_rx_merged - merged0 == 3 && g_rx_cbs - cbs0 == 1, "they went to the queue in one commit");
    CHECK(atomic_load(&g_sub_cmt) >= f[2], "their commit notes are passed");
    uint64_t r[3][2]; memcpy(r, rep.contents, sizeof r);
    CHECK(r[0][1] && r[1][1] >= r[0][1] && r[2][1] >= r[1][1] && r[2][1] > r[0][1], "each one's report reads its own end, not the shared command buffer's");
    // a queue operation recorded inside the next submission, after its first pass: the held submission goes to the queue before it, and that submission's own command buffer,
    // which holds the pass, after it (an upload batch flushed in the middle of a submission is committed before the draws it serves, wherever in the submission they were recorded)
    rec_drain(); for (int i = 0; i < 20000 && (atomic_load(&g_rx_fly) || atomic_load(&g_seg_inflight)); i++) usleep(100);
    cbs0 = g_rx_cbs; merged0 = g_rx_merged; __block uint64_t held_at_block = ~0ull; __block int pend_at_block = -1, nb_at_block = -1;
    atomic_fetch_add(&g_rx_busy_hold, 1);
    rec_queue_block(^{ dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); }); rec_publish();
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour; clear(MTLClearColorMake(1, 1, 0, 1)); rp_end(); uint64_t fa = mtl_sub_commit();
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour; clear(MTLClearColorMake(0, 1, 1, 1)); rp_end();
    rec_queue_block(^{ held_at_block = g_rx_cbs; pend_at_block = g_rx_npend; nb_at_block = g_rx_last_nb; }); rec_publish();
    clear(MTLClearColorMake(1, 0, 1, 1)); rp_end(); uint64_t fb2 = mtl_sub_commit();
    dispatch_semaphore_signal(gate);
    atomic_fetch_sub(&g_rx_busy_hold, 1);
    CHECK(mtl_wait(fb2, 5000000000ull), "the submissions around a queue operation complete");
    rec_drain();
    CHECK(pend_at_block == 0 && held_at_block == cbs0 + 1 && g_rx_merged - merged0 >= 1 && fa < fb2, "the held submission was committed before the queue operation ran");
    CHECK(nb_at_block == 1, "and alone: the next submission's pass recorded before the queue operation was not in that commit");
    // the GPU has run out of work but the commit feedback has not reported it yet (its count held up here): the next submission is not held, as the GPU is read through the
    // encode thread's event (DM_NVN_SUB_IDLE_FB=1 reads that count and holds it)
    rec_drain(); for (int i = 0; i < 20000 && (atomic_load(&g_rx_fly) || atomic_load(&g_seg_inflight)); i++) usleep(100);
    for (int i = 0; i < 20000 && g_rx_ev && g_rx_ev.signaledValue < g_rx_seq; i++) usleep(100);
    cbs0 = g_rx_cbs; merged0 = g_rx_merged;
    atomic_fetch_add(&g_rx_fly, 1);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &colour; clear(MTLClearColorMake(0, 0, 1, 1)); rp_end(); uint64_t fi = mtl_sub_commit();
    for (int i = 0; i < 50000 && g_done < fi; i++) usleep(100);   // (polled: a drain would send it at once either way)
    CHECK(g_done >= fi, "a submission after the GPU's work ended completes");
    atomic_fetch_sub(&g_rx_fly, 1);
    const char *fbm = getenv("DM_NVN_SUB_IDLE_FB");
    if (!(fbm && !strcmp(fbm, "1"))) CHECK(g_rx_merged == merged0 && g_rx_cbs == cbs0 + 1, "the GPU is idle before its feedback reports it: the submission went at once, not held");
}

// Output-mapping-only fixture (no rewritable sample): main0 writes magenta to color(0); main0_f remaps
// that output through OM_0_k. Specialized with OM_0_1, attachment 1 is magenta and attachment 0 stays the clear colour.
static void om_case(const char *dir) {
    int before = failed; NSString *d = @(dir);
    NSData *oj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"om.json"]];
    NSDictionary *facts = oj ? [NSJSONSerialization JSONObjectWithData:oj options:0 error:nil] : nil;
    NSArray *calls = facts[@"calls"], *outputs = facts[@"outputs"];
    CHECK(facts && calls.count == 0 && outputs.count == 1, "the om-only fixture has no samples and one colour output");
    NSError *e = nil;
    id<MTLLibrary> bfl = lib_from_file([d stringByAppendingPathComponent:@"fedcba9876543212_bf.metallib"], &e);
    CHECK(bfl && [bfl.functionNames containsObject:@"main0_f"], "the folded om-only library exposes main0_f");
    if (!bfl) { printf("feedback output-mapping-only: %s\n", before == failed ? "PASS" : "FAIL"); return; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    bool no = false;
    for (int k = 0; k < 8; k++) {
        [cv setConstantValue:&no type:MTLDataTypeBool atIndex:32 + k];
        bool om = (k == 1);
        [cv setConstantValue:&om type:MTLDataTypeBool atIndex:64 + k];
    }
    id<MTLFunction> ff = [bfl newFunctionWithName:@"main0_f" constantValues:cv error:&e];
    CHECK(ff != nil, "main0_f specializes with OM_0_1");
    if (!ff) { fprintf(stderr, "om specialize: %s\n", e.localizedDescription.UTF8String); printf("feedback output-mapping-only: FAIL\n"); return; }
    id<MTLLibrary> vl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        options:nil error:&e];
    CHECK(vl != nil, "the om-only vertex shader compiles");
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [vl newFunctionWithName:@"v"]; pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = vl ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    CHECK(ps != nil, "the om-only remap pipeline compiles");
    if (!ps) { fprintf(stderr, "om pipeline: %s\n", e.localizedDescription.UTF8String); printf("feedback output-mapping-only: FAIL\n"); return; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t0 = [g_dev newTextureWithDescriptor:td], t1 = [g_dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = t0; rpd.colorAttachments[0].loadAction = MTLLoadActionClear; rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    rpd.colorAttachments[1].texture = t1; rpd.colorAttachments[1].loadAction = MTLLoadActionClear; rpd.colorAttachments[1].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[1].clearColor = MTLClearColorMake(0, 1, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
    [enc setRenderPipelineState:ps];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.error == nil, "om-only GPU submission has no error");
    uint8_t px0[64], px1[64];
    [t0 getBytes:px0 bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    [t1 getBytes:px1 bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    int att0 = 1, att1 = 1;
    for (int i = 0; i < 16; i++) {
        const uint8_t *a = px0 + i * 4, *b = px1 + i * 4;
        att0 &= a[0] == 0 && a[1] == 255 && a[2] == 0 && a[3] == 255;
        att1 &= b[0] == 255 && b[1] == 0 && b[2] == 255 && b[3] == 255;
    }
    CHECK(att0, "attachment 0 stays the clear green; output 0 did not write there");
    CHECK(att1, "attachment 1 is magenta: OM_0_1 moved colour 0");
    printf("feedback output-mapping-only: %s\n", before == failed ? "PASS" : "FAIL");
}
// A draw that writes depth and samples its own depth attachment through a bound slot reads a copy taken after its pass ends (fb_depth_snap), reused without ending the pass until the
// game's next barrier; a draw that leaves depth alone reads the attachment directly, as before. Only serials matter to the rule, as in reuse_case: the stand-in depth texture is RGBA8,
// filled from the CPU
static void depth_snap_case(NvProg *pr) {
    int before = failed; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t output, depth; target(&output, 99920); id<MTLTexture> dtex = target(&depth, 99921); mtl_reg_tex(652, &depth, NULL);
    uint8_t px[64]; for (int i = 0; i < 16; i++) { px[4 * i] = 10; px[4 * i + 1] = 20; px[4 * i + 2] = 30; px[4 * i + 3] = 40; }
    [dtex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &output; S.tex[1][0] = 652; S.chmask.mask = 0xffffffffu; S.ds.dt = 1; S.ds.dw = 1;
    clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;
    uint64_t copies = g_fb_dsnaps;
    fb_prepare(pr);
    CHECK(!g_enc && g_fb_n == 1 && g_fb_use[0] && g_fb_use[0] != dtex && g_fb_dsnaps == copies + 1, "a draw that writes depth and samples it reads a copy, taken after its pass ends");
    CHECK(!g_fb_dread, "and does not count as reading the attachment");
    id<MTLTexture> first = g_fb_use[0];
    blit_barrier(); readback(first, result, 0);
    R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;   // the pass that draw opens
    fb_prepare(pr);
    CHECK(g_enc && g_fb_n == 1 && g_fb_use[0] == first && g_fb_dsnaps == copies + 1, "the next such draw reuses the copy without ending the pass");
    g_fb_bar++;
    fb_prepare(pr);
    CHECK(!g_enc && g_fb_n == 1 && g_fb_dsnaps == copies + 2, "a game barrier makes the next one take a fresh copy");
    R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); rp_end();   // that draw writes depth in the pass it opens: the next case starts in a clean one
    clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth; S.ds.dw = 0;
    fb_prepare(pr);
    CHECK(g_enc && g_fb_n == 0 && g_fb_dread && g_fb_dsnaps == copies + 2, "a draw that leaves depth alone reads the attachment directly");
    R.rt.depth = NULL; rp_end(); clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth; S.ds.dw = 1; g_fb_depth_m = 0;
    fb_prepare(pr);
    CHECK(g_enc && g_fb_n == 0 && g_fb_dsnaps == copies + 2, "with DM_NVN_FB_DEPTH=0 a draw that writes depth reads it directly, as before");
    g_fb_depth_m = 1; R.rt.depth = NULL; S.ds.dt = S.ds.dw = 0; S.tex[1][0] = 0; rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "depth copy submission completed without error");
    uint8_t want[4] = {10, 20, 30, 40}; pixels(result, 0, want, "the copy holds the attachment's content");
    mtl_tex_release(&output); mtl_tex_release(&depth);
    printf("depth copy: %s\n", before == failed ? "PASS" : "FAIL");
}
// A draw that samples the depth attachment after an earlier draw of its pass wrote depth, and writes none itself, ends the pass first (Red Dead Redemption's cloud fog); one that samples
// it first in its pass leaves the pass open, and a later draw that writes depth then ends it (g_fb_dread)
static void depth_rebind(NvProg *pr) {
    uint8_t ops[1024]={0}; size_t off=0; p_set_rt targets[2]={{0},R.rt};
    for (int i=0;i<2;i++) { op_hdr h={OP_SET_RT,8+sizeof(p_set_rt)}; memcpy(ops+off,&h,8); memcpy(ops+off+8,&targets[i],sizeof(p_set_rt)); off+=h.size; }
    op_hdr end={OP_END,8}; memcpy(ops+off,&end,8); mtl_exec(ops);
    CHECK(g_enc && g_rt_merged,"empty unbind/rebind preserves the open depth-feedback pass");
    g_samples_fb_left=1; CHECK(!draw_samples_rt(pr),"continued-pass guard hands exact depth reads to the depth feedback rule"); g_samples_fb_left=0;
}
static void depth_binding(int texu, int tid) { if (texu) g_ubo_words[0]=(uint32_t)tid; else S.tex[1][0]=(uint64_t)tid; }
static void depth_after_write_case(NvProg *pr, int texu) {
    int before = failed; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t output, depth; target(&output, 99940); target(&depth, 99941); mtl_reg_tex(654, &depth, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &output; S.chmask.mask = 0xffffffffu;
    clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;
    uint64_t splits = g_fb_dsplit;
    fs->rf.bindless=texu; fs->rf.b[0].kind=texu ? NVN_RB_TEXU : NVN_RB_TEX;
    if (texu) { fs->rf.b[0].reg=2; S.ubo[1][2]=(p_bind_buf){.addr=(uint64_t)(uintptr_t)g_ubo_words,.size=16}; }
    S.ds.dt = S.ds.dw = 1; depth_binding(texu,0);
    fb_prepare(pr);
    CHECK(g_enc && g_fb_dwrote && !g_fb_dread, "a draw that writes depth and samples nothing leaves the pass open");
    S.ds.dw = 0; depth_binding(texu,654); depth_rebind(pr);
    fb_prepare(pr);
    CHECK(!g_enc && g_fb_n == 0 && g_fb_dsplit == splits + 1, "a draw that samples the depth an earlier draw of its pass wrote ends the pass first");
    R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;   // the pass that draw opens
    fb_prepare(pr);
    CHECK(g_enc && g_fb_dread && g_fb_dsplit == splits + 1, "one that samples it first in its pass reads it there");
    S.ds.dw = 1; depth_binding(texu,0); depth_rebind(pr);
    fb_prepare(pr);
    CHECK(!g_enc, "and a later draw that writes depth ends that pass");
    g_fb_pass=UINT64_MAX; R.rt.depth=NULL; clear(MTLClearColorMake(0,0,0,1)); R.rt.depth=&depth; S.ds.dw=0; depth_binding(texu,654); fb_prepare(pr);
    S.ds.st=1; S.ds.zpass_f=NVN_STENCIL_OP_REPLACE; S.st_wmask[0]=255; depth_binding(texu,0); depth_rebind(pr); fb_prepare(pr);
    CHECK(!g_enc,"a stencil writer after a depth read still ends the continued pass"); S.ds.st=0;
    g_fb_pass=UINT64_MAX; R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth; g_fb_depth_m = 0;
    fb_prepare(pr); S.ds.dw = 0; depth_binding(texu,654);
    fb_prepare(pr);
    CHECK(g_enc && g_fb_dsplit == splits + 1, "with DM_NVN_FB_DEPTH=0 it reads it in the pass, as before");
    g_fb_depth_m = 1; R.rt.depth = NULL; S.ds.dt = S.ds.dw = 0; depth_binding(texu,0); rp_end();
    fs->rf.bindless=0; fs->rf.b[0].kind=NVN_RB_TEX; fs->rf.b[0].reg=0;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "that submission completed without error");
    mtl_tex_release(&output); mtl_tex_release(&depth);
    printf("depth after a write: %s\n", before == failed ? "PASS" : "FAIL");
}
// The copy of an actual private Depth32Float attachment, or a Depth32Float_Stencil8 one (Octopath Traveler II's), cleared through the production render pass, in a pass with a colour
// attachment and in a depth-only one: the stand-in above checks the rule, not a depth format, its store or its copy (the colour pass's case from akratch's review of PR #1917).
// pending: the attachment holds 0.75 from an earlier pass, and the clear to 0.375 still waits to be the load action of the pass the draw opens (no pass is open): the copy lands it first
static void real_depth_snap_case(NvProg *pr, int colour_pass, int stencil, int pending) {
    int before = failed; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t colour, depth;
    if (colour_pass) target(&colour, 99930);
    depth = (tex_t){.magic=MAGIC('T','E','X',0), .serial=99931 + (uint64_t)colour_pass + 2 * (uint64_t)stencil};
    depth.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=stencil ? NVN_FORMAT_DEPTH32F_STENCIL8 : NVN_FORMAT_DEPTH32F, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    id<MTLTexture> dt = [g_dev newTextureWithDescriptor:tex_desc(&depth, 0)];
    CHECK(dt != nil, "real depth attachment allocates");
    if (!dt) { if (colour_pass) mtl_tex_release(&colour); return; }
    res_add(dt); mtl_tex_init(&depth); serial_put(depth.serial, dt);
    mtl_reg_tex(655, &depth, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    CHECK(result != nil, "the readback buffer allocates");
    if (!result) { mtl_tex_release(&depth); if (colour_pass) mtl_tex_release(&colour); return; }
    res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = colour_pass; if (colour_pass) R.rt.color[0] = &colour; R.rt.depth = &depth;
    S.ds.dt = S.ds.dw = 1; S.tex[1][0] = 655;
    if (pending) { R.clear_d = 1; R.cd = 0.75; CHECK(rp_begin() != nil, "the earlier clearing pass opens"); rp_end(); }
    R.clear_d = 1; R.cd = 0.375;
    if (pending) CHECK(!g_enc, "no pass is open: the clear waits for the draw's pass");
    else if (colour_pass) clear(MTLClearColorMake(0, 0, 0, 1)); else CHECK(rp_begin() != nil, "production depth-only render pass opens");
    uint64_t copies = g_fb_dsnaps;
    if (colour_pass && !pending) depth_rebind(pr);
    fb_prepare(pr);
    id<MTLTexture> snap = g_fb_n == 1 ? g_fb_use[0] : nil;
    CHECK(!g_enc && snap && snap != dt && snap.pixelFormat == dt.pixelFormat && g_fb_dsnaps == copies + 1,
          colour_pass ? "real depth feedback substitutes a snapshot in the attachment's format" : "in a depth-only pass too");
    if (snap) { blit_barrier(); enc_b_copy_texture_buffer_opt(blit(), snap, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024, stencil ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone); }
    R.rt.depth = NULL; S.ds.dt = S.ds.dw = 0; S.tex[1][0] = 0;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "real depth copy and readback complete under Metal validation");
    int valid = snap != nil;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) valid &= *(const float *)((const uint8_t *)result.contents + y * 256 + x * 4) == 0.375f;
    CHECK(valid, pending ? "every real depth sample holds the clear that was still waiting, not the earlier content" : "every real depth sample holds the attachment's clear");
    if (colour_pass) mtl_tex_release(&colour);
    mtl_tex_release(&depth);
    printf("real depth copy (%s, %s%s): %s\n", stencil ? "Depth32Float_Stencil8" : "Depth32Float", colour_pass ? "colour pass" : "depth-only pass", pending ? ", clear waiting" : "", before == failed ? "PASS" : "FAIL");
}
// A colour snapshot taken while the attachment's clear still waits to be the load action of the pass the draw opens holds the clear: the attachment holds (10, 20, 30, 40) from the CPU,
// the clear is red, and the draw writes the attachment it samples
static void pending_clear_case(NvProg *pr) {
    int before = failed; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t source; id<MTLTexture> st = target(&source, 99950); mtl_reg_tex(656, &source, NULL);
    uint8_t px[64]; for (int i = 0; i < 16; i++) { px[4 * i] = 10; px[4 * i + 1] = 20; px[4 * i + 2] = 30; px[4 * i + 3] = 40; }
    [st replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:16];
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = 656; S.chmask.mask = 0xffffffffu;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(1, 0, 0, 1);
    uint64_t snaps = g_fb_count;
    fb_prepare(pr);
    id<MTLTexture> snap = g_fb_n == 1 ? g_fb_use[0] : nil;
    CHECK(!g_enc && snap && snap != st && g_fb_count == snaps + 1 && !R.clear_c[0], "a draw writing the attachment it samples takes a snapshot after the waiting clear lands");
    if (snap) { blit_barrier(); readback(snap, result, 0); }
    S.tex[1][0] = 0;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "pending clear submission completed without error");
    pixels(result, 0, (const uint8_t[]){255, 0, 0, 255}, "the snapshot holds the red clear, not the content before it");
    mtl_tex_release(&source);
    printf("snapshot after a waiting clear: %s\n", before == failed ? "PASS" : "FAIL");
}
int main(int argc, char **argv) { @autoreleasepool {
    setenv("DM_NVN_GPU_TS", "segment", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "struct A {texture2d<float, access::read> source[[id(0)]];};\n"
        "fragment float4 f(float4 p[[position]], constant A&a[[buffer(0)]]) {return a.source.read(uint2(p.xy));}\n"
        "struct I {texture2d<uint, access::read> source[[id(0)]];};\n"
        "fragment float4 mask(float4 p[[position]], constant I&a[[buffer(0)]]) {uint v = a.source.read(uint2(p.xy)).x; return float4(v==17u, v==29u, 0, 1);}\n"
        "struct AM {texture2d<float> source[[id(0)]];};\n"
        "fragment float4 fm1(constant AM&a[[buffer(0)]]) {constexpr sampler s(coord::normalized,filter::linear,mip_filter::linear); return a.source.sample(s,float2(.5),level(1));}\n"
        "fragment float4 fm2(constant AM&a[[buffer(0)]]) {constexpr sampler s(coord::normalized,filter::linear,mip_filter::linear); return a.source.sample(s,float2(.5),level(2));}\n"
        "struct AMP {texture2d<float> source[[id(0)]]; sampler filter[[id(1)]];};\n"
        "fragment float4 fmp(constant AMP&a[[buffer(0)]]) {return a.source.sample(a.filter,float2(.5),level(2));}\n"
        "struct AMA {texture2d_array<float> source[[id(0)]];};\n"
        "fragment float4 fma2(constant AMA&a[[buffer(0)]]) {constexpr sampler s(coord::normalized,filter::linear,mip_filter::linear); return a.source.sample(s,float2(.5),1,level(2));}\n"
        "struct AMC {texturecube<float> source[[id(0)]];};\n"
        "fragment float4 fmc2(constant AMC&a[[buffer(0)]]) {constexpr sampler s(coord::normalized,filter::linear,mip_filter::linear); return a.source.sample(s,float3(0,0,1),level(2));}\n"
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
    if (posix_memalign((void **)&g_ubo_words, getpagesize(), getpagesize())) abort(); memset(g_ubo_words, 0, getpagesize()); mtl_pool_add(g_ubo_words, getpagesize());
    const int slots[] = {0, 128, 255};
    for (int i = 0; i < 3; i++) slot_case(pr, pipeline, slots[i], i);
    reuse_case(pr);
    depth_snap_case(pr);
    depth_after_write_case(pr,0); depth_after_write_case(pr,1);
    for (int pending = 0; pending < 2; pending++) for (int stencil = 0; stencil < 2; stencil++) { real_depth_snap_case(pr, 1, stencil, pending); real_depth_snap_case(pr, 0, stencil, pending); }
    pending_clear_case(pr);
    shape_case();
    same_draw_case(0); same_draw_case(1);
    mip_chain_case(pr);
    mip_pixels_case(lib);
    base_level_case(lib, 0, 1, 0, 0); base_level_case(lib, 1, 2, 1, 0); base_level_case(lib, 0, 2, 2, 1); base_level_case(lib, 0, 2, 3, 2);
    // A texture named by one handle in a uniform buffer (FINAL FANTASY XII THE ZODIAC AGE's limit-break echo): before fb_prepare took TEXU bindings, the draw read the attachment itself
    fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEXU, .aid=0, .dtt=MTLTextureType2D}; slot_case(pr, pipeline, 2, 4);
    fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .aid=0, .dtt=MTLTextureType2D};
    fs->fn = [lib newFunctionWithName:@"mask"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .aid=0, .dtt=MTLTextureType2D, .dpf=NVN_DPF_UINT};
    d.fragmentFunction = fs->fn;
    pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pipeline != nil, "integer image pipeline compiles");
    if (pipeline) slot_case(pr, pipeline, 3, 3);
    image_view_case(pr);
    swizzle_case();
    if (argc == 2) { tile_case(argv[1]); fpos_case(argv[1]); om_case(argv[1]); }
    pool_depth_case();
    reuse_tables_case();
    cut_boundary_case();
    merge_case();
    printf("feedback snapshot: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
