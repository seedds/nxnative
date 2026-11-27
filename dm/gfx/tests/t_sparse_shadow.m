// Per-run misaligned sparse mappings, sparse alias visibility in resource_vis, CPU_NO_ACCESS 16 KB re-backing,
// >32 handle binding splits, compressed IMAGE textures in sparse pools, geometry GPU-only scratch reuse + mem_trim,
// and clipped buffer texture / storage buffer length bounds. Runs under Metal validation (MTL_DEBUG_LAYER=1).
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
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static void init_pool(NVNdevice *dev, NVNmemoryPool *p, void *mem, size_t size, int flags) {
    NVNmemoryPoolBuilder b;
    h_nvnMemoryPoolBuilderSetDefaults(&b);
    h_nvnMemoryPoolBuilderSetDevice(&b, dev);
    h_nvnMemoryPoolBuilderSetStorage(&b, mem, size);
    h_nvnMemoryPoolBuilderSetFlags(&b, flags);
    CHECK("pool init", h_nvnMemoryPoolInitialize(p, &b));
}

void crash_install(void);

int main(void) {
    crash_install();
    mtl_init();
    nvn_dirty_own_thread();
    NVNdevice dev; NVNdeviceBuilder db;
    h_nvnDeviceBuilderSetDefaults(&db);
    h_nvnDeviceInitialize(&dev, &db);

    const size_t pg = (size_t)getpagesize();
    CHECK("host page >= 16 KB", pg >= 0x4000);

    // 1. resource_vis() enables MTL4VisibilityOptionResourceAlias when a sparse pool has mapped runs, and drops it on Finalize
    CHECK("resource_vis starts without sparse alias", !(resource_vis() & MTL4VisibilityOptionResourceAlias) || g_image_heap_n || g_big_n || g_pair_n);
    void *raw_noacc = mmap(NULL, 8 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK("mmap raw_noacc", raw_noacc != MAP_FAILED);
    uint8_t *mis_noacc = (uint8_t *)raw_noacc + 0x3000;
    memset(mis_noacc, 0x5A, 2 * pg);

    // 2. PHYSICAL | CPU_NO_ACCESS pool at +0x3000 is re-backed on a host page boundary and vm_remapped zero-copy (no shadow copy)
    NVNmemoryPool phys_noacc, virt;
    init_pool(&dev, &phys_noacc, mis_noacc, 2 * pg, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    init_pool(&dev, &virt, NULL, 16 * pg, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    uint8_t *vbase = ((pool_t *)&virt)->mem;
    CHECK("CPU_NO_ACCESS physical pool aligned", (((uintptr_t)((pool_t *)&phys_noacc)->mem) & (pg - 1)) == 0);
    CHECK("CPU_NO_ACCESS pre-init data preserved", ((pool_t *)&phys_noacc)->mem[0] == 0x5A && ((pool_t *)&phys_noacc)->mem[2 * pg - 1] == 0x5A);

    map_req mr0 = { &phys_noacc, 0, 0, 2 * pg, 0, 0 };
    CHECK("map CPU_NO_ACCESS into virtual pool", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&mr0));
    CHECK("resource_vis has ResourceAlias with active sparse pool", (resource_vis() & MTL4VisibilityOptionResourceAlias) != 0);
    CHECK("CPU_NO_ACCESS mapping used zero-copy vm_remap (no shadow bytes)", atomic_load(&g_sh_bytes) == 0);
    CHECK("virtual address reads pre-init data", vbase[0] == 0x5A && vbase[2 * pg - 1] == 0x5A);

    // 3. Misaligned CPU-accessible physical mappings (+0x1000 and +0x3000) join into one contiguous run, sync CPU writes,
    //    write back GPU writes through zero-fault RW alias, and preserve concurrent CPU + GPU writes in the same host page
    void *raw_cpu = mmap(NULL, 12 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK("mmap raw_cpu", raw_cpu != MAP_FAILED);
    uint8_t *p1_mem = (uint8_t *)raw_cpu + 0x1000;
    uint8_t *p2_mem = (uint8_t *)raw_cpu + 4 * pg + 0x3000;
    memset(p1_mem, 0x11, 2 * pg);
    memset(p2_mem, 0x22, 2 * pg);

    NVNmemoryPool phys1, phys2;
    init_pool(&dev, &phys1, p1_mem, 2 * pg, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    init_pool(&dev, &phys2, p2_mem, 2 * pg, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);

    map_req mrs[2] = {
        { &phys2, 0, (int64_t)(4 * pg), 2 * pg, 0, 0 },
        { &phys1, 0, (int64_t)(2 * pg), 2 * pg, 0, 0 },
    };
    CHECK("map two misaligned CPU physical pools adjacently", h_nvnMemoryPoolMapVirtual(&virt, 2, (const NVNmappingRequest *)mrs));
    CHECK("g_sh_bytes tracks exact shadow bytes", atomic_load(&g_sh_bytes) == 4 * pg);
    CHECK("all 3 runs merged into 1 contiguous run", ((pool_t *)&virt)->vp->nrun == 1);
    CHECK("shadow copies initialized", vbase[2 * pg] == 0x11 && vbase[4 * pg] == 0x22);
    CHECK("static CPU-only shadow has no snap allocated", ((pool_t *)&virt)->vp->sh[0].snap == NULL && ((pool_t *)&virt)->vp->sh[1].snap == NULL);

    // CPU write to phys1 syncs at mtl_sub_begin without allocating snap
    p1_mem[128] = 0x77;
    mtl_sub_begin();
    CHECK("CPU write synced into shadow copy", vbase[2 * pg + 128] == 0x77);
    CHECK("CPU-only sync still allocates no snap", ((pool_t *)&virt)->vp->sh[0].snap == NULL);

    // GPU fill on offset 0..64 of phys1's shadow, plus concurrent CPU write at offset 256 (same 4 KB & 16 KB page!)
    uint64_t sh_va = (uint64_t)(uintptr_t)(vbase + 2 * pg);
    mtl_sub_fill(sh_va, 64, 0xDEADBEEFu);
    // Simulate concurrent CPU write to another offset in the same 4 KB page before GPU completion
    p1_mem[256] = 0x99;
    uint64_t faults_before = 0, prots_before = 0, faults_after = 0, prots_after = 0;
    nvn_dirty_stats(&faults_before, &prots_before);
    uint64_t f1 = mtl_sub_commit();
    CHECK("GPU fill submission completed", mtl_wait(f1, 5000000000ull));
    nvn_dirty_stats(&faults_after, &prots_after);
    uint32_t wb_word = 0; memcpy(&wb_word, p1_mem, 4);
    CHECK("GPU fill written back to misaligned physical pool", wb_word == 0xDEADBEEFu);
    CHECK("writeback through RW alias triggered 0 dirty faults", faults_after == faults_before);
    CHECK("concurrent CPU write at offset 256 preserved across writeback", p1_mem[256] == 0x99);

    // Next submission syncs the CPU write at offset 256 without clobbering GPU write at offset 0
    mtl_sub_begin();
    uint32_t sh_word = 0; memcpy(&sh_word, vbase + 2 * pg, 4);
    CHECK("GPU write at offset 0 survived next CPU sync", sh_word == 0xDEADBEEFu);
    CHECK("concurrent CPU write at offset 256 synced to shadow", vbase[2 * pg + 256] == 0x99);
    // Event signal (ReportValue) into shadowed virtual address writes back to physical memory
    mtl_sub_event((volatile uint32_t *)(vbase + 2 * pg + 512), NVN_EVENT_SIGNAL_MODE_WRITE, 0x12345678u);
    uint64_t f2 = mtl_sub_commit();
    CHECK("event submission completed", mtl_wait(f2, 5000000000ull));
    uint32_t ev_word = 0; memcpy(&ev_word, p1_mem + 512, 4);
    CHECK("event writeback reached physical page", ev_word == 0x12345678u);

    // A wide writeback starts and ends in partial pages, with an unchanged
    // whole page between them and GPU changes on both sides.
    uint64_t wide_start = sh_va + 13, wide_size = 2 * pg - 30;
    CHECK("wide writeback armed", nvn_shadow_arm(wide_start, wide_size));
    p1_mem[pg] = 0x55;
    vbase[2 * pg + 13] = 0xA3;
    vbase[4 * pg - 18] = 0xB4;
    uint64_t wb_before = atomic_load(&g_sh_wb);
    nvn_shadow_writeback(wide_start, wide_size);
    CHECK("wide writeback reaches both partial pages", p1_mem[13] == 0xA3 && p1_mem[2 * pg - 18] == 0xB4);
    CHECK("wide writeback skips the unchanged whole page", p1_mem[pg] == 0x55 && atomic_load(&g_sh_wb) - wb_before == (64 - 13) + (64 - 17));

    // Completion must publish the physical bytes even when a mapping holds
    // the sparse mutex and no subsequent rendering submission runs.
    const uint64_t waited_offset = 768;
    CHECK("contended writeback armed", nvn_shadow_arm(sh_va + waited_offset, 1));
    vbase[2 * pg + waited_offset] = 0xC5;
    dispatch_semaphore_t started = dispatch_semaphore_create(0), finished = dispatch_semaphore_create(0);
    __block post_rec completion = {.n = 1, .ops = malloc(sizeof(post_op))};
    *completion.ops = (post_op){.kind = POST_SHADOW_WB, .addr = sh_va + waited_offset, .copy_end = sh_va + waited_offset + 1};
    pthread_mutex_lock(&g_vp_mu);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        pthread_mutex_lock(&g_post_mu);
        dispatch_semaphore_signal(started);
        post_run(&completion, 0, 0);
        dispatch_semaphore_signal(finished);
    });
    dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
    long waiting = dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC));
    CHECK("completion waits for the sparse mutex", waiting != 0 && !atomic_load(&completion.claimed));
    pthread_mutex_unlock(&g_vp_mu);
    if (waiting) dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
    CHECK("completion publishes the GPU write without another submit", atomic_load(&completion.claimed) && p1_mem[waited_offset] == 0xC5);

    // Coalesce repeats that the short look-back missed without including
    // gaps or moving a writeback across a CPU-side effect.
    g_npost = 0;
    post((post_op){.kind = POST_SHADOW_WB, .addr = 512, .copy_end = 576});
    post((post_op){.kind = POST_SHADOW_WB, .addr = 128, .copy_end = 192});
    post((post_op){.kind = POST_SHADOW_WB, .addr = 160, .copy_end = 256});
    post((post_op){.kind = POST_BUFFER_COPY, .addr = 384, .copy_end = 448});
    post((post_op){.kind = POST_SHADOW_WB, .addr = 576, .copy_end = 640});
    post_shadow_coalesce();
    CHECK("writeback coalescing merges the overlap and leaves the gap", g_npost == 4 && g_post[0].addr == 128 && g_post[0].copy_end == 256 && g_post[1].addr == 512 && g_post[1].copy_end == 576);
    CHECK("writeback coalescing keeps the CPU-side effect in order", g_npost == 4 && g_post[2].kind == POST_BUFFER_COPY && g_post[3].addr == 576);
    g_npost = 0;

    // A raw shader's predicted set can include two sparse pools while the
    // GPU's write log names only one. Completion must read that log itself.
    mtl_sub_begin();
    id<MTLBuffer> raw_map = [g_dev newBufferWithLength:3 * DMSH_GMEM_MAP_ENTRY options:MTLResourceStorageModeShared];
    uint64_t *table = raw_map.contents;
    table[DMSH_GMEM_MAP_ENTRY / 8] = sh_va;
    table[DMSH_GMEM_MAP_ENTRY / 8 + 1] = sh_va + 2 * pg;
    table[2 * DMSH_GMEM_MAP_ENTRY / 8] = sh_va + 2 * pg;
    table[2 * DMSH_GMEM_MAP_ENTRY / 8 + 1] = sh_va + 4 * pg;
    g_gml_n = g_gml_stable = 2;
    g_gml_lo[0] = sh_va; g_gml_hi[0] = g_gml_lo[1] = sh_va + 2 * pg; g_gml_hi[1] = sh_va + 4 * pg;
    (void)gml_take(raw_map);
    CHECK("raw shadow tracking preserves hazards without predicted writebacks", sbw_hits(sh_va, sh_va + 1) && sbw_hits(sh_va + 2 * pg, sh_va + 2 * pg + 1) && g_npost == 0);
    ((uint32_t *)g_gml_last->b.contents)[0] = 1u << 1;
    vbase[2 * pg + 1024] = 0xA1;
    vbase[4 * pg + 1024] = 0xB2;
    CHECK("raw GPU writeback completed", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("raw writeback uses the recorded pool before fence completion", p1_mem[1024] == 0xA1 && p2_mem[1024] == 0x22);
    vbase[4 * pg + 1024] = 0x22;

    // 4. Clipped buffer texture over sparse pool (declared 16 * pg bytes, only 6 * pg mapped)
    NVNtextureBuilder tb; NVNtexture t_buf, t_bc, t_rgba;
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetDevice(&tb, &dev);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_BUFFER);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_R32UI);
    h_nvnTextureBuilderSetSize1D(&tb, (int)((16 * pg) / 4));
    h_nvnTextureBuilderSetStorage(&tb, &virt, 0);
    CHECK("buffer texture init", h_nvnTextureInitialize(&t_buf, &tb));
    id<MTLTexture> mt_buf = tex_buffer((const tex_t *)&t_buf);
    CHECK("clipped buffer texture created", mt_buf != nil && mt_buf.width == (6 * pg) / 4);

    // 5. tex_from_storage allows NVN_TEXTURE_FLAGS_IMAGE on compressed formats (BC1) in sparse pool, rejects uncompressed
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetDevice(&tb, &dev);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGB_DXT1);
    h_nvnTextureBuilderSetSize2D(&tb, 64, 64);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_IMAGE);
    h_nvnTextureBuilderSetStorage(&tb, &virt, 0);
    CHECK("BC1 IMAGE texture init", h_nvnTextureInitialize(&t_bc, &tb));
    CHECK("tex_from_storage accepts BC1+IMAGE in sparse pool", tex_from_storage((const tex_t *)&t_bc) == 1);

    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    CHECK("RGBA8 IMAGE texture init", h_nvnTextureInitialize(&t_rgba, &tb));
    CHECK("tex_from_storage rejects RGBA8+IMAGE in sparse pool", tex_from_storage((const tex_t *)&t_rgba) == 0);

    // 6. Cutting across two shadows leaves their outer portions addressable.
    map_req middle = { NULL, 0, (int64_t)(3 * pg), 2 * pg, 0, 0 };
    CHECK("unmap spans the tail and head of two shadows", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&middle));
    CHECK("only outer pages count as shadow bytes", atomic_load(&g_sh_bytes) == 2 * pg);
    CHECK("lowest remaining shadow can be armed", nvn_shadow_arm(sh_va + pg - 1, 1));
    CHECK("shadow lookup skips the gap", !nvn_shadow_arm(sh_va + pg, 2 * pg));
    CHECK("highest remaining shadow can be armed", nvn_shadow_arm(sh_va + 3 * pg, 1));

    map_req unmap = { NULL, 0, (int64_t)(2 * pg), 4 * pg, 0, 0 };
    CHECK("unmap shadowed ranges", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&unmap));
    CHECK("g_sh_bytes returned to 0 after unmap", atomic_load(&g_sh_bytes) == 0);

    // Consecutive requests over one backing range share a shadow. GPU stores
    // can cross the old request boundary, and an unmap can still trim one side.
    map_req adjacent[2] = {
        { &phys1, 0, (int64_t)(2 * pg), pg, 0, 0 },
        { &phys1, (int64_t)pg, (int64_t)(3 * pg), pg, 0, 0 },
    };
    CHECK("map adjacent physical and virtual ranges", h_nvnMemoryPoolMapVirtual(&virt, 2, (const NVNmappingRequest *)adjacent));
    CHECK("adjacent requests use one shadow", ((pool_t *)&virt)->vp->nsh == 1);
    CHECK("combined shadow contains both source ranges", !memcmp(vbase + 2 * pg, p1_mem, 2 * pg));
    mtl_sub_begin();
    mtl_sub_fill(sh_va + pg - 32, 64, 0x5C5C5C5Cu);
    CHECK("GPU write across the former boundary completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("GPU writeback reaches both sides", p1_mem[pg - 1] == 0x5C && p1_mem[pg] == 0x5C);
    map_req tail = { NULL, 0, (int64_t)(3 * pg), pg, 0, 0 };
    CHECK("unmap the tail of a combined shadow", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&tail));
    CHECK("trimming leaves only the first side", atomic_load(&g_sh_bytes) == pg && nvn_shadow_arm(sh_va, pg) && !nvn_shadow_arm(sh_va + pg, pg));
    p1_mem[pg - 1] = 0x91;
    mtl_sub_begin();
    CHECK("CPU write still syncs into the trimmed shadow", vbase[3 * pg - 1] == 0x91);
    CHECK("submission after trimming completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("unmap the remaining side", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&unmap));

    // A new mapping spanning two existing shadows can keep their backing.
    CHECK("map adjacent shadows for remap", h_nvnMemoryPoolMapVirtual(&virt, 2, (const NVNmappingRequest *)mrs));
    void *before = NULL, *after = NULL;
    uint64_t before_off = 0, after_off = 0;
    CHECK("original run resolves", mtl_addr(sh_va, 4 * pg, &before, &before_off));
    id<MTLBuffer> original_run = (__bridge id<MTLBuffer>)before;
    uint64_t old_gen = nvn_sparse_gen(), old_stamp = nvn_dirty_now();
    map_req replace = { &phys2, 0, (int64_t)(3 * pg), 2 * pg, 0, 0 };
    CHECK("remap across both shadows", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&replace));
    CHECK("remap keeps the existing run buffer", mtl_addr(sh_va, 4 * pg, &after, &after_off) &&
          after == (__bridge void *)original_run && before_off == after_off);
    CHECK("remap updates sparse texture generation", nvn_sparse_since(&virt, sh_va + pg, 2 * pg, old_gen, old_stamp));
    CHECK("remap copies only the replaced span", !memcmp(vbase + 2 * pg, p1_mem, pg) &&
          !memcmp(vbase + 3 * pg, p2_mem, 2 * pg) && !memcmp(vbase + 5 * pg, p2_mem + pg, pg));
    uint8_t old_source = p1_mem[pg];
    mtl_sub_begin();
    mtl_sub_fill(sh_va + pg, 4, 0x75757575u);
    CHECK("GPU write through remapped run completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    CHECK("remapped run writes back to the new source", p2_mem[0] == 0x75 && p1_mem[pg] == old_source);
    CHECK("unmap the remapped shadows", h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&unmap));

    // 7. bind_handles_ex & BindTexturesDeferred >32 handles (49 handles)
    NVNcommandBuffer cmd; NVNmemoryPool cpool;
    uint8_t *cmem = calloc(1, 64 << 10);
    init_pool(&dev, &cpool, cmem, 64 << 10, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    h_nvnCommandBufferInitialize(&cmd, &dev);
    uint8_t ctl_mem[4096];
    h_nvnCommandBufferAddCommandMemory(&cmd, &cpool, 0, 64 << 10);
    h_nvnCommandBufferAddControlMemory(&cmd, ctl_mem, sizeof ctl_mem);
    NVNtextureHandle handles[49];
    for (size_t i = 0; i < 49; i++) handles[i] = i + 100;
    h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferBindTextures(&cmd, NVN_SHADER_STAGE_FRAGMENT, 0, 49, handles);
    NVNcommandHandle ch = h_nvnCommandBufferEndRecording(&cmd);
    mtl_sub_begin();
    memset(S.tex[1], 0, sizeof S.tex[1]);
    uint8_t *lin = nvn_rec_linear(ch, NULL);
    mtl_exec(lin); free(lin);
    int all49 = 1; for (size_t i = 0; i < 49; i++) if (S.tex[1][i] != i + 100) all49 = 0;
    CHECK("BindTextures 49 handles replayed across split records", all49);
    // Deferred bind >32 handles: mutate array after recording, before replay
    h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferBindTexturesDeferred(&cmd, NVN_SHADER_STAGE_VERTEX, 0, 49, handles);
    NVNcommandHandle ch_def = h_nvnCommandBufferEndRecording(&cmd);
    for (size_t i = 0; i < 49; i++) handles[i] = i + 500;
    memset(S.tex[0], 0, sizeof S.tex[0]);
    lin = nvn_rec_linear(ch_def, NULL);
    mtl_exec(lin); free(lin);
    int def49 = 1; for (size_t i = 0; i < 49; i++) if (S.tex[0][i] != i + 500) def49 = 0;
    CHECK("BindTexturesDeferred 49 handles read updated handles at replay", def49);

    // 8. Geometry GPU-only scratch pool: reuses multiple blocks across geo_big_reset, trims completed blocks on memory pressure
    uint64_t made0 = g_gb_made;
    uint64_t a1 = geo_big_take(40ull << 20);
    uint64_t a2 = geo_big_take(40ull << 20);   // spills into 2nd 64 MB block
    CHECK("two 40 MB takes allocated 2 private blocks", g_gb_made == made0 + 2 && a1 != a2);
    geo_big_reset();
    uint64_t b1 = geo_big_take(40ull << 20);
    uint64_t b2 = geo_big_take(40ull << 20);
    CHECK("geo_big_reset reused BOTH blocks without allocating", g_gb_made == made0 + 2 && ((b1 == a1 && b2 == a2) || (b1 == a2 && b2 == a1)));
    uint64_t f3 = mtl_sub_commit();
    CHECK("scratch submission completed", mtl_wait(f3, 5000000000ull));
    unsigned trimmed = 0;
    uint64_t freed = geo_big_trim(0, &trimmed);
    CHECK("geo_big_trim(0) reclaimed completed geometry scratch blocks", trimmed == 2 && freed == (128ull << 20) && g_gb_live_bytes == 0);

    // Cleanup and verify resource_vis drops sparse alias flag
    h_nvnMemoryPoolFinalize(&cpool); free(cmem);
    h_nvnMemoryPoolFinalize(&virt);
    h_nvnMemoryPoolFinalize(&phys1);
    h_nvnMemoryPoolFinalize(&phys2);
    h_nvnMemoryPoolFinalize(&phys_noacc);
    CHECK("resource_vis cleared sparse alias after Finalize", g_sparse_n == 0);
    munmap(raw_noacc, 8 * pg);
    munmap(raw_cpu, 12 * pg);

    printf("t_sparse_shadow: %d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
