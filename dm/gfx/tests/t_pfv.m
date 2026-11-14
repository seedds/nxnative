#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

// Texel (0, 0) of t as a shader reads it, in a command buffer of its own
static int read_texel(id<MTLTexture> t, float out[4]) {
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void rd(texture2d<float> t [[texture(0)]], device float4 *o [[buffer(0)]]) { o[0] = t.read(uint2(0, 0)); }\n";
    NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"rd"];
    id<MTLComputePipelineState> ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (!ps || !t) return 0;
    id<MTLBuffer> o = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:ps]; [en setTexture:t atIndex:0]; [en setBuffer:o offset:0 atIndex:0];
    [en dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(out, o.contents, 16); return cb.status == MTLCommandBufferStatusCompleted;
}
// A colour render target view of another class (R32_UI) over a colour texture of the same texel size. Nintendo Switch Sports Resort's NDSR upscaler input is an RGB9E5F texture
// written through an R32_UI view by a fragment shader whose output is the packed bits as uint: attach attaches the R32Uint view, so the pipeline takes the uint output and the stored
// bits are the shader's (before, the base RGB9E5Float texture was attached, the shader's float entry wrote the bits' float value, and the upscaled picture came out red). An RGB9E5F
// texture has the format-view usage from its creation; another format is upgraded to it at its first such pass. Returns the number of failed checks
static int rt_view_into(int nvn_format, MTLPixelFormat want_base, uint64_t serial, int made_with_pfv, const float want_texel[3]) {
    int failed = 0;
    tex_t e = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial};
    e.d.width = 4; e.d.height = 1; e.d.depth = 1; e.d.levels = 1; e.d.target = NVN_TEXTURE_TARGET_2D; e.d.format = nvn_format;
    MTLTextureDescriptor *d = tex_desc(&e, 0);
    failed += !!(d.usage & MTLTextureUsagePixelFormatView) != made_with_pfv;
    serial_put(e.serial, [g_dev newTextureWithDescriptor:d]);
    view_t v = {0}; v.set_format = 1; v.format = NVN_FORMAT_R32UI;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int tid = (int)(serial - 69000);   // a texture ID registered before the pass, resolved (and cached) as a shader would, and sampled after it
    mtl_reg_tex(tid, &e, NULL);
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> before = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    failed += before != serial_get(e.serial);
    uint64_t upgrades = g_pfv_upgrades;
    mtl_sub_begin();
    attach(rp.colorAttachments[0], &e, &v, 1);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLTexture> base = serial_get(e.serial), att = rp.colorAttachments[0].texture;
    failed += g_pfv_upgrades - upgrades != (uint64_t)!made_with_pfv;   // upgraded only when made without the usage
    failed += att.pixelFormat != MTLPixelFormatR32Uint;
    failed += att.parentTexture != base;                                 // a view of the registered texture, whose bits the game later samples in the base format
    failed += base.pixelFormat != want_base || !(base.usage & MTLTextureUsagePixelFormatView);
    // A packed RGB9E5 word drawn through the attachment as uint is stored as it is
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 p [[position]]; };\n"
                     "vertex O vs(uint i [[vertex_id]]) { O o; float2 q = float2((i << 1) & 2, i & 2); o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
                     "fragment uint4 fs() { return uint4(0x3c003800u, 0, 0, 0); }\n";
    NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = att.pixelFormat;
    id<MTLRenderPipelineState> ps = lib ? [g_dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    failed += !ps;
    mtl_sub_commit(); mtl_drain();
    if (ps) {
        id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
        rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
        id<MTLBuffer> out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:base sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 1, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:16];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *w = out.contents; for (int i = 0; i < 4; i++) failed += w[i] != 0x3c003800u;
        // A later pass samples the texture through the ID registered before the upgrade: the ID cache follows the upgrade (g_ser_gen), so it reads the new texture and the bits drawn
        os_unfair_lock_lock(&g_rmu); id<MTLTexture> after = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
        failed += (after.parentTexture ? after.parentTexture : after) != base;
        failed += !made_with_pfv && after == before;
        float got[4] = {0}; failed += !read_texel(after, got);
        failed += fabsf(got[0] - want_texel[0]) > 1e-6f || fabsf(got[1] - want_texel[1]) > 1e-6f || fabsf(got[2] - want_texel[2]) > 1e-6f;
    }
    printf("R32_UI render target view of NVN format %#x: failed=%d\n", nvn_format, failed);
    return failed;
}
static tex_t rg16_tex(uint64_t serial) {
    tex_t e = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial};
    e.d.width = 4; e.d.height = 1; e.d.depth = 1; e.d.levels = 1; e.d.target = NVN_TEXTURE_TARGET_2D; e.d.format = NVN_FORMAT_RG16F;
    return e;
}
// Two colour slots over one texture (two serials sharing storage, as the sharing table makes them): slot 0 plain, slot 1 through an R32_UI view. The upgrade for slot 1 moves both
// serials to the new texture, so it runs for every slot before any is attached (rt_view_upgrade_all), and slot 0 does not keep the retired texture. The pipeline prediction gives
// the view's format before the upgrade
static int rt_view_slots_share(void) {
    int failed = 0;
    tex_t a = rg16_tex(70005), b = rg16_tex(70006);
    id<MTLTexture> old = [g_dev newTextureWithDescriptor:tex_desc(&a, 0)];
    serial_put(a.serial, old); serial_put(b.serial, old);
    view_t v = {0}; v.set_format = 1; v.format = NVN_FORMAT_R32UI;
    failed += rt_view_fmt_predict(&b, old, &v, 1) != MTLPixelFormatR32Uint;
    memset(&R.rt, 0, sizeof R.rt); memset(R.clear_c, 0, sizeof R.clear_c);
    R.rt.n = 2; R.rt.color[0] = &a; R.rt.color[1] = &b; R.rt.has_cview[1] = 1; R.rt.cview[1] = v;
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    mtl_sub_begin();
    rt_view_upgrade_all(0);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    attach(pd.colorAttachments[0], &a, NULL, 0); attach(pd.colorAttachments[1], &b, &v, 1);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLTexture> nw = serial_get(a.serial);
    failed += nw == old || serial_get(b.serial) != nw;            // both serials moved to the upgraded texture
    failed += pd.colorAttachments[0].texture != nw;                // slot 0 attached after the upgrade: not the retired texture
    failed += pd.colorAttachments[1].texture.parentTexture != nw || pd.colorAttachments[1].texture.pixelFormat != MTLPixelFormatR32Uint;
    mtl_sub_commit(); mtl_drain();
    memset(&R.rt, 0, sizeof R.rt);
    printf("render target view upgrade before any slot is attached: failed=%d\n", failed);
    return failed;
}
// A refused format pair (Metal made no view after an upgrade) or a texture whose upgrade failed is not upgraded again at every pass: drawn in the base format, with the red line
static int rt_view_refusals(void) {
    int failed = 0;
    view_t v = {0}; v.set_format = 1; v.format = NVN_FORMAT_R32UI;
    tex_t a = rg16_tex(70007), b = rg16_tex(70008);
    serial_put(a.serial, [g_dev newTextureWithDescriptor:tex_desc(&a, 0)]); serial_put(b.serial, [g_dev newTextureWithDescriptor:tex_desc(&b, 0)]);
    uint64_t upgrades = g_pfv_upgrades;
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    mtl_sub_begin();
    rtv_note_bad_pair(MTLPixelFormatRG16Float, MTLPixelFormatR32Uint);
    attach(pd.colorAttachments[0], &a, &v, 1);
    failed += pd.colorAttachments[0].texture.pixelFormat != MTLPixelFormatRG16Float;
    failed += rt_view_fmt_predict(&a, serial_get(a.serial), &v, 1) != MTLPixelFormatRG16Float;
    g_rtv_bad_npf = 0;
    g_rtv_bad_ser[g_rtv_bad_nser++ % RTV_BAD_N] = b.serial;
    attach(pd.colorAttachments[1], &b, &v, 1);
    failed += pd.colorAttachments[1].texture.pixelFormat != MTLPixelFormatRG16Float;
    failed += g_pfv_upgrades != upgrades;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_sub_commit(); mtl_drain();
    g_rtv_bad_nser = 0;
    printf("refused render target view upgrades are not retried: failed=%d\n", failed);
    return failed;
}
static int rt_view_class_change(void) {
    // 0x3c003800 as RGB9E5: exponent 7, red 0, green 28, blue 256 (x 2^-17); as RG16F: red 0x3800 = 0.5, green 0x3c00 = 1
    const float e5[3] = {0, 28.0f / 131072, 256.0f / 131072}, h16[3] = {0.5f, 1.0f, 0};
    int failed = rt_view_into(NVN_FORMAT_RGB9E5F, MTLPixelFormatRGB9E5Float, 70002, 1, e5) + rt_view_into(NVN_FORMAT_RG16F, MTLPixelFormatRG16Float, 70003, 0, h16);
    // Another texel size (R32_UI over RGBA16F): no Metal view bridges them, the texture is left as it was
    tex_t h = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 70004};
    h.d.width = 4; h.d.height = 1; h.d.depth = 1; h.d.levels = 1; h.d.target = NVN_TEXTURE_TARGET_2D; h.d.format = NVN_FORMAT_RGBA16F;
    serial_put(h.serial, [g_dev newTextureWithDescriptor:tex_desc(&h, 0)]);
    view_t v = {0}; v.set_format = 1; v.format = NVN_FORMAT_R32UI;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    mtl_sub_begin();
    attach(rp.colorAttachments[0], &h, &v, 1);
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    failed += rp.colorAttachments[0].texture.pixelFormat != MTLPixelFormatRGBA16Float || (((id<MTLTexture>)serial_get(h.serial)).usage & MTLTextureUsagePixelFormatView);
    failed += rt_view_fmt_predict(&h, serial_get(h.serial), &v, 1) != MTLPixelFormatRGBA16Float;   // the prediction keeps the base format where no upgrade happens
    mtl_sub_commit(); mtl_drain();
    failed += rt_view_slots_share() + rt_view_refusals();
    return failed;
}

int main(void) {
    int failed = 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm) != 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint) != 1;
    tex_t t = {.magic=MAGIC('T', 'E', 'X', 0), .serial = 70001};
    t.d.width = 4; t.d.height = 1; t.d.depth = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    mtl_init();
    MTLTextureDescriptor *d = tex_desc(&t, 0);
    failed += (d.usage & MTLTextureUsagePixelFormatView) != 0;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    uint8_t bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id<MTLBuffer> src = [g_dev newBufferWithBytes:bytes length:sizeof bytes options:MTLResourceStorageModeShared];
    mtl_sub_begin();
    res_add(src); res_add(base);   // Metal 4: both ends of a copy must be in the residency set
    id fill = blit();   // copies on the main command buffer go through the host wrapper (which carries its own barrier)
    [fill copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:16 sourceSize:MTLSizeMake(4, 1, 1)
               toTexture:base destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    enc_b_end(g_blit); g_blit = nil;
    serial_put(t.serial, base);
    id<MTLTexture> upgraded = tex_upgrade_pfv(&t, base, MTLPixelFormatR32Uint);
    failed += upgraded.pixelFormat != MTLPixelFormatRGBA8Unorm;   // the upgrade keeps the original format
    failed += !(upgraded.usage & MTLTextureUsagePixelFormatView);
    failed += serial_get(t.serial) != upgraded;   // the registration has been replaced
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLTexture> view = [upgraded newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
    id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof bytes options:MTLResourceStorageModeShared];
    res_add(dst);
    id read = blit();
    [read copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 1, 1)
                 toBuffer:dst destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:16];
    enc_b_end(g_blit); g_blit = nil;
    mtl_sub_commit(); mtl_drain();
    failed += memcmp(dst.contents, bytes, sizeof bytes) != 0;
    failed += rt_view_class_change();
    printf("pfv predicate failed=%d\n", failed);
    return failed != 0;
}
