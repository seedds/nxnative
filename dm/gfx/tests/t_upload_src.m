// Uploads from a large pool (BIG_MIN) read their storage through a zero-copy buffer of the upload batch (up_big_src), which the batch makes resident and retires. One texture
// needs one such buffer: a second is made only when taking its staging flushed the batch that held the first. Before, every staged upload made two, and a scene load of Switch
// Sports Resort (855 textures, 908 MB) added twice its storage to the residency set. Checks the count and every texel, for small textures in one batch and for 64 MB ones whose
// staging splits the batches (UP_BATCH_BYTES). A buffer copy over a texture's storage: after it completes, only that texture is uploaded again (its pages stamped at completion,
// post_run); in the same submission, a texture already uploaded and one not yet uploaded, both read after the run of copies has ended, show the copied texels (b2b_flush: the
// copy is committed before the upload batch that reads it), including a destination accessed before the copy in that submission. A CPU upload waits for the copy before reading
// its storage; a queue event keeps the copy pending until after the lookup starts. DM_NVN_COPY_CUT=0 fails the same-submission checks
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
    if (!rb) abort();
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

    upl incoming;
    make(&incoming, mem, &pool, 9u << 20, small[0].t.d.width, small[0].t.d.height, 99);
    mtl_sub_begin();
    mtl_sub_copy((uintptr_t)incoming.t.mem, (uintptr_t)small[0].t.mem, small[0].t.L.size);
    CHECK("buffer copy completes", mtl_wait(mtl_sub_commit(), 10000000000ull));
    free(small[0].ref); small[0].ref = incoming.ref;
    upload_all(small, 2, &made, &batches);
    CHECK("only the overwritten texture was uploaded", made == 1);
    mtl_tex_release(&incoming.t);

    // Pool compaction as Skywalker Saga does it: copies move texture storage, then a command, then the textures are read in the same submission
    upl moved[2], late;
    make(&late, mem, &pool, 11u << 20, small[1].t.d.width, small[1].t.d.height, 200);   // written by the CPU, not uploaded yet
    for (int i = 0; i < 2; i++) make(&moved[i], mem, &pool, (size_t)(12 + i) << 20, small[1].t.d.width, small[1].t.d.height, 300 + i);
    struct { op_hdr c0; uint64_t a0[3]; op_hdr c1; uint64_t a1[3]; op_hdr bc; p_blend_color col; op_hdr end; } ops = {
        .c0 = {OP_COPY_B2B, sizeof(op_hdr) + 24}, .a0 = {(uintptr_t)moved[0].t.mem, (uintptr_t)small[1].t.mem, small[1].t.L.size},
        .c1 = {OP_COPY_B2B, sizeof(op_hdr) + 24}, .a1 = {(uintptr_t)moved[1].t.mem, (uintptr_t)late.t.mem, late.t.L.size},
        .bc = {OP_BLEND_COLOR, sizeof(op_hdr) + sizeof(p_blend_color)}, .end = {OP_END, sizeof(op_hdr)}};
    mtl_sub_begin();
    CHECK("the destination is accessed before its copy in the same submission", tex_mtl(&small[1].t) != nil);
    mtl_exec((const uint8_t *)&ops);
    id<MTLTexture> kept = tex_mtl(&small[1].t), fresh = tex_mtl(&late.t);
    CHECK("same-submission copy completes", mtl_wait(mtl_sub_commit(), 10000000000ull) && !mtl_last_commit_error());
    CHECK("an uploaded texture read after a copy over it in the same submission shows the copy", kept && same_texels(kept, &moved[0]));
    CHECK("a new texture read after a copy over it in the same submission shows the copy", fresh && same_texels(fresh, &moved[1]));

    // Force the CPU fallback while the GPU is held behind an event. Committing the copy alone is insufficient: the CPU must wait before snapshotting its destination.
    upl cpu;
    make(&cpu, mem, &pool, 14u << 20, moved[1].t.d.width, moved[1].t.d.height, 500);
    struct { op_hdr c; uint64_t a[3]; op_hdr bc; p_blend_color col; op_hdr end; } cpu_ops = {
        .c = {OP_COPY_B2B, sizeof(op_hdr) + 24}, .a = {(uintptr_t)moved[1].t.mem, (uintptr_t)cpu.t.mem, cpu.t.L.size},
        .bc = {OP_BLEND_COLOR, sizeof(op_hdr) + sizeof(p_blend_color)}, .end = {OP_END, sizeof(op_hdr)}};
    int gpu = g_nvn_deswz_gpu; g_nvn_deswz_gpu = 0;
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    [g_q4 waitForEvent:gate value:1];
    mtl_sub_begin(); mtl_exec((const uint8_t *)&cpu_ops);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200000000), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ gate.signaledValue = 1; });
    id<MTLTexture> from_cpu = tex_mtl(&cpu.t);
    CHECK("CPU upload waits for the buffer copy", gate.signaledValue == 1);
    CHECK("CPU upload submission completes", mtl_wait(mtl_sub_commit(), 10000000000ull) && !mtl_last_commit_error());
    CHECK("CPU upload reads copied texels", from_cpu && same_texels(from_cpu, &moved[1]));
    g_nvn_deswz_gpu = gpu;
    mtl_tex_release(&cpu.t); free(cpu.ref);
    for (int i = 0; i < 2; i++) { mtl_tex_release(&moved[i].t); free(moved[i].ref); }
    mtl_tex_release(&late.t); free(late.ref);

    upl big[3];   // 64 MB each: the third's staging no longer fits the batch's 128 MB, so taking it flushes the batch
    for (int i = 0; i < 3; i++) make(&big[i], mem, &pool, (size_t)(16 + 72 * i) << 20, 4096, 4096, 100 + i);
    upload_all(big, 3, &made, &batches);
    printf("big: %llu source buffers for 3 textures, %u batches\n", (unsigned long long)made, batches);
    if (up_src_budget()) {
        CHECK("the source budget splits before each 64 MiB texture", batches == 3);
        CHECK("each texture needs only the source buffer in its own batch", made == 3);
    } else {
        CHECK("staging splits the batches", batches >= 2);
        CHECK("a second source buffer only for a texture whose staging flushed its batch", made == 3 + (batches - 1));
    }

    for (int i = 0; i < 8; i++) { mtl_tex_release(&small[i].t); free(small[i].ref); }
    for (int i = 0; i < 3; i++) { mtl_tex_release(&big[i].t); free(big[i].ref); }
    mtl_drain(); mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain(); munmap(mem, POOL_SIZE);
    printf("t_upload_src: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
