// Sparse mappings as notes (treg_sparse_notes): a mapping request that changes a sparse texture's pages, with no registration, serial change or CPU write, reaches the texture
// ID views, the pool tables and the handle tables at the next submission, and the GPU reads the newly mapped page; a request outside the texture notes nothing; serial changes
// while a sparse pool exists keep the pool tables. Physical page contents are literal colours. After the fixture of PR #1721 (NicholasDominici)
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
_Static_assert(DMSH_POOL_HEAD == 16, "the reader below takes the table head as 16 bytes");
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void next_sub(void) { mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); }
// The pixel at (0, 0) of element `slot` of a resource-ID table, read on the GPU; then the next submission
static uint32_t gpu_read(id<MTLBuffer> table, unsigned slot) {
    static id<MTLComputePipelineState> ps;
    if (!ps) {
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct Pool { uint n, pad; ulong seen; texture2d<float> t[2]; };\n"
                              "kernel void read_pool(constant Pool &p [[buffer(0)]], device uint *out [[buffer(1)]]) { uint4 c = uint4(round(p.t[out[1]].read(uint2(0)) * 255.0f)); out[0] = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }\n"
                                                     options:nil error:&error];
        id<MTLFunction> fn = [lib newFunctionWithName:@"read_pool"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
        CHECK(ps != nil, "the table reader compiles"); if (!ps) return 0;
    }
    uint32_t initial[2] = {0, slot};
    id<MTLBuffer> out = [g_dev newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared]; res_add(out);
    MTL4ArgumentTableDescriptor *d = [MTL4ArgumentTableDescriptor new]; d.maxBufferBindCount = 2;
    id<MTL4ArgumentTable> args = rec_table([g_dev newArgumentTableWithDescriptor:d error:nil]);
    [args setAddress:table.gpuAddress atIndex:0]; [args setAddress:out.gpuAddress atIndex:1];
    id<MTL4ComputeCommandEncoder> enc = enc_c_begin(); [enc setArgumentTable:args]; enc_c_pso(enc, ps);
    enc_c_dispatch(enc, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1)); enc_c_end(enc);
    next_sub();
    CHECK(!mtl_last_commit_error(), "the reader's submission completes");
    return *(uint32_t *)out.contents;
}
static int map(pool_t *virt, pool_t *phys, uint64_t voff, uint64_t poff, uint64_t size) {
    map_req req = {.phys = (NVNmemoryPool *)phys, .voff = (int64_t)voff, .poff = (int64_t)poff, .size = size};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)virt, 1, (const NVNmappingRequest *)&req);
}
static uint32_t gpu_read_tex(id<MTLTexture> tex) {
    static id<MTLComputePipelineState> ps;
    static id<MTLCommandQueue> queue;
    if (!ps) {
        NSError *error = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                              "kernel void read_one(texture2d<float> t [[texture(0)]], device uint *out [[buffer(0)]]) { uint4 c = uint4(round(t.read(uint2(0)) * 255.0f)); out[0] = c.x | (c.y << 8) | (c.z << 16) | (c.w << 24); }\n"
                                                     options:nil error:&error];
        id<MTLFunction> fn = [lib newFunctionWithName:@"read_one"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
        queue = [g_dev newCommandQueue];
        CHECK(ps != nil && queue != nil, "the texture reader compiles"); if (!ps || !queue) return 0;
    }
    if (!tex) return 0;
    id<MTLBuffer> out = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps]; [ce setTexture:tex atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
    [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "the texture readback completes");
    return *(uint32_t *)out.contents;
}
struct bg_fill { uint32_t *dst; uint32_t color; };
static void *bg_fill_fn(void *arg) {
    struct bg_fill *f = arg;
    dm_guest_write_prepare(f->dst, 0x1000);
    for (unsigned i = 0; i < 0x1000 / 4; i++) f->dst[i] = f->color;
    return NULL;
}
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    const uint32_t A = 0xff112233, B = 0xff445566, C = 0xff778899;
    uint8_t *mem = NULL; posix_memalign((void **)&mem, 16384, 1 << 20); memset(mem, 0, 1 << 20);
    for (unsigned i = 0; i < 0x1000 / 4; i++) { ((uint32_t *)mem)[i] = A; ((uint32_t *)(mem + 0x10000))[i] = B; ((uint32_t *)(mem + 0x20000))[i] = C; }
    pool_t phys = {0}, virt = {0}; pool_builder_t pb = {.mem = mem, .size = 1 << 20, .flags = 0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys, (NVNmemoryPoolBuilder *)&pb), "physical pool");
    pb = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};   // larger than the application pool: sparse from the start
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virt, (NVNmemoryPoolBuilder *)&pb), "sparse virtual pool");
    CHECK(map(&virt, &phys, 0, 0, 0x4000), "first mapping");
    CHECK(nvn_sparse_pool(&virt), "the pool reads through its page table");
    const uint64_t virtual_base = (uint64_t)(uintptr_t)virt.vp->base;
    CHECK(mtl_addr_known(virtual_base, 0x4000), "the new sparse run is immediately visible to a range lookup");
    pthread_mutex_lock(&g_mu);
    int run_index = pool_peek(virtual_base);
    CHECK(run_index >= 0 && !g_pools[run_index].buf, "mapping and probing the sparse run leave its Metal buffer deferred");
    pthread_mutex_unlock(&g_mu);
    void *run_buffer = NULL;
    uint64_t run_offset = UINT64_MAX, run_gpu = 0;
    CHECK(mtl_gpu_addr(virtual_base, 0x4000, &run_buffer, &run_offset, &run_gpu) &&
          run_buffer && !run_offset && run_gpu, "the first GPU lookup makes a buffer for the exact range");
    // A 32x32 RGBA8 texture: one 4 KB page at the pool's start
    NVNtextureBuilder tb; NVNtexture tx; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, 0);
    CHECK(h_nvnTextureInitialize(&tx, &tb), "sparse texture");
    mtl_reg_tex(1, (const tex_t *)&tx, NULL);
    g_seen_done[0] |= 2; g_seen_typed[0][0] |= 2;   // a shader has read ID 1 through a 2D table: the walk makes its texture (no stand-in)
    __unsafe_unretained id<MTLBuffer> table = nil;
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == A, "the pool table reads the first mapping's page");

    // The texture uses physical pages [0, 0x4000), regardless of its guest virtual address.
    mtl_sub_copy((uintptr_t)mem + 0x20000, (uintptr_t)mem + 0x10000, sizeof A);
    CHECK(!b2b_texture_dep((const tex_t *)&tx), "a copy to unrelated physical pages leaves a sparse upload independent");
    mtl_sub_copy((uintptr_t)mem + 0x20000, (uintptr_t)mem, sizeof A);
    CHECK(b2b_texture_dep((const tex_t *)&tx), "a copy to the backing of a sparse upload is a dependency");
    next_sub();
    dm_guest_write_prepare(mem, sizeof A);
    dm_guest_write_prepare(mem + 0x10000, sizeof B);
    *(uint32_t *)mem = A;
    *(uint32_t *)(mem + 0x10000) = B;

    // Serial changes while a sparse pool exists keep the table: they note no sparse texture
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); id<MTLBuffer> before = table; uint64_t builds = g_stat_pool_builds;
    for (int i = 0; i < 128; i++) { serial_put(98000 + i, [NvProg new]); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table); }
    CHECK(table == before && g_stat_pool_builds == builds, "128 program changes keep the pool table and build none");

    // A request past the texture's storage (a sparse texture's size is whole 64 KB tiles): nothing noted at the next submission
    uint64_t tsize = ((const tex_t *)&tx)->L.size, past = (tsize + 0xFFFF) & ~0xFFFFull;
    CHECK(tsize >= 0x1000 && tsize <= past, "the texture's storage size");
    uint64_t sg = g_ser_gen;
    CHECK(map(&virt, &phys, past, 0x30000, 0x1000), "a mapping beside the texture");
    next_sub();
    CHECK(g_ser_gen == sg, "a mapping outside every texture notes nothing");

    // A 4 KB request over the texture's page: no registration, serial change or CPU write at the request, a note at the next submission
    uint64_t rg = atomic_load(&g_reg_gen), dg = nvn_dirty_now(); sg = g_ser_gen;
    args_sync_dirty(); for (int st = 0; st < 6; st++) g_args_wrote[st] = g_args_seq;   // as if every stage had just written its block
    CHECK(map(&virt, &phys, 0, 0x10000, 0x1000), "the texture's page mapped from elsewhere");
    CHECK(atomic_load(&g_reg_gen) == rg && g_ser_gen == sg && nvn_dirty_now() == dg, "the request itself changes no registration, serial or CPU-write generation");
    next_sub();
    CHECK(g_ser_gen == sg + 1, "the next submission notes the texture's serial once");
    args_sync_dirty(); int all = 1; for (int st = 0; st < 6; st++) all &= args_changed(st);
    CHECK(all, "the note sends cached argument blocks through their check");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(g_stat_pool_builds == builds, "the pool table is patched, not built whole");
    CHECK(gpu_read(table, 1) == B, "the pool table reads the newly mapped page");

    // A table of handles in a uniform buffer, unchanged while the page behind its texture is mapped again
    uint32_t *handles = (uint32_t *)(mem + 0x40000); handles[0] = 1; handles[1] = 0;
    S.ubo[0][0] = (p_bind_buf){.addr = (uintptr_t)handles, .size = 8};
    nvn_rbind binding = {.kind = NVN_RB_TEXH, .reg = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    htab_gpu(0, &binding, &table);
    CHECK(gpu_read(table, 0) == B, "the handle table reads the current page");
    CHECK(map(&virt, &phys, 0, 0x20000, 0x1000), "the texture's page mapped again");
    next_sub();
    htab_gpu(0, &binding, &table);
    CHECK(gpu_read(table, 0) == C, "the handle table reads the newly mapped page");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == C, "so does the pool table");

    // Views of a sparse texture whose finest level is not mapped. Its host texture is the view of levels [1, 3) (sparse_view), and a texture view's levels count from NVN's level 0:
    // Resident Evil 4 samples its streamed textures through views of levels 1 + 0 and 2 + 0. The level 2 view started past the host texture's two levels and was left out (the
    // dummy); the level 1 view read level 2
    const uint32_t D = 0xff0a0b0c, E = 0xff0d0e0f; const uint64_t VO = 0x100000, PO = 0x80000;
    NVNtexture tm; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetWidth(&tb, 256); h_nvnTextureBuilderSetHeight(&tb, 256); h_nvnTextureBuilderSetLevels(&tb, 3);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, VO);
    CHECK(h_nvnTextureInitialize(&tm, &tb), "sparse texture of three levels");
    const tex_t *mt = (const tex_t *)&tm;
    uint64_t l1 = nvn_tex_level_offset(&mt->d, 1), l2 = nvn_tex_level_offset(&mt->d, 2), lend = (mt->L.size + 0xFFF) & ~0xFFFull;
    CHECK(l1 && !(l1 & 0xFFF) && l2 > l1 && PO + lend - l1 <= 0x100000, "level 1 starts on a page, and levels 1 and 2 fit the physical pool");
    for (uint64_t o = l1; o < lend; o += 4) *(uint32_t *)(mem + PO + o - l1) = o < l2 ? D : E;
    CHECK(map(&virt, &phys, VO + l1, PO, lend - l1), "levels 1 and 2 mapped, level 0 not");
    view_t v1 = {.set_levels = 1, .base_level = 1}, v2 = {.set_levels = 1, .base_level = 2}, v0 = {.set_levels = 1, .base_level = 0, .nlevels = 1};
    mtl_reg_tex(2, mt, &v1); mtl_reg_tex(3, mt, &v2); mtl_reg_tex(4, mt, &v0);
    g_seen_done[0] |= 0x1c; g_seen_typed[0][0] |= 0x1c;
    next_sub();
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 2) == D, "a view from level 1 reads level 1");
    CHECK(gpu_read(table, 3) == E, "a view from level 2 reads level 2, not the dummy");
    CHECK(gpu_read(table, 4) == D, "a view of the unmapped level 0 reads the first mapped level, as the texture's own binding does");
    mtl_reg_tex(2, NULL, NULL); mtl_reg_tex(3, NULL, NULL); mtl_reg_tex(4, NULL, NULL);
    h_nvnTextureFinalize(&tm);

    // An identical registration revives an ID pruned from the sparse scan while it was gone.
    mtl_reg_tex_gone(1);
    CHECK(map(&virt, &phys, past, 0x50000, 0x1000), "a mapping beside the gone descriptor's texture");
    next_sub();
    CHECK(g_treg[1].gone && !g_treg[1].sp, "the scan removes the gone descriptor");
    mtl_reg_tex(1, (const tex_t *)&tx, NULL);
    CHECK(!g_treg[1].gone && g_treg[1].sp, "an identical registration restores sparse tracking");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == C, "the revived descriptor reads the current page");
    sg = g_ser_gen;
    CHECK(map(&virt, &phys, 0, 0x10000, 0x1000), "remap the revived descriptor's page");
    next_sub();
    CHECK(g_ser_gen == sg + 1, "the next submission notes the revived descriptor's serial");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
    CHECK(gpu_read(table, 1) == B, "the revived descriptor reads the newly mapped page");

    // In-place CPU writes to an already-mapped physical page (no MapVirtual, no registration change):
    // tex_fast must track nvn_dirty_now via view_pkg, and a write by another thread during a submission
    // that sparse_checked defers must set g_pkg_kept so the next submission still re-uploads it.
    {
        const uint32_t F = 0xff336699, G = 0xffaa5511;
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> t0 = (__bridge id<MTLTexture>)tex_fast(1);
        int vp = g_treg[1].view_pkg;
        os_unfair_lock_unlock(&g_rmu);
        CHECK(vp && t0 != nil, "sparse texture ID sets view_pkg");
        next_sub();
        dm_guest_write_prepare(mem + 0x10000, 0x1000);
        for (unsigned i = 0; i < 0x1000 / 4; i++) ((uint32_t *)(mem + 0x10000))[i] = F;
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> t1 = (__bridge id<MTLTexture>)tex_fast(1);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(t1 != nil && t1 != t0, "in-place CPU write to mapped sparse page invalidates cached texture ID");
        next_sub();
        CHECK(gpu_read_tex(t1) == F, "tex_fast uploads the in-place CPU write to the mapped sparse page");

        // Mid-submission write from another thread: kept during the open submission, picked up on the next.
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> t2 = (__bridge id<MTLTexture>)tex_fast(1);
        os_unfair_lock_unlock(&g_rmu);
        struct bg_fill bf = {.dst = (uint32_t *)(mem + 0x10000), .color = G};
        pthread_t th; CHECK(!pthread_create(&th, NULL, bg_fill_fn, &bf), "background writer thread starts");
        pthread_join(th, NULL);
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> t3 = (__bridge id<MTLTexture>)tex_fast(1);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(t3 == t2 && g_pkg_kept, "mid-submission background write keeps the current texture and marks g_pkg_kept");
        next_sub();
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> t4 = (__bridge id<MTLTexture>)tex_fast(1);
        os_unfair_lock_unlock(&g_rmu);
        CHECK(t4 != nil && t4 != t3, "deferred mid-submission CPU write to sparse page invalidates on the next submission");
        next_sub();
        CHECK(gpu_read_tex(t4) == G, "next submission reads the deferred background CPU write");
    }

    // A scene change finalizes a pool while its descriptor stays registered.
    // Make the freed pool object's page inaccessible, then map the live pool:
    // the next submission must retire the old sparse-scan entry without reading it.
    const size_t page = (size_t)getpagesize();
    pool_t *dead = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(dead != MAP_FAILED, "pool object page allocated");
    pb = (pool_builder_t){.size = 0x20000, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)dead, (NVNmemoryPoolBuilder *)&pb), "temporary virtual pool");
    CHECK(map(dead, &phys, 0, 0x30000, 0x1000), "temporary pool mapped sparsely");
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)dead, 0);
    NVNtexture td;
    CHECK(h_nvnTextureInitialize(&td, &tb), "temporary sparse texture");
    mtl_reg_tex(5, (const tex_t *)&td, NULL);
    CHECK(g_treg[5].sp, "temporary descriptor belongs to sparse scan");
    h_nvnTextureFinalize(&td);
    CHECK(!g_treg[5].gone, "texture finalization keeps the registered descriptor live until its pool ends");
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)dead);
    CHECK(g_treg[5].gone, "finalization marks the descriptor gone");
    CHECK(!mprotect(dead, page, PROT_NONE), "freed pool object is inaccessible");
    CHECK(map(&virt, &phys, past, 0x40000, 0x1000), "another pool's mapping triggers sparse notes");
    next_sub();
    CHECK(!g_treg[5].sp, "gone descriptor removed without reading freed pool object");
    CHECK(!munmap(dead, page), "temporary object page released");

    // Sparse buffer-backed textures (LINEAR and TARGET_BUFFER) across a sparse run merge (mtl_pool_replace):
    // mapping an adjacent range replaces the run's Metal buffer without touching the texture's own pages.
    // The old run buffer stays resident until the texture refreshes onto the new run buffer, block-linear
    // textures in the same merged run are not re-uploaded, and unmapping drops the texture so the run buffer retires.
    {
        const uint32_t L1 = 0xff22aa44, L2 = 0xff55cc88;
        for (unsigned i = 0; i < 0x10000 / 4; i++) { ((uint32_t *)(mem + 0x60000))[i] = L1; ((uint32_t *)(mem + 0x70000))[i] = L2; }
        const uint64_t lv = 0x200000;
        CHECK(map(&virt, &phys, lv, 0x60000, 0x10000), "linear sparse run mapped");
        NVNtexture ltx, bltx;
        h_nvnTextureBuilderSetDefaults(&tb);
        h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
        h_nvnTextureBuilderSetWidth(&tb, 64); h_nvnTextureBuilderSetHeight(&tb, 64);
        h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
        h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR);
        h_nvnTextureBuilderSetStride(&tb, 256);
        h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, lv);
        CHECK(h_nvnTextureInitialize(&ltx, &tb), "linear texture in sparse pool");
        h_nvnTextureBuilderSetDefaults(&tb);
        h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
        h_nvnTextureBuilderSetWidth(&tb, 32); h_nvnTextureBuilderSetHeight(&tb, 32);
        h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
        h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_SPARSE);
        h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, lv);   // one 64 KB sparse tile [lv, lv+0x10000), strictly inside the first run
        CHECK(h_nvnTextureInitialize(&bltx, &tb), "block-linear texture in same sparse run");
        const tex_t *lt = (const tex_t *)&ltx, *blt = (const tex_t *)&bltx;
        mtl_reg_tex(6, lt, NULL); mtl_reg_tex(7, blt, NULL);
        g_seen_done[0] |= (1u << 6) | (1u << 7); g_seen_typed[0][0] |= (1u << 6) | (1u << 7);
        next_sub();
        pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
        CHECK(gpu_read(table, 6) == L1, "sparse linear texture reads mapped run");
        CHECK(gpu_read(table, 7) == L1, "block-linear texture in same run reads mapped run");
        id<MTLTexture> lt_old = tex_get(lt->serial);
        id<MTLBuffer> b_old = lt_old ? lt_old.buffer : nil;
        CHECK(lt_old && b_old, "sparse linear texture is backed directly by the run buffer");
        pthread_mutex_lock(&g_res_mu); int old_res = b_old && [g_res_objects containsObject:b_old]; pthread_mutex_unlock(&g_res_mu);
        CHECK(old_res, "old run buffer is resident");
        // Verify DM_RES_CHECK=1 visits bindless registered views and their backing buffers.
        g_res_check_on = 1;
        treg_res_check();
        pthread_mutex_lock(&g_res_mu);
        NSArray *u_tex = g_res_last_use ? [g_res_last_use objectForKey:lt_old] : nil;
        NSArray *u_buf = g_res_last_use ? [g_res_last_use objectForKey:b_old] : nil;
        pthread_mutex_unlock(&g_res_mu);
        g_res_check_on = 0;
        CHECK(u_tex && u_buf && [u_buf[0] unsignedLongLongValue] == g_fence + 1, "DM_RES_CHECK tracks bindless linear texture and its backing run buffer");
        uint64_t reup_before = g_pkg_reuploads;
        // Map the adjacent 64 KB run: vrun_set merges [lv, lv+0x10000) and [lv+0x10000, lv+0x20000) into a new Metal buffer.
        CHECK(map(&virt, &phys, lv + 0x10000, 0x70000, 0x10000), "adjacent range mapped to trigger run merge");
        mtl_sub_commit(); mtl_drain();
        pthread_mutex_lock(&g_res_mu); int kept_res = [g_res_objects containsObject:b_old]; pthread_mutex_unlock(&g_res_mu);
        CHECK(kept_res, "replaced run buffer stays resident while the linear texture still references it");
        mtl_sub_begin();   // treg_sparse_notes notes the linear texture's serial, not the block-linear one
        os_unfair_lock_lock(&g_rmu);
        uint64_t ep = treg_ep();
        int lin_invalid = g_treg[6].view_ser != ep;
        int bl_valid = g_treg[7].view_ser == ep;
        os_unfair_lock_unlock(&g_rmu);
        CHECK(lin_invalid && bl_valid, "run merge invalidates only the buffer-backed linear texture, not the block-linear texture");
        pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
        CHECK(gpu_read(table, 6) == L1, "linear texture reads cleanly after run merge");
        CHECK(g_pkg_reuploads == reup_before, "block-linear texture in merged run is not re-uploaded");
        id<MTLTexture> lt_new = tex_get(lt->serial);
        id<MTLBuffer> b_new = lt_new ? lt_new.buffer : nil;
        CHECK(lt_new && b_new && b_new != b_old, "linear texture switched to the merged run's new Metal buffer");
        next_sub();   // drain retire_resume(b_old)
        pthread_mutex_lock(&g_res_mu); int old_gone = ![g_res_objects containsObject:b_old]; pthread_mutex_unlock(&g_res_mu);
        CHECK(old_gone, "old run buffer leaves residency once the linear texture switches to the new buffer");
        // Unmapping the linear texture's range drops the texture and releases the run buffer.
        CHECK(map(&virt, NULL, lv, 0, 0x10000), "linear texture's range unmapped");
        next_sub();
        pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &table);
        CHECK(tex_get(lt->serial) == nil, "unmapping drops the sparse linear texture");
        next_sub();
        pthread_mutex_lock(&g_res_mu); int new_gone = ![g_res_objects containsObject:b_new]; pthread_mutex_unlock(&g_res_mu);
        CHECK(new_gone, "unmapped run buffer leaves residency instead of staying pinned");
        mtl_reg_tex(6, NULL, NULL); mtl_reg_tex(7, NULL, NULL);
        h_nvnTextureFinalize(&ltx); h_nvnTextureFinalize(&bltx);

        // Same run-merge lifetime check for TARGET_BUFFER (including clipped size extending past the first run).
        const uint64_t bv = 0x300000;
        CHECK(map(&virt, &phys, bv, 0x60000, 0x10000), "buffer texture sparse run mapped");
        NVNtexture btx;
        h_nvnTextureBuilderSetDefaults(&tb);
        h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER);
        h_nvnTextureBuilderSetWidth(&tb, 0x20000 / 4);   // 128 KB descriptor over a 64 KB run: exercises clipped mtl_addr(..., 1)
        h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI);
        h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&virt, bv);
        CHECK(h_nvnTextureInitialize(&btx, &tb), "clipped buffer texture in sparse pool");
        const tex_t *bt = (const tex_t *)&btx;
        mtl_reg_tex(8, bt, NULL);
        next_sub();
        os_unfair_lock_lock(&g_rmu); void *bv_old = tex_fast(8); os_unfair_lock_unlock(&g_rmu);
        id<MTLTexture> bt_old = (__bridge id<MTLTexture>)bv_old;
        id<MTLBuffer> tb_old = bt_old ? bt_old.buffer : nil;
        CHECK(bt_old && tb_old, "clipped buffer texture resolves onto the first run buffer");
        CHECK(map(&virt, &phys, bv + 0x10000, 0x70000, 0x10000), "adjacent range mapped to merge buffer texture's run");
        mtl_sub_commit(); mtl_drain();
        pthread_mutex_lock(&g_res_mu); int tb_kept = [g_res_objects containsObject:tb_old]; pthread_mutex_unlock(&g_res_mu);
        CHECK(tb_kept, "replaced run buffer stays resident while buffer texture still references it");
        mtl_sub_begin();
        os_unfair_lock_lock(&g_rmu);
        uint64_t ep2 = treg_ep();
        int tb_invalid = g_treg[8].view_ser != ep2;
        void *bv_new = tex_fast(8);
        os_unfair_lock_unlock(&g_rmu);
        id<MTLTexture> bt_new = (__bridge id<MTLTexture>)bv_new;
        id<MTLBuffer> tb_new = bt_new ? bt_new.buffer : nil;
        CHECK(tb_invalid && bt_new && tb_new && tb_new != tb_old, "buffer texture invalidates on run merge and switches to the merged run buffer");
        next_sub();
        pthread_mutex_lock(&g_res_mu); int tb_old_gone = ![g_res_objects containsObject:tb_old]; pthread_mutex_unlock(&g_res_mu);
        CHECK(tb_old_gone, "buffer texture's old run buffer leaves residency after switch");
        CHECK(map(&virt, NULL, bv, 0, 0x20000), "buffer texture's range unmapped");
        next_sub();
        os_unfair_lock_lock(&g_rmu); void *bv_none = tex_fast(8); os_unfair_lock_unlock(&g_rmu);
        CHECK(bv_none == NULL && tex_get(bt->serial) == nil, "unmapping drops the sparse buffer texture");
        next_sub();
        pthread_mutex_lock(&g_res_mu); int tb_new_gone = ![g_res_objects containsObject:tb_new]; pthread_mutex_unlock(&g_res_mu);
        CHECK(tb_new_gone, "unmapped buffer texture's run buffer leaves residency");
        mtl_reg_tex(8, NULL, NULL);
        h_nvnTextureFinalize(&btx);
    }

    h_nvnTextureFinalize(&tx);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virt); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys);
    mtl_sub_commit(); mtl_drain(); free(mem);
    printf("sparse pool cache: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
