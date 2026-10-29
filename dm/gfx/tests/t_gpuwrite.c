// Unit tests of the GPU-write ledger (nvn_gpuwrite.c), CPU only, on every host: writes queued in a submission (overlap, newest first, the wide list, more than the old 256 limit,
// overflow), the bytes of queued inline updates as the GPU will read them (whole, partly covered, two updates, a later copy, gone with the next submission), the other GPU writes of a
// submission (shader and copy writes count, counter reports do not), and the pages earlier submissions' writes keep until their fence completes. Addresses are only compared, never
// dereferenced; expected values are written out by hand
#include "../nvn/nvn_gpuwrite.h"
#include <stdio.h>
#include <string.h>

static int fail, total;
static void t(const char *name, int ok) {
    total++; fail += !ok;
    printf("%-74s %s\n", name, ok ? "✓" : "✗");
}
static int update(uint64_t dst, const void *data, uint64_t n) { int e = nvn_gw_pend_add(dst, n, 1); if (e >= 0) nvn_gw_pend_data(e, dst, data); return e; }

int main(void) {
    // ---------- Queued writes ----------
    nvn_gw_reset();
    t("empty submission: nothing queued", nvn_gw_pend_last(0x10000, 16) == -1 && !nvn_gw_pend_hits(0x10000, 16) && !g_npend);
    int e0 = nvn_gw_pend_add(0x10000, 0x100, 2);
    t("a copy queued: index 0, its range and kind", e0 == 0 && g_npend == 1 && g_pend_lo[0] == 0x10000 && g_pend_hi[0] == 0x10100 && g_pend_kind[0] == 2 && !g_pend_upd[0]);
    t("a read inside it hits", nvn_gw_pend_hits(0x10080, 4));
    t("reads just before and just after it do not", !nvn_gw_pend_hits(0xfffc, 4) && !nvn_gw_pend_hits(0x10100, 4));
    t("a read across its start hits", nvn_gw_pend_hits(0xfffc, 8));
    int e1 = nvn_gw_pend_add(0x10080, 0x100, 3);
    t("the newest write over a range is told", e1 == 1 && nvn_gw_pend_last(0x10090, 4) == 1 && nvn_gw_pend_last(0x10000, 4) == 0 && nvn_gw_pend_last(0x10170, 4) == 1);
    int ew = nvn_gw_pend_add(0x100000, 0x10000, 2);   // 16 pages: the wide list
    t("a write over 16 pages (wide list) is found from any of its pages", ew == 2 && nvn_gw_pend_last(0x10f000, 8) == 2 && nvn_gw_pend_hits(0x100000, 1) && !nvn_gw_pend_hits(0x110000, 4));
    t("a lookup over many pages scans every entry", nvn_gw_pend_last(0, 0x200000) == 2 && nvn_gw_pend_last(0x10000, 0x10000) == 1);
    int out[PEND_MAX + PEND_WIDE];
    int n = nvn_gw_pend_since(0x10000, 0x10200, 0, out);
    t("writes over a range, newest first", n == 2 && out[0] == 1 && out[1] == 0);
    n = nvn_gw_pend_since(0x10000, 0x10200, 1, out);
    t("writes over a range since index 1", n == 1 && out[0] == 1);
    n = nvn_gw_pend_since(0xff000, 0x101000, 0, out);
    t("writes over two pages, the wide one among them, each once", n == 1 && out[0] == 2);

    nvn_gw_reset();
    int all = 1; for (int i = 0; i < 300; i++) all &= nvn_gw_pend_add(0x200000 + 0x100 * (uint64_t)i, 0x10, 2) == i;
    for (int i = 0; i < 300; i++) all &= nvn_gw_pend_last(0x200000 + 0x100 * (uint64_t)i + 4, 4) == i;
    t("300 writes in a submission (more than the old limit of 256) each told", all && !g_pend_over);
    for (int i = 300; i < PEND_MAX; i++) nvn_gw_pend_add(0x4000000 + 0x40 * (uint64_t)i, 0x10, 2);
    t("the list full: still exact", !g_pend_over && g_npend == PEND_MAX && !nvn_gw_pend_hits(0x100, 4));
    t("one write more: overflow, every address counts as written", nvn_gw_pend_add(0x8000000, 4, 2) == -1 && g_pend_over && nvn_gw_pend_last(0x100, 4) == -2 && nvn_gw_pend_hits(0x100, 4));
    n = nvn_gw_pend_since(0x100, 0x104, 0, out);
    t("overflow: the writes over a range cannot be told", n == -1);

    // ---------- Inline updates read as the GPU will see them ----------
    nvn_gw_reset();
    t("a new submission: no write, no overflow", !g_pend_over && !g_npend && !nvn_gw_pend_hits(0x100, 4));
    uint8_t d1[16], d2[16], got[16]; int kind = -1;
    for (int i = 0; i < 16; i++) { d1[i] = (uint8_t)(0x10 + i); d2[i] = (uint8_t)(0x80 + i); }
    int u1 = update(0x20000, d1, 16);
    t("an inline update keeps its bytes (index + 1 into the ring list)", u1 == 0 && g_pend_upd[0] == 1 && g_upd[0].lo == 0x20000 && g_upd[0].data == d1);
    memset(got, 0, sizeof got);
    t("a read it covers: its bytes", nvn_gw_pend_read(0x20004, 4, got, &kind) == 1 && kind == 1 && !memcmp(got, d1 + 4, 4));
    t("a read past its end: not known, written by an update", nvn_gw_pend_read(0x2000c, 8, got, &kind) == 0 && kind == 1);
    t("a read before it: nothing queued there", nvn_gw_pend_read(0x1fff0, 8, got, &kind) == 0 && kind == 0);
    int u2 = update(0x20008, d2, 16);
    memset(got, 0, sizeof got);
    t("two updates: a read the newer covers takes the newer's bytes", u2 == 1 && nvn_gw_pend_read(0x20008, 4, got, &kind) == 1 && !memcmp(got, d2, 4));
    memset(got, 0, sizeof got);
    t("two updates: a read only the older covers takes the older's", nvn_gw_pend_read(0x20000, 8, got, &kind) == 1 && !memcmp(got, d1, 8));
    t("two updates: a read across both is not known (newest does not cover it)", nvn_gw_pend_read(0x20006, 4, got, &kind) == 0 && kind == 1);
    memset(got, 0, sizeof got);
    t("the newer update's end: its last bytes", nvn_gw_pend_read(0x20014, 4, got, &kind) == 1 && !memcmp(got, d2 + 12, 4));
    nvn_gw_pend_add(0x20000, 4, 2);
    t("a copy queued after the updates: not known, written by a copy", nvn_gw_pend_read(0x20000, 4, got, &kind) == 0 && kind == 2);
    t("beside the copy the update still reads", nvn_gw_pend_read(0x20004, 4, got, &kind) == 1 && !memcmp(got, d1 + 4, 4));
    nvn_gw_reset();
    t("the next submission: the updates are gone", nvn_gw_pend_read(0x20004, 4, got, &kind) == 0 && kind == 0 && !nvn_gw_pend_hits(0x20000, 32));
    uint32_t gen = g_pend_gen; nvn_gw_reset();
    t("each submission takes a new generation", g_pend_gen == gen + 1);

    // ---------- Other GPU writes of this submission ----------
    uint32_t sg = g_sbw_gen;
    nvn_gw_sbw_note(0x50000, 64, SBW_GPU);
    t("a shader write: noted, generation bumped", g_sbw_gen == sg + 1 && nvn_gw_sbw_hits(0x50020, 0x50024) && !g_sbw_over);
    t("another page: not written", !nvn_gw_sbw_hits(0x51000, 0x51004) && !nvn_gw_sbw_hits(0x4f000, 0x50000));
    nvn_gw_sbw_note(0x60000, 16, SBW_REPORT);
    t("a counter report: not a write this submission's reads see", g_sbw_gen == sg + 1 && !nvn_gw_sbw_hits(0x60000, 0x60010));
    nvn_gw_sbw_note(0x70ff0, 32, SBW_GPU);
    t("a shader write across two pages: both", nvn_gw_sbw_hits(0x70000, 0x70004) && nvn_gw_sbw_hits(0x71000, 0x71004));
    t("no overflow so far", !g_sbw_over);
    nvn_gw_sbw_note(0x1000000, ((uint64_t)SBW_PAGES << 12) + 1, SBW_GPU);
    t("a shader write too large to keep: overflow", g_sbw_over);
    nvn_gw_reset();
    t("the next submission: the set and its overflow are gone", !g_sbw_over && !nvn_gw_sbw_hits(0x50000, 0x50040));
    nvn_gw_sbw_note(0x50000, 0, SBW_GPU);
    t("a shader write of unknown size: overflow", g_sbw_over);

    // ---------- Earlier submissions' writes until their fence completes ----------
    nvn_gw_reset(); memset(g_gw, 0, sizeof g_gw); g_gw_all = 0;
    t("no earlier writes: nothing live", !nvn_gw_live(0x70000, 0x70010, 0));
    nvn_gw_pend_add(0x70000, 32, 2); nvn_gw_sbw_note(0x80000, 64, SBW_GPU); nvn_gw_sbw_note(0x90000, 16, SBW_REPORT);
    nvn_gw_commit(5, 4);
    t("committed with fence 5: its queued write live while fence 4 is done", nvn_gw_live(0x70000, 0x70010, 4) && nvn_gw_live(0x70ffc, 0x71000, 4));
    t("its shader write's page live", nvn_gw_live(0x80800, 0x80804, 4));
    t("its counter report's page live for later submissions", nvn_gw_live(0x90000, 0x90010, 4));
    t("another page: not live", !nvn_gw_live(0xa0000, 0xa0010, 4));
    t("fence 5 completed: nothing live", !nvn_gw_live(0x70000, 0x70010, 5) && !nvn_gw_live(0x80000, 0x81000, 6));
    nvn_gw_reset();
    t("the next submission: queued list empty, earlier writes still live", !nvn_gw_pend_hits(0x70000, 16) && nvn_gw_live(0x70000, 0x70010, 4));
    // two pages that share a slot of the page table while both are live: any page there counts as written until the later fence (slot = page * K >> 52 & (GW_N - 1), nvn_gpuwrite.c)
    uint64_t p1 = 0x123, p2 = p1 + 1; unsigned s1 = (unsigned)((p1 * 0x9E3779B97F4A7C15ull) >> 52) & (GW_N - 1);
    while ((((unsigned)((p2 * 0x9E3779B97F4A7C15ull) >> 52)) & (GW_N - 1)) != s1) p2++;
    memset(g_gw, 0, sizeof g_gw);
    nvn_gw_put(p1 << 12, (p1 + 1) << 12, 7, 6); nvn_gw_put(p2 << 12, (p2 + 1) << 12, 8, 6);
    t("two live pages in one slot: both live until the later fence", nvn_gw_live(p1 << 12, (p1 << 12) + 4, 7) && nvn_gw_live(p2 << 12, (p2 << 12) + 4, 7));
    t("the shared slot: nothing live once fence 8 is done", !nvn_gw_live(p1 << 12, (p1 << 12) + 4, 8));
    memset(g_gw, 0, sizeof g_gw);
    nvn_gw_put(p1 << 12, (p1 + 1) << 12, 7, 6); nvn_gw_put(p2 << 12, (p2 + 1) << 12, 9, 7);
    t("a page over a completed one in its slot takes the slot alone", !nvn_gw_live(p1 << 12, (p1 << 12) + 4, 7) && nvn_gw_live(p2 << 12, (p2 << 12) + 4, 7));
    memset(g_gw, 0, sizeof g_gw); g_gw_all = 0;
    nvn_gw_put(0x10000000, 0x10000000 + ((uint64_t)GW_PAGES << 12), 11, 10);
    t("a write over 4096 pages: kept page by page", !g_gw_all && nvn_gw_live(0x10fff000, 0x10fff004, 10) && !nvn_gw_live(0x10fff000, 0x10fff004, 11));
    memset(g_gw, 0, sizeof g_gw);
    nvn_gw_put(0x10000000, 0x10000000 + ((uint64_t)(GW_PAGES + 1) << 12), 11, 10);
    t("a write over 4097 pages: every page live until its fence", g_gw_all == 11 && nvn_gw_live(0x1000, 0x1004, 10) && !nvn_gw_live(0x1000, 0x1004, 11));
    g_gw_all = 0; nvn_gw_reset();
    nvn_gw_sbw_note(0x50000, 0, SBW_GPU); nvn_gw_commit(12, 11);
    t("a submission whose writes could not all be listed: everything live", g_gw_all == 12 && nvn_gw_live(0xdead000, 0xdead004, 11) && !nvn_gw_live(0xdead000, 0xdead004, 12));
    g_gw_all = 0; nvn_gw_reset();
    nvn_gw_put(0x5000, 0x5000, 13, 12);
    t("an empty range is not recorded", !nvn_gw_live(0x5000, 0x5004, 12) && !g_gw_all);
    printf("== t_gpuwrite: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
