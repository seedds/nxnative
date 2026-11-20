// An sRGB texture in an image slot is loaded and stored raw, as NVN's image loads and stores do (no sRGB conversion), while its sampled descriptor still decodes.
// Star Fox writes its upscaler's sRGB-encoded output into an RGBA8_SRGB texture through an image: through the texture's own format Metal encoded it once more.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void read_bytes(id<MTLTexture> t, uint8_t *out) {
    memset(out, 0, 64);
    id<MTLBuffer> rb = [g_dev newBufferWithLength:4 * 4 * 4 options:MTLResourceStorageModeShared];
    CHECK(rb != nil, "readback buffer made");
    if (!rb) return;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:64];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "readback completed");
    memcpy(out, rb.contents, 64);
}
// One thread: reads texel (0,0) through the image and through the sampled texture into out[0..7], writes (0.5, 0.25, 0.75, 1) to texel (1,1) through the image
static void run_kernel(id<MTLTexture> image, id<MTLTexture> sampled, float *out) {
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void k(texture2d<float, access::read_write> im [[texture(0)]], texture2d<float> tx [[texture(1)]], device float4 *o [[buffer(0)]]) {\n"
        "  o[0] = im.read(uint2(0, 0)); o[1] = tx.read(uint2(0, 0)); im.write(float4(0.5, 0.25, 0.75, 1.0), uint2(1, 1)); }\n" options:nil error:&e];
    id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
    id<MTLComputePipelineState> p = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
    CHECK(p != nil, "image kernel compiled");
    if (!p) return;
    id<MTLBuffer> ob = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:p]; [c setTexture:image atIndex:0]; [c setTexture:sampled atIndex:1]; [c setBuffer:ob offset:0 atIndex:0];
    [c dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "image kernel completed");
    memcpy(out, ob.contents, 32);
}
static void srgb_case(int fmt, int bgra, uint64_t serial, int tid) { @autoreleasepool {
    tex_t texture = {.magic = MAGIC('T', 'E', 'X', 0), .serial = serial, .off = serial << 12};
    texture.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = (uint32_t)fmt, .flags = NVN_TEXTURE_FLAGS_IMAGE, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&texture.d, &texture.L); mtl_tex_init(&texture);
    id<MTLTexture> base = tex_mtl(&texture);
    MTLPixelFormat srgb = bgra ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm_sRGB, lin = bgra ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    CHECK(base && base.pixelFormat == srgb, "the texture is made in its sRGB format");
    uint8_t seed[64] = {0}; seed[0] = seed[1] = seed[2] = 0x80; seed[3] = 0xff;   // texel (0,0): stored bytes 0x80
    { id<MTLBuffer> sb = [g_dev newBufferWithBytes:seed length:64 options:MTLResourceStorageModeShared];
      CHECK(sb != nil, "seed buffer made");
      if (!sb) return;
      id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
      [b copyFromBuffer:sb sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:64 sourceSize:MTLSizeMake(4, 4, 1) toTexture:base destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
      [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; }
    mtl_reg_tex(tid, &texture, NULL); mtl_reg_image(tid + 1, &texture, NULL);
    NvProg *program = [NvProg new]; NvShader *shader = [NvShader new]; program->sh[5] = shader;
    shader->rf.nb = 1; shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0, .rw = 1};
    mtl_sub_begin(); memset(&R, 0, sizeof R);
    S.img[5][0] = (uint32_t)(tid + 1); image_prepare(program, 0x20);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> sampled = tex_by_id(tid); uint64_t rid = 0; int real = 0;
    id<MTLTexture> image = img_binding(tid + 1, &shader->rf.b[0], &rid, &real);
    os_unfair_lock_unlock(&g_rmu);
    mtl_sub_commit(); mtl_drain();
    CHECK(sampled && sampled.pixelFormat == srgb, "the sampled descriptor keeps the sRGB format (sampling decodes)");
    CHECK(real && image && image.pixelFormat == lin, "the image slot gets the linear twin of the sRGB format");
    CHECK(image && rid == image.gpuResourceID._impl, "the image argument names that view");
    id<MTLTexture> root = image.parentTexture ? image.parentTexture : image, sroot = sampled.parentTexture ? sampled.parentTexture : sampled;
    CHECK(root == sroot && (image.usage & MTLTextureUsageShaderWrite), "the view shares the sampled texture's writable storage");
    float out[8] = {0}; if (image && sampled) run_kernel(image, sampled, out);
    CHECK(fabsf(out[0] - 128.0f / 255.0f) < 1e-3f, "an image load returns the stored byte as it is");
    CHECK(fabsf(out[4] - 0.2158605f) < 2e-3f, "a sampled read of the same texel decodes it");
    uint8_t px[64]; read_bytes(root, px);
    const uint8_t *t11 = px + 4 * (4 + 1); uint8_t r = bgra ? t11[2] : t11[0], g = t11[1], b = bgra ? t11[0] : t11[2];
    CHECK(r == 0x80 && g == 0x40 && b == 0xbf && t11[3] == 0xff, "an image store keeps the written value as it is (0.5 → 0x80, not 0xbc)");
} }
int main(void) { @autoreleasepool {
    mtl_init();
    srgb_case(NVN_FORMAT_RGBA8_SRGB, 0, 99901, 801);
    srgb_case(NVN_FORMAT_BGRA8_SRGB, 1, 99911, 811);
    printf("sRGB image loads and stores: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
