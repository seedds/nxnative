// Game-mode draw passes (DM_NVN_BARRIER=game): a draw pass ends with a barrier that lets the next pass's vertex work overlap its
// fragments, while the next pass's object (task) and mesh shaders wait for them. Each of the 64x64 fragments of a slow pass adds 1
// to a counter after a long dependent loop; the next production pass reads the counter from a vertex shader, a mesh shader and an
// object shader. The mesh and object readers must see all 4,096 adds. The vertex reader is the control that the window exists: it
// must read early at least once, so a failing control means this GPU shows no overlap (or the loop was optimized away), not a
// regression. Readers write counter + 1, so 0 means the reader never ran.
// On an M4 Max a barrier with only mesh in its later stages already holds the object reader back, so the object reader pins the
// gap, not the object bit itself; object is in the mask because Metal tracks it as its own stage.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { SIDE = 64, ALL = SIDE * SIDE, RUNS = 32 };
static NSString *const src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct VO { float4 p [[position]]; };\n"
    "vertex VO v_full(uint n [[vertex_id]]) { float2 q[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; VO o; o.p = float4(q[n], 0, 1); return o; }\n"
    "fragment float4 f_slow(VO in [[stage_in]], device atomic_uint *count [[buffer(0)]], constant uint &iters [[buffer(1)]]) {\n"
    "  float x = in.p.x; for (uint i = 0; i < iters; i++) x = fma(x, 0.9999999f, 0.25f);\n"
    "  atomic_fetch_add_explicit(count, x < -1e30f ? 2u : 1u, memory_order_relaxed); return float4(x * 0.0f, 0, 0, 1); }\n"
    "vertex VO v_read(uint n [[vertex_id]], device atomic_uint *count [[buffer(0)]], device uint *out [[buffer(2)]]) {\n"
    "  if (n == 0) out[0] = atomic_load_explicit(count, memory_order_relaxed) + 1; VO o; o.p = float4(2, 2, 0, 1); return o; }\n"
    "using M = metal::mesh<VO, void, 3, 1, topology::triangle>;\n"
    "[[mesh]] void m_read(M m, uint t [[thread_index_in_threadgroup]], device atomic_uint *count [[buffer(0)]], device uint *out [[buffer(2)]]) {\n"
    "  if (t == 0) { out[1] = atomic_load_explicit(count, memory_order_relaxed) + 1; m.set_primitive_count(0); } }\n"
    "struct PL { uint x; };\n"
    "[[object]] void o_read(object_data PL &p [[payload]], mesh_grid_properties g, uint t [[thread_index_in_threadgroup]], device atomic_uint *count [[buffer(0)]], device uint *out [[buffer(2)]]) {\n"
    "  if (t == 0) { out[2] = atomic_load_explicit(count, memory_order_relaxed) + 1; p.x = 0; g.set_threadgroups_per_grid(uint3(1, 1, 1)); } }\n"
    "[[mesh]] void m_nop(M m, uint t [[thread_index_in_threadgroup]]) { if (t == 0) m.set_primitive_count(0); }\n"
    "fragment float4 f_nop(VO in [[stage_in]]) { return float4(0); }\n";

static tex_t target(void) {
    static uint64_t serial = 990000; serial++;
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = (int64_t)serial << 20};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = SIDE, .height = SIDE, .depth = 1, .levels = 1, .samples = 1};
    return t;
}
static id<MTL4RenderCommandEncoder> pass(tex_t *t) {
    state_reset(); memset(&R, 0, sizeof R);
    R.has_rt = R.has_sc = 1; R.sc = (p_scissor){0, 0, SIDE, SIDE}; R.rt.n = 1; R.rt.color[0] = t; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    id<MTL4RenderCommandEncoder> e = rp_begin_ex(0);
    if (e) { enc_r_viewport(e, (MTLViewport){0, 0, SIDE, SIDE, 0, 1}); enc_r_scissor(e, (MTLScissorRect){0, 0, SIDE, SIDE}); enc_r_cull(e, MTLCullModeNone); }
    return e;
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_BARRIER", "game", 1); mtl_init();
    CHECK(barrier_game() && !g_image_heap_n, "draw passes are ordered by the game's barriers");
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.vertexFunction = [lib newFunctionWithName:@"v_full"]; d.fragmentFunction = [lib newFunctionWithName:@"f_slow"];
    id<MTLRenderPipelineState> slow = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    d.vertexFunction = [lib newFunctionWithName:@"v_read"]; d.fragmentFunction = [lib newFunctionWithName:@"f_nop"];
    id<MTLRenderPipelineState> vread = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new]; m.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    m.meshFunction = [lib newFunctionWithName:@"m_read"]; m.fragmentFunction = [lib newFunctionWithName:@"f_nop"]; m.maxTotalThreadsPerMeshThreadgroup = 32;
    id<MTLRenderPipelineState> mread = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&error];
    m.objectFunction = [lib newFunctionWithName:@"o_read"]; m.meshFunction = [lib newFunctionWithName:@"m_nop"];
    m.maxTotalThreadsPerObjectThreadgroup = 1; m.payloadMemoryLength = 16;
    id<MTLRenderPipelineState> oread = [g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&error];
    CHECK(slow && vread && mread && oread, "pipelines compile");
    if (!slow || !vread || !mread || !oread) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLBuffer> count = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared], it = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared],
                  out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    res_add(count); res_add(it); res_add(out); *(uint32_t *)it.contents = 400000;   // a few milliseconds of fragment work on an M4 Max; a slower GPU gets a longer window
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 3;
    id<MTL4ArgumentTable> at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    [at setAddress:count.gpuAddress atIndex:0]; [at setAddress:it.gpuAddress atIndex:1]; [at setAddress:out.gpuAddress atIndex:2];
    tex_t a = target(), b = target();
    // Without object and mesh in the barrier, 18 to 29 of 32 mesh and object reads came early (M4 Max): 32 clean runs by chance at the low end would be about 1 in 300 billion
    int early[3] = {0}, ran[3] = {1, 1, 1};
    for (int i = 0; i < RUNS; i++) for (int k = 0; k < 3; k++) {
        memset(count.contents, 0, 16); memset(out.contents, 0, 16);
        mtl_sub_begin();
        id<MTL4RenderCommandEncoder> e = pass(&a);
        [e setArgumentTable:at atStages:MTLRenderStageVertex | MTLRenderStageFragment]; enc_r_pso(e, slow); enc_r_draw(e, MTLPrimitiveTypeTriangle, 0, 3);
        rp_end();
        CHECK(g_last_end_relaxed, "the slow pass ends with the game-mode barrier");
        uint64_t full = g_bar_full;
        e = pass(&b);
        CHECK(g_bar_full == full, "the reading pass opens without a full barrier");
        if (k == 0) { [e setArgumentTable:at atStages:MTLRenderStageVertex | MTLRenderStageFragment]; enc_r_pso(e, vread); enc_r_draw(e, MTLPrimitiveTypeTriangle, 0, 3); }
        else { [e setArgumentTable:at atStages:(k == 2 ? MTLRenderStageObject : 0) | MTLRenderStageMesh | MTLRenderStageFragment]; enc_r_pso(e, k == 2 ? oread : mread);
               [e drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)]; }
        rp_end(); mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "both passes complete without Metal errors");
        uint32_t v = ((uint32_t *)out.contents)[k];
        ran[k] &= v != 0; early[k] += v != 0 && v != ALL + 1;
    }
    printf("reads before all %d fragments added: vertex %d, mesh %d, object %d of %d\n", ALL, early[0], early[1], early[2], RUNS);
    CHECK(ran[0] && ran[1] && ran[2], "every reader ran");
    CHECK(early[0] > 0, "control: the next pass's vertex work starts while the slow fragments still run (the window this test needs)");
    CHECK(early[1] == 0, "the next pass's mesh shader waits for every fragment of the slow pass");
    CHECK(early[2] == 0, "the next pass's object shader waits for every fragment of the slow pass");
    printf("== %d checks, %d failed ==\n", checks, failed); return failed != 0;
} }
