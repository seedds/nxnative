// A large memory pool in chunks (nvn_mtl.m BIG_MIN): a lookup gets a zero-copy buffer that holds what it asked and starts at its stride, so only what lookups reach is wired. The buffers must
// say the same memory as the guest address, a read of unknown extent must have BIG_UNKNOWN past it, a chunk asked for more is lengthened, a texture's upload wires no chunk (up_big_src),
// raw global memory's pool table covers the whole pool (big_full), the pool's removal takes every entry, and a smaller pool stays one buffer.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static void finish(void) { CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK("GPU has no submission error", !mtl_last_commit_error()); }
// The lookup holds [a, a + n) in one buffer whose contents are the guest's memory there, and the GPU address agrees
static id<MTLBuffer> look(uint64_t a, uint64_t n) {
    void *mb = NULL; uint64_t mo = 0, gpu = 0;
    if (!mtl_gpu_addr(a, n, &mb, &mo, &gpu)) { CHECK("lookup found", 0); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    CHECK("buffer holds the range", mo + (n ? n : 1) <= b.length);
    CHECK("contents are the guest's memory", (uint64_t)(uintptr_t)b.contents + mo == a);
    CHECK("GPU address is the buffer's", gpu == b.gpuAddress + mo);
    void *cb = NULL; uint64_t co = 0; CHECK("the CPU lookup agrees", mtl_addr(a, n, &cb, &co) && (uint64_t)(uintptr_t)((__bridge id<MTLBuffer>)cb).contents + co == a);
    return b;
}
int main(void) { @autoreleasepool {
    mtl_init();
    const uint64_t GB = 1ull << 30, MB = 1ull << 20;
    uint8_t *mem = mmap(NULL, GB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t base = (uint64_t)(uintptr_t)mem;
    int pools0 = g_npool;
    mtl_pool_add(mem, GB);
    CHECK("no buffer until a lookup", g_npool == pools0 && g_nbig == 1);
    CHECK("overlapping buffers: barriers carry alias visibility", resource_vis() & MTL4VisibilityOptionResourceAlias);
    id<MTLBuffer> a = look(base + 100, 4096);
    CHECK("one chunk", g_npool == pools0 + 1);
    CHECK("a chunk is far smaller than the pool", a.length <= 3 * BIG_S);
    CHECK("the same stride: the same buffer", look(base + 200, 4096) == a);
    CHECK("no more chunks", g_npool == pools0 + 1);
    id<MTLBuffer> far = look(base + 512 * MB + 5, 64);
    CHECK("another stride: another buffer", far != a && g_npool == pools0 + 2);
    id<MTLBuffer> cross = look(base + 48 * MB - 8, 64);   // across a stride's end: one buffer holds it
    CHECK("across strides: its stride's chunk", cross && cross != a);
    // Length 1: the read's extent is not known
    void *mb = NULL; uint64_t mo = 0, gpu = 0;   // CHECK goes on after a failed lookup, which writes none of them
    uint64_t un = base + 100 * MB + 3;
    CHECK("unknown extent: found", mtl_gpu_addr(un, 1, &mb, &mo, &gpu));
    CHECK("unknown extent: BIG_UNKNOWN past the address", ((__bridge id<MTLBuffer>)mb).length - mo >= BIG_UNKNOWN);
    (void)look(base + 200 * MB + 3, 100);   // a short chunk first
    CHECK("then unknown in its stride: found", mtl_gpu_addr(base + 200 * MB + 9, 1, &mb, &mo, &gpu));
    CHECK("then unknown in its stride: lengthened", ((__bridge id<MTLBuffer>)mb).length - mo >= BIG_UNKNOWN);
    int n1 = g_npool;
    id<MTLBuffer> big = look(base + 300 * MB, 100 * MB);
    CHECK("a long range in one buffer", big.length >= 100 * MB && g_npool == n1 + 1);
    id<MTLBuffer> again = look(base + 300 * MB + 4096, 4096);
    CHECK("its stride keeps the long chunk", again == big);
    // The pool's end
    CHECK("past the end: not found", !mtl_gpu_addr(base + GB - 8, 64, &mb, &mo, &gpu));
    CHECK("at the end: found", mtl_gpu_addr(base + GB - 8, 8, &mb, &mo, &gpu));
    CHECK("at the end, unknown: found", mtl_gpu_addr(base + GB - 8, 1, &mb, &mo, &gpu));
    CHECK("the end's buffer stops at the pool", (uint64_t)(uintptr_t)((__bridge id<MTLBuffer>)mb).contents + ((__bridge id<MTLBuffer>)mb).length == base + GB);
    // A texture's upload: right bytes, through a buffer of its own, no chunk made and no chunk's memory left resident
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .has_layout = 1, .serial = 99700};
    t.d = (nvn_texdesc){.target = 1, .format = NVN_FORMAT_RGBA8, .width = 256, .height = 256, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    pool_t pool = {.magic = MAGIC('P', 'O', 'O', 'L'), .mem = mem, .size = GB, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    t.pool = &pool; t.mem = mem + 700 * MB;
    uint32_t *lin = malloc(256 * 256 * 4); for (uint32_t i = 0; i < 256 * 256; i++) lin[i] = i * 2654435761u;
    CHECK("swizzle", !nvn_swizzle(&t.d, 0, 0, lin, 256 * 4, t.mem));
    int n2 = g_npool; mtl_sub_begin(); id<MTLTexture> image = tex_mtl(&t); CHECK("texture resolves", image != nil); finish();
    CHECK("the upload made no chunk", g_npool == n2);
    id<MTLBuffer> rb = [g_dev newBufferWithLength:256 * 256 * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> ce = [cb blitCommandEncoder];
    [ce copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(256, 256, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:256 * 4 destinationBytesPerImage:256 * 256 * 4];
    [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK("the texture holds what the pool holds", !memcmp(rb.contents, lin, 256 * 256 * 4));
    free(lin);
    mtl_tex_release(&t);
    // Raw global memory: the table reaches all of it, and lookups then have the whole pool
    mtl_sub_begin(); id<MTLBuffer> old_map = gmem_map(); CHECK("pool table made", old_map != nil); finish();
    const uint64_t *ranges = old_map.contents;
    uint64_t chunk = (base + 900 * MB) >> DMSH_GMEM_HINT_SHIFT;
    size_t hint_word = (chunk % DMSH_GMEM_MAP_ENTRIES) * (DMSH_GMEM_MAP_ENTRY / sizeof(uint32_t)) + 6;
    uint32_t hint = ((const uint32_t *)old_map.contents)[hint_word];
    unsigned entry = hint % DMSH_GMEM_MAP_ENTRIES;
    const uint64_t *range = ranges + entry * (DMSH_GMEM_MAP_ENTRY / sizeof *ranges);
    CHECK("the hint names its chunk and pool", entry && hint / DMSH_GMEM_MAP_ENTRIES == chunk);
    CHECK("the entire hinted chunk lies within its pool range", range[0] <= chunk << DMSH_GMEM_HINT_SHIFT && range[1] >= (chunk + 1) << DMSH_GMEM_HINT_SHIFT);
    CHECK("the whole pool has an entry", g_big[0].full > 0 && g_pools[g_big[0].full - 1].end == base + GB);
    CHECK("lookups take it", look(base + 900 * MB, 4096).length == GB);
    // A smaller pool is one buffer, and the removal of the large one takes all its entries
    uint8_t *small = mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (small == MAP_FAILED) abort();
    int n3 = g_npool; mtl_pool_add(small, 64 * MB); CHECK("one entry", g_npool == n3 + 1 && g_nbig == 1);
    CHECK("its buffer is the whole pool", look((uint64_t)(uintptr_t)small + 10, 16).length == 64 * MB);
    mtl_pool_del(mem); mtl_drain();
    CHECK("no large pool left", g_nbig == 0);
    CHECK("only the small pool's entry is left of those added", g_npool == pools0 + 1);
    CHECK("the small pool still answers, its entry moved", look((uint64_t)(uintptr_t)small + 20, 16).length == 64 * MB);
    CHECK("the large pool's addresses do not", !mtl_gpu_addr(base + 100, 4, &mb, &mo, &gpu));
    mtl_sub_begin(); id<MTLBuffer> new_map = gmem_map(); CHECK("pool table rebuilt", new_map && new_map != old_map); finish();
    id<MTLBuffer> log = [g_dev newBufferWithLength:GML_BYTES options:MTLResourceStorageModeShared];
    memset(log.contents, 0, GML_BYTES);
    ((uint32_t *)log.contents)[entry / 32] = 1u << (entry % 32);
    gml_read(log, old_map, -1, 0);
    CHECK("a completed bitmap resolves its entry in the original map", g_gml_n == 1 && g_gml_lo[0] == range[0] && g_gml_hi[0] == range[1]);
    mtl_pool_del(small); mtl_drain();
    printf("t_pool_chunks: %d checks, %d failed\n", checks, failed); return failed != 0;
}}
