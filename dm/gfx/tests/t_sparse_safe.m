// DM_NVN_SPARSE_SAFE (nvn_mtl.m nocopy_buf, hle_nvn.c vpool_map): with it on (the default) a sparse pool's unmap keeps its pages (a zero-copy buffer made over them may still be
// resident or in submitted work: inaccessible pages under it were PRAGMATA's GPU page fault and kernel panic), a mapping over pages mapped before waits for the GPU first, and a
// zero-copy range with a hole or an unreadable page is refused before IOGPU sees it; write-protected pages are made writable first. CPU only: no command buffer is committed
// (mtl_finish with nothing submitted returns at once).
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
static int map_at(uint64_t voff, uint64_t size, int64_t poff);
static __weak id<MTLBuffer> t20_w[8]; static uint8_t *t20_base; static dispatch_semaphore_t t20_done, t20_end; static int t20_ok = 1;
static void *t20_body(void *arg) {   // case 20: a run grown a step at a time on a thread with no autorelease pool of its own
    (void)arg; uint64_t B = 800u << 20;
    for (int i = 0; i < 8; i++) { t20_ok &= map_at(B + (uint64_t)i * 0x10000, 0x10000, (14u << 20) + (uint64_t)i * 0x10000);
        void *mb = NULL; uint64_t mo = 0; t20_ok &= mtl_addr((uint64_t)(uintptr_t)(t20_base + B), 4, &mb, &mo); t20_w[i] = (__bridge id<MTLBuffer>)mb; }
    dispatch_semaphore_signal(t20_done); dispatch_semaphore_wait(t20_end, DISPATCH_TIME_FOREVER); return NULL;
}
static int map_at(uint64_t voff, uint64_t size, int64_t poff) {   // poff < 0: unmap
    map_req r = {.phys = poff < 0 ? NULL : (NVNmemoryPool *)&g_phys, .poff = poff < 0 ? 0 : poff, .voff = (int64_t)voff, .size = size};
    return h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&g_sparse, 1, (const NVNmappingRequest *)&r);
}
static int prot_at(const void *p) {   // the protection of the page at p, -1 if unmapped
    vm_address_t q = (vm_address_t)p; vm_size_t rs; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (vm_region_64(mach_task_self(), &q, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj) != KERN_SUCCESS || q > (vm_address_t)p) return -1;
    return vi.protection;
}
int main(void) { @autoreleasepool {
    mtl_init();
    mtl_pool_addressed((const void *)(uintptr_t)0x1000, 1ull << 47);   // the driver gave out addresses everywhere (pool_mark_addressed): the pool table reaches every pool here (addr_meets)
    CHECK("safe mode is the default", mtl_sparse_safe());
    size_t plen = 16u << 20, pg = (size_t)getpagesize();
    uint8_t *phys = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    for (size_t i = 0; i < plen / 4; i++) ((uint32_t *)phys)[i] = (uint32_t)i * 2654435761u;
    pool_builder_t pb = {.mem = phys, .size = plen, .flags = 0x24};
    CHECK("physical pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_phys, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size = 1ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    CHECK("sparse pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_sparse, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("first mapping", map_at(0, 1u << 20, 0));
    const struct vpool *v = vpool_of(&g_sparse); CHECK("pool is sparse", v != NULL);
    if (!v) { fprintf(stderr, "%d checks, %d failed\n", checks, failed); return 1; }
    uint8_t *base = v->base;
    // 1. a run, looked up (a buffer over it), then unmapped: the pages stay readable, the page table says unmapped
    CHECK("map a run", map_at(4u << 20, 1u << 20, 2u << 20));
    void *mb; uint64_t mo; CHECK("lookup of the run", mtl_addr((uint64_t)(uintptr_t)(base + (4u << 20)), 4, &mb, &mo));
    uint64_t kept0 = atomic_load(&g_vp_kept);
    CHECK("unmap", map_at(4u << 20, 1u << 20, -1));
    CHECK("unmap kept the pages", atomic_load(&g_vp_kept) == kept0 + 1);
    CHECK("pages still readable after the unmap", prot_at(base + (4u << 20)) > 0 && (prot_at(base + (4u << 20)) & VM_PROT_READ));
    CHECK("still the physical data", *(const uint32_t *)(base + (4u << 20) + 64) == *(const uint32_t *)(phys + (2u << 20) + 64));
    pthread_mutex_lock(&g_vp_mu); const vpte *e = vpt_get(v, 4u << 20); CHECK("page table: unmapped", e && !e->phys); pthread_mutex_unlock(&g_vp_mu);
    // 2. a mapping into pages never mapped does not wait; one over kept pages (their unmap's work not complete yet) does
    uint64_t w0 = atomic_load(&g_vp_waits);
    CHECK("map fresh pages", map_at(8u << 20, 1u << 20, 3u << 20));
    CHECK("fresh pages: no wait", atomic_load(&g_vp_waits) == w0);
    CHECK("map over the kept pages", map_at(4u << 20, 1u << 20, 5u << 20));
    CHECK("over kept pages: waited", atomic_load(&g_vp_waits) == w0 + 1);
    CHECK("new data there", *(const uint32_t *)(base + (4u << 20) + 64) == *(const uint32_t *)(phys + (5u << 20) + 64));
    // 2b. deferred unmap: once the work submitted before the unmap has completed, the pages become inaccessible as before, and a mapping over them no longer waits
    CHECK("unmap again", map_at(4u << 20, 1u << 20, -1));
    CHECK("queued", g_nvun >= 1);
    CHECK("still readable before the work completes", prot_at(base + (4u << 20)) > 0);
    // a run unmapped and mapped again before its work completes keeps the new mapping
    CHECK("map C", map_at(20u << 20, 1u << 20, 7u << 20)); CHECK("unmap C", map_at(20u << 20, 1u << 20, -1)); CHECK("map C again", map_at(20u << 20, 1u << 20, 8u << 20));
    atomic_store(&g_done, 1000);   // as if the GPU had completed everything submitted
    uint64_t rel0 = atomic_load(&g_vp_released);
    CHECK("another mapping runs the queue", map_at(16u << 20, 1u << 20, 9u << 20));
    CHECK("queue worked through", g_nvun == 0 && atomic_load(&g_vp_released) > rel0);
    CHECK("released pages inaccessible", prot_at(base + (4u << 20)) == 0);
    CHECK("remapped run untouched", prot_at(base + (20u << 20)) > 0 && *(const uint32_t *)(base + (20u << 20) + 64) == *(const uint32_t *)(phys + (8u << 20) + 64));
    uint64_t w1 = atomic_load(&g_vp_waits);
    CHECK("map over released pages", map_at(4u << 20, 1u << 20, 10u << 20));
    CHECK("released pages: no wait", atomic_load(&g_vp_waits) == w1);
    CHECK("readable again with the new data", *(const uint32_t *)(base + (4u << 20) + 64) == *(const uint32_t *)(phys + (10u << 20) + 64));
    // 3. zero-copy ranges: a hole is refused before IOGPU sees it, a readable range is made, write-protected pages are made writable first
    uint8_t *m = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (m == MAP_FAILED) abort();
    memset(m, 1, 4 * pg);
    CHECK("readable range accepted", nocopy_check((uintptr_t)m, (uintptr_t)m + 4 * pg, "test"));
    mprotect(m + pg, pg, PROT_NONE);
    uint64_t r0 = atomic_load(&g_nc_refused);
    CHECK("inaccessible page refused", !nocopy_check((uintptr_t)m, (uintptr_t)m + 4 * pg, "test") && atomic_load(&g_nc_refused) == r0 + 1);
    CHECK("nocopy_buf returns nil and says so", nocopy_buf((uintptr_t)m, (uintptr_t)m + 4 * pg, "test") == nil && t_nc_refused);
    munmap(m + 2 * pg, pg);
    mprotect(m + pg, pg, PROT_READ | PROT_WRITE);
    CHECK("hole refused", !nocopy_check((uintptr_t)m, (uintptr_t)m + 4 * pg, "test"));
    CHECK("range before the hole accepted", nocopy_check((uintptr_t)m, (uintptr_t)m + 2 * pg, "test"));
    // a tracked range write-protected by the dirty tracker: accepted, and writable afterwards
    uint8_t *t = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (t == MAP_FAILED) abort();
    memset(t, 2, 4 * pg); nvn_dirty_add_range(t, 4 * pg); nvn_dirty_protect(t, 4 * pg);
    CHECK("tracked pages are read-only", !(prot_at(t) & VM_PROT_WRITE));
    uint64_t u0 = atomic_load(&g_nc_unprot);
    id<MTLBuffer> b = nocopy_buf((uintptr_t)t, (uintptr_t)t + 4 * pg, "test");
    CHECK("write-protected range made", b != nil && atomic_load(&g_nc_unprot) == u0 + 1);
    CHECK("and writable now", (prot_at(t) & VM_PROT_WRITE) != 0);
    // 5. large pools in chunks: a lengthened chunk's old buffer is retired (deferred release), not kept resident in b->old; the whole-pool entry made for the pool table
    // retires the stride chunks, and every lookup then returns it
    { size_t blen = 512u << 20; uint8_t *bp = mmap(NULL, blen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (bp == MAP_FAILED) abort();
      mtl_pool_add(bp, blen);
      pthread_mutex_lock(&g_mu); int bg = big_of((uint64_t)(uintptr_t)bp); CHECK("pool in chunks", bg >= 0);
      if (bg >= 0) { big_pool *b = &g_big[bg];
          int i0 = big_lookup(b, (uint64_t)(uintptr_t)bp + 4096, 4); CHECK("chunk made", i0 >= 0);
          uint64_t r0 = g_big_retired; int npool0 = g_npool;
          int i1 = big_lookup(b, (uint64_t)(uintptr_t)bp + 4096, 200u << 20); CHECK("lengthened", i1 >= 0 && g_pools[i1].end >= (uintptr_t)bp + 4096 + (200u << 20));
          CHECK("old chunk retired, not kept", g_big_retired == r0 + 1 && b->old.count == 0 && g_npool == npool0);
          int i2 = big_lookup(b, (uint64_t)(uintptr_t)bp + (300u << 20), 4); CHECK("second stride chunk", i2 >= 0 && i2 != i1);
          pthread_mutex_unlock(&g_mu);
          uint64_t r1 = g_big_retired; (void)gmem_map_gpu();
          pthread_mutex_lock(&g_mu); b = &g_big[big_of((uint64_t)(uintptr_t)bp)];
          CHECK("no whole-pool entry: the table has its own segments", b->full == 0 && b->nseg == 8);
          int nseg_ok = 1; for (int q = 0; q < b->nseg; q++) nseg_ok &= b->seg[(NSUInteger)q] != (id)[NSNull null];
          CHECK("every segment made", nseg_ok);
          CHECK("the lookup chunks stay", g_big_retired == r1);
          { int c0 = b->chunk[0] - 1; id<MTLBuffer> cb = c0 >= 0 ? g_pools[c0].buf : nil;
            CHECK("a segment a lookup chunk covers takes its buffer", cb && b->seg[0] == cb && !b->segown[0] && b->segbase[0] == g_pools[c0].base && g_big_seg_taken > 0);
            int own = 0; for (int q = 0; q < b->nseg; q++) own += b->segown[q];
            CHECK("the others keep their own", own > 0 && own < b->nseg);
            uint64_t g0 = atomic_load(&g_pool_gen); big_chunk_retire(b, cb); [g_keep addObject:cb];   // as big_lookup's lengthening does (the entry stays here)
            CHECK("retiring that chunk clears the segment", b->seg[0] == (id)[NSNull null] && b->segend[0] == 0 && atomic_load(&g_pool_gen) > g0); } }
      pthread_mutex_unlock(&g_mu);
      CHECK("wired memory readable", sys_wired_bytes() > 0); }
    // 6. CPU upload staging is pooled: a buffer back from its batch is reused for the next upload of a similar size, and counted out of flight
    { id<MTLBuffer> a = cstg_get(3u << 20); CHECK("staging made", a != nil && a.length == (3u << 20) && cstg_owned(a));
      CHECK("in flight", atomic_load(&g_cstg_inflight) >= (3u << 20));
      uint64_t inf = atomic_load(&g_cstg_inflight); cstg_put(a); CHECK("back from flight", atomic_load(&g_cstg_inflight) == inf - (3u << 20));
      uint64_t r0 = g_cstg_reused; id<MTLBuffer> b2 = cstg_get((5u << 19)); CHECK("reused for a similar size", b2 == a && g_cstg_reused == r0 + 1);
      id<MTLBuffer> c = cstg_get(8u << 20); CHECK("a larger one is new", c != a); cstg_put(b2); cstg_put(c); }
    // 7. a sparse run of a large pool that grows at its end keeps its pool entry (mtl_pool_extend), and the pool table reaches it through its own segments, of which only the
    // new or lengthened last ones are made (PRAGMATA: 71 whole-pool buffers of one growing run, 43 GB)
    { static pool_t ph2, sp2; size_t l2 = 768u << 20; uint8_t *p2 = mmap(NULL, l2, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (p2 == MAP_FAILED) abort();
      pool_builder_t b2 = {.mem = p2, .size = l2, .flags = 0x24}; CHECK("physical pool 2", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&ph2, (const NVNmemoryPoolBuilder *)&b2));
      b2 = (pool_builder_t){.size = 2ull << 30, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL}; CHECK("sparse pool 2", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&sp2, (const NVNmemoryPoolBuilder *)&b2));
      map_req r2 = {.phys = (NVNmemoryPool *)&ph2, .poff = 0, .voff = 0, .size = 512u << 20};
      CHECK("map 512 MB", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sp2, 1, (const NVNmappingRequest *)&r2));
      const struct vpool *v2 = vpool_of(&sp2); uint8_t *b2b = v2 ? v2->base : NULL;
      pthread_mutex_lock(&g_mu); int bg = b2b ? big_of((uint64_t)(uintptr_t)b2b) : -1; int nbig0 = g_nbig; pthread_mutex_unlock(&g_mu);
      CHECK("the run is a large pool", bg >= 0);
      uint64_t e0 = g_pool_extends;
      r2 = (map_req){.phys = (NVNmemoryPool *)&ph2, .poff = 512u << 20, .voff = 512u << 20, .size = 128u << 20};
      CHECK("map the next 128 MB", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sp2, 1, (const NVNmappingRequest *)&r2));
      pthread_mutex_lock(&g_mu); int bg2 = big_of((uint64_t)(uintptr_t)b2b + (600u << 20));
      CHECK("grown in place", g_pool_extends == e0 + 1 && g_nbig == nbig0 && bg2 == bg && g_big[bg].hi == (uintptr_t)b2b + (640u << 20)); pthread_mutex_unlock(&g_mu);
      void *mb2; uint64_t mo2; CHECK("lookup in the new part", mtl_addr((uint64_t)(uintptr_t)b2b + (600u << 20), 4, &mb2, &mo2) && *(const uint32_t *)((const uint8_t *)((__bridge id<MTLBuffer>)mb2).contents + mo2) == *(const uint32_t *)(p2 + (600u << 20)));
      (void)gmem_map_gpu(); pthread_mutex_lock(&g_mu); CHECK("ten segments for 640 MB", g_big[bg].nseg == 10); pthread_mutex_unlock(&g_mu);
      r2 = (map_req){.phys = (NVNmemoryPool *)&ph2, .poff = 640u << 20, .voff = 640u << 20, .size = 64u << 20};
      CHECK("map 64 MB more", h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sp2, 1, (const NVNmappingRequest *)&r2));
      uint64_t s1 = g_big_seg_made; (void)gmem_map_gpu(); CHECK("only the last segment lengthened and one new", g_big_seg_made == s1 + 2);
      // the table finds an address in the new part through a segment
      const uint8_t *tb = (const uint8_t *)g_gmem_buf.contents; uint64_t want = (uint64_t)(uintptr_t)b2b + (680u << 20); int hit = 0;
      for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) { const uint64_t *e = (const uint64_t *)(tb + (size_t)k * DMSH_GMEM_MAP_ENTRY); if (e[0] == ~0ull) break; if (want >= e[0] && want < e[1]) { hit = 1; break; } }
      CHECK("pool table holds the grown part", hit); }
    // 8. pool entries follow the run list through joins, holes filled, unmaps inside a run and maps again (PRAGMATA: a mapped run whose pool entries had a 64 KB hole, the GPU read 0 there)
    { uint64_t B = 64u << 20; int ok = 1; void *mb; uint64_t mo;
      CHECK("8 a", map_at(B, 0x30000, 1u << 20)); CHECK("8 b", map_at(B + 0x40000, 0x80000, 2u << 20));
      CHECK("8 fill the hole", map_at(B + 0x30000, 0x10000, 3u << 20));
      for (uint64_t o = 0; o < 0xc0000; o += 0x1000) ok &= mtl_addr((uint64_t)(uintptr_t)(base + B + o), 4, &mb, &mo);
      CHECK("8 every page of the joined run is looked up", ok);
      CHECK("8 unmap inside", map_at(B + 0x50000, 0x10000, -1)); CHECK("8 map it again", map_at(B + 0x50000, 0x10000, 4u << 20));
      ok = 1; for (uint64_t o = 0; o < 0xc0000; o += 0x1000) ok &= mtl_addr((uint64_t)(uintptr_t)(base + B + o), 4, &mb, &mo);
      CHECK("8 still every page after unmap + map", ok);
      CHECK("8 extend at the end", map_at(B + 0xc0000, 0x30000, 5u << 20));
      ok = 1; for (uint64_t o = 0; o < 0xf0000; o += 0x1000) ok &= mtl_addr((uint64_t)(uintptr_t)(base + B + o), 4, &mb, &mo);
      CHECK("8 and after growing at the end", ok);
      // the pool table holds it too
      (void)gmem_map_gpu(); const uint8_t *tb = (const uint8_t *)g_gmem_buf.contents; int hits = 0;
      for (uint64_t o = 0; o < 0xf0000; o += 0x10000) { uint64_t want = (uint64_t)(uintptr_t)(base + B + o);
          for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) { const uint64_t *e = (const uint64_t *)(tb + (size_t)k * DMSH_GMEM_MAP_ENTRY); if (e[0] == ~0ull) break; if (want >= e[0] && want < e[1]) { hits++; break; } } }
      CHECK("8 the pool table has every 64 KB of it", hits == 15); }
    // 9. a pool deleted while an upload batch begun before it is still open or running: its buffer is not released (nor taken out of the residency set) until that batch completes
    // (PRAGMATA: an upload command buffer's page fault); the submission's fence alone is not enough
    { size_t pl = 4u << 20; uint8_t *pm = mmap(NULL, pl, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (pm == MAP_FAILED) abort();
      __weak id<MTLBuffer> wb; @autoreleasepool { mtl_pool_add(pm, pl); void *mb9; uint64_t mo9; CHECK("9 lookup", mtl_addr((uint64_t)(uintptr_t)pm, 4, &mb9, &mo9)); wb = (__bridge id<MTLBuffer>)mb9; }
      unsigned seq0 = g_up_seq; g_up_seq = seq0 + 1; atomic_store(&g_up_slot_seq[seq0 % 3], (int64_t)seq0); atomic_store(&g_up_alloc_busy[seq0 % 3], 1);   // a batch open
      @autoreleasepool { mtl_pool_del(pm); }
      atomic_store(&g_done, 100000); @autoreleasepool { defer_drain(); }
      int k9; @autoreleasepool { k9 = wb != nil; } CHECK("9 kept while the batch is open", k9);
      atomic_store(&g_up_alloc_busy[seq0 % 3], 0); @autoreleasepool { defer_drain(); res_commit(); }
      int g9; @autoreleasepool { g9 = wb == nil; } CHECK("9 released after the batch completed", g9); }
    // 10. the pool table built in this submission is filled again at commit when pools changed meanwhile (a MapVirtual on another thread during the encode): the new pages are in it
    { (void)gmem_map_gpu(); id<MTLBuffer> t0 = g_gmem_buf; uint64_t p0 = g_gmem_patched;
      CHECK("map pages after the table was built", map_at(200u << 20, 0x40000, 6u << 20));
      gmem_commit_patch();
      CHECK("filled again in place", g_gmem_buf == t0 && g_gmem_patched == p0 + 1);
      const uint8_t *tb = (const uint8_t *)g_gmem_buf.contents; uint64_t want = (uint64_t)(uintptr_t)(base + (200u << 20) + 0x100); int hit = 0;
      for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) { const uint64_t *e = (const uint64_t *)(tb + (size_t)k * DMSH_GMEM_MAP_ENTRY); if (e[0] == ~0ull) break; if (want >= e[0] && want < e[1]) { hit = 1; break; } }
      CHECK("the new pages are in the table", hit);
      g_gmem_first_fence = 0; CHECK("map more", map_at(208u << 20, 0x10000, 7u << 20)); uint64_t p1 = g_gmem_patched; gmem_commit_patch();
      CHECK("a table an earlier submission binds is left alone", g_gmem_patched == p1); }
    // 11. a deferred sparse unmap also waits for the upload batches begun before it (a deswizzle may read the pages through the run's buffer)
    { CHECK("11 map", map_at(300u << 20, 0x40000, 8u << 20));
      unsigned sq = g_up_seq; g_up_seq = sq + 1; atomic_store(&g_up_slot_seq[sq % 3], (int64_t)sq); atomic_store(&g_up_alloc_busy[sq % 3], 1);
      CHECK("11 unmap", map_at(300u << 20, 0x40000, -1)); atomic_store(&g_done, 200000);
      CHECK("11 another request", map_at(310u << 20, 0x10000, 9u << 20));
      CHECK("11 pages kept while the batch is open", prot_at(base + (300u << 20)) > 0);
      atomic_store(&g_up_alloc_busy[sq % 3], 0);
      CHECK("11 and another", map_at(312u << 20, 0x10000, 10u << 20));
      CHECK("11 released after it", prot_at(base + (300u << 20)) == 0); }
    // 12. deswizzle bounds: a job whose source or destination range leaves its buffer is refused (-2), a fitting one passes
    { nvn_texdesc dd; memset(&dd, 0, sizeof dd); dd.width = 256; dd.height = 256; dd.depth = 1; dd.levels = 1; dd.format = 0x25; dd.target = 1; dd.samples = 1;
      nvn_texlayout lay; nvn_tex_layout(&dd, &lay);
      CHECK("12 fits", nvn_deswz_check(&dd, 0, 0, 0, 0, lay.size, 0, 256u * 256u * 4u, 256 * 4) == 0);
      CHECK("12 source too short refused", nvn_deswz_check(&dd, 0, 0, 0, 0, lay.size / 2, 0, 256u * 256u * 4u, 256 * 4) == -2);
      CHECK("12 source offset past the end refused", nvn_deswz_check(&dd, 0, 0, 0, lay.size, lay.size, 0, 256u * 256u * 4u, 256 * 4) == -2);
      CHECK("12 destination too short refused", nvn_deswz_check(&dd, 0, 0, 0, 0, lay.size, 0, 1024, 256 * 4) == -2);
      dd.width = 1u << 20; dd.height = 1u << 20; CHECK("12 absurd grid refused", nvn_deswz_check(&dd, 0, 0, 0, 0, ~0ull >> 8, 0, ~0ull >> 8, (1u << 20) * 4u) == -2); }
    // 13. a table built in an earlier submission and reused in this one: at commit this submission's argument words are pointed at a new table that has the pages mapped meanwhile
    { (void)gmem_map_gpu(); g_gmem_first_fence = 0; uint64_t word = g_gmem_buf.gpuAddress, old = word; gmem_slot_note(&word);
      CHECK("13 map after the encode", map_at(400u << 20, 0x40000, 11u << 20));
      gmem_commit_patch();
      CHECK("13 the word points at a new table", word != old && word == g_gmem_buf.gpuAddress);
      const uint8_t *tb = (const uint8_t *)g_gmem_buf.contents; uint64_t want = (uint64_t)(uintptr_t)(base + (400u << 20) + 0x80); int hit = 0;
      for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) { const uint64_t *e = (const uint64_t *)(tb + (size_t)k * DMSH_GMEM_MAP_ENTRY); if (e[0] == ~0ull) break; if (want >= e[0] && want < e[1]) { hit = 1; break; } }
      CHECK("13 the new table has the pages", hit);
      uint64_t w2 = 0x1234; gmem_slot_note(&w2); gmem_commit_patch(); CHECK("13 an up-to-date table: words pointed at it", w2 == g_gmem_buf.gpuAddress); }
    // 14. a segment of this submission committed (sub_cut / an indirect-count wait): the table it may read is brought up to date first, then never filled again in place: a pool
    // change after it gives a new table, and the segment's table stays as it was (PRAGMATA: rows rewritten under a running binary search)
    { (void)gmem_map_gpu(); id<MTLBuffer> t0 = g_gmem_buf; uint64_t word = t0.gpuAddress; gmem_slot_note(&word);
      CHECK("14 map before the segment", map_at(500u << 20, 0x40000, 12u << 20));
      uint64_t p0 = g_gmem_patched; gmem_segment_commit();
      CHECK("14 the segment's table brought up to date in place (built in this submission, not yet on the GPU)", g_gmem_buf == t0 && g_gmem_patched == p0 + 1 && word == t0.gpuAddress);
      CHECK("14 then it counts as in flight", g_gmem_first_fence == 0);
      uint8_t snap[64]; memcpy(snap, t0.contents, sizeof snap);
      uint64_t w2 = 0; gmem_slot_note(&w2);
      CHECK("14 map after the segment", map_at(508u << 20, 0x10000, 13u << 20));
      uint64_t p1 = g_gmem_patched; gmem_commit_patch();
      CHECK("14 the end of the submission makes a new table", g_gmem_buf != t0 && g_gmem_patched == p1 && w2 == g_gmem_buf.gpuAddress);
      CHECK("14 the segment's table is untouched", !memcmp(snap, t0.contents, sizeof snap));
    }
    // 15. breadcrumbs (DM_NVN_BREADCRUMB): the first dispatch of the failed fence without its GPU-written mark is the one in flight
    { g_csp_buf = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; memset(g_csp_buf.contents, 0, 256);
      const char *keys[5] = {"aaaa", "bbbb", "cccc", "dddd", "eeee"};
      for (int i = 0; i < 5; i++) { uint64_t seq = ++g_csp_seq; g_csp[seq % CSP_N].seq = seq; g_csp[seq % CSP_N].key = keys[i]; g_csp[seq % CSP_N].fence = i < 2 ? 40 : 41; }
      *(uint64_t *)g_csp_buf.contents = g_csp_seq - 3;   // the GPU finished the first two
      uint64_t u15 = csp_first_unfinished(41); CHECK("15 the dispatch in flight", u15 == g_csp_seq - 2 && !strcmp(g_csp[u15 % CSP_N].key, "cccc"));
      *(uint64_t *)g_csp_buf.contents = g_csp_seq; CHECK("15 all finished: none", csp_first_unfinished(41) == 0); g_csp_buf = nil; }
    // 16. a removed allocation stays alive until the removal is committed (PRAGMATA under shader validation: a buffer released right after res_remove was still listed in the residency set)
    { __weak id<MTLBuffer> wb16;
      @autoreleasepool { id<MTLBuffer> b = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; wb16 = b; res_add(b); res_commit(); res_remove(b); b = nil; }
      int alive; @autoreleasepool { alive = wb16 != nil; } CHECK("16 removed but not committed: still alive", alive);
      @autoreleasepool { res_commit(); }
      int gone; @autoreleasepool { gone = wb16 == nil; } CHECK("16 released once the removal is committed", gone); }
    // 20. the same on a guest-like thread with no autorelease pool that drains (MapVirtual runs on the game's threads): an object autoreleased there stays alive until the thread
    // ends, so the old run buffers must not be autoreleased on the way (checked while the thread still lives)
    { t20_base = base; t20_done = dispatch_semaphore_create(0); t20_end = dispatch_semaphore_create(0); pthread_t th;
      pthread_create(&th, NULL, t20_body, NULL); dispatch_semaphore_wait(t20_done, DISPATCH_TIME_FOREVER);
      atomic_store(&g_done, g_fence + 2); @autoreleasepool { defer_drain(); res_commit(); }
      int gone = 1, last; @autoreleasepool { for (int i = 0; i + 1 < 8; i++) gone &= t20_w[i] == nil; last = t20_w[7] != nil; }
      CHECK("20 grown on a thread with no draining pool", t20_ok);
      CHECK("20 every replaced run buffer released while that thread lives", gone); CHECK("20 the current one alive", last);
      dispatch_semaphore_signal(t20_end); pthread_join(th, NULL); }
    // 4. off: the old behaviour (inaccessible pages after an unmap)
    g_vp_safe_test_off = 1;
    CHECK("map a run (safe off)", map_at(12u << 20, 1u << 20, 6u << 20));
    CHECK("unmap (safe off)", map_at(12u << 20, 1u << 20, -1));
    CHECK("pages inaccessible after an unmap with it off", prot_at(base + (12u << 20)) == 0);
    fprintf(stderr, "t_sparse_safe: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
