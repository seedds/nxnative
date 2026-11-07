// sphole4 (2026-11-07, lockup-9afa34b0 / pre1): a SIZED lookup over a sparse range that is ALL MAPPED but in several physical pieces (PRAGMATA's b56b768f pixel shader binds
// 0x45e290000+0x3cbdab4, 61 MB, as a storage buffer with its size) found no pool and was bound to the shared 64 KB dummy: the shader read its header words as 0 and then read far past
// the dummy (BIF0 page fault, the GPU error at fence 24965 in run pre1). nvn_sparse_holes said 0 holes, so DM_NVN_SPARSE_SPAN_SIZED never looked at it, and the old lookup failed.
// This test builds such ranges several ways (64 KB pieces from scattered physical offsets; mapped in one call and in many; inside a large run >= 512 MB that the host keeps in chunks;
// remapped in place; crossing the boundary of two mapping calls in reverse order) and checks that every sized lookup gets a buffer that holds the whole range and reads the right pages.
// DM_NVN_SPARSE_SPAN_ALL=0 (sphole4's switch) shows the old behaviour. Also DM_NVN_BIND_CLAMP: a binding that still resolves to no pool gets a buffer of the binding's length (zeros), not 64 KB.
// Two fixes, each enough alone: DM_NVN_REDIR_RETRY (a physical redirect that finds no pool is looked up again over the virtual range) and DM_NVN_SPARSE_SPAN_ALL (a span buffer).
// Run: ./t_sparse_span3 ; DM_NVN_REDIR_RETRY=0 ./t_sparse_span3 ; DM_NVN_REDIR_RETRY=0 DM_NVN_SPARSE_SPAN_ALL=0 ./t_sparse_span3 (prints which cases fail the old way)
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
static int checks, failed, oldfail;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static pool_t g_phys, g_sparse, g_physA, g_physB;
static void map_at(uint64_t voff, uint64_t size, uint64_t poff, int unmap) {
    map_req r = {.phys = unmap ? NULL : (NVNmemoryPool *)&g_phys, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static void map_from(pool_t *pp, uint64_t voff, uint64_t size, uint64_t poff) {
    map_req r = {.phys = (NVNmemoryPool *)pp, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static void map_many(uint64_t voff, uint64_t n, const uint64_t *poffs, uint64_t piece) {   // n pieces in one request list
    map_req *r = calloc(n, sizeof *r);
    for (uint64_t i = 0; i < n; i++) r[i] = (map_req){.phys = (NVNmemoryPool *)&g_phys, .poff = (int64_t)poffs[i], .voff = (int64_t)(voff + i * piece), .size = piece};
    CHECK("mapping requests", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, (int)n, (const NVNmappingRequest *)r));
    free(r);
}
static id<MTLCommandQueue> g_q;
static int t3_gpu = 1;   // T3_NOGPU=1: CPU-side checks only (no command buffers)
static NSData *gpu_read(void *mb, uint64_t mo, uint64_t n) {
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb, dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:src sourceOffset:mo toBuffer:dst destinationOffset:0 size:n]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return [NSData dataWithBytes:dst.contents length:n];
}
static uint64_t blen(void *mb, uint64_t mo) { return mb ? (uint64_t)[(__bridge id<MTLBuffer>)mb length] - mo : 0; }
static uint8_t *g_pm; static uint8_t *g_base;
// expected bytes of the sparse range [voff, voff + n): what the page table maps there
static int matches(const uint8_t *g, uint64_t voff, uint64_t n) {
    const struct vpool *v = vpool_of(&g_sparse);
    for (uint64_t o = 0; o < n; ) { uint64_t at = voff + o, k = 0x1000 - (at & 0xFFF); if (k > n - o) k = n - o;
        const vpte *e = vpt_get(v, at & ~0xFFFull); if (!e || !e->phys) return 0;
        if (memcmp(g + o, e->phys + (at & 0xFFF), k)) return 0; o += k; }
    return 1;
}
static void one(const char *name, uint64_t voff, uint64_t len) {
    uint64_t a = (uint64_t)(uintptr_t)g_base + voff, k, f, l;
    int h = nvn_sparse_holes(a, len, &k, &f, &l);
    void *mb = NULL; uint64_t mo = 0, gpu = 0;
    int okold = mtl_gpu_addr_(a, len, &mb, &mo, &gpu); int oldbad = !okold || blen(mb, mo) < len;
    mb = NULL; mo = 0; gpu = 0; int ok = mtl_gpu_addr(a, len, &mb, &mo, &gpu);
    char lab[200];
    printf("  %-44s holes %d, old lookup %s, now %s (buffer holds %#llx of %#llx)\n", name, h, oldbad ? (okold ? "SHORT" : "FAILED") : "ok", ok && blen(mb, mo) >= len ? "ok" : "BAD", (unsigned long long)blen(mb, mo), (unsigned long long)len);
    if (oldbad) oldfail++;
    snprintf(lab, sizeof lab, "%s: all mapped", name); CHECK(lab, h == 0);
    snprintf(lab, sizeof lab, "%s: lookup succeeds and holds the range", name); CHECK(lab, ok && blen(mb, mo) >= len);
    if (ok && blen(mb, mo) >= len) {
        snprintf(lab, sizeof lab, "%s: CPU view is the guest address", name); CHECK(lab, (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo == (const uint8_t *)(uintptr_t)a || matches((const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo, voff, len));
        if (t3_gpu) { NSData *d = gpu_read(mb, mo, len); snprintf(lab, sizeof lab, "%s: GPU reads the mapped pages", name); CHECK(lab, matches(d.bytes, voff, len)); }
    }
}
int main(void) { @autoreleasepool {
    const char *e = getenv("T3_NOGPU"); t3_gpu = !(e && atoi(e));
    mtl_init(); g_q = [g_dev newCommandQueue];
    size_t plen = 640u << 20;   // unique physical pages inside every run (Metal refuses a zero-copy buffer that aliases its own pages)
    g_pm = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (g_pm == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)g_pm)[i] = (uint32_t)i * 2654435761u | 1u;
    pool_builder_t pb = {.mem = g_pm, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 2ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { P = 0x10000 };
    const uint64_t LEN = 0x3cbdab4;   // pre1's binding
    uint64_t np = (64u << 20) / P, npp = plen / P, *perm = malloc(npp * sizeof *perm);
    for (uint64_t i = 0; i < npp; i++) perm[i] = i * P;
    for (uint64_t i = npp - 1; i > 0; i--) { uint64_t j = (i * 2654435761u) % (i + 1), t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
    // large runs: 1 MB pieces (a zero-copy buffer over ~8,000 separately remapped 64 KB VM regions of 500 MB is refused by Metal: the host then ends the process, see the write-up)
    uint64_t nq = (64u << 20) / (16 * P), nqq = plen / (16 * P), *permq = malloc(nqq * sizeof *permq);
    for (uint64_t i = 0; i < nqq; i++) permq[i] = i * 16 * P;
    for (uint64_t i = nqq - 1; i > 0; i--) { uint64_t j = (i * 40503u + 7) % (i + 1), x = permq[i]; permq[i] = permq[j]; permq[j] = x; }
    map_at(0, P, 0, 0);
    const struct vpool *v = vpool_of(&g_sparse); if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    g_base = v->base;
    // A. 64 MB in 64 KB pieces from scattered physical offsets, one request list
    uint64_t va = 64ull << 20; map_many(va, np, perm, P);
    one("A scattered pieces, one request", va + 0x290000 - 0x290000, LEN);
    one("A' same, at +0x290000", va + 0x290000, LEN - 0x290000);
    // B. the same, mapped piece by piece in reverse order (many separate vrun_set merges)
    uint64_t vb = 192ull << 20; for (uint64_t i = np; i-- > 0;) map_at(vb + i * P, P, perm[np + (i * 7) % np], 0);
    one("B scattered pieces, mapped in reverse", vb, LEN);
    // C. a run of 512 MB or more (the host keeps it in 16 MB chunks), the range in its middle
    uint64_t vc = 512ull << 20;
    for (uint64_t c = 0; c < 9; c++) map_many(vc + c * (64u << 20), nq, permq + c * nq, 16 * P);   // 576 MB
    one("C inside a large run (chunks)", vc + (300ull << 20) + 0x290000, LEN);
    one("C' large run, crossing a 16 MB chunk start", vc + (31ull << 20), LEN);
    // D. a piece of A remapped in place (the run's entry made again)
    map_at(va + 5 * P, P, perm[17], 0);
    one("D after a remap inside the range", va, LEN);
    // E. the range is the join of two runs mapped separately (second one first)
    uint64_t ve = 1200ull << 20; map_many(ve + (32u << 20), np / 2, perm, P); map_many(ve, np / 2, perm + np / 2, P);
    one("E two runs joined", ve + 0x10000, LEN);
    // F. a large run grown at its end (mtl_pool_extend) by the range's pages
    uint64_t vf = 1300ull << 20; for (uint64_t c = 0; c < 8; c++) map_many(vf + c * (64u << 20), nq, permq + c * nq, 16 * P);   // 512 MB: a large pool
    map_many(vf + (512ull << 20), nq, permq + 8 * nq, 16 * P);   // grown by 64 MB
    one("F large run grown at its end, range in the new part", vf + (512ull << 20) - 0x100000, LEN);
    // H. ONE contiguous host range that is the end of one physical pool and the start of the next (pools carved from one guest heap sit back to back): the page-table walk sees one
    //    physical piece and redirects the lookup to it, but no physical pool holds all of it
    { size_t hl = 128u << 20; uint8_t *h = mmap(NULL, hl, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (h == MAP_FAILED) abort();
      for (size_t i = 0; i < hl / 4; i++) ((uint32_t *)h)[i] = (uint32_t)i * 2246822519u | 1u;
      pool_builder_t q = {.mem = h, .size = 64u << 20, .flags = 0x24}; CHECK("physical pool A", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_physA, (const NVNmemoryPoolBuilder *)&q));
      q.mem = h + (64u << 20); CHECK("physical pool B", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_physB, (const NVNmemoryPoolBuilder *)&q));
      uint64_t vh = 1100ull << 20; map_from(&g_physA, vh, 32u << 20, 32u << 20); map_from(&g_physB, vh + (32u << 20), 32u << 20, 0);
      one("H one host range across two physical pools", vh, LEN); }
    // G. DM_NVN_BIND_CLAMP: a binding over nothing mapped gets a buffer of its length (zeros), not the 64 KB dummy
    { uint64_t n = 0x3cbdab4; id<MTLBuffer> b = bind_clamp_buf(n);
      CHECK("bind clamp: a zero buffer at least the binding's length", b && b.length >= n);
      if (b && t3_gpu) { NSData *d = gpu_read((__bridge void *)b, n - 0x1000, 0x1000); int z = 1; for (int i = 0; i < 0x1000; i++) if (((const uint8_t *)d.bytes)[i]) z = 0; CHECK("bind clamp: reads zeros at its end", z); }
      CHECK("bind clamp: reused", bind_clamp_buf(0x1000) == b); }
    printf("%s: %d checks, %d failed; %d of the cases failed or came back short through the old lookup (DM_NVN_SPARSE_SPAN_ALL=%d)\n", failed ? "FAIL" : "ok", checks, failed, oldfail, span_all_on());
    return failed != 0;
} }
