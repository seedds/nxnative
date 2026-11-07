// Buffers in sparse virtual pools (hle_nvn.c vrun_set, nvn_sparse_phys; nvn_mtl.m sparse_redirect): more than 1024 separately mapped runs all stay addressable; a lookup of a
// multi-page buffer whose mapped part is one contiguous physical range resolves to the physical pool's own buffer at that offset (a zero-copy buffer over the remapped virtual
// range does not see the physical pages on the Mac: PRAGMATA's title Earth read zeros); a lookup across pieces from different physical places is not redirected to one, but
// resolves to the buffer of its mapped run, which the GPU reads both pieces through (PRAGMATA's title terrain).
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
static pool_t g_phys, g_sparse;
static void map_at(uint64_t voff, uint64_t size, uint64_t poff) {
    map_req r = {.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
int main(void) { @autoreleasepool {
    mtl_init();
    size_t plen = 64u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u;
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    // 1. 1500 separate 64 KB runs (a hole after each), as PRAGMATA maps its 1 GB pool: every one stays addressable (the old 1024-run table lost the runs merged into a full one)
    enum { N = 1500, PIECE = 0x10000 };
    map_at(0, PIECE, 0);   // the first mapping makes the pool sparse
    for (int k = 1; k < N; k++) map_at((uint64_t)k * 2 * PIECE, PIECE, ((uint64_t)k * PIECE) % (48u << 20));
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (v) { CHECK("all runs tracked", v->nrun == N);
        int bad = 0; for (int k = 0; k < N; k++) { void *mb; uint64_t mo; uint64_t a = (uint64_t)(uintptr_t)v->base + (uint64_t)k * 2 * PIECE + 16;
            if (!mtl_addr(a, 4, &mb, &mo)) { if (bad++ < 3) fprintf(stderr, "run %d: no lookup\n", k); continue; }
            uint32_t got = *(const uint32_t *)((const uint8_t *)((__bridge id<MTLBuffer>)mb).contents + mo), want = *(const uint32_t *)(phys + ((uint64_t)k * PIECE) % (48u << 20) + 16), cpu = *(const uint32_t *)(uintptr_t)a;
            if (got != want) { if (bad++ < 3) fprintf(stderr, "run %d: buffer %p+%#llx len %#lx reads %08x, cpu %08x, want %08x\n", k, ((__bridge id<MTLBuffer>)mb).contents, (unsigned long long)mo, (unsigned long)((__bridge id<MTLBuffer>)mb).length, got, cpu, want); } }
        CHECK("every run resolves to its pages", bad == 0); }
    // 2. A 4 MB buffer mapped contiguously from one physical range, followed by unmapped pages: a 16 MB lookup gets the physical pool's buffer at the right offset
    uint64_t vo = 512ull << 20, po = 8ull << 20; map_at(vo, 4u << 20, po);
    if (v) { uint64_t a = (uint64_t)(uintptr_t)v->base + vo, mp = 0;
        CHECK("contiguous mapping redirected", nvn_sparse_phys(a, 16u << 20, &mp) == (uint64_t)(uintptr_t)(phys + po) && mp == (4u << 20));
        void *mb; uint64_t mo, gpu;
        CHECK("lookup succeeds", mtl_gpu_addr(a, 16u << 20, &mb, &mo, &gpu));
        const uint8_t *c = (const uint8_t *)((__bridge id<MTLBuffer>)mb).contents + mo;
        CHECK("the buffer is the physical pool's", c == phys + po);
        CHECK("and reads its data", !memcmp(c + 0x123450, phys + po + 0x123450, 64)); }
    // 3. Two pieces from different physical places: not redirected
    uint64_t vo2 = 600ull << 20; map_at(vo2, 1u << 20, 16u << 20); map_at(vo2 + (1u << 20), 1u << 20, 40u << 20);
    if (v) { uint64_t mp = 0; CHECK("non-contiguous mapping not redirected", nvn_sparse_phys((uint64_t)(uintptr_t)v->base + vo2, 2u << 20, &mp) == 0); }
    // 4. A 16 MB lookup over those two pieces and the unmapped pages after them (PRAGMATA's terrain instance buffer: 80 MB, 448 KB mapped in two pieces) resolves to the buffer
    // of their mapped run, and the GPU reads both pieces through it. A copy on the GPU, as the CPU-side view of a zero-copy buffer is not what failed on the Mac
    if (v) { uint64_t a = (uint64_t)(uintptr_t)v->base + vo2, ph = 1, rk = 0; void *mb = NULL; uint64_t mo = 0, gpu = 0;
        CHECK("virtual run of two pieces", nvn_sparse_vrun(a, 16u << 20, &ph, &rk) == (2u << 20) && ph == 0 && rk == a);
        CHECK("lookup succeeds", mtl_gpu_addr(a, 16u << 20, &mb, &mo, &gpu));
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb;
        CHECK("the run's own buffer", src && (const uint8_t *)src.contents + mo == (const uint8_t *)(uintptr_t)a && src.length - mo >= (2u << 20));
        if (src) { id<MTLBuffer> dst = [g_dev newBufferWithLength:2u << 20 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> q = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromBuffer:src sourceOffset:mo toBuffer:dst destinationOffset:0 size:2u << 20]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
            const uint8_t *d = dst.contents;
            CHECK("the GPU reads the first piece", !memcmp(d, phys + (16u << 20), 1u << 20));
            CHECK("the GPU reads the second piece", !memcmp(d + (1u << 20), phys + (40u << 20), 1u << 20)); } }
    // 5. A piece mapped after a hole: no virtual run (the part past the hole would be out of the run's buffer)
    if (v) { uint64_t vo3 = 700ull << 20; map_at(vo3, 1u << 20, 20u << 20); map_at(vo3 + (2u << 20), 1u << 20, 44u << 20);
        uint64_t ph = 0, rk = 0; CHECK("no run across a hole", nvn_sparse_vrun((uint64_t)(uintptr_t)v->base + vo3, 4u << 20, &ph, &rk) == 0 && rk == 0); }
    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "ok", checks, failed);
    return failed != 0;
} }
