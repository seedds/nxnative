// A storage buffer bound by its address alone (NVN_RB_SBUFU / NVN_RB_SBUFQ: nvn_mtl.m sbuf_addr) in a sparse pool whose mapping has a hole after its first run: the binding covers
// the whole span, and the GPU reads the pages before the hole, zeros in it and the pages after it, through the real NVN entry points. Remapping the hole gives a buffer that sees the
// new pages; an unmap retires the span buffers over its pages at once; a hole whose unmap is still queued gets no span until it is released, then reads 0; a hole whose host pages
// are inaccessible is backed by zero pages; an address in a run with nothing after it gets the zero tail (DM_NVN_SPARSE_SPAN_TAIL_MB=0: resolves as before); the pool's Finalize
// takes its span buffers out of the residency set.
// Before, the binding was the buffer of the first run alone, which ends at the hole
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
static void map_at(uint64_t voff, uint64_t size, uint64_t poff, int mapped) {
    map_req r = {.phys = mapped ? (NVNmemoryPool *)&g_phys : NULL, .poff = (int64_t)poff, .voff = (int64_t)voff, .size = size};
    CHECK("mapping request", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r));
}
static uint64_t blen(void *mb, uint64_t mo) { return mb ? (uint64_t)[(__bridge id<MTLBuffer>)mb length] - mo : 0; }
// What the GPU reads through the bound buffer, n bytes from the binding (NULL when the buffer is shorter: reading past it is what went wrong)
static NSData *gpu_read(void *mb, uint64_t mo, uint64_t n) {
    if (blen(mb, mo) < n) return nil;
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb, dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!dst) return nil;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:src sourceOffset:mo toBuffer:dst destinationOffset:0 size:n]; [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted ? [NSData dataWithBytes:dst.contents length:n] : nil;
}
// Whether a retired buffer is still in the residency set once its deferred release has had a second to drain (defer_kick drains on another queue)
static int left_resident(id<MTLBuffer> b) {
    for (int i = 0; i < 100; i++) { pthread_mutex_lock(&g_res_mu); int in = [g_res_objects containsObject:b]; pthread_mutex_unlock(&g_res_mu); if (!in) return 0; usleep(10000); }
    return 1;
}
static int is_zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
int main(void) { @autoreleasepool {
    mtl_init();
    size_t plen = 64u << 20;
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u | 1u;   // never 0
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    enum { P = 0x10000 };
    map_at(0, P, 0, 1);   // the first mapping, not the whole pool: the pool is sparse
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (!v) { printf("FAIL: no sparse pool\n"); return 1; }
    // 1. a buffer of 256 KB: [0, 64K) from 1 MB, a hole [64K, 128K) never mapped, [128K, 256K) from 9 MB
    uint64_t vo = 256ull << 20, a = (uint64_t)(uintptr_t)v->base + vo;
    map_at(vo, P, 1u << 20, 1); map_at(vo + 2 * P, 2 * P, 9u << 20, 1);
    void *mb = NULL; uint64_t mo = 0, gpu = 0;
    CHECK("binding resolves", sbuf_addr(a, &mb, &mo, &gpu));
    CHECK("the bound buffer covers the whole range", blen(mb, mo) >= 4 * P);
    CHECK("its GPU address is the binding's", mb && gpu == [(__bridge id<MTLBuffer>)mb gpuAddress] + mo);
    { NSData *d = gpu_read(mb, mo, 4 * P); const uint8_t *g = d.bytes; CHECK("GPU reads the whole range", d != nil);
      if (d) { CHECK("GPU reads the piece before the hole", !memcmp(g, phys + (1u << 20), P));
               CHECK("GPU reads zeros in the hole", is_zero(g + P, P));
               CHECK("GPU reads the piece after the hole", !memcmp(g + 2 * P, phys + (9u << 20), 2 * P)); } }
    // 2. bound again with no change: the same buffer
    { void *mb2 = NULL; uint64_t mo2, g2; CHECK("bound again: the same buffer", sbuf_addr(a, &mb2, &mo2, &g2) && mb2 == mb && mo2 == mo); }
    // 3. the hole mapped from 20 MB: the binding sees the new pages
    map_at(vo + P, P, 20u << 20, 1);
    { void *mb3 = NULL; uint64_t mo3, g3; CHECK("binding after the remap", sbuf_addr(a, &mb3, &mo3, &g3) && blen(mb3, mo3) >= 4 * P);
      NSData *d = gpu_read(mb3, mo3, 4 * P); const uint8_t *g = d.bytes; CHECK("GPU reads the range after the remap", d != nil);
      if (d) { CHECK("GPU reads the newly mapped page", !memcmp(g + P, phys + (20u << 20), P));
               CHECK("and the pieces around it", !memcmp(g, phys + (1u << 20), P) && !memcmp(g + 2 * P, phys + (9u << 20), 2 * P)); } }
    // 3b. the game unmaps the run after a hole: the span buffer over it is retired at once, out of the cache and (no submission open) out of the residency set, before the
    //     unmap's pages are released; before, it stayed cached and resident until the address was bound again or evicted
    uint64_t ve = 704ull << 20, ee = (uint64_t)(uintptr_t)v->base + ve;
    map_at(ve, P, 10u << 20, 1); map_at(ve + 2 * P, 2 * P, 11u << 20, 1);
    { void *m = NULL; uint64_t mo_, g_; CHECK("a span before the unmap", sbuf_addr(ee, &m, &mo_, &g_) && blen(m, mo_) >= 4 * P);
      id<MTLBuffer> held = (__bridge id<MTLBuffer>)m; uint64_t ret0 = g_span_retired;
      map_at(ve + 2 * P, 2 * P, 0, 0);
      int left = 0; for (int i = 0; i < SPAN_C; i++) left += g_span_c[i].buf == held;
      CHECK("the unmap retires the span buffer over its pages", !left && g_span_retired > ret0);
      CHECK("and takes it out of the residency set", held && left_resident(held) == 0);
      held = nil; }
    // 3c. a hole page whose unmap is still queued holds the old physical page: no span over it until the unmap has released it, then zeros there
    uint64_t vd = 640ull << 20, dd = (uint64_t)(uintptr_t)v->base + vd;
    map_at(vd, P, 6u << 20, 1); map_at(vd + P, P, 7u << 20, 1); map_at(vd + P, P, 0, 0); map_at(vd + 2 * P, P, 8u << 20, 1);
    { void *m = NULL; uint64_t mo_ = 0, g_; uint64_t pend0 = g_span_pending;
      CHECK("binding with the unmap queued", sbuf_addr(dd, &m, &mo_, &g_));
      if (blen(m, mo_) >= 3 * P) { NSData *d = gpu_read(m, mo_, 3 * P); CHECK("a span with the unmap queued reads zeros, not the old page", d && is_zero((const uint8_t *)d.bytes + P, P)); }
      else CHECK("no span while the unmap is queued: the first run alone", blen(m, mo_) == P && g_span_pending > pend0);
      mtl_sub_begin(); CHECK("a submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));   // the unmap's fence
      CHECK("binding once the unmap has released its pages", sbuf_addr(dd, &m, &mo_, &g_) && blen(m, mo_) >= 3 * P);
      NSData *d = gpu_read(m, mo_, 3 * P); const uint8_t *g = d.bytes;
      CHECK("GPU reads zeros in the released hole", d && is_zero(g + P, P));
      CHECK("and the runs around it", d && !memcmp(g, phys + (6u << 20), P) && !memcmp(g + 2 * P, phys + (8u << 20), P)); }
    // 4. a hole whose host pages are inaccessible (never mapped, 192 KB) gets zero pages; mapping into it later gives a buffer that sees the new page
    uint64_t vb = 512ull << 20, b = (uint64_t)(uintptr_t)v->base + vb;
    map_at(vb, P, 2u << 20, 1); map_at(vb + 4 * P, P, 3u << 20, 1);
    void *mb5 = NULL; uint64_t mo5 = 0, g5;
    CHECK("binding over an inaccessible hole", sbuf_addr(b, &mb5, &mo5, &g5) && blen(mb5, mo5) >= 5 * P);
    { NSData *d = gpu_read(mb5, mo5, 5 * P); const uint8_t *g = d.bytes; CHECK("GPU reads across the inaccessible hole", d != nil);
      if (d) { CHECK("GPU: first piece", !memcmp(g, phys + (2u << 20), P)); CHECK("GPU: zeros in the hole", is_zero(g + P, 3 * P)); CHECK("GPU: last piece", !memcmp(g + 4 * P, phys + (3u << 20), P)); } }
    if (blen(mb5, mo5) >= 5 * P) CHECK("the CPU reads the hole as zeros", is_zero((const uint8_t *)(uintptr_t)(b + P), 3 * P));   // inaccessible without the span buffer
    map_at(vb + 2 * P, P, 4u << 20, 1);
    { void *mb6 = NULL; uint64_t mo6, g6; CHECK("binding after mapping into the hole", sbuf_addr(b, &mb6, &mo6, &g6) && mb6 != mb5 && blen(mb6, mo6) >= 5 * P);
      NSData *d = gpu_read(mb6, mo6, 5 * P); CHECK("GPU reads the page mapped into the hole", d && !memcmp((const uint8_t *)d.bytes + 2 * P, phys + (4u << 20), P)); }
    // 5. a run with nothing mapped after it: with DM_NVN_SPARSE_SPAN_TAIL_MB=0, resolved as before, to the run's own buffer; by default a span buffer over the run and a zero
    //    tail (t_sparse_span_tail)
    uint64_t vc = 768ull << 20, c = (uint64_t)(uintptr_t)v->base + vc;
    map_at(vc, 2 * P, 5u << 20, 1);
    { void *m1 = NULL, *m2 = NULL; uint64_t o1 = 0, o2 = 0, g1, g2; const char *te = getenv("DM_NVN_SPARSE_SPAN_TAIL_MB");
      CHECK("a lone run resolves", sbuf_addr(c, &m1, &o1, &g1) && mtl_gpu_addr(c, 1, &m2, &o2, &g2));
      if (te && !atoi(te)) CHECK("to the same buffer as the plain lookup", m1 == m2 && o1 == o2 && blen(m1, o1) >= 2 * P);
      else { NSData *d = gpu_read(m1, o1, 3 * P); CHECK("to a span buffer: the run, then zeros", d && !memcmp(d.bytes, phys + (5u << 20), 2 * P) && is_zero((const uint8_t *)d.bytes + 2 * P, P)); } }
    // 6. the pool goes: its span buffers with it, out of the residency set before its pages go (no submission is open, so nothing can still reach them); before, they were
    //    released behind the next submission's fence, which a game that stops submitting never reaches
    id<MTLBuffer> held6 = nil; { void *m = NULL; uint64_t mo_, g_; if (sbuf_addr(b, &m, &mo_, &g_)) held6 = (__bridge id<MTLBuffer>)m; }
    CHECK("a span buffer before Finalize", held6 && [g_res_objects containsObject:held6]);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_sparse);
    int left = 0; for (int i = 0; i < SPAN_C; i++) left += g_span_c[i].buf != nil;
    CHECK("no span buffer left after Finalize", !left);
    CHECK("the span buffer is out of the residency set at Finalize", held6 && left_resident(held6) == 0);
    held6 = nil;
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&g_phys);
    printf("%s: %d checks, %d failed\n", failed ? "FAIL" : "ok", checks, failed);
    return failed != 0;
} }
