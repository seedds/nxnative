// The index of the writes a submission queues (pend_add, pend_last, pend_since): writes of every size are found by the granule chains, newest first, with no count after which the
// submission counts as a write anywhere. Large writes (over more than PEND_SPAN pages) once had a list of their own that stopped at 256 (Mortal Kombat 11 passed it), and that every lookup
// scanned. CPU only: no Metal device is needed, so CI runs it
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

// Newest first, each once, and exactly the entries since `since` that overlap [a, b)
static int since_right(uint64_t a, uint64_t b, int since) {
    int n = pend_since(a, b, since, g_ubp_idx), want = 0;
    for (int e = g_npend - 1; e >= since; e--) if (a < g_pend_hi[e] && g_pend_lo[e] < b) { if (want >= n || g_ubp_idx[want] != e) return 0; want++; }
    return n == want;
}
static int last_right(uint64_t a, uint64_t n) {
    int want = -1; for (int e = g_npend - 1; e >= 0; e--) if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) { want = e; break; }
    return pend_last(a, n) == want;
}

int main(void) { @autoreleasepool {
    const uint64_t base = 0x10000000, step = 0x20000;
    pend_reset();

    // 9,000 copies of 64 KB (16 pages each): every one is kept, none makes the submission a write anywhere
    int kept = 1; for (int i = 0; i < 9000; i++) kept &= pend_add(base + step * i, 0x10000, 2) == i;
    CHECK(kept && !g_pend_over && g_npend == 9000, "9,000 large writes are all kept (the old list stopped at 256)");
    int found = 1; for (int i = 0; i < 9000; i += 7) found &= pend_last(base + step * i + 8, 4) == i && pend_last(base + step * i + 0xfffc, 4) == i && pend_last(base + step * i + 0x10000, 4) == -1;
    CHECK(found, "each large write is found over its bytes, and not past its end");
    CHECK(pend_last(0x1000, 4) == -1 && pend_last(base - 4, 4) == -1, "an address no write touched has none");

    // A later small update over a large write is the newest there; beside it the large write is
    uint64_t handle = base + step * 8999 + 8;
    int small = pend_add(handle, 4, 1);
    CHECK(pend_last(handle, 4) == small && pend_last(handle + 8, 4) == 8999, "a small update after a large write is the newest over its bytes only");
    CHECK(since_right(handle, handle + 4, 0), "a lookup over both lists them newest first");

    // Many large writes over one handle: the lookup returns all of them, in order (g_ubp_idx holds every entry a lookup can meet)
    for (int i = 0; i < 10000; i++) pend_add(handle - 0x100, 0x10000, 2);
    CHECK(pend_since(handle, handle + 4, 0, g_ubp_idx) == 10002 && since_right(handle, handle + 4, 0), "10,002 writes over one handle, newest first, each once");
    CHECK(since_right(handle, handle + 4, 15000), "only the writes since an index");

    // Writes of every size: 4 KB to 1 GB, unaligned; lookups at their edges, and lookups wider than PEND_SPAN pages (every entry scanned)
    pend_reset();
    const uint64_t big = 0x4000000000ull; uint64_t sizes[] = {16, 0x1000, 0x9000, 0x48000, 0x240000, 0x1200000, 0x9000000, 0x40000000};
    for (int i = 0; i < 8; i++) pend_add(big + 0x123 + (uint64_t)i * 0x80000000ull, sizes[i], 3);
    int edges = 1;
    for (int i = 0; i < 8; i++) { uint64_t lo = big + 0x123 + (uint64_t)i * 0x80000000ull, hi = lo + sizes[i];
        edges &= pend_last(lo, 1) == i && pend_last(hi - 1, 1) == i && pend_last(hi, 1) == -1 && pend_last(lo - 1, 1) == -1 && pend_last(lo - 0x10, 0x20) == i; }
    CHECK(edges, "writes from 16 bytes to 1 GB are found at their first and last byte, and not beside them");
    CHECK(g_pend_lv != 1, "large writes go into coarser granules");
    int wide = 1; for (uint64_t a = big - 0x100000; a < big + 8 * 0x80000000ull; a += 0x3300000) wide &= last_right(a, 0x50000) && since_right(a, a + 0x50000, 0) && last_right(a, 0x5000000);
    CHECK(wide, "lookups of 320 KB and 80 MB match a scan of every entry");

    // Mixed: random narrow and wide writes and lookups agree with a scan of every entry
    pend_reset();
    uint64_t r = 0x9E3779B97F4A7C15ull; int mixed = 1;
    for (int i = 0; i < 20000; i++) { r = r * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t a = 0x20000000 + (r >> 20) % 0x8000000, n = (r & 3) == 0 ? 0x8000 + (r >> 8) % 0x400000 : 1 + (r >> 8) % 0x2000;
        if (i & 1) pend_add(a, n, 1 + (int)(r >> 60) % 3); else mixed &= last_right(a, n) && since_right(a, a + n, (int)((r >> 40) % (g_npend + 1))); }
    CHECK(mixed && !g_pend_over, "20,000 mixed writes and lookups match a scan of every entry");

    // A write past the end of the address space cannot be indexed: the submission counts as a write anywhere
    pend_reset();
    CHECK(pend_add(UINT64_MAX - 0x100, 0x200, 2) == -1 && g_pend_over && pend_last(0x1000, 4) == -2, "a write that wraps counts as a write anywhere");

    // The next submission starts empty and keeps its storage; generation rollover clears the chains
    int cap = g_pend_cap; pend_reset();
    CHECK(!g_pend_over && pend_last(base + 8, 4) == -1 && pend_add(base, 0x10000, 2) == 0 && g_pend_cap == cap && pend_last(base + 8, 4) == 0, "the next submission starts empty");
    g_pend_gen = UINT32_MAX; pend_reset();
    CHECK(g_pend_gen == 1 && pend_last(base + 8, 4) == -1, "generation rollover");

    printf("t_pend_index: %d/%d checks passed\n", checks - failed, checks);
    return failed != 0;
} }
