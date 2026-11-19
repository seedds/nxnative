// Tiled resolves must produce pixels, including the layers selected when recorded.
// Distinct sample colours distinguish a multisample average from a sample-zero copy.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, what) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", what); } } while (0)

static tex_t texture(int samples, int layers) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=nvn_new_serial()};
    t.d = (nvn_texdesc){.target=samples > 1 ? (layers > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY : NVN_TEXTURE_TARGET_2D_MULTISAMPLE)
                         : (layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D),
        .format=NVN_FORMAT_RGBA8, .width=8, .height=8, .depth=layers, .levels=1, .samples=samples};
    return t;
}
static void completed(id<MTLCommandBuffer> cb) {
    [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted, "fixture GPU work completed");
}
static void run_case(id<MTLLibrary> lib, int tiled, int views, int samples) {
    tex_t src = texture(samples, views ? 2 : 1), dst = texture(1, views ? 3 : 1);
    id<MTLTexture> s = [g_dev newTextureWithDescriptor:tex_desc(&src, 0)];
    id<MTLTexture> d = [g_dev newTextureWithDescriptor:tex_desc(&dst, 0)];
    CHECK(s && d, "resolve textures allocated"); if (!s || !d) return;
    res_add(s); res_add(d); serial_put(src.serial, s); serial_put(dst.serial, d);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"v"]; pd.fragmentFunction = [lib newFunctionWithName:@"f"];
    pd.rasterSampleCount = samples; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *err = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
    CHECK(ps != nil, "sample-colour fixture pipeline builds"); if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return; }
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    for (unsigned layer = 0; layer < s.arrayLength; layer++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = s; rp.colorAttachments[0].slice = layer;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:ps]; [e setFragmentBytes:&layer length:sizeof layer atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    }
    for (unsigned layer = 0; layer < d.arrayLength; layer++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d; rp.colorAttachments[0].slice = layer;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1,0,1,1);
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    }
    completed(cb);
    cmd_t cmd; h_nvnCommandBufferInitialize((NVNcommandBuffer *)&cmd, NULL);
    h_nvnCommandBufferBeginRecording((NVNcommandBuffer *)&cmd);
    tex_t rs = src, rd = dst;
    view_t sv = {.set_layers=1, .min_layer=1, .nlayers=1}, dv = {.set_layers=1, .min_layer=2, .nlayers=1};
    const char *name = views ? (tiled ? "nvnCommandBufferTiledDownsampleTextureView" : "nvnCommandBufferDownsampleTextureView")
                             : (tiled ? "nvnCommandBufferTiledDownsample" : "nvnCommandBufferDownsample");
    void *fn = nvn_lookup(name); CHECK(fn != NULL, "NVN entry point resolves");
    if (views) ((NVNT_nvnCommandBufferDownsampleTextureView)fn)((NVNcommandBuffer *)&cmd, (NVNtexture *)&rs, (NVNtextureView *)&sv, (NVNtexture *)&rd, (NVNtextureView *)&dv);
    else ((NVNT_nvnCommandBufferDownsample)fn)((NVNcommandBuffer *)&cmd, (NVNtexture *)&rs, (NVNtexture *)&rd);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording((NVNcommandBuffer *)&cmd);
    // The API objects can change after recording. The record owns their descriptors.
    memset(&rs, 0, sizeof rs); memset(&rd, 0, sizeof rd); sv.min_layer = dv.min_layer = 0;
    uint8_t *record = nvn_rec_linear(handle, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(record);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "resolve submission completed");
    CHECK(!mtl_last_commit_error(), "resolve has no Metal error"); free(record);
    id<MTLBuffer> out = [g_dev newBufferWithLength:256*d.arrayLength options:MTLResourceStorageModeShared];
    CHECK(out != nil, "readback buffer allocated"); if (!out) abort();
    cb = [q commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    for (unsigned layer = 0; layer < d.arrayLength; layer++)
        [blit copyFromTexture:d sourceSlice:layer sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
              toBuffer:out destinationOffset:256*layer destinationBytesPerRow:32 destinationBytesPerImage:256];
    [blit endEncoding]; completed(cb);
    const uint8_t *pixels = out.contents;
    for (unsigned layer = 0; layer < d.arrayLength; layer++) {
        int selected = !views || layer == 2, good = 1;
        // One red sample per pixel in layer0, every sample red in layer1; green is half the samples. B/A are always1.
        int red = selected ? (views ? 255 : samples == 2 ? 128 : 64) : 255;
        int green = selected ? 128 : 0;
        for (int i = 0; i < 64; i++) {
            const uint8_t *p = pixels + layer*256 + i*4;
            good &= abs((int)p[0]-red) <= 1 && abs((int)p[1]-green) <= 1 && p[2] == 255 && p[3] == 255;
        }
        CHECK(good, selected ? "selected layer contains the averaged samples" : "other destination layers keep their pixels");
    }
    printf("%s samples=%d: checked pixels and retained recording\n", name, samples);
    h_nvnCommandBufferFinalize((NVNcommandBuffer *)&cmd); mtl_tex_release(&src); mtl_tex_release(&dst);
}
int main(void) { @autoreleasepool {
    mtl_init(); NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }\n"
        "fragment float4 f(uint s[[sample_id]], constant uint &layer[[buffer(0)]]) { return float4(layer || s==0,s%2==0,1,1); }\n"
        options:nil error:&err];
    CHECK(lib != nil, "synthetic sample shaders compile"); if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    for (int samples = 2; samples <= 4; samples *= 2) for (int views = 0; views <= 1; views++) for (int tiled = 0; tiled <= 1; tiled++) run_case(lib, tiled, views, samples);
    printf("tiled resolves: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
