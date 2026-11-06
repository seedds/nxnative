// A 32-bit integer texture in a slot a shader declares float gets the float view of the same size (tex_float_fit, tbuf_view_format), and the shader takes the bits back with as_type<int>.
// That holds only if the read hands the stored bits over unchanged, in compute and in a fragment shader: small integers are denormal floats, and a light list holds NaN patterns and negative values too.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static const uint32_t words[8] = {0u, 1u, 5u, 0x7fffffu, 0x80000000u, 0x7f800001u, 0x7fc00123u, 0xffffffffu};

int main(void) { @autoreleasepool {
    int failed = 0;
    failed += tbuf_view_format(MTLPixelFormatR32Uint, NVN_DPF_FLOAT) != MTLPixelFormatR32Float;
    failed += tbuf_view_format(MTLPixelFormatR32Sint, NVN_DPF_FLOAT) != MTLPixelFormatR32Float;
    failed += tbuf_view_format(MTLPixelFormatRG16Uint, NVN_DPF_FLOAT) != MTLPixelFormatRG16Uint;   // no float format of that size keeps its bits
    failed += tbuf_view_format(MTLPixelFormatR32Uint, NVN_DPF_UINT) != MTLPixelFormatR32Uint;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d]; [t replaceRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0 withBytes:words bytesPerRow:32];
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:MTLPixelFormatR32Float];
    id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void cs(texture2d<float> t [[texture(0)]], device uint *o [[buffer(0)]], uint id [[thread_position_in_grid]]) { o[id] = as_type<uint>(t.read(uint2(id, 0), 0).x); }\n"
        "vertex float4 vs(uint id [[vertex_id]]) { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0); }\n"
        "fragment uint fs(float4 p [[position]], texture2d<float> t [[texture(0)]]) { return as_type<uint>(t.read(uint2(p.xy), 0).x); }\n" options:nil error:&e];
    if (!lib) { printf("shader source: %s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cs"] error:&e];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!cp || !rp) { printf("pipelines: %s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLBuffer> co = [dev newBufferWithLength:32 options:MTLResourceStorageModeShared], fo = [dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:8 height:1 mipmapped:NO]; rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rd];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:cp]; [ce setTexture:v atIndex:0]; [ce setBuffer:co offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [ce endEncoding];
    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor]; rpd.colorAttachments[0].texture = rt; rpd.colorAttachments[0].loadAction = MTLLoadActionClear; rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rpd]; [re setRenderPipelineState:rp]; [re setFragmentTexture:v atIndex:0]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 1, 1) toBuffer:fo destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:32]; [be endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    const uint32_t *c = co.contents, *f = fo.contents;
    for (int i = 0; i < 8; i++) { int ok = c[i] == words[i] && f[i] == words[i]; failed += !ok; printf("%08x: compute %08x fragment %08x %s\n", words[i], c[i], f[i], ok ? "ok" : "CHANGED"); }
    printf("t_int_float_view: %s (%s)\n", failed ? "FAILED" : "ok", dev.name.UTF8String);
    return failed != 0;
} }
