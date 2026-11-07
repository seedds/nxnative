// DM_NVN_SPARSE_SPAN (hle_nvn.c nvn_sparse_span / nvn_sparse_span_buf, nvn_mtl.m sbufu_span): a storage buffer bound by address alone in a sparse pool whose mapping has a hole
// gets one buffer over the whole span: the GPU reads the pages before the hole, zeros in the hole, and the pages after it (PRAGMATA's surfel colour buffer, 64 KB pieces with holes,
// read past the first run's buffer: the SDF GI flare). Remapping the hole gives a new buffer that sees the new pages; unmapping shrinks the span; the old buffers go through
// deferred release. 2026-11-07, agent 41eax
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
static void map_at(uint64_t voff, uint64_t size, uint64_t poff, int unmap) {
    map_req r = {.phys = unmap ? NULL : (NVNmemoryPool *)&g_phys, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static id<MTLCommandQueue> g_q;
static NSData *gpu_read(void *mb, uint64_t mo, uint64_t n) {   // what the GPU reads through the buffer
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb, dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:src sourceOffset:mo toBuffer:dst destinationOffset:0 size:n]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return [NSData dataWithBytes:dst.contents length:n];
}
static int is_zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
int main(void) { @autoreleasepool {
    mtl_init(); g_q = [g_dev newCommandQueue];
    size_t plen = 64u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u | 1u;   // never 0
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { P = 0x10000 };
    uint64_t vo = 256ull << 20;
    map_at(0, P, 0, 0);   // the first mapping makes the pool sparse
    // a buffer of 4 pieces: [0, 64K) from 1 MB, hole [64K, 128K), [128K, 256K) from 9 MB
    map_at(vo, P, 1u << 20, 0); map_at(vo + 2 * P, 2 * P, 9u << 20, 0);
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    uint64_t a = (uint64_t)(uintptr_t)v->base + vo;
    // 1. the old lookup: the binding is the first run's buffer, shorter than the span (the bug)
    { void *mb; uint64_t mo, gpu; CHECK("old lookup succeeds", mtl_gpu_addr(a, 1, &mb, &mo, &gpu));
      CHECK("old lookup: the buffer ends at the hole", [(__bridge id<MTLBuffer>)mb length] - mo == P); }
    // 2. the span: 256 KB, one hole
    uint64_t key = 0; int holes = 0;
    CHECK("span to the last mapped page", nvn_sparse_span(a, 16u << 20, &key, &holes) == 4 * P && holes == 16);
    void *mb = NULL; uint64_t mo = 0, gpu = 0;
    CHECK("span lookup succeeds", sbufu_span(a, &mb, &mo, &gpu));
    CHECK("span buffer holds all of it", mb && [(__bridge id<MTLBuffer>)mb length] - mo >= 4 * P && gpu == [(__bridge id<MTLBuffer>)mb gpuAddress] + mo);
    if (mb) { NSData *d = gpu_read(mb, mo, 4 * P); const uint8_t *g = d.bytes;
        CHECK("GPU reads the first piece", !memcmp(g, phys + (1u << 20), P));
        CHECK("GPU reads zeros in the hole", is_zero(g + P, P));
        CHECK("GPU reads the piece after the hole", !memcmp(g + 2 * P, phys + (9u << 20), 2 * P)); }
    CHECK("the CPU reads the hole as zero", is_zero((const uint8_t *)(uintptr_t)(a + P), P));
    // 3. cached: the same buffer again
    { void *mb2; uint64_t mo2, g2; CHECK("second lookup", sbufu_span(a, &mb2, &mo2, &g2) && mb2 == mb); CHECK("one buffer made", g_span_made == 1); }
    // 4. remap the hole from 20 MB: a new buffer that reads the new pages
    map_at(vo + P, P, 20u << 20, 0);
    { void *mb3; uint64_t mo3, g3; CHECK("lookup after remap", sbufu_span(a, &mb3, &mo3, &g3));
      // no hole left: the plain lookup over the span (the run's own buffer) or a span buffer, either holds all of it
      CHECK("after remap the buffer holds the span", mb3 && [(__bridge id<MTLBuffer>)mb3 length] - mo3 >= 4 * P);
      if (mb3) { NSData *d = gpu_read(mb3, mo3, 4 * P); const uint8_t *g = d.bytes;
          CHECK("GPU reads the new middle piece", !memcmp(g + P, phys + (20u << 20), P));
          CHECK("and the first", !memcmp(g, phys + (1u << 20), P)); CHECK("and the last", !memcmp(g + 2 * P, phys + (9u << 20), 2 * P)); } }
    // 5. unmap the middle again (a hole of kept pages, DM_NVN_SPARSE_SAFE) and the end: the span shrinks to the first piece... plus nothing after
    map_at(vo + P, P, 0, 1); map_at(vo + 2 * P, 2 * P, 0, 1);
    { uint64_t k2 = 0; int h2 = 0; CHECK("span after unmaps", nvn_sparse_span(a, 16u << 20, &k2, &h2) == P && h2 == 0);
      void *mb4; uint64_t mo4, g4; CHECK("lookup after unmaps", sbufu_span(a, &mb4, &mo4, &g4) && [(__bridge id<MTLBuffer>)mb4 length] - mo4 >= P); }
    // 6. a hole whose pages were released (inaccessible) gets zero pages: map far after, unmap middle, let the deferred unmap release, then bind across it
    uint64_t vb = 512ull << 20; uint64_t b = (uint64_t)(uintptr_t)v->base + vb;
    map_at(vb, P, 2u << 20, 0); map_at(vb + 4 * P, P, 3u << 20, 0);   // hole [64K, 256K) never mapped: inaccessible
    { uint64_t k3 = 0; int h3 = 0; CHECK("span over a never-mapped hole", nvn_sparse_span(b, 16u << 20, &k3, &h3) == 5 * P && h3 == 48);
      void *mb5; uint64_t mo5, g5; CHECK("span buffer over an inaccessible hole", sbufu_span(b, &mb5, &mo5, &g5));
      if (mb5) { NSData *d = gpu_read(mb5, mo5, 5 * P); const uint8_t *g = d.bytes;
          CHECK("GPU: first piece", !memcmp(g, phys + (2u << 20), P)); CHECK("GPU: zero hole", is_zero(g + P, 3 * P)); CHECK("GPU: last piece", !memcmp(g + 4 * P, phys + (3u << 20), P)); }
      // the hole is mapped later: the page is overwritten (the mapping waits for the GPU: the hole was marked live), the next lookup sees it
      map_at(vb + 2 * P, P, 4u << 20, 0);
      void *mb6; uint64_t mo6, g6; CHECK("lookup after mapping into the hole", sbufu_span(b, &mb6, &mo6, &g6) && mb6 != mb5);
      if (mb6) { NSData *d = gpu_read(mb6, mo6, 5 * P); CHECK("GPU reads the newly mapped page", !memcmp((const uint8_t *)d.bytes + 2 * P, phys + (4u << 20), P)); } }
    printf("%s: %d checks, %d failed (span buffers made %llu, reused %llu, plain %llu)\n", failed ? "FAIL" : "ok", checks, failed, (unsigned long long)g_span_made, (unsigned long long)g_span_hits, (unsigned long long)g_span_plain);
    return failed != 0;
} }
