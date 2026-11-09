// A colour output's components outside the shader's output map (SPH word 18, reflection raster.omap) keep the target's value: the translator stores only the mapped
// components and Metal writes the whole attachment under its write mask, so pso_desc masks each target with its output's nibble. The shader below returns 0.25 for alpha,
// standing in for the undefined value an unstored component has; with output 0 mapped RGB only, the target's cleared alpha must stay
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void pixels(id<MTLRenderPipelineState> ps, __fp16 out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(.75, .5, .25, .875); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted, "draw completes");
    [t getBytes:out bytesPerRow:32 fromRegion:MTLRegionMake2D(1,1,1,1) mipmapLevel:0];
}

int main(void) { @autoreleasepool {
    mtl_init(); NSError *error = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(.125,.25,.5,.25); }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "shaders compile"); if (!lib) return 1;
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    vs->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    fs->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    fs->rf.color_known = 1; fs->rf.color_mask = 1; fs->rf.omap_known = 1; fs->rf.omap = 0x7;
    pso_key k = {0}; k.samples = 1; k.chmask = 15; k.cfmt[0] = MTLPixelFormatRGBA16Float;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
    CHECK(d.colorAttachments[0].writeMask == (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue), "alpha outside the output map is masked");
    for (int metal4 = 0; metal4 < 2; metal4++) {
        id<MTLRenderPipelineState> ps = metal4 ? pso_build(d, &error) : [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
        CHECK(ps != nil, "pipeline compiles on both paths");
        if (ps) { __fp16 got[4]; pixels(ps, got); CHECK(got[0] == (__fp16).125 && got[1] == (__fp16).25 && got[2] == (__fp16).5 && got[3] == (__fp16).875, "mapped components written, alpha left as cleared"); }
    }
    k.chmask = 0xd;   // the channel mask and the output map together: green off by the channel mask, alpha by the map
    CHECK(pso_desc(&k, vs, fs, nil, 0).colorAttachments[0].writeMask == (MTLColorWriteMaskRed | MTLColorWriteMaskBlue), "channel mask and output map combine");
    k.chmask = 15; fs->rf.omap_known = 0;
    CHECK(pso_desc(&k, vs, fs, nil, 0).colorAttachments[0].writeMask == MTLColorWriteMaskAll, "no recorded output map: the channel mask alone");
    CHECK(pf_chan_bits(MTLPixelFormatR8Unorm) == 0x1 && pf_chan_bits(MTLPixelFormatRG16Float) == 0x3 && pf_chan_bits(MTLPixelFormatRG11B10Float) == 0x7 && pf_chan_bits(MTLPixelFormatRGBA8Unorm) == 0xf,
          "stored channels per format");
    printf("output map write mask: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
