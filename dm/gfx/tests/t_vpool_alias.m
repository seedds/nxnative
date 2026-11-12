// Aliased virtual memory pools inherit physical storage flags (vmode == 1) while preserving guest CPU_NO_ACCESS map behavior, pre-mapping pool use forces sparse mode (vmode == 2), and sampler pool capacity bounds out-of-range sampler IDs without false [RED] errors.
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
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }

static int texels_match(id<MTLTexture> m, unsigned w, unsigned h, const uint32_t *want) {
    if (!m) return 0;
    id<MTLBuffer> rb = [g_dev newBufferWithLength:(size_t)w * h * 4 options:MTLResourceStorageModeShared];
    if (!rb) return 0;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:m sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return memcmp(rb.contents, want, (size_t)w * h * 4) == 0;
}

static void init_rgba8_2d(NVNtexture *tx, const pool_t *pool, unsigned w, unsigned h) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&tb, (int)w); h_nvnTextureBuilderSetHeight(&tb, (int)h);
    h_nvnTextureBuilderSetLevels(&tb, 1); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)pool, 0);
    CHECK("texture initialize", h_nvnTextureInitialize(tx, &tb));
}

int main(void) { @autoreleasepool {
    mtl_init();
    enum { W = 32, H = 32, POOL_SZ = 128u << 10 };

    uint8_t *phys_u_mem = mmap(NULL, POOL_SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *phys_c_mem = mmap(NULL, POOL_SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *phys_s_mem = mmap(NULL, POOL_SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK("mmap physical backing", phys_u_mem != MAP_FAILED && phys_c_mem != MAP_FAILED && phys_s_mem != MAP_FAILED);

    pool_t phys_u, phys_c, vpool_u, vpool_c;
    pool_builder_t pb = {.mem = phys_u_mem, .size = POOL_SZ, .flags = 0x22};
    CHECK("phys uncached pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys_u, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.mem = phys_c_mem, .size = POOL_SZ, .flags = 0x24};
    CHECK("phys cached pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys_c, (const NVNmemoryPoolBuilder *)&pb));

    pb = (pool_builder_t){.size = POOL_SZ, .flags = 0x221};
    CHECK("virtual uncached alias candidate", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&vpool_u, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = POOL_SZ, .flags = 0x221};
    CHECK("virtual cached alias candidate", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&vpool_c, (const NVNmemoryPoolBuilder *)&pb));

    // Prepare distinct swizzled non-zero RGBA8 payloads in the two physical pools.
    NVNtexture probe; init_rgba8_2d(&probe, &phys_u, W, H);
    const tex_t *pt = (const tex_t *)&probe;
    uint32_t lin_u[W * H], lin_c[W * H];
    for (size_t i = 0; i < W * H; i++) {
        lin_u[i] = 0xFF000001u + (uint32_t)(i * 2654435761u);
        lin_c[i] = 0x80204060u ^ (uint32_t)(i * 2246822519u);
    }
    CHECK("swizzle uncached", !nvn_swizzle(&pt->d, 0, 0, lin_u, W * 4, phys_u_mem));
    CHECK("swizzle cached", !nvn_swizzle(&pt->d, 0, 0, lin_c, W * 4, phys_c_mem));
    h_nvnTextureFinalize(&probe);

    map_req req_u = {.phys = (NVNmemoryPool *)&phys_u, .poff = 0, .voff = 0, .size = POOL_SZ};
    CHECK("map uncached 1:1", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&vpool_u, 1, (const NVNmappingRequest *)&req_u));
    CHECK("vmode == 1 for uncached alias", vpool_u.vmode == 1 && vpool_u.host_flags == 0x22 && pool_host_flags(&vpool_u) == 0x22 && vpool_u.flags == 0x221);

    NVNbufferBuilder bb; NVNbuffer buf;
    h_nvnBufferBuilderSetDefaults(&bb);
    h_nvnBufferBuilderSetStorage(&bb, (NVNmemoryPool *)&vpool_u, 0, 256);
    CHECK("buffer init on aliased virtual pool", h_nvnBufferInitialize(&buf, &bb));
    CHECK("nvnBufferMap preserves virtual CPU_NO_ACCESS", h_nvnBufferMap(&buf) == NULL);
    CHECK("nvnMemoryPoolMap preserves virtual CPU_NO_ACCESS", h_nvnMemoryPoolMap((const NVNmemoryPool *)&vpool_u) == NULL);

    map_req req_c = {.phys = (NVNmemoryPool *)&phys_c, .poff = 0, .voff = 0, .size = POOL_SZ};
    CHECK("map cached 1:1", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&vpool_c, 1, (const NVNmappingRequest *)&req_c));
    CHECK("vmode == 1 for cached alias", vpool_c.vmode == 1 && vpool_c.host_flags == 0x24 && pool_host_flags(&vpool_c) == 0x24);
    h_nvnMemoryPoolFlushMappedRange((const NVNmemoryPool *)&vpool_c, 0, POOL_SZ);

    NVNtexture tx_u, tx_c;
    init_rgba8_2d(&tx_u, &vpool_u, W, H);
    init_rgba8_2d(&tx_c, &vpool_c, W, H);
    mtl_sub_begin();
    id<MTLTexture> mu = tex_mtl((const tex_t *)&tx_u);
    id<MTLTexture> mc = tex_mtl((const tex_t *)&tx_c);
    finish();
    CHECK("uncached aliased virtual texture uploads non-zero texels", texels_match(mu, W, H, lin_u));
    CHECK("cached aliased virtual texture uploads flushed texels", texels_match(mc, W, H, lin_c));

    // Pre-mapping address or texture use prevents aliasing and forces sparse mode (vmode == 2).
    pool_t phys_s, vpool_addr, vpool_tex;
    pb = (pool_builder_t){.mem = phys_s_mem, .size = POOL_SZ, .flags = 0x22};
    CHECK("phys sparse test pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&phys_s, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = POOL_SZ, .flags = 0x221};
    CHECK("vpool_addr init", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&vpool_addr, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("GetBufferAddress returns reserved range", h_nvnMemoryPoolGetBufferAddress((const NVNmemoryPool *)&vpool_addr) != 0);
    CHECK("used marked before MapVirtual (addr)", vpool_addr.vp && vpool_addr.vp->used == 1);
    map_req req_s = {.phys = (NVNmemoryPool *)&phys_s, .poff = 0, .voff = 0, .size = POOL_SZ};
    CHECK("map vpool_addr 1:1", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&vpool_addr, 1, (const NVNmappingRequest *)&req_s));
    CHECK("vpool_addr stays sparse (vmode == 2)", vpool_addr.vmode == 2 && vpool_addr.vp != NULL);

    pb = (pool_builder_t){.size = POOL_SZ, .flags = 0x221};
    CHECK("vpool_tex init", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&vpool_tex, (const NVNmemoryPoolBuilder *)&pb));
    NVNtexture tx_pre; init_rgba8_2d(&tx_pre, &vpool_tex, W, H);
    CHECK("used marked before MapVirtual (tex)", vpool_tex.vp && vpool_tex.vp->used == 1);
    CHECK("map vpool_tex 1:1", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&vpool_tex, 1, (const NVNmappingRequest *)&req_s));
    CHECK("vpool_tex stays sparse (vmode == 2)", vpool_tex.vmode == 2 && vpool_tex.vp != NULL);

    // Sampler pool capacity bounds out-of-range IDs cleanly while keeping in-pool errors strict.
    NVNsamplerPool sp;
    CHECK("sampler pool init n=512", h_nvnSamplerPoolInitialize(&sp, (const NVNmemoryPool *)&phys_u, 0, 512));
    CHECK("g_spool_n recorded", g_spool_n == 512);
    os_unfair_lock_lock(&g_rmu);
    id<MTLSamplerState> s_oob = samp_by_id(4095);
    id<MTLSamplerState> s_in = samp_by_id(300);
    id<MTLSamplerState> s_zero = samp_by_id(0);
    os_unfair_lock_unlock(&g_rmu);
    CHECK("OOB sampler ID 4095 returns nil without RED", s_oob == nil && g_stat_samp_oob == 1);
    CHECK("in-pool unregistered sampler ID 300 and slot 0 trigger RED, not OOB", s_in == nil && s_zero == nil && g_red_once[12] == 2 && g_stat_samp_oob == 1);

    h_nvnTextureFinalize(&tx_pre);
    h_nvnTextureFinalize(&tx_u);
    h_nvnTextureFinalize(&tx_c);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vpool_tex);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vpool_addr);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vpool_c);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&vpool_u);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys_s);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys_c);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&phys_u);
    munmap(phys_s_mem, POOL_SZ); munmap(phys_c_mem, POOL_SZ); munmap(phys_u_mem, POOL_SZ);

    if (failed) { fprintf(stderr, "t_vpool_alias: %d/%d failed\n", failed, checks); return 1; }
    printf("t_vpool_alias: ok (%d checks)\n", checks);
    return 0;
}}
