// Cross-submission write-after-read on the GPU, through the replay's own encoders (PRAGMATA's ship-rear / cabin light flicker, 2026-11-06: was the next frame's light culling
// rewriting the cluster grid while this frame's lighting still read it?). Submission A: one draw pass whose fragment shader spins (a bounded loop, calibrated to about 20-40 ms,
// never past about 80 ms) and then reads a word of a storage buffer and a texel of a 3D R32Uint texture (the address depends on the loop, so the read cannot be hoisted) into an
// RG32Uint target. Submission B, committed right after A without a wait: a dispatch in the copy encoder (dispatch_enc, as every game dispatch) writing the buffer and, as an image,
// the texture. Every pixel of A must show the values from before B. Cases: the draw pass begun / ended as a game pass (enc_r_begin_pass / enc_r_end_pass with
// DM_NVN_BARRIER=game: the relaxed end barrier, the weakest the replay sets) and as a full one (enc_r_begin / enc_r_end). Control: B in a compute encoder without any barrier
// and a pass without any (neither is something the replay makes) show whether this GPU runs B under A at all; if it never does, the test cannot see a missing order on this machine and says so.
// Run only when no game is running (sw2_tools/safety/gpu_preflight.sh): make GAME=<id> check_t_xsub_war
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
enum { W = 64, H = 64, NW = 64, TD = 4 };   // target; buffer words; the 3D texture is TD^3
static const double AIM_MS = 25, STOP_MS = 15, CAP_MS = 80;

static id<MTLRenderPipelineState> t_rps; static id<MTLComputePipelineState> t_cps;
static id<MTLTexture> g_rt, g_t3; static uint8_t *g_mem; static uint64_t g_xa; static id<MTLBuffer> g_lbuf;   // g_xa: the buffer's guest address (a pool), g_lbuf: the loop count

static void reset_inputs(void) {   // buffer and texture back to 1, the target to 0 (the GPU is idle: every case waits)
    uint32_t *x = (uint32_t *)(uintptr_t)g_xa; for (int i = 0; i < NW; i++) x[i] = 1;
    uint32_t ones[TD * TD * TD]; for (int i = 0; i < TD * TD * TD; i++) ones[i] = 1;
    [g_t3 replaceRegion:MTLRegionMake3D(0, 0, 0, TD, TD, TD) mipmapLevel:0 slice:0 withBytes:ones bytesPerRow:4 * TD bytesPerImage:4 * TD * TD];
    static uint32_t zero[W * H * 2]; [g_rt replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:zero bytesPerRow:8 * W];
}
static uint64_t xgpu(void) { void *b; uint64_t o, g; if (!mtl_gpu_addr(g_xa, 4 * NW, &b, &o, &g)) { fprintf(stderr, "the buffer is in no pool\n"); exit(1); } return g; }
// Submission A: the reading pass. full: 1 enc_r_begin / enc_r_end (full barriers), 0 the game pass (enc_r_begin_pass / enc_r_end_pass), 2 no barrier at all (the control)
static uint64_t sub_read(uint32_t loops, int full) {
    mtl_sub_begin();
    *(uint32_t *)g_lbuf.contents = loops;
    MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
    d.colorAttachments[0].texture = g_rt; d.colorAttachments[0].loadAction = MTLLoadActionClear; d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTL4RenderCommandEncoder> e = full == 2 ? [(id<MTL4CommandBuffer>)g_cb renderCommandEncoderWithDescriptor:d] : full ? enc_r_begin(d) : enc_r_begin_pass(d);
    if (full == 2) { [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; }
    [g_ft setAddress:xgpu() atIndex:0]; [g_ft setAddress:g_lbuf.gpuAddress atIndex:1]; [g_ft setTexture:g_t3.gpuResourceID atIndex:0];
    [e setRenderPipelineState:t_rps]; [e setViewport:(MTLViewport){0, 0, W, H, 0, 1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    if (full == 2) { [e endEncoding]; g_last_end_relaxed = 0; } else if (full) enc_r_end(e); else enc_r_end_pass(e);
    return mtl_sub_commit();
}
// Submission B: the writer. raw: a compute encoder with no barrier at all (the control), else the copy encoder every game dispatch goes into
static uint64_t sub_write(int raw) {
    mtl_sub_begin();
    id<MTL4ComputeCommandEncoder> e = raw ? [(id<MTL4CommandBuffer>)g_cb computeCommandEncoder] : dispatch_enc();
    if (raw) [e setArgumentTable:g_ct];
    [g_ct setAddress:xgpu() atIndex:0]; [g_ct setTexture:g_t3.gpuResourceID atIndex:0];
    [e setComputePipelineState:t_cps];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(NW, 1, 1)];
    if (raw) [e endEncoding];
    args_dirty_all();
    return mtl_sub_commit();
}
static double alone_ms(uint32_t loops) {   // A by itself, commit to completion
    reset_inputs(); uint64_t t0 = bp_now(); int ok = mtl_wait(sub_read(loops, 1), 3000000000ull); double ms = (bp_now() - t0) / 1e6;
    CHECK(ok && !mtl_last_commit_error(), "the reading pass completes"); return ms;
}
// Pixels of the target that saw B's buffer write, B's texture write, and the pixels A did not draw (still 0)
static void count(int *nb, int *nt, int *nz) {
    static uint32_t px[W * H * 2]; [g_rt getBytes:px bytesPerRow:8 * W fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    *nb = *nt = *nz = 0; for (int i = 0; i < W * H; i++) { *nb += px[2 * i] == 2; *nt += px[2 * i + 1] == 2; *nz += px[2 * i] == 0 || px[2 * i + 1] == 0; }
}
static void trial(const char *name, uint32_t loops, int full, int raw, int *nb, int *nt) {
    reset_inputs();
    uint64_t fa = sub_read(loops, full), fb = sub_write(raw);
    int ok = mtl_wait(fb, 3000000000ull) && mtl_wait(fa, 3000000000ull);
    CHECK(ok && !mtl_last_commit_error(), "%s: both submissions complete", name);
    int z; count(nb, nt, &z);
    CHECK(z == 0, "%s: the pass drew every pixel (%d not)", name, z);
    uint32_t xv = ((uint32_t *)(uintptr_t)g_xa)[5]; CHECK(xv == 2, "%s: B wrote the buffer (%u)", name, xv);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_BARRIER", "game", 1);   // enc_r_begin_pass / enc_r_end_pass take the relaxed path (PRAGMATA's profile is full: the full case below)
    mtl_init(); state_reset(); memset(&R, 0, sizeof R);
    CHECK(barrier_game() == 1, "DM_NVN_BARRIER=game is on");
    NSError *er = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vt(uint i [[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0.5, 1); }\n"
        "fragment uint4 fr(float4 p [[position]], device const uint *x [[buffer(0)]], constant uint &loops [[buffer(1)]], texture3d<uint, access::read> t [[texture(0)]]) {\n"
        "    uint h = uint(p.x) * 977u + uint(p.y) * 131u + 1u; for (uint i = 0; i < loops; i++) h = h * 1664525u + 1013904223u;\n"
        "    uint k = (h >> 7) & 63u; return uint4(x[k], t.read(uint3(k & 3u, (k >> 2) & 3u, (k >> 4) & 3u)).x, h, 1); }\n"
        "kernel void wr(device uint *x [[buffer(0)]], texture3d<uint, access::write> t [[texture(0)]], uint i [[thread_position_in_grid]]) {\n"
        "    x[i] = 2u; t.write(uint4(2u), uint3(i & 3u, (i >> 2) & 3u, (i >> 4) & 3u)); }\n" options:nil error:&er];
    if (!lib) { fprintf(stderr, "%s\n", er.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vt"]; pd.fragmentFunction = [lib newFunctionWithName:@"fr"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRG32Uint;
    t_rps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&er]; t_cps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"wr"] error:&er];
    if (!t_rps || !t_cps) { fprintf(stderr, "pipelines: %s\n", er.localizedDescription.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Uint width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; g_rt = [g_dev newTextureWithDescriptor:td];
    MTLTextureDescriptor *t3 = [MTLTextureDescriptor new]; t3.textureType = MTLTextureType3D; t3.pixelFormat = MTLPixelFormatR32Uint; t3.width = t3.height = t3.depth = TD;
    t3.storageMode = MTLStorageModeShared; t3.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; g_t3 = [g_dev newTextureWithDescriptor:t3];
    g_lbuf = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    size_t msz = (size_t)getpagesize(); if (posix_memalign((void **)&g_mem, msz, msz)) abort(); memset(g_mem, 0, msz); mtl_pool_add(g_mem, msz); g_xa = (uint64_t)(uintptr_t)g_mem;
    res_add(g_rt); res_add(g_t3); res_add(g_lbuf); res_commit();

    // calibration: A alone, the loop count grown at most 4x a step from a pass that takes well under a millisecond, until it takes STOP_MS (never extrapolated past CAP_MS)
    uint32_t loops = 256; double ms = alone_ms(loops);
    while (ms < STOP_MS && loops < (1u << 26)) { loops *= 4; ms = alone_ms(loops); if (ms > CAP_MS) break; }
    if (ms < STOP_MS) { CHECK(0, "the reading pass never took %.0f ms (%.2f ms at %u loops)", STOP_MS, ms, loops); goto done; }
    if (ms < AIM_MS) { uint32_t m = (uint32_t)(loops * (AIM_MS / ms)); loops = m; ms = alone_ms(loops); }
    printf("  calibration: the reading pass alone takes %.1f ms at %u loops a pixel (%dx%d)\n", ms, loops, W, H);

    enum { TRIALS = 4 };
    int ctl_b = 0, ctl_t = 0;
    for (int k = 0; k < TRIALS; k++) { int nb, nt;
        trial("game pass, copy-encoder dispatch", loops, 0, 0, &nb, &nt);
        CHECK(nb == 0 && nt == 0, "game pass, copy-encoder dispatch, trial %d: %d pixels read B's buffer write, %d B's texture write (must be 0)", k, nb, nt);
        trial("full pass, copy-encoder dispatch", loops, 1, 0, &nb, &nt);
        CHECK(nb == 0 && nt == 0, "full pass, copy-encoder dispatch, trial %d: %d pixels read B's buffer write, %d B's texture write (must be 0)", k, nb, nt);
        trial("control: pass and dispatch with no barrier", loops, 2, 1, &nb, &nt); ctl_b += nb; ctl_t += nt;
        trial("game pass, dispatch with no barrier of its own", loops, 0, 1, &nb, &nt);   // A's end barrier alone (the relaxed producer barrier) must hold B
        CHECK(nb == 0 && nt == 0, "game pass, unbarriered dispatch, trial %d: %d / %d pixels saw B (must be 0)", k, nb, nt);
    }
    printf("  ordered cases: 0 pixels of %d saw B in %d trials each (or a FAIL above)\n", W * H, TRIALS);
    printf("  control (no barrier in B): %d buffer / %d texture pixel reads of B's values over %d trials\n", ctl_b, ctl_t, TRIALS);
    if (!ctl_b && !ctl_t) printf("  NOTE: this GPU never ran B under A even without a barrier: the ordered cases above prove nothing on this machine\n");
done:
    mtl_drain(); mtl_pool_del(g_mem); free(g_mem);
    printf("t_xsub_war: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
