// Uploads from a large pool (BIG_MIN) read their storage through a zero-copy buffer of the upload batch (up_big_src), which the batch makes resident and retires. One texture
// needs one such buffer: a second is made only when taking its staging flushed the batch that held the first. Before, every staged upload made two, and a scene load of Switch
// Sports Resort (855 textures, 908 MB) added twice its storage to the residency set. Checks the count and every texel, for small textures in one batch and for 64 MB ones whose
// staging splits the batches (UP_BATCH_BYTES)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { POOL_SIZE = 768u << 20 };

typedef struct { tex_t t; uint8_t *ref; size_t bytes; } upl;

static void make(upl *u, uint8_t *pool_mem, pool_t *pool, size_t off, unsigned w, unsigned h, unsigned seed) {
    static uint64_t serial = 77000;
    memset(u, 0, sizeof *u);
    u->t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = serial++};
    u->t.d = (nvn_texdesc){.target = 1, .format = NVN_FORMAT_RGBA8, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&u->t.d, &u->t.L);
    u->t.pool = pool; u->t.mem = pool_mem + off; u->t.off = off;
    u->bytes = (size_t)w * h * 4;
    u->ref = malloc(u->bytes);
    uint32_t *p = (uint32_t *)u->ref;
    for (size_t i = 0; i < u->bytes / 4; i++) p[i] = (uint32_t)(i * 2654435761u + seed * 40503u);
    dm_guest_write_prepare(u->t.mem, u->t.L.size);
    CHECK("reference swizzle succeeds", !nvn_swizzle(&u->t.d, 0, 0, u->ref, w * 4, u->t.mem));
}

static int same_texels(id<MTLTexture> tex, const upl *u) {
    unsigned w = u->t.d.width, h = u->t.d.height;
    id<MTLBuffer> rb = [g_dev newBufferWithLength:u->bytes options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:u->bytes];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted && !memcmp(rb.contents, u->ref, u->bytes);
}

// Uploads n textures in one submission; returns the zero-copy source buffers made and the upload batches begun
static void upload_all(upl *u, int n, uint64_t *made, unsigned *batches) {
    uint64_t m0 = g_up_src_made; unsigned s0 = g_up_seq;
    mtl_sub_begin();
    id<MTLTexture> got[16];
    for (int i = 0; i < n; i++) { got[i] = tex_mtl(&u[i].t); CHECK("texture resolves", got[i] != nil); }
    CHECK("submission completes", mtl_wait(mtl_sub_commit(), 10000000000ull));
    CHECK("no GPU error", !mtl_last_commit_error());
    *made = g_up_src_made - m0; *batches = g_up_seq - s0;
    for (int i = 0; i < n; i++) CHECK("every texel matches the CPU reference", got[i] && same_texels(got[i], &u[i]));
}

int main(void) { @autoreleasepool {
    mtl_init();
    CHECK("GPU deswizzle is available", g_nvn_deswz_gpu);
    uint8_t *mem = mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = POOL_SIZE, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem, POOL_SIZE); nvn_dirty_add_range(mem, POOL_SIZE);

    upl small[8]; uint64_t made; unsigned batches;
    for (int i = 0; i < 8; i++) make(&small[i], mem, &pool, (size_t)i << 20, 256, 128 + 16 * i, i);
    upload_all(small, 8, &made, &batches);
    printf("small: %llu source buffers for 8 textures, %u batches\n", (unsigned long long)made, batches);
    CHECK("one zero-copy source buffer per texture in one batch", made == 8 && batches == 1);

    upl big[3];   // 64 MB each: the third's staging no longer fits the batch's 128 MB, so taking it flushes the batch
    for (int i = 0; i < 3; i++) make(&big[i], mem, &pool, (size_t)(16 + 72 * i) << 20, 4096, 4096, 100 + i);
    upload_all(big, 3, &made, &batches);
    printf("big: %llu source buffers for 3 textures, %u batches\n", (unsigned long long)made, batches);
    CHECK("staging splits the batches", batches >= 2);
    CHECK("a second source buffer only for a texture whose staging flushed its batch", made == 3 + (batches - 1));

    for (int i = 0; i < 8; i++) { mtl_tex_release(&small[i].t); free(small[i].ref); }
    for (int i = 0; i < 3; i++) { mtl_tex_release(&big[i].t); free(big[i].ref); }
    mtl_drain(); mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain(); munmap(mem, POOL_SIZE);
    printf("t_upload_src: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
