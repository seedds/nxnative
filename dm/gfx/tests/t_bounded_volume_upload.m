// Validate the production sparse-pool upload policy and compare every block
// with the CPU deswizzle, including the last partial depth chunk.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static unsigned checks, failed;
#define CHECK(condition, label) do { checks++; if (!(condition)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void finish_upload(void) {
    up_flush();
    rec_publish();
    up_wait_zero(&g_up_pending, WS_UPALLOC);
}

static int equal_bytes(id<MTLTexture> a, id<MTLTexture> b, size_t row, size_t rows) {
    if (!a || !b) return 0;
    const size_t image = row * rows;
    const unsigned step = 8;
    id<MTLBuffer> out = [g_dev newBufferWithLength:2 * step * image options:MTLResourceStorageModeShared];
    if (!out) return 0;
    for (unsigned z = 0; z < a.depth; z += step) { @autoreleasepool {
        unsigned depth = MIN(step, a.depth - z);
        id<MTLCommandBuffer> cb = q_cb();
        id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
        id<MTLTexture> textures[] = {a, b};
        for (unsigned i = 0; i < 2; i++)
            [ce copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, z)
                    sourceSize:MTLSizeMake(a.width, a.height, depth) toBuffer:out destinationOffset:i * step * image
                    destinationBytesPerRow:row destinationBytesPerImage:image];
        [ce endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted ||
            memcmp(out.contents, (uint8_t *)out.contents + step * image, depth * image)) return 0;
    } }
    return 1;
}

static void run(pool_t *physical, pool_t *pool, uint64_t offset, unsigned format, unsigned order, unsigned depth) { @autoreleasepool {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 100 + offset, .pool = pool, .mem = pool->mem + offset + 512};
    t.d = (nvn_texdesc){.width = 1020, .height = format == 0x42 ? 1020 : 508, .depth = depth, .levels = 1,
        .samples = 1, .format = format, .target = NVN_TEXTURE_TARGET_3D, .flags = NVN_TEXTURE_FLAGS_SPARSE,
        .gob_order = order};
    nvn_tex_layout(&t.d, &t.L);
    for (uint64_t p = 0; p + 0x10000 < t.L.size; p += 0x100000) {
        map_req requests[] = {
            {.phys = (NVNmemoryPool *)physical, .voff = offset + p, .poff = 0x20000, .size = 0x4000},
            {.phys = (NVNmemoryPool *)physical, .voff = offset + p + 0x4000, .poff = 0x8000, .size = 0x4000},
            {.phys = (NVNmemoryPool *)physical, .voff = offset + p + 0xc000, .poff = 0x20000, .size = 0x4000}
        };
        CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)pool, 3, (const NVNmappingRequest *)requests), "map aliases around a hole");
    }
    int bw, bh;
    int bpb = nvn_fmt_bpb(format, &bw, &bh);
    size_t row = (t.d.width + bw - 1) / bw * bpb, rows = (t.d.height + bh - 1) / bh;
    size_t output = row * rows * depth;
    mtl_sub_begin();
    unsigned seq = g_up_seq;
    id<MTLTexture> actual = tex_mtl_all(&t);
    CHECK(actual && g_up_seq == seq + 1 && g_up_cb, "conversion uses one ordinary open upload batch");
    if (output >= 2 * UP_BLOCK_SIZE) CHECK(g_up_block.allocatedSize <= UP_BLOCK_SIZE, "large upload bounds linear staging");
    else CHECK(g_up_block.allocatedSize >= output, "small upload uses ordinary staging");
    finish_upload();

    uint64_t stamp;
    int gpu = g_nvn_deswz_gpu;
    g_nvn_deswz_gpu = 0;
    id<MTLTexture> reference = pkg_upload(&t, &stamp, nil, 0, nil, NULL, 0, NULL);
    g_nvn_deswz_gpu = gpu;
    finish_upload();
    CHECK(equal_bytes(actual, reference, row, rows), "all compressed blocks match CPU reference");
    if (reference) res_commit_retiring(@[reference]);
    serial_del(t.serial);
    mtl_sub_commit();
    mtl_drain();
    CHECK(!mtl_last_commit_error(), "submission completes");
} }

int main(void) { @autoreleasepool {
    mtl_init();
    if (!g_dev.supportsBCTextureCompression || !g_nvn_deswz_gpu || !nvn_deswz_gpu_sparse_supported()) return 77;
    size_t length = 1u << 20;
    uint8_t *data = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (data == MAP_FAILED) return 1;
    for (size_t i = 0; i < length; i++) data[i] = (i >> 4) ^ (i >> 12);
    pool_t physical, virtual;
    pool_builder_t b = {.mem = data, .size = length, .flags = NVN_MEMORY_POOL_FLAGS_PHYSICAL |
        NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    if (!h_nvnMemoryPoolInitialize((NVNmemoryPool *)&physical, (NVNmemoryPoolBuilder *)&b)) return 1;
    b = (pool_builder_t){.size = DM_GAME_APP_POOL + (1ull << 32), .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    if (!h_nvnMemoryPoolInitialize((NVNmemoryPool *)&virtual, (NVNmemoryPoolBuilder *)&b)) return 1;
    run(&physical, &virtual, 0, 0x42, 0, 63);
    for (unsigned fmt = 0; fmt < 2; fmt++) for (unsigned order = 0; order < 2; order++)
        run(&physical, &virtual, (1ull + fmt * 2 + order) << 28, fmt ? 0x50 : 0x42, order, 137);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&virtual);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&physical);
    munmap(data, length);
    printf("bounded volume upload: %u checks, %u failed\n", checks, failed);
    return failed != 0;
} }
