// The NVN mesh argument is produced on the GPU, not in CPU-visible command
// memory at record time. Check the real replay converter's grid and base.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

// A cold task/mesh pipeline must be ready for its first draw. Returning nil
// while a background compile runs drops geometry (and its depth) for whole frames.
static void first_mesh_pipeline(void) {
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "[[mesh]] void cold_mesh(mesh<V, void, 3, 1, topology::triangle> out) {\n"
        "out.set_vertex(0, V{float4(-1,-1,0,1)}); out.set_vertex(1, V{float4(3,-1,0,1)});\n"
        "out.set_vertex(2, V{float4(-1,3,0,1)}); out.set_index(0,0); out.set_index(1,1); out.set_index(2,2); out.set_primitive_count(1); }\n"
        options:nil error:&error];
    assert(library && !error);
    NvShader *mesh = [NvShader new];
    mesh->fn = [library newFunctionWithName:@"cold_mesh"]; mesh->mesh_threads = 1;
    strcpy(mesh->key, "cold_mesh");
    NvProg *raster = [NvProg new];
    state_reset(); memset(&g_pass, 0, sizeof g_pass);
    g_pass.dfmt = MTLPixelFormatDepth32Float; g_pass.samples = 1;
    g_mtk.ts = nil; g_mtk.ms = mesh;
    id<MTLRenderPipelineState> pipeline = mtk_pso(raster);
    assert(pipeline && "first mesh draw was dropped while its pipeline compiled");
    assert(mtk_pso(raster) == pipeline);
    g_mtk.ms = nil;
    puts("Cold mesh pipeline ready on first lookup and reused PASS");
}

int main(void) { @autoreleasepool {
    mtl_init(); first_mesh_pipeline(); size_t bytes = (size_t)getpagesize();
    uint32_t *guest = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guest != MAP_FAILED); mtl_pool_add(guest, bytes);
    id<MTLBuffer> seed = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    const uint32_t input[] = {3, 17, 0, 23}; memcpy(seed.contents, input, sizeof input); res_add(seed);
    void *buffer; uint64_t offset;
    assert(mtl_addr((uintptr_t)guest, sizeof input, &buffer, &offset));
    mtl_sub_begin();
    enc_b_copy_buffer(blit(), seed, 0, (__bridge id<MTLBuffer>)buffer, offset, sizeof input);
    const p_draw draw_3 = {.prim=DM_PRIM_MESH_TASKS, .ind=(uintptr_t)guest};
    const p_draw draw_0 = {.prim=DM_PRIM_MESH_TASKS, .ind=(uintptr_t)(guest + 2)};
    ixc_run(&draw_3, NULL); ixc_run(&draw_0, NULL);
    const ixc_ent *a = ixc_find(&draw_3, NULL), *b = ixc_find(&draw_0, NULL);
    assert(a && a->args && b && b->args && guest[0] == 0);
    id<MTLBuffer> converted = g_cur_arena->buf;
    size_t ao = (size_t)(a->args - converted.gpuAddress), bo = (size_t)(b->args - converted.gpuAddress);
    assert(ao + 20 <= converted.length && bo + 20 <= converted.length);
    // A deferred write to a third argument must be landed before snapshotting
    // the conversion epoch, so the GPU result is still found by the draw.
    const p_draw draw_queued = {.prim=DM_PRIM_MESH_TASKS, .ind=(uintptr_t)(guest + 4)};
    g_ubd[g_nubd++] = (ubd_ent){draw_queued.ind, draw_queued.ind + 8, seed.gpuAddress, 0, seed};
    ixc_run(&draw_queued, NULL);
    const ixc_ent *queued = ixc_find(&draw_queued, NULL);
    assert(queued && queued->args && !g_nubd);
    size_t qo = (size_t)(queued->args - converted.gpuAddress);
    assert(qo + 20 <= converted.length);
    mtl_sub_commit(); mtl_drain();
    const uint32_t expected_3[] = {3, 1, 1, 0, 17}, expected_0[] = {0, 1, 1, 0, 23};
    assert(!memcmp((const uint8_t *)converted.contents + ao, expected_3, sizeof expected_3));
    assert(!memcmp((const uint8_t *)converted.contents + bo, expected_0, sizeof expected_0));
    assert(!memcmp((const uint8_t *)converted.contents + qo, expected_3, sizeof expected_3));
    res_remove(seed); mtl_pool_del(guest); mtl_drain(); munmap(guest, bytes);
    puts("GPU-written mesh arguments: grid, first task and zero-work dispatch PASS");
} return 0; }
