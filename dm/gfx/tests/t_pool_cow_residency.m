// Host NVN pool-table and residency lifetime guarantees (Refs #1810):
// 1. pool_seen_scan updates live texture pool tables via copy-on-write (pool_snap_set), never mutating a buffer an in-flight submission may read, and bumps g_psnap_gen.
// 2. pool_walk past POOL_SUB_MS never returns a dirty table from an earlier submission whose retired textures may already have left the residency set, while still letting a table
//    validated in the current submission (or an unchanged table) stand.
// 3. res_check flags a placed texture removed from g_res_objects even when its MTLHeapTypePlacement heap remains resident, without false-positiving on MTLHeapTypeAutomatic stand-in heaps.
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
#define CHECK(c, msg) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
static uint64_t dummy(id<MTLBuffer> b) { return entry(b, (int)*(const uint32_t *)b.contents); }
static pool_snap *snap2d(void) {
    for (int k = 0; k < POOL_SNAPS; k++)
        if (g_psnap[k].buf && !g_psnap[k].samp && g_psnap[k].dtt == MTLTextureType2D && g_psnap[k].dpf == NVN_DPF_FLOAT && !g_psnap[k].base) return &g_psnap[k];
    return NULL;
}
static int in_res_objects(id o) {
    pthread_mutex_lock(&g_res_mu);
    int ok = [g_res_objects containsObject:o];
    pthread_mutex_unlock(&g_res_mu);
    return ok;
}
static void read_mark(int i) {
    pool_seen_gpu();
    atomic_fetch_or((_Atomic uint32_t *)g_seen_buf.contents + i / 32, 1u << (i % 32));
}
static int gpu_sample_2d(id<MTLBuffer> tab, int idx, float out[4]) {
    static id<MTLComputePipelineState> ps;
    if (!ps) {
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct P { uint n; ulong seen; array<texture2d<float>, 1> e; };\n"
            "kernel void k(constant P& p [[buffer(0)]], constant uint& i [[buffer(1)]], device float4 *o [[buffer(2)]]) { o[0] = (&p.e[0])[min(i, p.n)].read(uint2(0, 0), 0u); }\n";
        NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) { fprintf(stderr, "library: %s\n", err.description.UTF8String); return 0; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"k"];
        if (!fn) { fputs("pipeline: missing Metal function k\n", stderr); return 0; }
        ps = [g_dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps) { fprintf(stderr, "pipeline: %s\n", err.description.UTF8String); return 0; }
    }
    id<MTLBuffer> ob = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> ib = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *(uint32_t *)ib.contents = (uint32_t)idx;
    res_add(ob); res_add(ib); res_commit();
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    [cb useResidencySet:g_res];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    [ce setBuffer:tab offset:0 atIndex:0];
    [ce setBuffer:ib offset:0 atIndex:1];
    [ce setBuffer:ob offset:0 atIndex:2];
    [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    int ok = cb.status == MTLCommandBufferStatusCompleted && !cb.error;
    if (ok) memcpy(out, ob.contents, 16);
    res_remove(ob); res_remove(ib); res_commit();
    return ok;
}
static int approx(float x, uint8_t b) { return fabsf(x - (float)b / 255.0f) < 0.02f; }

int main(void) { @autoreleasepool {
    alarm(30);
    setenv("DM_NVN_POOL_STANDIN", "0", 1);   // keep unread entries as dummy so pool_seen_scan copy-on-write is tested directly
    mtl_init();

    enum { LEN = 4 << 20 };
    uint8_t *ma = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (ma == MAP_FAILED) abort();
    uint8_t *mb = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mb == MAP_FAILED) abort();
    uint8_t *mc = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mc == MAP_FAILED) abort();
    memset(ma, 0x22, 65536);
    memset(ma + 65536, 0x55, 65536);
    pool_t pa = {0}, pb = {0}, pc = {0};
    pool_builder_t pba = {.mem = ma, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_builder_t pbb = {.mem = mb, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_builder_t pbc = {.mem = mc, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pa, (const NVNmemoryPoolBuilder *)&pba), "CPU-visible pool initialized");
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pb, (const NVNmemoryPoolBuilder *)&pbb), "GPU-only pool B initialized");
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pc, (const NVNmemoryPoolBuilder *)&pbc), "GPU-only pool C initialized");

    NVNtextureBuilder tb = {0};
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 64); h_nvnTextureBuilderSetHeight(&tb, 64);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);

    NVNtexture t10 = {0}, t11 = {0};
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pa, 0);
    CHECK(h_nvnTextureInitialize(&t10, &tb), "t10 initialized");
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pa, 65536);
    CHECK(h_nvnTextureInitialize(&t11, &tb), "t11 initialized");
    mtl_reg_tex(10, (const tex_t *)&t10, NULL);
    mtl_reg_tex(11, (const tex_t *)&t11, NULL);

    // 1. pool_seen_scan copy-on-write: old buffer stays untouched for in-flight submissions while new buffer gets the newly materialized texture
    read_mark(10);
    mtl_sub_begin();
    g_walk_frame = atomic_load(&g_cur_frame); g_walk_spent = POOL_FRAME_BYTES; g_walk_req = 0;
    __unsafe_unretained id<MTLBuffer> tmp = nil;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &tmp); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b0 = tmp;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "initial pool table submission completed");
    CHECK(entry(b0, 10) != dummy(b0) && entry(b0, 11) == dummy(b0), "b0 holds t10 and dummy for unread t11");

    read_mark(11);
    uint64_t pgen0 = g_psnap_gen;
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu); pool_seen_scan(); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b1 = snap2d()->buf;
    CHECK(b1 != b0 && g_psnap_gen > pgen0, "pool_seen_scan forks the live table buffer and bumps g_psnap_gen");
    CHECK(entry(b0, 11) == dummy(b0), "in-flight buffer b0 is not mutated in place");
    CHECK(g_treg[11].view && entry(b1, 11) == g_treg[11].view_rid && entry(b1, 11) != dummy(b1), "forked buffer b1 names the newly materialized t11");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "copy-on-write submission completed");
    float px0[4] = {-1}, px1[4] = {-1};
    CHECK(gpu_sample_2d(b0, 11, px0) && approx(px0[0], 0), "GPU sampling old table b0[11] still reads dummy cleanly");
    CHECK(gpu_sample_2d(b1, 11, px1) && approx(px1[0], 0x55), "GPU sampling new table b1[11] reads t11 pixels cleanly");

    // 2. pool_walk past POOL_SUB_MS rejects a dirty table from an earlier submission whose placed texture was retired and drained
    NVNtexture t20 = {0}, t21 = {0};
    NVNtextureBuilder rtb = {0};
    h_nvnTextureBuilderSetDefaults(&rtb);
    h_nvnTextureBuilderSetTarget(&rtb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&rtb, 128); h_nvnTextureBuilderSetHeight(&rtb, 128);
    h_nvnTextureBuilderSetFormat(&rtb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetFlags(&rtb, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    h_nvnTextureBuilderSetStorage(&rtb, (NVNmemoryPool *)&pc, 0);
    CHECK(h_nvnTextureInitialize(&t20, &rtb), "placed RT texture t20 initialized");
    h_nvnTextureBuilderSetStorage(&rtb, (NVNmemoryPool *)&pb, 262144);
    CHECK(h_nvnTextureInitialize(&t21, &rtb), "placed RT texture t21 initialized");
    mtl_reg_tex(20, (const tex_t *)&t20, NULL);
    // Materialize t20 as a placed texture (as a render pass does), then put it into table b_s1 in submission S1
    mtl_sub_begin();
    id<MTLTexture> rt20 = tex_mtl_((const tex_t *)&t20, NULL);
    CHECK(rt20 && rt20.heap && rt20.heap.type == MTLHeapTypePlacement, "t20 is backed by a placement heap");
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &tmp); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b_s1 = tmp;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "S1 holding placed t20 completed");
    CHECK(entry(b_s1, 20) != dummy(b_s1) && in_res_objects(rt20), "b_s1 holds placed t20 and t20 is resident");

    // Finalize t20 and its memory pool pc (triggers own_pool_finalize_locked -> own_epoch() and retires rt20), then drain residency
    __weak id<MTLTexture> weak_rt20 = rt20;
    id<MTLHeap> rt20_heap = rt20.heap;
    h_nvnTextureFinalize(&t20);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pc);
    rt20 = nil;
    mtl_sub_begin();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "drain submission completed");
    defer_drain(); res_commit();
    CHECK(!weak_rt20 || !in_res_objects(weak_rt20), "retired placed texture t20 has left residency");
    (void)rt20_heap;

    // Next submission S2 with g_walk_ms already at POOL_SUB_MS: must NOT return stale b_s1 from S1
    mtl_sub_begin();
    g_walk_sub = g_sub_serial; g_walk_ms = POOL_SUB_MS;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &tmp); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b_s2 = tmp;
    CHECK(b_s2 != b_s1 && entry(b_s2, 20) == dummy(b_s2), "past POOL_SUB_MS in a new submission, a dirty prior-submission table is rebuilt instead of reused");
    CHECK(snap2d()->sync_sub == g_sub_serial, "rebuilt table records current sync_sub");

    // Same submission S2, now validated in S2: an unpatchable change + POOL_SUB_MS is allowed to stand for the remainder of S2
    mtl_reg_tex(21, (const tex_t *)&t21, NULL);
    pthread_mutex_lock(&g_smu); own_epoch(); pthread_mutex_unlock(&g_smu);
    g_walk_ms = POOL_SUB_MS;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &tmp); os_unfair_lock_unlock(&g_rmu);
    CHECK(tmp == b_s2, "within the same validated submission, POOL_SUB_MS still lets the table stand");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "S2 completed cleanly");
    float px20[4] = {-1};
    CHECK(gpu_sample_2d(b_s2, 20, px20) && approx(px20[0], 0), "GPU sampling rebuilt table b_s2[20] reads dummy cleanly");

    // 3. res_check flags a placed texture missing from g_res_objects even when its placement heap is resident, and accepts an automatic-heap texture
    g_res_check_on = 1; g_res_missing = 0;
    MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    pd.storageMode = MTLStorageModePrivate; pd.usage = MTLTextureUsageShaderRead;
    MTLSizeAndAlign psa = [g_dev heapTextureSizeAndAlignWithDescriptor:pd];
    MTLHeapDescriptor *phd = [MTLHeapDescriptor new]; phd.type = MTLHeapTypePlacement; phd.size = psa.size; phd.storageMode = MTLStorageModePrivate;
    id<MTLHeap> pheap = [g_dev newHeapWithDescriptor:phd];
    id<MTLTexture> ptex = [pheap newTextureWithDescriptor:pd offset:0];
    res_add(ptex); res_commit();
    res_check(ptex, "placed_resident", 0);
    CHECK(g_res_missing == 0, "res_check accepts a resident placement-heap texture");
    res_remove(ptex); res_commit();   // heap remains resident, placed texture removed
    CHECK(in_res_objects(pheap) && !in_res_objects(ptex), "placement heap stays resident while placed texture left g_res_objects");
    res_check(ptex, "placed_removed", 0);
    CHECK(g_res_missing == 1, "res_check flags a removed placement-heap texture even while its heap stays resident");
    res_remove(pheap); res_commit();

    MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    ad.storageMode = MTLStorageModeShared; ad.usage = MTLTextureUsageShaderRead;
    MTLHeapDescriptor *ahd = [MTLHeapDescriptor new]; ahd.type = MTLHeapTypeAutomatic; ahd.size = 256u << 10; ahd.storageMode = MTLStorageModeShared;
    id<MTLHeap> aheap = [g_dev newHeapWithDescriptor:ahd];
    id<MTLTexture> atex = [aheap newTextureWithDescriptor:ad];
    res_add(aheap); res_commit();   // automatic heap resident, child texture not added individually (as stand_alloc does)
    res_check(atex, "auto_heap_child", 0);
    CHECK(g_res_missing == 1, "res_check accepts an automatic-heap child texture whose heap is resident");
    res_remove(aheap); res_commit();
    g_res_check_on = 0;

    // Cleanup
    mtl_reg_tex(10, NULL, NULL); mtl_reg_tex(11, NULL, NULL); mtl_reg_tex(20, NULL, NULL); mtl_reg_tex(21, NULL, NULL);
    h_nvnTextureFinalize(&t10); h_nvnTextureFinalize(&t11); h_nvnTextureFinalize(&t21);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pa); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pb);
    mtl_drain();
    munmap(ma, LEN); munmap(mb, LEN); munmap(mc, LEN);
    printf("t_pool_cow_residency: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
} }
