// Index-range cache GPU-write reverse index (ix_range / vc_gpu_write): exact half-open byte overlap, 4 KB page and 1 MB wide-entry boundaries, wide-write binary search,
// CPU-dirty re-protection without duplicate registration, dead-entry unlinking, overflow guards, and clearing at IR_MAX (65,536 entries)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; printf("FAIL: %s\n", label); } } while (0)

static p_draw_idx make_di(uint64_t ibuf, int32_t count, int32_t itype) {
    p_draw_idx di; memset(&di, 0, sizeof di);
    di.ibuf = ibuf; di.count = count; di.itype = itype;
    return di;
}

int main(void) { @autoreleasepool {
    const size_t poolsize = 4u << 20;   // 4 MiB, aligned to 1 MiB so 4 KB and 1 MB boundaries are exact
    uint8_t *mem = NULL;
    if (posix_memalign((void **)&mem, 1u << 20, poolsize)) abort();
    for (size_t i = 0; i < poolsize / 2; i++) ((uint16_t *)mem)[i] = (uint16_t)(i & 1023);
    nvn_dirty_add_range(mem, poolsize);
    uint64_t base = (uint64_t)(uintptr_t)mem;
    uint32_t mn = 0, mx = 0;

    // 1. Half-open byte boundaries & adjacent writes (level 0: single 4 KB page)
    p_draw_idx d0 = make_di(base + 0x100, 16, 1);   // [base+0x100, base+0x120)
    CHECK("small entry cached", ix_range(&d0, &mn, &mx) && g_ir.n == 1 && g_ir_live == 1 && g_ir_lv[0].map.n == 1 && g_ir_lv[1].map.n == 0);
    vc_gpu_write(base + 0x0e0, 0x20);   // [base+0x0e0, base+0x100): adjacent left
    vc_gpu_write(base + 0x120, 0x20);   // [base+0x120, base+0x140): adjacent right
    vc_gpu_write(base + 0x100, 0);      // zero-length write
    vc_gpu_write(UINT64_MAX - 8, 64);   // wrapping high-address write
    CHECK("adjacent, zero-length, and wrapping writes leave entry intact", ix_range(&d0, &mn, &mx) && g_ir_live == 1);

    // Write to another 4 KB page in the SAME 1 MB region: 0 candidates inspected
    uint64_t cand0 = g_ir_cand;
    vc_gpu_write(base + 0x4000, 64);
    CHECK("write to sibling 4 KB page in same 1 MB region checks 0 candidates", g_ir_cand == cand0 && ix_range(&d0, &mn, &mx));

    // 1-byte write at exact last byte invalidates d0 and unlinks it
    vc_gpu_write(base + 0x11f, 1);
    CHECK("1-byte write at last byte invalidates entry", !ix_range(&d0, &mn, &mx) && g_ir_live == 0 && g_ir_lv[0].map.n == 0);
    cand0 = g_ir_cand;
    vc_gpu_write(base + 0x100, 32);
    CHECK("dead entry unlinked: repeated write checks 0 candidates", g_ir_cand == cand0);

    // 2. Multi-page entry crossing a 4 KB boundary (level 0) + re-using an emptied page without duplicating order
    p_draw_idx d1 = make_di(base + 0xff0, 16, 1);   // [base+0xff0, base+0x1010): spans pages 0 and 1
    uint32_t order_before = g_ir_lv[0].order_n;
    CHECK("cross-page entry cached across 2 pages", ix_range(&d1, &mn, &mx) && g_ir_live == 1 && g_ir_lv[0].map.n == 2 && g_ir_lv[0].order_n == order_before + 1);
    // CPU-dirty re-protection must NOT insert duplicate reverse-index nodes
    uint32_t nodes_before = g_ir_node_n;
    dm_guest_write_prepare(mem + 0xff0, 2);
    *(uint16_t *)(mem + 0xff0) = 9999;
    CHECK("CPU-dirty re-protect updates range without duplicate index nodes", ix_range(&d1, &mn, &mx) && mx == 9999 && g_ir_node_n == nodes_before && g_ir_live == 1);
    // Invalidate via the second page, then touch the first page to verify dead node unlinks cleanly without double-decrement
    vc_gpu_write(base + 0x1000, 4);
    CHECK("write to second page invalidates cross-page entry", !ix_range(&d1, &mn, &mx) && g_ir_live == 0 && g_ir_lv[0].map.n == 1);
    // Add another live entry on page 0 so g_ir_live > 0, then write page 0 (non-overlapping with d2) to sweep d1's dead node
    p_draw_idx d2 = make_di(base + 0x200, 8, 1);
    CHECK("new entry on page 0 cached", ix_range(&d2, &mn, &mx) && g_ir_live == 1);
    vc_gpu_write(base + 0xff0, 4);
    CHECK("dead node swept from page 0 without hurting live sibling or g_ir_live", ix_range(&d2, &mn, &mx) && g_ir_live == 1);

    // 3. Wide entry spanning > IR_SPAN (8) 4 KB pages and crossing a 1 MB region boundary (level 1)
    uint64_t w_lo = base + (1u << 20) - 0x4000;   // starts 16 KB before 1 MB boundary
    p_draw_idx dw = make_di(w_lo, 24576, 1);      // 48 KB = 12 pages, spans regions 0 and 1
    CHECK("wide cross-1MB entry indexed at level 1 across 2 regions", ix_range(&dw, &mn, &mx) && g_ir_lv[1].map.n == 2);
    vc_gpu_write(w_lo - 16, 16);                  // adjacent left
    vc_gpu_write(w_lo + 49152, 16);               // adjacent right
    CHECK("adjacent writes leave wide entry intact", ix_range(&dw, &mn, &mx));
    vc_gpu_write(base + (1u << 20) + 0x7fff, 1);  // last byte in second 1 MB region
    CHECK("write in second 1 MB region invalidates wide entry", !ix_range(&dw, &mn, &mx));

    // 4. Wide writes (>= IR_SPAN pages) use sorted-order binary search and only hit overlapping entries
    ir_clear();
    p_draw_idx left = make_di(base + 0x2000, 8, 1);                // page 2
    p_draw_idx mid  = make_di(base + 0x100000 + 0x3000, 8, 1);     // 1 MB + page 3
    p_draw_idx rght = make_di(base + 0x200000 + 0x4000, 8, 1);     // 2 MB + page 4
    p_draw_idx midw = make_di(base + 0x120000, 32768, 1);          // 64 KB wide entry in region 1
    CHECK("four test entries cached", ix_range(&left, &mn, &mx) && ix_range(&mid, &mn, &mx) && ix_range(&rght, &mn, &mx) && ix_range(&midw, &mn, &mx) && g_ir_live == 4);
    vc_gpu_write(base + 0x080000, 0x100000);   // 1 MB write [0.5 MB, 1.5 MB): covers mid and midw, spares left and rght
    CHECK("wide write invalidates only entries in its span", ix_range(&left, &mn, &mx) && !ix_range(&mid, &mn, &mx) && !ix_range(&midw, &mn, &mx) && ix_range(&rght, &mn, &mx) && g_ir_live == 2);

    // Wide write spanning >= IR_SPAN (8) 1 MB regions exercises level 1's sorted-order binary search
    p_draw_idx w_low  = make_di(base + 0x10000, 32768, 1);     // 64 KB in region 0
    p_draw_idx w_high = make_di(base + 0x220000, 32768, 1);    // 64 KB in region 2
    CHECK("two wide entries cached at level 1", ix_range(&w_low, &mn, &mx) && ix_range(&w_high, &mn, &mx) && g_ir_lv[1].map.n == 2);
    vc_gpu_write(base + 0x200000, 16u << 20);   // 16 MB write [base + 2 MB, base + 18 MB): hits w_high and rght, spares w_low and left
    CHECK("level-1 wide write invalidates only overlapping wide/small entries", ix_range(&w_low, &mn, &mx) && !ix_range(&w_high, &mn, &mx) && ix_range(&left, &mn, &mx) && !ix_range(&rght, &mn, &mx));

    // Integration through pend_add (the production caller of vc_gpu_write)
    pend_reset();
    pend_add(base + 0x2000, 4, 1);
    pend_reset();
    CHECK("pend_add invalidates overlapping cached index range", !ix_range(&left, &mn, &mx) && ix_range(&w_low, &mn, &mx));

    // 5. Fill cache to IR_MAX (65,536 entries) in region 0, verify O(1) sibling writes, then overflow-clear
    ir_clear();
    for (uint32_t i = 0; i < IR_MAX; i++) {
        // Place 65,536 distinct keys across [base + 0x10000, base + 0x50000) (pages 16..79 in region 0)
        p_draw_idx di = make_di(base + 0x10000 + (uint64_t)i * 4, 2, 1);
        if (!ix_range(&di, &mn, &mx)) { CHECK("fill to IR_MAX", 0); break; }
    }
    CHECK("cache filled to IR_MAX", g_ir.n == IR_MAX && g_ir_live == IR_MAX);
    cand0 = g_ir_cand;
    for (int i = 0; i < 1000; i++) vc_gpu_write(base + 0x80000 + (uint64_t)(i & 15) * 64, 64);   // same 1 MB region, different 4 KB pages
    CHECK("1000 writes in same 1 MB region on sibling pages check 0 candidates", g_ir_cand == cand0);
    // Invalidate one specific entry out of 65,536: only its 4 KB page's entries are inspected, not all 65,536
    cand0 = g_ir_cand;
    vc_gpu_write(base + 0x10000, 4);
    CHECK("targeted write checks only one 4 KB page (<= 1024 entries), not full table", g_ir_cand - cand0 == 1024 && g_ir_live == IR_MAX - 1);
    p_draw_idx hit0 = make_di(base + 0x10000, 2, 1), keep1 = make_di(base + 0x10004, 2, 1);
    CHECK("targeted write invalidates only overlapping entry", !ix_range(&hit0, &mn, &mx) && ix_range(&keep1, &mn, &mx));

    // 65,537th entry clears both g_ir and the reverse index cleanly
    p_draw_idx after = make_di(base + 0x300000, 8, 1);
    CHECK("IR_MAX overflow clears cache and reverse index cleanly",
          ix_range(&after, &mn, &mx) && g_ir.n == 1 && g_ir_live == 1 && g_ir_lv[0].map.n == 1 && g_ir_lv[1].map.n == 0 && g_ir_node_n == 1);
    vc_gpu_write(base + 0x300000, 16);
    CHECK("post-clear entry invalidates normally", !ix_range(&after, &mn, &mx) && g_ir_live == 0);

    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
