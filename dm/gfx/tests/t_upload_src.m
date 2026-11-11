// A packaged texture's upload reads its storage through a no-copy buffer of its own (up_big_src) when its pool is a large one no table holds whole, or one no address was given out in.
// pkg_upload_ makes it once, and again only when its staging allocation (up_alloc) flushed the batch that held the first one (g_up_seq). Cases: a fresh batch makes one (one residency
// add at up_big_src's site); a batch the texture's own staging must flush makes two, one in each batch, and both complete; DM_NVN_UPSRC_DEDUPE=0 makes two in a fresh batch, as before;
// a large pool before its table holds it whole, and after (none made, the switch on or off: big_full). Every texture must read back as the CPU reference. The test also runs with
// DM_NVN_UPSRC_DEDUPE=0 in the environment (check_t_upload_src)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
enum { W = 512, H = 512, BYTES = W * H * 4 };   // RGBA8, 1 MB
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
static uint64_t site_adds(void) {   // residency adds at up_big_src's res_add (its source line in the power line's additions, mtl_res_stats)
    for (int k = 0; k < RS_SITES; k++) if (g_up_src_site && g_rs_site[k].line == g_up_src_site) return g_rs_site[k].n;
    return 0;
}
static int tex_holds(id<MTLTexture> image, const uint32_t *lin) {
    if (!image) return 0;
    id<MTLBuffer> rb = [g_dev newBufferWithLength:BYTES options:MTLResourceStorageModeShared]; memset(rb.contents, 0xcd, BYTES);
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:BYTES];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted && !memcmp(rb.contents, lin, BYTES);
}
typedef struct { uint64_t made, remade, kept, adds; unsigned seq; } snap;
static snap now(void) { return (snap){g_up_src_made, g_up_src_remade, g_up_src_kept, site_adds(), g_up_seq}; }
// One texture at mem in pool, its contents written through the guest path (tracked pool) or directly; prefill: fill the open batch first so that the texture's own staging flushes it
static void upload(const char *what, pool_t *pool, uint8_t *mem, int tracked, int prefill, uint64_t want_made, uint64_t want_remade) {
    static uint64_t serial = 99800;
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = serial++};
    t.d = (nvn_texdesc){.target = 1, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    t.pool = pool; t.mem = mem; t.off = (uint64_t)(mem - (uint8_t *)pool->mem);   // the offset keys the sharing table (tex_alias_key): every case is a texture of its own
    uint32_t *lin = malloc(BYTES); for (uint32_t i = 0; i < W * H; i++) lin[i] = i * 2654435761u ^ (uint32_t)serial;
    if (tracked) dm_guest_write_prepare(mem, t.L.size);
    CHECK("swizzle", !nvn_swizzle(&t.d, 0, 0, lin, W * 4, t.mem));
    mtl_sub_begin();
    CHECK("no upload batch open before the case", g_up_cb == nil);
    snap s0 = now(); unsigned pre_seq = 0;
    if (prefill) {   // stage 8 MB pieces until one more block would pass UP_BATCH_BYTES, then use up the block: the texture's 1 MB of staging must flush the batch
        up_begin(); pre_seq = g_up_seq; size_t off;
        while (g_up_staging_bytes <= UP_BATCH_BYTES - UP_BLOCK_SIZE) (void)up_alloc(8u << 20, &off);
        if (g_up_block_size > g_up_block_off) (void)up_alloc(g_up_block_size - g_up_block_off, &off);
        CHECK("prefill stays in one batch", g_up_seq == pre_seq && g_up_cb != nil);
        s0 = now();
    }
    id<MTLTexture> image = tex_mtl(&t);
    CHECK("texture resolves", image != nil);
    snap s1 = now();
    if (prefill) CHECK("the texture's staging flushed the prefilled batch", s1.seq == pre_seq + 1);
    else CHECK("the texture went into one fresh batch", s1.seq == s0.seq + 1);
    finish();
    mtl_drain();
    CHECK("both batches completed", atomic_load(&g_up_pending) == 0 && !atomic_load(&g_up_lost_n));
    uint64_t want_kept = want_made && !want_remade;
    char label[256];
    snprintf(label, sizeof label, "%s: %llu no-copy source buffers made (want %llu)", what, (unsigned long long)(s1.made - s0.made), (unsigned long long)want_made); CHECK(label, s1.made - s0.made == want_made);
    snprintf(label, sizeof label, "%s: %llu made by the second call (want %llu)", what, (unsigned long long)(s1.remade - s0.remade), (unsigned long long)want_remade); CHECK(label, s1.remade - s0.remade == want_remade);
    snprintf(label, sizeof label, "%s: %llu second calls left out (want %llu)", what, (unsigned long long)(s1.kept - s0.kept), (unsigned long long)want_kept); CHECK(label, s1.kept - s0.kept == want_kept);
    snprintf(label, sizeof label, "%s: %llu residency adds at up_big_src's site (want %llu)", what, (unsigned long long)(s1.adds - s0.adds), (unsigned long long)want_made); CHECK(label, s1.adds - s0.adds == want_made);
    snprintf(label, sizeof label, "%s: the texture holds the CPU reference", what); CHECK(label, tex_holds(image, lin));
    free(lin);
    mtl_tex_release(&t); mtl_drain();
}
int main(void) { @autoreleasepool {
    mtl_init();
    CHECK("GPU deswizzle loads", g_nvn_deswz_gpu);
    g_direct_upload_on = 0;   // RGBA8 has no direct path, but keep every case on the staged one
    int on = upsrc_dedupe_on();   // DM_NVN_UPSRC_DEDUPE from the environment
    printf("t_upload_src: DM_NVN_UPSRC_DEDUPE %s\n", on ? "on (default)" : "off");
    // A pool no address was given out in, not resident: every upload reads it through a buffer of its own
    size_t psize = 16u << 20; void *pm = NULL; if (posix_memalign(&pm, getpagesize(), psize)) abort(); memset(pm, 0, psize);
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = pm, .size = psize, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(pm, psize); nvn_dirty_add_range(pm, psize);
    pthread_mutex_lock(&g_mu); int pi = pool_find((uint64_t)(uintptr_t)pm, 4096), texonly = pi >= 0 && !g_pools[pi].res && !addr_meets(g_pools[pi].lo, g_pools[pi].hi); pthread_mutex_unlock(&g_mu);
    CHECK("the pool holds only textures (no address given out, not resident)", texonly);
    uint8_t *m = pm;
    upload("fresh batch", &pool, m, 1, 0, on ? 1 : 2, on ? 0 : 1);
    upload("batch flushed by the texture's staging", &pool, m + (2u << 20), 1, 1, 2, 1);
    upload("fresh batch again", &pool, m + (4u << 20), 1, 0, on ? 1 : 2, on ? 0 : 1);
    if (on) {   // the switch off: two in a fresh batch, as before
        g_upsrc_dedupe = 0;
        upload("DM_NVN_UPSRC_DEDUPE=0, fresh batch", &pool, m + (6u << 20), 1, 0, 2, 1);
        upload("DM_NVN_UPSRC_DEDUPE=0, flushed batch", &pool, m + (8u << 20), 1, 1, 2, 1);
        g_upsrc_dedupe = 1;
    }
    pthread_mutex_lock(&g_mu); pi = pool_find((uint64_t)(uintptr_t)pm, 4096); int still = pi >= 0 && !g_pools[pi].res; pthread_mutex_unlock(&g_mu);
    CHECK("the uploads left the pool out of the residency set", still);
    mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(pm); mtl_pool_del(pm); mtl_drain();
    // A large pool (BIG_MIN): a buffer of its own while no pool table holds it whole; none once one does (big_full), the lookup's whole-pool buffer instead, the switch on or off
    const uint64_t GB = 1ull << 30, MB = 1ull << 20;
    uint8_t *big = mmap(NULL, GB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (big == MAP_FAILED) abort();
    pool_t bpool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = big, .size = GB, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(big, GB);
    CHECK("a large pool", g_nbig == 1 && !g_big[0].full);
    upload("large pool, not whole in a table", &bpool, big + 100 * MB, 0, 0, on ? 1 : 2, on ? 0 : 1);
    upload("large pool, not whole in a table, flushed batch", &bpool, big + 200 * MB, 0, 1, 2, 1);
    mtl_pool_addressed(big + 900 * MB, 4096);   // an address given out in it: raw global memory's table takes it whole
    mtl_sub_begin(); CHECK("pool table made", gmem_map_gpu() != 0); finish();
    CHECK("the whole pool has an entry", g_big[0].full > 0);
    upload("large pool held whole (big_full)", &bpool, big + 300 * MB, 0, 0, 0, 0);
    upload("large pool held whole, flushed batch", &bpool, big + 500 * MB, 0, 1, 0, 0);
    if (on) {
        g_upsrc_dedupe = 0;
        upload("DM_NVN_UPSRC_DEDUPE=0, large pool held whole", &bpool, big + 700 * MB, 0, 0, 0, 0);
        g_upsrc_dedupe = 1;
    }
    mtl_alias_drop_pool(&bpool); mtl_pool_del(big); mtl_drain();
    printf("t_upload_src: %d checks, %d failed; no-copy source buffers made %llu (second call %llu), second calls left out %llu\n", checks, failed,
           (unsigned long long)g_up_src_made, (unsigned long long)g_up_src_remade, (unsigned long long)g_up_src_kept);
    return failed != 0;
} }
