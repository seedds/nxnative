// The ledger of GPU writes to guest memory (see nvn_gpuwrite.h). The code is nvn_mtl.m's as it was before the move (docs/design/vulkan-backend.md step M2): the completed fence
// gw_live and gw_commit read (nvn_mtl.m g_done) is now their argument, an inline update's bytes are a pointer instead of the Metal buffer and offset holding them, and the index-range
// invalidation pend_add began with stays in the Metal backend (nvn_mtl.m pend_add calls vc_gpu_write, then nvn_gw_pend_add)
#include "nvn_gpuwrite.h"
#include <string.h>

uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; int g_npend, g_pend_over; uint8_t g_pend_kind[PEND_MAX]; uint16_t g_pend_upd[PEND_MAX];
static int32_t g_pend_head[PEND_HB]; static uint32_t g_pend_hgen[PEND_HB]; uint32_t g_pend_gen = 1;   // a bucket's chain is valid when its generation is this submission's
static struct { int32_t e, next; } g_pend_node[PEND_NODES]; static int g_npnode;
static int32_t g_pend_wide[PEND_WIDE]; static int g_nwide;
upd_ent g_upd[PEND_MAX]; static int g_nupd;
static struct { uint64_t page; uint32_t gen; uint8_t src; } g_sbwp[SBW_N]; static int g_nsbw; int g_sbw_over;   // src: SBW_* bits; pages in the set this submission
static int sbw_slot(uint64_t pg, int add) {   // open addressing; −1 = not there (or no room to add)
    unsigned h = (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 50) & (SBW_N - 1);
    for (int i = 0; i < 64; i++, h = (h + 1) & (SBW_N - 1)) {
        if (g_sbwp[h].gen != g_pend_gen) { if (!add) return -1; g_sbwp[h].page = pg; g_sbwp[h].gen = g_pend_gen; g_sbwp[h].src = 0; g_nsbw++; return (int)h; }
        if (g_sbwp[h].page == pg) return (int)h; }
    return -1;
}
uint32_t g_sbw_gen;
void nvn_gw_sbw_note(uint64_t lo, uint64_t n, int src) {
    if (src == SBW_GPU) g_sbw_gen++;
    if (!n || n > (uint64_t)SBW_PAGES << 12) { if (src == SBW_GPU) g_sbw_over = 1; return; }   // a report too large to keep cannot happen (16 bytes)
    for (uint64_t pg = lo >> 12; pg <= (lo + n - 1) >> 12; pg++) { int k = sbw_slot(pg, 1); if (k < 0) { g_sbw_over = 1; return; } g_sbwp[k].src |= (uint8_t)src; }
}
int nvn_gw_sbw_hits(uint64_t lo, uint64_t hi) { if (!g_nsbw) return 0; for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { int k = sbw_slot(pg, 0); if (k >= 0 && (g_sbwp[k].src & SBW_GPU)) return 1; } return 0; }
static inline unsigned pend_bucket(uint64_t page) { return (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 53) & (PEND_HB - 1); }
void nvn_gw_reset(void) { g_npend = g_pend_over = g_nupd = g_npnode = g_nwide = g_nsbw = g_sbw_over = 0; if (!++g_pend_gen) { memset(g_pend_hgen, 0, sizeof g_pend_hgen); memset(g_sbwp, 0, sizeof g_sbwp); g_pend_gen = 1; } }
int nvn_gw_pend_add(uint64_t dst, uint64_t n, int kind) {
    if (g_pend_over || g_npend == PEND_MAX) { g_pend_over = 1; return -1; }
    int e = g_npend; uint64_t p0 = dst >> 12, p1 = (dst + (n ? n : 1) - 1) >> 12;
    if (p1 - p0 < PEND_SPAN && g_npnode + (int)(p1 - p0 + 1) <= PEND_NODES)
        for (uint64_t pg = p0; pg <= p1; pg++) { unsigned b = pend_bucket(pg); int k = g_npnode++;
            g_pend_node[k].e = e; g_pend_node[k].next = g_pend_hgen[b] == g_pend_gen ? g_pend_head[b] : -1; g_pend_head[b] = k; g_pend_hgen[b] = g_pend_gen; }
    else if (g_nwide < PEND_WIDE) g_pend_wide[g_nwide++] = e;
    else { g_pend_over = 1; return -1; }
    g_pend_lo[e] = dst; g_pend_hi[e] = dst + n; g_pend_kind[e] = (uint8_t)kind; g_pend_upd[e] = 0; return g_npend++;
}
void nvn_gw_pend_data(int e, uint64_t lo, const void *data) { g_upd[g_nupd] = (upd_ent){lo, data}; g_pend_upd[e] = (uint16_t)++g_nupd; }
int nvn_gw_pend_last(uint64_t a, uint64_t n) {
    if (g_pend_over) return -2;
    int best = -1; uint64_t p0 = a >> 12, p1 = (a + (n ? n : 1) - 1) >> 12;
    for (int i = 0; i < g_nwide; i++) { int e = g_pend_wide[i]; if (e > best && a < g_pend_hi[e] && a + n > g_pend_lo[e]) best = e; }
    if (p1 - p0 >= PEND_SPAN) { for (int e = g_npend - 1; e > best; e--) if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) return e; return best; }
    for (uint64_t pg = p0; pg <= p1; pg++) { unsigned b = pend_bucket(pg); if (g_pend_hgen[b] != g_pend_gen) continue;
        for (int k = g_pend_head[b]; k >= 0 && g_pend_node[k].e > best; k = g_pend_node[k].next) { int e = g_pend_node[k].e; if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) { best = e; break; } } }
    return best;
}
int nvn_gw_pend_hits(uint64_t a, uint64_t n) { return nvn_gw_pend_last(a, n) != -1; }
int nvn_gw_pend_read(uint64_t a, uint64_t n, void *out, int *kind) {
    int e = nvn_gw_pend_last(a, n); *kind = e >= 0 ? g_pend_kind[e] : 0;
    if (e < 0 || !g_pend_upd[e] || a < g_pend_lo[e] || a + n > g_pend_hi[e]) return 0;
    const upd_ent *u = &g_upd[g_pend_upd[e] - 1]; memcpy(out, u->data + (a - u->lo), n); return 1;
}

gw_page g_gw[GW_N];
uint64_t g_gw_all;
static inline unsigned gw_slot(uint64_t pg) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 52) & (GW_N - 1); }
void nvn_gw_put(uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done) {
    if (hi <= lo) return;
    uint64_t p0 = lo >> 12, p1 = (hi - 1) >> 12;
    if (p1 - p0 >= GW_PAGES) { if (fence > g_gw_all) g_gw_all = fence; return; }
    for (uint64_t pg = p0; pg <= p1; pg++) { unsigned s = gw_slot(pg);
        g_gw[s].page = g_gw[s].page == pg || g_gw[s].fence <= done ? pg : ~0ull;
        if (fence > g_gw[s].fence) g_gw[s].fence = fence; }
}
int nvn_gw_live(uint64_t lo, uint64_t hi, uint64_t done) {
    if (g_gw_all > done) return 1;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { unsigned s = gw_slot(pg); if ((g_gw[s].page == pg || g_gw[s].page == ~0ull) && g_gw[s].fence > done) return 1; }
    return 0;
}
void nvn_gw_commit(uint64_t fence, uint64_t done) {
    if ((g_pend_over || g_sbw_over) && fence > g_gw_all) g_gw_all = fence;
    for (int e = 0; e < g_npend; e++) nvn_gw_put(g_pend_lo[e], g_pend_hi[e], fence, done);
    if (g_nsbw) for (int i = 0; i < SBW_N; i++) if (g_sbwp[i].gen == g_pend_gen) nvn_gw_put(g_sbwp[i].page << 12, (g_sbwp[i].page + 1) << 12, fence, done);
}
int nvn_gw_pend_since(uint64_t a, uint64_t b, int since, int *out) {
    if (g_pend_over) return -1;
    int n = 0; uint64_t p0 = a >> 12, p1 = (b - 1) >> 12;
    if (p1 - p0 >= PEND_SPAN) { for (int e = g_npend - 1; e >= since; e--) if (a < g_pend_hi[e] && g_pend_lo[e] < b) out[n++] = e; return n; }   // every entry, newest first
    for (int i = 0; i < g_nwide; i++) { int e = g_pend_wide[i]; if (e >= since && a < g_pend_hi[e] && g_pend_lo[e] < b) out[n++] = e; }
    for (uint64_t pg = p0; pg <= p1; pg++) { unsigned bk = pend_bucket(pg); if (g_pend_hgen[bk] != g_pend_gen) continue;
        for (int k = g_pend_head[bk]; k >= 0 && g_pend_node[k].e >= since; k = g_pend_node[k].next) { int e = g_pend_node[k].e; if (a < g_pend_hi[e] && g_pend_lo[e] < b) out[n++] = e; } }   // chains run newest first
    if (n > 1 && (g_nwide || p1 > p0)) {   // several lists: newest first, each once
        for (int i = 1; i < n; i++) { int v = out[i], j = i; while (j > 0 && out[j - 1] < v) { out[j] = out[j - 1]; j--; } out[j] = v; }
        int m = 1; for (int i = 1; i < n; i++) if (out[i] != out[m - 1]) out[m++] = out[i];
        n = m; }
    return n;
}
