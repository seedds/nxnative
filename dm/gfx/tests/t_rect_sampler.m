// Rectangle textures are addressed in texels (samp_px, tid_rect): a rectangle texture, by its own target or a view's, is told from a 2D one; the pixel-coordinate twin of a
// registered sampler (linear, repeat, mipmapped: what a game's depth sampler may be) is made, clamps instead of wrapping, is dropped when its ID is registered again; and on the GPU,
// a sample through it from an argument buffer at a texel centre in pixels reads that texel, as Fast Fusion's tornadoes read the depth at gl_FragCoord.xy
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
static void check(int ok, const char *what) { checks++; fails += !ok; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

int main(void) { @autoreleasepool {
    mtl_init();
    treg_t regs[5]; memset(regs, 0, sizeof regs); g_treg = regs; g_ntreg = 5;
    regs[1].ok = 1; regs[1].t.d.target = NVN_TEXTURE_TARGET_2D;
    regs[2].ok = 1; regs[2].t.d.target = NVN_TEXTURE_TARGET_RECTANGLE;
    regs[3].ok = 1; regs[3].t.d.target = NVN_TEXTURE_TARGET_2D; regs[3].has_v = 1; regs[3].v.set_target = 1; regs[3].v.target = NVN_TEXTURE_TARGET_RECTANGLE;
    regs[4].ok = 1; regs[4].t.d.target = NVN_TEXTURE_TARGET_RECTANGLE; regs[4].has_v = 1; regs[4].v.set_target = 1; regs[4].v.target = NVN_TEXTURE_TARGET_2D;
    check(!tid_rect(0) && !tid_rect(1) && tid_rect(2) && tid_rect(3) && !tid_rect(4) && !tid_rect(5), "rectangle by the texture's target or a view's, a view's 2D target wins, unknown IDs are not");
    g_treg = NULL; g_ntreg = 0;

    samp_par sp = {0}; sp.magf = 1; sp.minf = 5; sp.ws = sp.wt = sp.wr = 1; sp.lod_max = 1000; sp.aniso = 4;   // linear, mipmapped, repeat, anisotropic
    mtl_reg_samp(7, &sp);
    id<MTLSamplerState> s = samp_px(7);
    check(s != nil && samp_px(7) == s, "the twin of a linear repeat mipmapped sampler is made once");
    check(samp_px(8) == nil, "an unregistered sampler has no twin");

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:8 height:4 mipmapped:NO]; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td]; float px[32]; for (int i = 0; i < 32; i++) px[i] = (float)((i % 8) + 10 * (i / 8));
    [t replaceRegion:MTLRegionMake2D(0, 0, 8, 4) mipmapLevel:0 withBytes:px bytesPerRow:32];
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { texture2d<float> t [[id(0)]]; sampler s [[id(1)]]; };\n"
        "kernel void k(constant A& a [[buffer(0)]], constant float2 *uv [[buffer(1)]], device float *o [[buffer(2)]], uint i [[thread_position_in_grid]]) { o[i] = a.t.sample(a.s, uv[i]).x; }\n" options:nil error:&e];
    id<MTLComputePipelineState> cp = lib ? [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e] : nil;
    if (!cp) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    const float uv[4][2] = {{0.5f, 0.5f}, {5.5f, 2.5f}, {6.0f, 0.5f}, {20.5f, 1.5f}};   // texel centres, halfway between two texels, past the right edge
    const float want[4] = {0, 25, 5.5f, 17};   // the texel; linear between 5 and 6; clamped to the edge, not wrapped (repeat would read 4 + 10)
    id<MTLBuffer> args = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared], ub = [g_dev newBufferWithBytes:uv length:sizeof uv options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    uint64_t *a = args.contents; a[0] = t.gpuResourceID._impl; a[1] = s.gpuResourceID._impl;
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:cp]; [ce setBuffer:args offset:0 atIndex:0]; [ce setBuffer:ub offset:0 atIndex:1]; [ce setBuffer:out offset:0 atIndex:2]; [ce useResource:t usage:MTLResourceUsageRead];
    [ce dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const float *o = out.contents;
    for (int i = 0; i < 4; i++) { char w[96]; snprintf(w, sizeof w, "pixel coordinates (%g, %g) read %g, want %g", uv[i][0], uv[i][1], o[i], want[i]); check(fabsf(o[i] - want[i]) < 1e-4f, w); }

    mtl_reg_samp(7, &sp);
    check(g_samp_px[@7] == nil, "registering the ID again drops the twin");
    printf("t_rect_sampler: %d checks, %s\n", checks, fails ? "FAILED" : "ok");
    return fails != 0;
} }
