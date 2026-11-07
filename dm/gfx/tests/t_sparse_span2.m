// sphole3 (2026-11-07, sparse audit): the other sparse-pool resolutions besides the address-only storage binding (t_sparse_span).
// DM_NVN_SPARSE_SPAN_SIZED: a sized lookup (uniform / storage buffer with its size, vertex / index / indirect buffer, copy, fill, update) over a range with an interior hole got the mapped
// piece holding its first byte, or no pool at all; now a span buffer over the whole range (holes read 0). A range with holes only at its end keeps the old lookup, unless that failed or came
// back short (then a span too). DM_NVN_SPARSE_SPAN_EXT: an SBUFU span capped at 16 MB with pages mapped on is walked on (up to 64 MB). nvn_sparse_holes outside sparse pools, the write
// sink, and the watchdog's ring. Run: ./t_sparse_span2 (default switches), DM_NVN_SPARSE_SPAN_SIZED=0 ./t_sparse_span2 (the old behaviour: the bug cases fail as expected)
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
static uint64_t blen(void *mb, uint64_t mo) { return mb ? (uint64_t)[(__bridge id<MTLBuffer>)mb length] - mo : 0; }
int main(void) { @autoreleasepool {
    const char *se = getenv("DM_NVN_SPARSE_SPAN_SIZED"); int sized = !se ? 1 : atoi(se);
    mtl_init(); g_q = [g_dev newCommandQueue];
    size_t plen = 64u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u | 1u;   // never 0
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { P = 0x10000 };
    map_at(0, P, 0, 0);   // the first mapping makes the pool sparse
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    uint8_t *base = v->base;
    // 0. nvn_sparse_holes: outside sparse pools -1; all mapped 0
    { uint64_t k, f, l; CHECK("holes: physical pool address is not sparse", nvn_sparse_holes((uint64_t)(uintptr_t)phys + 4096, 4096, &k, &f, &l) == -1);
      CHECK("holes: mapped page has none", nvn_sparse_holes((uint64_t)(uintptr_t)base + 0x100, 0x1000, &k, &f, &l) == 0 && l == 0x1000); }
    // 1. an interior hole, unaligned start: [0, 64K) from 1 MB, hole [64K, 128K), [128K, 256K) from 9 MB; the range [0x100, 256K - 0x100)
    uint64_t vo = 256ull << 20; map_at(vo, P, 1u << 20, 0); map_at(vo + 2 * P, 2 * P, 9u << 20, 0);
    uint64_t a = (uint64_t)(uintptr_t)base + vo + 0x100, len = 4 * P - 0x200;
    { uint64_t k, f, l; int h = nvn_sparse_holes(a, len, &k, &f, &l);
      CHECK("holes: 16 in the interior", h == 16 && f == P - 0x100 && l == len); }
    { void *mb = NULL; uint64_t mo = 0, gpu = 0; int ok = mtl_gpu_addr_(a, len, &mb, &mo, &gpu);
      CHECK("the old lookup fails or is short (the bug)", !ok || blen(mb, mo) < len); }
    { void *mb = NULL; uint64_t mo = 0, gpu = 0; int ok = mtl_gpu_addr(a, len, &mb, &mo, &gpu);
      if (sized) {
          CHECK("sized: lookup succeeds", ok);
          CHECK("sized: the buffer holds the whole range", ok && blen(mb, mo) >= len && gpu == [(__bridge id<MTLBuffer>)mb gpuAddress] + mo);
          CHECK("sized: CPU view is the guest address", ok && (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo == (const uint8_t *)(uintptr_t)a);
          if (ok) { NSData *d = gpu_read(mb, mo, len); const uint8_t *g = d.bytes;
              CHECK("sized: GPU reads the first piece", !memcmp(g, phys + (1u << 20) + 0x100, P - 0x100));
              CHECK("sized: GPU reads zeros in the hole", is_zero(g + P - 0x100, P));
              CHECK("sized: GPU reads the piece after the hole", !memcmp(g + 2 * P - 0x100, phys + (9u << 20), 2 * P - 0x100)); }
          void *mb2 = NULL; uint64_t mo2 = 0, g2 = 0; CHECK("sized: cached", mtl_gpu_addr(a, len, &mb2, &mo2, &g2) && mb2 == mb && g_sz_made == 1);
          void *mb3 = NULL; uint64_t mo3 = 0; CHECK("sized: the CPU-side lookup gets it too", mtl_addr(a, len, &mb3, &mo3) && mb3 == mb);
          // the hole remapped from 20 MB: a new buffer that reads the new pages
          map_at(vo + P, P, 20u << 20, 0);
          void *mb4 = NULL; uint64_t mo4 = 0, g4 = 0; CHECK("sized: lookup after the remap", mtl_gpu_addr(a, len, &mb4, &mo4, &g4) && blen(mb4, mo4) >= len);
          if (mb4) { NSData *d = gpu_read(mb4, mo4, len); CHECK("sized: GPU reads the newly mapped middle", !memcmp((const uint8_t *)d.bytes + P - 0x100, phys + (20u << 20), P)); }
          map_at(vo + P, P, 0, 1);   // the hole again
      } else CHECK("off: as before, failed or short", !ok || blen(mb, mo) < len);
      CHECK("the old-way counters or the span counters moved", sized ? g_sz_holes >= 1 : g_sz_old_fail + g_sz_old_short >= 1); }
    // 2. holes only at the end, one physical piece: the old lookup (the physical pool's buffer) as in t_sparse_buf 2
    uint64_t vb = 384ull << 20; map_at(vb, 4 * P, 2u << 20, 0);
    { uint64_t b = (uint64_t)(uintptr_t)base + vb; void *mb = NULL; uint64_t mo = 0, gpu = 0; uint64_t t0 = g_sz_tail;
      CHECK("tail: lookup succeeds", mtl_gpu_addr(b, 16 * P, &mb, &mo, &gpu));
      CHECK("tail: the physical pool's buffer, as before", mb && (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo == phys + (2u << 20));
      CHECK("tail: counted apart", g_sz_tail == t0 + 1); }
    // 3. a small range (a uniform block) straddling the end of a run into an unmapped page, nothing mapped after: the old lookup fails (the zero bank); now a span (force)
    uint64_t vc = 448ull << 20; map_at(vc, P, 3u << 20, 0);
    { uint64_t c = (uint64_t)(uintptr_t)base + vc + P - 0x80; void *mb = NULL; uint64_t mo = 0, gpu = 0;
      CHECK("small straddle: the old lookup fails (the bug)", !mtl_gpu_addr_(c, 0x100, &mb, &mo, &gpu));
      int ok = mtl_gpu_addr(c, 0x100, &mb, &mo, &gpu);
      if (sized) { CHECK("small straddle: resolved", ok && blen(mb, mo) >= 0x100 && g_sz_tail_fixed >= 1);
          if (ok) { NSData *d = gpu_read(mb, mo, 0x100); CHECK("small straddle: data then zeros", !memcmp(d.bytes, phys + (3u << 20) + P - 0x80, 0x80) && is_zero((const uint8_t *)d.bytes + 0x80, 0x80)); } }
      else CHECK("off: small straddle fails as before", !ok); }
    // 4. a run buffer ending at a tail hole (two physical pieces, then nothing): old lookup short → span (force)
    uint64_t vd = 512ull << 20; map_at(vd, P, 4u << 20, 0); map_at(vd + P, P, 30u << 20, 0);
    { uint64_t d0 = (uint64_t)(uintptr_t)base + vd; void *mb = NULL; uint64_t mo = 0, gpu = 0;
      int ok0 = mtl_gpu_addr_(d0, 8 * P, &mb, &mo, &gpu); CHECK("run tail: the old lookup is short (the bug)", !ok0 || blen(mb, mo) < 8 * P);
      int ok = mtl_gpu_addr(d0, 8 * P, &mb, &mo, &gpu);
      if (sized) { CHECK("run tail: the buffer holds the range", ok && blen(mb, mo) >= 8 * P);
          if (ok) { NSData *d = gpu_read(mb, mo, 8 * P); const uint8_t *g = d.bytes;
              CHECK("run tail: both pieces, then zeros", !memcmp(g, phys + (4u << 20), P) && !memcmp(g + P, phys + (30u << 20), P) && is_zero(g + 2 * P, 6 * P)); } } }
    // 5. a range with nothing mapped: the old fallback (fails), no span
    { uint64_t e0 = (uint64_t)(uintptr_t)base + (600ull << 20); void *mb = NULL; uint64_t mo = 0, gpu = 0; uint64_t m0 = g_sz_made;
      CHECK("nothing mapped: fails as before", !mtl_gpu_addr(e0, 0x1000, &mb, &mo, &gpu) && g_sz_made == m0); }
    // 6. SBUFU: a span capped at 16 MB with pages mapped on (a hole near the start) is walked on to the last mapped page (DM_NVN_SPARSE_SPAN_EXT)
    uint64_t vf = 640ull << 20; map_at(vf, P, 24u << 20, 0); map_at(vf + 2 * P, 20u << 20, 32u << 20, 0);
    { uint64_t f0 = (uint64_t)(uintptr_t)base + vf, k = 0; int h = 0, c = 0;
      CHECK("span2: capped at 16 MB", nvn_sparse_span2(f0, 16u << 20, &k, &h, &c) == (16u << 20) && c == 1 && h == 16);
      CHECK("span2: walked on, not capped", nvn_sparse_span2(f0, 64u << 20, &k, &h, &c) == 2 * P + (20u << 20) && c == 0);
      void *mb = NULL; uint64_t mo = 0, gpu = 0;
      CHECK("sbufu ext: span buffer over all of it", sbufu_span(f0, &mb, &mo, &gpu) && blen(mb, mo) >= 2 * P + (20u << 20));
      if (mb && blen(mb, mo) >= 2 * P + (20u << 20)) { NSData *d = gpu_read(mb, mo + 2 * P + (20u << 20) - P, P); CHECK("sbufu ext: GPU reads the last 64 KB", !memcmp(d.bytes, phys + (32u << 20) + (20u << 20) - P, P)); } }
    // 7. the write sink and the ring
    CHECK("write sink is 32 MB", wsink().length == (32u << 20) && wsink_gpu() == wsink().gpuAddress);
    { int m = 0; uint64_t n = atomic_load(&g_srr_n); for (uint64_t i = 0; i < n && i < SRR_N; i++) if (g_srr[i].out == (sized ? 'M' : 'F') || g_srr[i].out == 'T') m = 1;
      CHECK("the watchdog's ring has the resolutions", m); }
    printf("%s: %d checks, %d failed (DM_NVN_SPARSE_SPAN_SIZED=%d; sized spans made %llu reused %llu; old short %llu failed %llu; tails %llu fixed %llu; SBUFU spans walked on %llu)\n", failed ? "FAIL" : "ok", checks, failed, sized,
           (unsigned long long)g_sz_made, (unsigned long long)g_sz_hits, (unsigned long long)g_sz_old_short, (unsigned long long)g_sz_old_fail, (unsigned long long)g_sz_tail, (unsigned long long)g_sz_tail_fixed, (unsigned long long)g_span_ext);
    return failed != 0;
} }
