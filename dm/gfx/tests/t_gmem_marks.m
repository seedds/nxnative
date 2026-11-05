// Raw global memory's pool table (nvn_mtl.m gmem_map_gpu) lists only the pools the game took a buffer address in (mtl_pool_buffer_addr: nvnMemoryPoolGetBufferAddress,
// nvnBufferInitialize, nvnTextureGetTextureAddress), and only those come into the residency set for it. A pool holding only textures stays out of the table and out of the residency
// set, through a pool walk's storage check (tex_storage_live) and a texture upload (up_big_src) too, and its texture still holds what the pool holds. DM_NVN_GMEM_ALL lists every pool.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
static void table(void) { mtl_sub_begin(); CHECK("pool table made", gmem_map_gpu() != 0); finish(); }
static int listed(const void *mem) {   // an entry of the current table starts at mem
    const uint8_t *t = (const uint8_t *)g_gmem_buf.contents;
    for (size_t k = 1; k < g_gmem_buf.length / DMSH_GMEM_MAP_ENTRY; k++) { uint64_t lo; memcpy(&lo, t + k * DMSH_GMEM_MAP_ENTRY, 8); if (lo == ~0ull) break; if (lo == (uint64_t)(uintptr_t)mem) return 1; }
    return 0;
}
static int resident(const void *mem) { pthread_mutex_lock(&g_mu); int i = pool_find((uint64_t)(uintptr_t)mem, 1), r = i >= 0 && g_pools[i].res; pthread_mutex_unlock(&g_mu); return r; }
static uint8_t *pool_mem(size_t len) { uint8_t *m = NULL; if (posix_memalign((void **)&m, (size_t)getpagesize(), len)) abort(); memset(m, 0, len); mtl_pool_add(m, len); return m; }
int main(void) { @autoreleasepool {
    mtl_init();
    g_gmem_all = 0;
    const size_t LEN = 1 << 20;
    uint8_t *bufs = pool_mem(LEN), *ubo = pool_mem(LEN), *texs = pool_mem(LEN);
    mtl_pool_buffer_addr(bufs, LEN);          // nvnMemoryPoolGetBufferAddress
    mtl_pool_buffer_addr(ubo, LEN);           // nvnBufferInitialize marks the buffer's pool
    uint64_t gen = atomic_load(&g_pool_gen);
    mtl_pool_buffer_addr(bufs, LEN);
    CHECK("marking a pool again does not rebuild the table", atomic_load(&g_pool_gen) == gen);
    table();
    CHECK("a pool with a buffer address is listed", listed(bufs) && listed(ubo));
    CHECK("and resident for the table", resident(bufs) && resident(ubo));
    CHECK("a pool without one is not listed", !listed(texs));
    CHECK("nor resident", !resident(texs));
    // A texture in that pool: the pool walk's storage check and its upload leave the pool out of the residency set
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = 99901};
    t.d = (nvn_texdesc){.target = 1, .format = NVN_FORMAT_RGBA8, .width = 128, .height = 128, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = texs, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    t.pool = &pool; t.mem = texs + 65536;
    uint32_t *lin = malloc(128 * 128 * 4); for (uint32_t i = 0; i < 128 * 128; i++) lin[i] = i * 2654435761u;
    CHECK("swizzle", !nvn_swizzle(&t.d, 0, 0, lin, 128 * 4, t.mem));
    CHECK("its storage counts as live", tex_storage_live(&t));
    CHECK("the storage check leaves the pool out of the residency set", !resident(texs));
    mtl_sub_begin(); id<MTLTexture> image = tex_mtl(&t); CHECK("texture resolves", image != nil); finish();
    CHECK("the upload leaves the pool out of the residency set", !g_nvn_deswz_gpu || !resident(texs));
    id<MTLBuffer> rb = [g_dev newBufferWithLength:128 * 128 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(128, 128, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:128 * 4 destinationBytesPerImage:128 * 128 * 4];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("the texture holds what the pool holds", !memcmp(rb.contents, lin, 128 * 128 * 4));
    free(lin); mtl_tex_release(&t);
    // nvnTextureGetTextureAddress on it: from then on the pool is listed
    mtl_pool_buffer_addr(texs, LEN);
    table();
    CHECK("a pool marked later is listed", listed(texs) && resident(texs));
    // DM_NVN_GMEM_ALL=1: every pool, marked or not
    uint8_t *other = pool_mem(LEN);
    table();
    CHECK("a new unmarked pool is not listed", !listed(other) && !resident(other));
    g_gmem_all = 1; atomic_fetch_add(&g_pool_gen, 1);
    table();
    CHECK("DM_NVN_GMEM_ALL: every pool is listed", listed(other) && resident(other) && listed(bufs));
    mtl_pool_del(bufs); mtl_pool_del(ubo); mtl_pool_del(texs); mtl_pool_del(other); mtl_drain();
    printf("t_gmem_marks: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
