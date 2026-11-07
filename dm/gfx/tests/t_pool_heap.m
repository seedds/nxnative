// A CPU_NO_ACCESS pool's render targets share one placement heap (nvn_mtl.m pool_heap_texture): textures over the same storage share memory, storage apart stays apart,
// a texture made over memory another has used leaves it as it is, and what is not a render target, or not in such a pool, keeps an allocation of its own.
// "./t_pool_heap off" runs with DM_NVN_POOL_HEAP=0: no texture is placed.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { POOL = 32 << 20 };
static tex_t target(pool_t *pool, uint64_t serial, int64_t off, unsigned w, unsigned h, int flags) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .pool = pool, .off = off, .mem = pool->mem + off};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .flags = flags, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static id<MTLBuffer> read_image(id<MTLTexture> t) {
    NSUInteger row = t.width * 4, bytes = row * t.height;
    id<MTLBuffer> b = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(t.width,t.height,1), b, 0, row, bytes);
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "a read completes without GPU error");
    res_remove(b); mtl_sub_begin(); return b;
}
static void write_image(id<MTLTexture> t, uint32_t seed) {
    NSUInteger bytes = t.width * t.height * 4;
    id<MTLBuffer> b = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    uint32_t *p = b.contents; for (NSUInteger i = 0; i < bytes / 4; i++) p[i] = seed ^ (uint32_t)i;
    res_add(b); enc_b_copy_buffer_texture(blit(), b, 0, t.width * 4, bytes, MTLSizeMake(t.width,t.height,1), t, 0, 0, MTLOriginMake(0,0,0));
    mtl_sub_commit(); mtl_drain(); res_remove(b); mtl_sub_begin();
}
static int holds(id<MTLTexture> t, uint32_t seed) {
    id<MTLBuffer> b = read_image(t); const uint32_t *p = b.contents;
    for (NSUInteger i = 0; i < b.length / 4; i++) if (p[i] != (seed ^ (uint32_t)i)) return 0;
    return 1;
}
static int zeros(id<MTLTexture> t) {
    id<MTLBuffer> b = read_image(t); const uint32_t *p = b.contents;
    for (NSUInteger i = 0; i < b.length / 4; i++) if (p[i]) return 0;
    return 1;
}

int main(int argc, char **argv) { @autoreleasepool {
    int off = argc > 1 && !strcmp(argv[1], "off");
    if (off) setenv("DM_NVN_POOL_HEAP", "0", 1);
    mtl_init();
    uint8_t *mem = aligned_alloc(1 << 16, POOL); memset(mem, 0xa5, POOL);   // storage the GPU-only pool's textures must not take their content from
    uint8_t *cpu_mem = aligned_alloc(1 << 16, 4 << 20); memset(cpu_mem, 0, 4 << 20);
    mtl_pool_add(mem, POOL); mtl_pool_add(cpu_mem, 4 << 20);
    pool_t pool = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem = mem, .size = POOL};
    pool_t cpu = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = cpu_mem, .size = 4 << 20};
    __weak id<MTLHeap> weak_heap = nil;
    @autoreleasepool {
    mtl_sub_begin();   // inside this pool: a submission pushes one of its own, popped at its commit
    tex_t a = target(&pool, 995000, 0, 512, 512, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    id<MTLTexture> ma = tex_mtl(&a);
    if (off) {
        CHECK(ma && !ma.heap, "DM_NVN_POOL_HEAP=0: a render target keeps an allocation of its own");
    } else {
        CHECK(ma.heap != nil, "a render target of a CPU_NO_ACCESS pool is placed in the pool's heap");
        CHECK(ma.heap.size <= (NSUInteger)(POOL * POOL_HEAP_SCALE) + (2 << 20), "the heap is about the pool's size, scaled");
        CHECK(zeros(ma), "memory no texture has covered starts as zeros, not the storage's bytes");
        write_image(ma, 0x11223344);

        tex_t b = target(&pool, 995001, 0, 256, 256, NVN_TEXTURE_FLAGS_COMPRESSIBLE);   // another description over the same storage, as a frame graph places its targets
        id<MTLTexture> mb = tex_mtl(&b);
        CHECK(mb.heap == ma.heap && mb != ma, "another description over the same storage shares the heap and is a texture of its own");
        CHECK(mb.heapOffset < ma.heapOffset + ma.allocatedSize && ma.heapOffset < mb.heapOffset + mb.allocatedSize, "storage shared in the pool is shared in the heap");
        CHECK(holds(ma, 0x11223344), "making a texture over memory another has used leaves that memory as it is");

        tex_t c = target(&pool, 995002, 8 << 20, 512, 512, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        id<MTLTexture> mc = tex_mtl(&c);
        CHECK(mc.heap == ma.heap, "a render target elsewhere in the pool shares the heap");
        CHECK(mc.heapOffset >= ma.heapOffset + ma.allocatedSize, "storage apart in the pool is apart in the heap");
        write_image(mc, 0x55667788);
        CHECK(holds(ma, 0x11223344) && holds(mc, 0x55667788), "writes to storage apart do not reach each other");

        // An image (COMPRESSIBLE|IMAGE) in the pool heap, then another description over its storage: the image family lookup must not take the pool's heap as a family's
        tex_t img = target(&pool, 995010, 24 << 20, 512, 512, NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE);
        id<MTLTexture> mi = tex_mtl(&img);
        tex_t img2 = target(&pool, 995011, 24 << 20, 256, 256, NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE);
        id<MTLTexture> mi2 = tex_mtl(&img2);
        CHECK(mi.heap == ma.heap && mi2.heap == ma.heap, "images of the pool share its heap");
        CHECK(mi2.heapOffset >= (NSUInteger)((double)(24 << 20) * POOL_HEAP_SCALE) - (1 << 20), "a late image description keeps its scaled pool offset, not the heap's start");
        write_image(mi2, 0x99aabbcc);
        CHECK(holds(ma, 0x11223344) && holds(mc, 0x55667788), "a late image's writes do not reach separate pool storage");
        mtl_tex_release(&img); mtl_tex_release(&img2); mi = nil; mi2 = nil;

        tex_t plain = target(&pool, 995003, 16 << 20, 256, 256, 0);
        CHECK(!tex_mtl(&plain).heap, "a texture without COMPRESSIBLE keeps an allocation of its own");
        tex_t sparse = target(&pool, 995004, 20 << 20, 256, 256, NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_SPARSE);
        CHECK(!tex_mtl(&sparse).heap, "a sparse texture keeps an allocation of its own");
        tex_t visible = target(&cpu, 995005, 0, 256, 256, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        CHECK(!tex_mtl(&visible).heap, "a render target of a pool the CPU can write keeps an allocation of its own");
        CHECK([g_res_objects containsObject:ma.heap] && ![g_res_objects containsObject:ma], "the heap is resident, not each texture");
        weak_heap = ma.heap;
        mtl_tex_release(&b); mtl_tex_release(&c); mtl_tex_release(&plain); mtl_tex_release(&sparse); mtl_tex_release(&visible);
        mb = nil; mc = nil;
    }
    mtl_tex_release(&a); ma = nil;
    mtl_alias_drop_pool(&pool); mtl_alias_drop_pool(&cpu);
    mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    dispatch_sync(g_feedback_q, ^{}); defer_drain();
    }
    CHECK(!weak_heap || ![g_res_objects containsObject:weak_heap], "the pool's heap leaves the residency set with its last texture");
    CHECK(g_image_heap_n == 0, "no heap owner is left after the pool goes");
    free(mem); free(cpu_mem);
    printf("t_pool_heap%s: %d checks, %d failed\n", off ? " (off)" : "", checks, failed); return failed != 0;
} }
