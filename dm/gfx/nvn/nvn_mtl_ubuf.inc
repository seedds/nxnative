// nvn_mtl.m, part: ubuf
// Inline uniform updates: the deferred-update shadow (ubd_*), partial uniform reads (ubp_*), UpdateUniformBuffer / fill / event / counter, boot and A/B instrumentation, and the submission's commit, wait and finish.
// Included by nvn_mtl.m in its original order, as one translation unit: statics are shared with the other parts.
// Inline uniform update (UpdateUniformBuffer): the data is copied into the host's own shared ring at encode time, then copied in order to the destination.
// Previously it was read from command memory only when the GPU executed - if the game recycles command memory before the GPU runs, it reads the bytes of the next batch of commands (SMBW's fountain water wave simulation got the wrong coefficients and diverged to ±65504).
static id<MTLBuffer> g_ubs_buf; static size_t g_ubs_off, g_ubs_size;
// Inline updates inside an open render pass are deferred instead of ending the pass. Metal cannot copy into a buffer inside a render pass, so each update used to end it: all attachments stored,
// reloaded by the next pass, new encoders (Metroid Dread about 246 times a frame, Tears of the Kingdom about 50). NVN applies the update in command order, so the draws after it in the pass must
// read the new data: a draw whose uniform binding overlaps a deferred update reads a shadow. When the updates deferred in this pass cover all of the binding the shader can read, the shadow is
// made of them alone, latest first per byte (exactly what the GPU would read, whatever earlier frames still have in flight); when they cover only part of it, the rest comes from memory (ubp_get).
// The real copies are encoded in order when the pass ends (rp_end), so later passes and the CPU see them as before. The pass still ends before a draw that would need more: a partly covered binding
// whose rest memory cannot give, or a read of the deferred data through a vertex, index, indirect or storage buffer (ubd_conflict). Off in TBDR mode (it reorders passes). DM_NVN_UB_DEFER=0 turns it off
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf); static inline void args_dirty_all(void); static void ubd_dirty(uint64_t lo, uint64_t hi);
typedef struct { uint64_t lo, hi, gpu; size_t off; __unsafe_unretained id<MTLBuffer> buf; } ubd_ent;   // data at buf + off (the ubs ring; a ring block replaced meanwhile lives until this submission completes)
enum { UBD_MAX = 512 };
static ubd_ent g_ubd[UBD_MAX]; static int g_nubd;
static uint64_t g_ubd_deferred, g_ubd_conflicts;   // updates deferred, passes ended before a draw that needed more (the residency set line)
static int ubd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_UB_DEFER"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_UB_DEFER=0: uniform updates inside a pass end the pass as before"); } return v && !tbdr_on(); }
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = g_ubd_deferred; *conflicts = g_ubd_conflicts; }
// Across passes (DM_NVN_UB_LAZY=n, at most n updates kept past a pass end, default 160; 0 = landed at every pass end as before): the deferred updates are not landed when their pass ends but
// when something needs the memory itself: a copy or dispatch (they go into that compute encoder first), a draw that reads them other than as a uniform buffer the shadows cover
// (ubd_conflict), the end of the submission, more than n of them. An update with no pass
// open is deferred too, unless a compute encoder is open (it then goes in there). Breath of the Wild had about 45 small copy encoders between its render passes a frame, each with
// 10-50 µs of idle GPU before and after it: GPU time 9.3 → 8.0 ms a frame, frame 11.9 → 10.7 ms at the 120 fps cap (M2 Max), for about 0.8 ms more translation (more shadows).
// The list can then hold hundreds of updates: a page filter answers most lookups without the scan
static int ubd_lazy(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_UB_LAZY"); v = e ? atoi(e) : 160; if (v < 0) v = 0; } return v; }   // the most updates kept across a pass end (0 = off)
enum { UBD_PF = 1024 };
static uint64_t g_ubd_pf[UBD_PF];   // pages (address >> 12, +1) the deferred updates touch, open addressing; cleared when the list is landed
static int g_ubd_wide;              // an update spans more than 16 pages: the filter is skipped
static inline unsigned ubd_pf_slot(uint64_t pg) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 54) & (UBD_PF - 1); }
static void ubd_pf_add(uint64_t lo, uint64_t hi) {
    if (((hi - 1) >> 12) - (lo >> 12) > 16) { g_ubd_wide = 1; return; }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { unsigned k = ubd_pf_slot(pg);
        while (g_ubd_pf[k] && g_ubd_pf[k] != pg + 1) k = (k + 1) & (UBD_PF - 1);
        g_ubd_pf[k] = pg + 1; }
}
static inline int ubd_pf_hit(uint64_t lo, uint64_t hi) {
    if (g_ubd_wide || ((hi - 1) >> 12) - (lo >> 12) > 16) return 1;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { unsigned k = ubd_pf_slot(pg);
        while (g_ubd_pf[k]) { if (g_ubd_pf[k] == pg + 1) return 1; k = (k + 1) & (UBD_PF - 1); } }
    return 0;
}
static int ubd_hits(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !ubd_pf_hit(lo, hi)) return 0;
    for (int i = 0; i < g_nubd; i++) if (lo < g_ubd[i].hi && g_ubd[i].lo < hi) return 1;
    return 0;
}
// Whether the deferred updates cover [lo, hi) entirely (any order of overlap): the union of their ranges clipped to it, swept from lo
static int ubd_covers(uint64_t lo, uint64_t hi) {
    for (uint64_t at = lo; at < hi;) { uint64_t next = at;
        for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo <= at && g_ubd[i].hi > next) next = g_ubd[i].hi;
        if (next == at) return 0; at = next; }
    return 1;
}
// GPU address of the shadow of uniform range [lo, hi) (ubd_covers holds): one deferred update holding all of it and suitably aligned is used in place; otherwise the updates are laid into the arena in order
static uint64_t ubd_shadow(uint64_t lo, uint64_t hi) {
    for (int i = g_nubd - 1; i >= 0; i--) { const ubd_ent *e = &g_ubd[i];
        if (e->lo <= lo && hi <= e->hi) { int later = 0; for (int j = i + 1; j < g_nubd; j++) later |= lo < g_ubd[j].hi && g_ubd[j].lo < hi;
            uint64_t g = e->gpu + (lo - e->lo); if (!later && !(g & 15)) return g; break; } }
    id<MTLBuffer> ab; size_t o = arena_alloc((size_t)(hi - lo), &ab); uint8_t *d = (uint8_t *)ab.contents + o;
    for (int i = 0; i < g_nubd; i++) { const ubd_ent *e = &g_ubd[i]; uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < hi ? e->hi : hi;
        if (a < b) memcpy(d + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a)); }
    return ab.gpuAddress + o;
}
// Encode the deferred copies in order (rp_end, once the pass has ended)
static void ubd_flush(void) {
    int n = g_nubd; g_nubd = 0; memset(g_ubd_pf, 0, sizeof g_ubd_pf); g_ubd_wide = 0;
    for (int i = 0; i < n; i++) { const ubd_ent *e = &g_ubd[i]; void *db; uint64_t dof;
        if (!mtl_addr(e->lo, e->hi - e->lo, &db, &dof)) continue;
        enc_b_copy_buffer(blit_buf(0, 0, e->lo, e->hi), e->buf, e->off, (__bridge id<MTLBuffer>)db, dof, e->hi - e->lo); g_gp_ubd++; }
}
// The bytes [lo, lo+n) as the deferred updates leave them (ubd_covers holds): for bindless handles read from uniform memory (ubo_handle)
static void ubd_read(uint64_t lo, size_t n, void *out) {
    for (int i = 0; i < g_nubd; i++) { const ubd_ent *e = &g_ubd[i]; uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < lo + n ? e->hi : lo + n;
        if (a < b) memcpy((uint8_t *)out + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a)); }
}
// Bindings the deferred updates cover only in part. The rest of the binding is what memory holds when the pass runs on the GPU. The shadow then starts from memory as the CPU sees it at encode
// time and lays over it every inline update queued in this submission that overlaps it, in queue order (earlier passes' updates, encoded but not yet run, and this pass's deferred ones): exactly
// what the GPU would read. It is made only when nothing else can have written the range by then: no copy, fill or feedback write queued in this submission (pend kinds 2–4), no writable storage
// buffer, texture→buffer copy or counter report over it in this submission (sbw), and no GPU write of an earlier submission still running (gw, by 4 KB page). Otherwise the pass ends as before.
// Breath of the Wild updates part of a bound block about 226 times a frame outdoors and ended a pass for nearly every one (about 900 MB of attachment loads and stores a frame on an iPhone).
// DM_NVN_UB_PARTIAL=0 turns it off
static int ub_partial_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_UB_PARTIAL"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_UB_PARTIAL=0: a binding the deferred uniform updates cover only in part ends the pass as before"); } return v; }
enum { GW_N = 4096, GW_PAGES = 4096 };
static struct { uint64_t page, fence; } g_gw[GW_N];   // page ~0: two live pages met in the slot, so any page there counts as written until fence completes
static uint64_t g_gw_all;                            // every page counts as written until this fence completes (a submission whose writes could not all be listed)
static uint64_t g_ubp_made, g_ubp_reused, g_ubp_refused;   // partial shadows made / taken from the cache / refused (the pass ended), for the residency set line
static uint64_t g_ubp_why[5];   // why ubp_get refused: 0 too large or off, 1 other GPU writes in this submission (sbw), 2 GPU writes of an earlier submission still running, 3 a copy, fill or feedback write queued, 4 unreadable or too many writes
static inline unsigned gw_slot(uint64_t pg) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 52) & (GW_N - 1); }
static void gw_put(uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done) {
    if (hi <= lo) return;
    uint64_t p0 = lo >> 12, p1 = (hi - 1) >> 12;
    if (p1 - p0 >= GW_PAGES) { if (fence > g_gw_all) g_gw_all = fence; return; }
    for (uint64_t pg = p0; pg <= p1; pg++) { unsigned s = gw_slot(pg);
        g_gw[s].page = g_gw[s].page == pg || g_gw[s].fence <= done ? pg : ~0ull;
        if (fence > g_gw[s].fence) g_gw[s].fence = fence; }
}
static int gw_live(uint64_t lo, uint64_t hi) {
    uint64_t done = g_done;   // read without the lock as elsewhere: a stale value only refuses more
    if (g_gw_all > done) return 1;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { unsigned s = gw_slot(pg); if ((g_gw[s].page == pg || g_gw[s].page == ~0ull) && g_gw[s].fence > done) return 1; }
    return 0;
}
// The submission's GPU writes go into the page table with its fence (mtl_sub_commit)
static void gw_commit(uint64_t fence) {
    uint64_t done = g_done;
    if ((g_pend_over || g_sbw_over) && fence > g_gw_all) g_gw_all = fence;
    for (int e = 0; e < g_npend; e++) gw_put(g_pend_lo[e], g_pend_hi[e], fence, done);
    if (g_nsbw) for (int i = 0; i < SBW_N; i++) if (g_sbwp[i].gen == g_pend_gen) gw_put(g_sbwp[i].page << 12, (g_sbwp[i].page + 1) << 12, fence, done);
    for (int i = 0; i < g_nsbw_w; i++) gw_put(g_sbw_wlo[i], g_sbw_whi[i], fence, done);
}
// The writes queued in this submission from index since on that overlap [a, b), newest first, into out (room for every entry a lookup can meet); -1 when they cannot all be told
static int pend_since(uint64_t a, uint64_t b, int since, int *out) {
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
int nvn_host_readable(uint64_t a, uint64_t n);   // hle_nvn.c: guest memory and sparse pools
// Shadows made from memory, per range. One is kept while no write is queued after it (the draws after an update bind the same block again and again); the next one for the same range starts
// from it and lays over only the writes queued since (the game rewrites one block hundreds of times a submission). A range read whole from memory once is not read from memory again in the submission
enum { UBP_CACHE = 16, UBP_MAX_BYTES = 65536 };
static int g_ubp_idx[PEND_MAX + PEND_WIDE];   // every entry a lookup can meet: the chains of the range's pages hold each entry at most once after dedup, the wide list at most PEND_WIDE
typedef struct { uint64_t lo, hi, gpu; uint8_t *cpu; int npend; uint32_t gen, sbw; uint64_t ready; } ubp_ent;   // sbw: g_sbw_gen when the other writes were last checked; ready: copy ops that fill it (ubc_push)
static uint64_t g_ubp_ready;   // ready of the shadow ubp_get returned last (a CPU read of it waits for that: ubc_wait)
static ubp_ent g_ubpc[UBP_CACHE]; static unsigned g_ubpc_i;
// The bytes [lo, hi) as the GPU will read them at this point of the submission (see above): 1 with the shadow's GPU and CPU addresses, 0 when they cannot be known (the pass ends instead)
static int ubp_get(uint64_t lo, uint64_t hi, uint64_t *gpu, const uint8_t **cpu) {
    if (!ub_partial_on() || hi <= lo || hi - lo > UBP_MAX_BYTES) { g_ubp_why[0]++; return 0; }
    ubp_ent *c = NULL;
    for (int i = 0; i < UBP_CACHE; i++) if (g_ubpc[i].lo == lo && g_ubpc[i].hi == hi && g_ubpc[i].gen == g_pend_gen) { c = &g_ubpc[i]; break; }
    if (!c || c->sbw != g_sbw_gen) { if (g_sbw_over || sbw_hits(lo, hi)) { g_ubp_why[1]++; return 0; } if (c) c->sbw = g_sbw_gen; }
    if (c && c->npend == g_npend) { g_ubp_reused++; *gpu = c->gpu; *cpu = c->cpu; g_ubp_ready = c->ready; return 1; }
    int since = c ? c->npend : 0;
    if (!c && gw_live(lo, hi)) { g_ubp_why[2]++; return 0; }
    if (!c && !nvn_host_readable(lo, hi - lo)) { g_ubp_why[4]++; return 0; }
    int n = pend_since(lo, hi, since, g_ubp_idx); if (n < 0) { g_ubp_why[4]++; return 0; }
    // Newest first, keep the writes that still show through somewhere (a write only newer ones cover is never read, whatever wrote it); stop once they cover the range
    enum { COVN = 64 }; uint64_t cl[COVN], ch[COVN]; int nc = 0, nk = 0, exact = 1;   // covered intervals, sorted and disjoint; exact = 0 once they no longer fit
    for (int i = 0; i < n; i++) { int e = g_ubp_idx[i];
        uint64_t x = g_pend_lo[e] > lo ? g_pend_lo[e] : lo, y = g_pend_hi[e] < hi ? g_pend_hi[e] : hi;
        int j = 0; while (j < nc && ch[j] < y) j++;
        if (exact && j < nc && cl[j] <= x && y <= ch[j]) continue;   // newer writes cover all of it
        if (g_pend_kind[e] != 1 || !g_pend_upd[e]) { g_ubp_why[3]++; return 0; }   // a copy, fill or feedback write the CPU cannot see
        g_ubp_idx[nk++] = e;
        if (exact) {   // add [x, y) and merge neighbours
            int k = 0; while (k < nc && ch[k] < x) k++;
            int m = k; uint64_t nl = x, nh = y; while (m < nc && cl[m] <= y) { nl = MIN(nl, cl[m]); nh = MAX(nh, ch[m]); m++; }
            if (nc - (m - k) + 1 > COVN) exact = 0;
            else { memmove(&cl[k + 1], &cl[m], (nc - m) * sizeof *cl); memmove(&ch[k + 1], &ch[m], (nc - m) * sizeof *ch); nc = nc - (m - k) + 1; cl[k] = nl; ch[k] = nh;
                   if (nc == 1 && cl[0] == lo && ch[0] == hi) break; } } }
    uint64_t len = hi - lo;
    id<MTLBuffer> ab; size_t o = arena_alloc((size_t)len, &ab); uint8_t *d = (uint8_t *)ab.contents + o;
    int defer = len >= 512 && ubc_on(); uint64_t ready = 0;   // small ones (bindless handles read on the CPU) are copied here
    if (defer) ready = ubc_push(d, c ? c->cpu : (const uint8_t *)(uintptr_t)lo, (size_t)len);   // the previous shadow's own copies are queued before these
    else { if (c) ubc_wait(c->ready); memcpy(d, c ? c->cpu : (const uint8_t *)(uintptr_t)lo, (size_t)len); }
    for (int i = nk - 1; i >= 0; i--) { int e = g_ubp_idx[i]; const upd_ent *u = &g_upd[g_pend_upd[e] - 1];   // oldest first: each byte ends with the newest write
        uint64_t x = g_pend_lo[e] > lo ? g_pend_lo[e] : lo, y = g_pend_hi[e] < hi ? g_pend_hi[e] : hi;
        const uint8_t *src = (const uint8_t *)u->buf.contents + u->off + (x - u->lo);
        if (defer) ready = ubc_push(d + (x - lo), src, (size_t)(y - x)); else memcpy(d + (x - lo), src, (size_t)(y - x)); }
    g_ubp_made++;
    if (!c) c = &g_ubpc[g_ubpc_i++ % UBP_CACHE];
    *c = (ubp_ent){lo, hi, ab.gpuAddress + o, d, g_npend, g_pend_gen, g_sbw_gen, ready};
    *gpu = c->gpu; *cpu = d; g_ubp_ready = ready; return 1;
}
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = g_ubp_made; *reused = g_ubp_reused; *refused = g_ubp_refused; }
void mtl_ubp_why(uint64_t w[5]) { memcpy(w, g_ubp_why, sizeof g_ubp_why); }
void mtl_sub_update(const void *data, uint64_t dst, uint64_t n) {
    if (!n) return;
    int pi = pend_add(dst, n, 1);
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("uniform update target is in no memory pool dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    int defer = (enc_open() || (ubd_lazy() && !g_blit)) && g_nubd < UBD_MAX && ubd_on();
    size_t need = (n + 255u) & ~(size_t)255u;   // 256-aligned slots: a deferred update is read in place by draws as a uniform buffer
    if (!g_ubs_buf || g_ubs_off + need > g_ubs_size) {
        if (g_ubs_buf) defer_obj_release(g_ubs_buf);   // copies already encoded still reference the old block: release after this submission completes
        g_ubs_size = MAX((size_t)1 << 20, need);
        g_ubs_buf = [g_dev newBufferWithLength:g_ubs_size options:MTLResourceStorageModeShared];
        if (!g_ubs_buf) MDIE("uniform update ring allocation failed %#zx", g_ubs_size);
        res_add(g_ubs_buf); g_ubs_off = 0;
    }
    memcpy((uint8_t *)g_ubs_buf.contents + g_ubs_off, data, n);
    if (pi >= 0) { g_upd[g_nupd] = (upd_ent){dst, g_ubs_off, g_ubs_buf}; g_pend_upd[pi] = (uint16_t)++g_nupd; }
    if (defer) {
        // A deferred update this one covers whole never needs to land: the same landing writes these bytes after it, and the draws that read it hold its ring copy (ubd_shadow).
        // Games rewrite one block before every draw group (Breath of the Wild: the same 64 bytes), so the list stays short: every lookup below scans it, for most bindings of most draws
        int k = 0; for (int i = 0; i < g_nubd; i++) if (!(dst <= g_ubd[i].lo && g_ubd[i].hi <= dst + n)) g_ubd[k++] = g_ubd[i];
        g_nubd = k;
        g_ubd[g_nubd++] = (ubd_ent){dst, dst + n, g_ubs_buf.gpuAddress + g_ubs_off, g_ubs_off, g_ubs_buf}; ubd_pf_add(dst, dst + n); g_ubd_deferred++; ubd_dirty(dst, dst + n); }   // see ubd_shadow
    else enc_b_copy_buffer(blit_buf(0, 0, dst, dst + n), g_ubs_buf, g_ubs_off, (__bridge id<MTLBuffer>)db, dof, n);
    g_ubs_off += need;
}

// In-order buffer fill within the submission: all four bytes equal → fillBuffer; otherwise copied in chunks from a 64 KiB pattern buffer
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n) return;
    pend_add(dst, n, 3); g_mdx_epoch++;
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("buffer fill address is in no memory pool dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    uint8_t b0 = (uint8_t)value;
    if (value == (uint32_t)b0 * 0x01010101u) { [blit_buf(0, 0, dst, dst + n) fillBuffer:(__bridge id<MTLBuffer>)db range:NSMakeRange(dof, n) value:b0]; return; }
    enum { PAT = 0x10000 }; static __strong id<MTLBuffer> pat; static uint32_t pat_v; static int pat_ok;
    if (!pat_ok || pat_v != value) { uint32_t *tmp = malloc(PAT); for (unsigned i = 0; i < PAT / 4; i++) tmp[i] = value;
        pat = [g_dev newBufferWithBytes:tmp length:PAT options:MTLResourceStorageModeShared]; free(tmp); pat_v = value; pat_ok = 1; res_add(pat); }
    for (uint64_t off = 0; off < n; off += PAT) { uint64_t k = n - off < PAT ? n - off : PAT; enc_b_copy_buffer(blit_buf(0, 0, dst + off, dst + off + k), pat, 0, (__bridge id<MTLBuffer>)db, dof + off, k); }
}

static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, NULL}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { sbw_note(addr, 16, SBW_REPORT); post((post_op){2, NULL, 0, 0, addr, type, NULL, g_seg}); }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { memcpy(rep, g_cnt_report, sizeof g_cnt_report); memcpy(rst, g_cnt_reset, sizeof g_cnt_reset); }

// Counter timestamps (614.4 MHz) are taken from the guest clock (hle_os.c guest_ns, the same epoch as svc GetSystemTick / cntpct, whose ticks are 19.2 MHz).
// They used to come from CLOCK_MONOTONIC, which keeps running while the Mac sleeps while the guest clock (mach_absolute_time) does not, so the two differed by the sleep time; once timestamps were converted to ns, Mario halved its frame rate. In graphics unit tests guest_ns is a weak definition returning 0; fall back to the monotonic clock
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; } return nvn_timestamp_from_ns(g); }
// Counter timestamps by segment (profile gpu_timestamps segment; DM_NVN_GPU_TS=segment / submission overrides it). On hardware a timestamp report is written when the GPU reaches it, so two reports of one submission bracket the GPU work between them.
// The host writes all reports of a submission in its completion handler; with one time for all of them a game that measures its GPU time that way reads 0 (Breath of the Wild: frame-interval controller and dynamic resolution).
// In segment mode a report takes the GPU time of the segment it was encoded in (sub_cut): TIMESTAMP_TOP (0x10) that segment's GPU start, other types its GPU end. The writes stay in the last segment's handler, before the fence,
// so a game that waits for the fence reads one submission's reports together; a segment whose feedback has not arrived by then takes the last segment's times
#define SEG_CLK_N 1024
struct seg_clk { _Atomic int refs; _Atomic uint64_t t[SEG_CLK_N][2]; };   // per submission (g_sclk, allocated at its first cut): guest ns of each cut segment's GPU start / end (0 = no feedback yet)
static int seg_ts(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_GPU_TS"); v = e && *e ? !strcmp(e, "segment") : DM_GAME_GPU_TS_SEGMENT; MLOG("counter timestamps: %s (profile key gpu_timestamps; DM_NVN_GPU_TS=segment / submission)", v ? "GPU start / end time of their segment" : "one time for the whole submission"); } return v; }
static void seg_clk_put(seg_clk *c) { if (c && atomic_fetch_sub(&c->refs, 1) == 1) free(c); }
static uint64_t gpu_s_guest_ns(double s) {   // a feedback GPU time (seconds on the mach absolute time base) on the guest clock; 0 when unknown
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t g = guest_ns(), now = tb.denom ? mach_absolute_time() * tb.numer / tb.denom : 0, at = (uint64_t)(s * 1e9);
    if (s <= 0 || !g || at > now || now - at > g) return 0;
    return g - (now - at);
}
static uint64_t rep_ticks(const post_op *o, seg_clk *sc, uint32_t last, uint64_t c0, uint64_t c1) {   // the time a counter report records (614.4 MHz); c0 / c1: the last segment's GPU start / end on the guest clock
    int top = o->type == 0x10; uint64_t ns = 0;
    if (sc && o->seg < last && o->seg < SEG_CLK_N) ns = atomic_load(&sc->t[o->seg][!top]);
    if (!ns) ns = top ? c0 : c1;
    return ns ? nvn_timestamp_from_ns(ns) : gpu_ticks();
}

// ---------- Boot instrumentation (DM_BOOTPROF=1): metallib loading, pipeline creation (both happen synchronously during replay on the submit thread), texture upload, first-frame present ----------
static uint64_t bp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint64_t g_bp_t0;   // Time of process start (when this module is loaded)
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;   // Cumulative upload time (same source as the per-frame @upload_ms)
// ---------- Per-frame GPU time aggregation ----------
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1: the frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns)
static _Atomic uint64_t g_cur_frame;
// ---------- Same-process alternating measurement (DM_NVN_UB, off by default: without the environment variable the code below only adds one 'is it enabled' check) ----------
// On a hot real device the system keeps adjusting power / frequency, and segment comparison (segments A and B each run for a minute) cannot resolve differences of <10%; within the same run, toggle by phase in turn and take differences of adjacent phase pairs, so the drift cancels.
// DM_NVN_UB="name=item,item;name=item", one phase every DM_NVN_UB_N frames (default 180, minimum 60): even phases = baseline (nothing changed), odd phases take one category in turn. Each item of a category:
//   @set:<global variable name>=<value>   on entering the phase set this int global variable to the value, on leaving restore it (address found by symbol name; switches to be used on a real device must have a line added to the registration table below)
//   <fragment shader key prefix> or *   the phase drops all draws of these fragment shaders (the picture will be wrong; it only measures 'how much this kind of draw is worth'; a nearly idle category needs a round of its own)
// Readings: each phase discards its first 45 frames (transition + present lag); for the remaining frames, the mean of the actual present interval (same source as perf.txt) and the frame counts in the three buckets 16.7 / 25 / ≥33 ms, one log line "UB present".
static int g_ub_n = -1, g_ub_N = 180; static _Atomic int g_ub_cur = -1; static char g_ub_name[16][16], g_ub_keys[16][16][48]; static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4]; static _Atomic uint64_t g_ubp_sum_us;
void mtl_ub_present(double iv) {   // Called once per frame by the present thread: actual present interval in ms (−1 = dropped, 0 = no reading)
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1); atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// Registered runtime switches: name → address. Main-program symbols of an iOS release build are not in the dynamic symbol table and dlsym cannot find them (in the first real-device round all four categories were "global variable not found" and the whole round was wasted),
// so a switch to be measured on a real device must have a line added here; on Mac / in unit tests unregistered ones are still looked up by symbol name.
static int *ub_var(const char *nm) {
    extern int g_dm_present_link;
    static const struct { const char *n; int *p; } t[] = { {"g_dm_present_link", &g_dm_present_link} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) { g_ub_n = 0; const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N"); if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) { const char *eq = strchr(q, '='), *end = strchr(q, ';'); if (!end) end = q + strlen(q); if (!eq || eq > end) break;
            int c = g_ub_n++; size_t l = (size_t)(eq - q); if (l > 15) l = 15; memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) { size_t kl = 0; while (k + kl < end && k[kl] != ',') kl++; size_t cl = kl > 47 ? 47 : kl; memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl); k += kl; if (k < end) k++; }
            q = *end ? end + 1 : end; }
        if (g_ub_n) MLOG("DM_NVN_UB: %d categories, %d frames per phase (measured alternately in one process; phases that drop draws render wrong)", g_ub_n, g_ub_N); }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) { for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0); atomic_store(&g_ubp_sum_us, 0); atomic_store(&g_ubp_on, 1); }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) { atomic_store(&g_ubp_on, 0); uint64_t pp = ph - 1; int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        MLOG("UB present %llu %s frames %d mean %.3f ms 2 ticks %d 3 ticks %d 4+ ticks %d discarded %d", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3); }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16]; static int sv[16], ns;   // Variables set by the previous phase: restore first
    for (int i = 0; i < ns; i++) *sp[i] = sv[i]; ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) { const char *k = g_ub_keys[nc][i]; if (strncmp(k, "@set:", 5)) continue;
        char nm[48]; const char *eq = strchr(k, '='); if (!eq) continue; size_t l = (size_t)(eq - (k + 5)); if (l >= sizeof nm) continue; memcpy(nm, k + 5, l); nm[l] = 0;
        int *a_ = ub_var(nm); static int bad, ok;
        if (!a_) { if (bad++ < 8) MLOG("[RED] DM_NVN_UB: global variable not found: %s (on a device, register it in the ub_var table first)", nm); continue; }
        sp[ns] = a_; sv[ns] = *a_; ns++; *a_ = atoi(eq + 1); if (ok++ < 8) MLOG("DM_NVN_UB: %s = %d (was %d)", nm, atoi(eq + 1), sv[ns - 1]); }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {   // Whether this phase should drop draws of this fragment shader
    int uc = atomic_load(&g_ub_cur); if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) { const char *k = g_ub_keys[uc][i]; if (k[0] == '@') continue; if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1; }
    return 0;
}
static long ab_len(void); static void ab_frame(uint64_t f);
// DM_GPU_CAPTURE=<frame>[:<frames>] (diagnostic): a Metal GPU trace of those frames (default 1) for Xcode's Metal debugger (per-draw cost, shader profiler), or DM_GPU_CAPTURE=gpu:<ms>[:<after frame>]
// of one frame in the first scene, after that frame, whose GPU time passes <ms> in 60 frames in a row (a single long frame in a light scene fired the first iPhone capture). Read again
// when it changes, so the Mac's DM_CTL "env DM_GPU_CAPTURE=..." can set it while the game runs. iOS needs a build with MetalCaptureEnabled (DM_METAL_CAPTURE=1 when the project is
// made), the Mac MTL_CAPTURE_ENABLED=1 in the environment. It captures the Metal 4 queue (Metal refuses to capture a Metal 4 device), so the old-queue helpers (present, some uploads)
// are not in it. The trace is dm_<date>-<time>_f<frame>.gputrace in DM_GPU_CAPTURE_DIR, by default gpu_captures/ in the working directory (the app's Documents on iOS, the run folder
// on the Mac). A trace takes 1.5-2 GB, so before each capture that folder alone is pruned: of its dm_*.gputrace entries the newest DM_GPU_CAPTURE_KEEP - 1 stay (default 2 with the new
// one); nothing outside it is removed
static long g_cap_f0 = -2, g_cap_n; static int g_cap_on;
static NSString *gpu_capture_dir(void) {
    const char *e = getenv("DM_GPU_CAPTURE_DIR"); NSString *dir;
    if (e && *e) dir = @(e); else { char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "."); dir = [@(cwd) stringByAppendingPathComponent:@"gpu_captures"]; }
    NSError *err = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err]) { MLOG("[RED] DM_GPU_CAPTURE: cannot make the folder %s: %s", dir.UTF8String, err.localizedDescription.UTF8String); return nil; }
    return dir;
}
static void gpu_capture_prune(NSString *dir) {   // only dm_*.gputrace entries directly in the capture folder; the newest DM_GPU_CAPTURE_KEEP - 1 stay
    const char *e = getenv("DM_GPU_CAPTURE_KEEP"); long keep = e ? atol(e) : 2; if (keep < 1) keep = 1;
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray<NSURL *> *traces = [NSMutableArray array];
    for (NSURL *u in [fm contentsOfDirectoryAtURL:[NSURL fileURLWithPath:dir isDirectory:YES] includingPropertiesForKeys:@[NSURLContentModificationDateKey] options:0 error:nil]) {
        NSString *n = u.lastPathComponent; if ([n hasPrefix:@"dm_"] && [n hasSuffix:@".gputrace"]) [traces addObject:u]; }
    [traces sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) { NSDate *da = nil, *db = nil;
        [a getResourceValue:&da forKey:NSURLContentModificationDateKey error:nil]; [b getResourceValue:&db forKey:NSURLContentModificationDateKey error:nil];
        return [db ?: NSDate.distantPast compare:da ?: NSDate.distantPast]; }];   // newest first
    for (NSUInteger i = (NSUInteger)keep - 1; i < traces.count; i++) if ([fm removeItemAtURL:traces[i] error:nil]) MLOG("DM_GPU_CAPTURE: removed %s (older captures beyond DM_GPU_CAPTURE_KEEP=%ld)", traces[i].lastPathComponent.UTF8String, keep);
}
static void gpu_capture_frame(uint64_t f) {
    static char last[32];   // read again when it changes
    { const char *e = getenv("DM_GPU_CAPTURE"); if (!g_cap_on && e && strncmp(e, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", e); g_cap_f0 = -2; } }
    static double gpu_thr; static long after; static int over;
    if (g_cap_f0 == -2) { const char *e = getenv("DM_GPU_CAPTURE"); gpu_thr = 0; over = 0;
        if (e && !strncmp(e, "gpu:", 4)) { gpu_thr = atof(e + 4); const char *c = strchr(e + 4, ':'); after = c ? atol(c + 1) : 0; g_cap_n = 1; g_cap_f0 = gpu_thr > 0 ? LONG_MAX : -1;   // armed: the frame is not known yet
            if (gpu_thr > 0) MLOG("DM_GPU_CAPTURE: the first frame after frame %ld that follows 60 frames in a row with more than %.1f ms of GPU time is captured", after, gpu_thr); }
        else { g_cap_f0 = e ? atol(e) : -1; g_cap_n = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 1; if (g_cap_n < 1) g_cap_n = 1; if (g_cap_f0 <= 0) g_cap_f0 = -1; } }
    if (g_cap_f0 < 0) return;
    if (gpu_thr > 0 && !g_cap_on && f > 8 && (long)f >= after) { double g = mtl_gpu_ms(f - 8); over = g >= gpu_thr ? over + 1 : g >= 0 ? 0 : over; if (over >= 60) g_cap_f0 = (long)f; }
    MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
    if (!g_cap_on && f == (uint64_t)g_cap_f0) {
        g_cap_f0 = -1;   // one capture per setting: a failure below does not retry every frame
        if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) { MLOG("[RED] DM_GPU_CAPTURE: this build cannot write GPU traces (iOS: MetalCaptureEnabled, a build made with DM_METAL_CAPTURE=1; Mac: MTL_CAPTURE_ENABLED=1)"); return; }
        NSString *dir = gpu_capture_dir(); if (!dir) return;
        gpu_capture_prune(dir);
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_%s_f%llu.gputrace", ts, (unsigned long long)f]];
        MTLCaptureDescriptor *d = [MTLCaptureDescriptor new]; d.captureObject = g_q4 ? (id)g_q4 : (id)g_dev; d.destination = MTLCaptureDestinationGPUTraceDocument; d.outputURL = [NSURL fileURLWithPath:path];
        NSError *err = nil;
        if ([m startCaptureWithDescriptor:d error:&err]) { g_cap_on = 1; g_cap_f0 = (long)f; MLOG("DM_GPU_CAPTURE: capturing frames %llu to %llu into %s", (unsigned long long)f, (unsigned long long)(f + g_cap_n - 1), path.UTF8String); }
        else MLOG("[RED] DM_GPU_CAPTURE: the capture did not start: %s", err.localizedDescription.UTF8String);
    } else if (g_cap_on && f >= (uint64_t)g_cap_f0 + (uint64_t)g_cap_n) { [m stopCapture]; g_cap_on = 0; g_cap_f0 = -1; gpu_thr = 0; MLOG("DM_GPU_CAPTURE: capture stopped at frame %llu (the trace is written in the background)", (unsigned long long)f); }
}
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); if (g_ub_n) ub_frame(f); if (ab_len()) ab_frame(f); gpu_capture_frame(f); }
// GPU timing registration: gpu_tag_begin(frame) before commit, gpu_tag_done(frame, GPU start, GPU end) on completion (seconds on the CACurrentMediaTime base).
// Old-queue command buffers go through gpu_tag_f; the Metal 4 main submission backfills in the feedback of mtl_sub_commit (when switching to Metal 4 all the call sites were lost and the GPU column of the timing line became empty)
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0 && g0 > 0) {   // a command buffer can complete with no start time (0, while a movie's frames upload each frame in Big Brain Academy: Brain vs. Brain), and its span would be the whole clock
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(d * 1e9));
        uint64_t s0 = (uint64_t)(g0 * 1e9), s1 = (uint64_t)(g1 * 1e9), v;
        for (v = atomic_load(&g_gpu[k].t0); s0 < v && !atomic_compare_exchange_weak(&g_gpu[k].t0, &v, s0);) {}
        for (v = atomic_load(&g_gpu[k].t1); s1 > v && !atomic_compare_exchange_weak(&g_gpu[k].t1, &v, s1);) {}
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
static void gpu_tag_f(id<MTLCommandBuffer> cb, uint64_t f) { gpu_tag_begin(f); [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { gpu_tag_done(f, c.GPUStartTime, c.GPUEndTime); }]; }
void gpu_tag(id<MTLCommandBuffer> cb) { gpu_tag_f(cb, atomic_load(&g_cur_frame)); }
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return 0;
    *t0 = atomic_load(&g_gpu[k].t0); *t1 = atomic_load(&g_gpu[k].t1);
    return *t1 >= *t0 && *t1;
}
static void rp_flush(void);
static const char *cctx_snap(void);   // What is in this submission (printed into the failure line on error; defined before mtl_exec)
static void lc_land(void);
// A submission's CPU-side writes at its end (events, samples-passed and counter reports): run once, by its feedback handler or, when a game's SyncWait wakes on the
// GPU's own signal of the fence first, by that waiter (DM_NVN_FAST_WAKE, on by default): the feedback handler comes 0.26 ms after the GPU end on an M2 Max
typedef struct { _Atomic uint64_t fence; _Atomic int claimed; int n; post_op *ops; seg_clk *sc; uint32_t seg_last; uint64_t t_commit; } post_rec;   // t_commit: guest ns when it was committed
#define POST_RING 128
static post_rec g_postr[POST_RING];
static pthread_mutex_t g_post_mu = PTHREAD_MUTEX_INITIALIZER;
// A successful claim holds the lock until post_run finishes. Ownership alone
// cannot publish completion: another waiter must see the actual writes first.
static int post_claim(post_rec *r, uint64_t fence) {
    pthread_mutex_lock(&g_post_mu);
    if (r->fence != fence || r->claimed) { pthread_mutex_unlock(&g_post_mu); return 0; }
    return 1;
}
static void post_run(post_rec *r, uint64_t c0, uint64_t c1) {
    int n = r->n; post_op *ops = r->ops; seg_clk *sc = r->sc; uint32_t seg_last = r->seg_last;
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1) { if (ops[i].mode == 1) *ops[i].val += ops[i].v; else *ops[i].val = ops[i].v; }
        else if (ops[i].kind == 3) {   // Samples passed: the sum of all slots since the reset
            vis_list *l = ops[i].vis; uint64_t sum = 0;
            for (int k = 0; k < l->n; k++) { sum += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot]; CFRelease(l->s[k].buf); }
            free(l); uint64_t rec[2] = {sum, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec);
        }
        else if (ops[i].kind == 4) mirror_rb_land(ops[i].vis);
        else { uint64_t t = rep_ticks(&ops[i], sc, seg_last, c0, c1), rec[2] = {(ops[i].type == 0 || ops[i].type == 0x10) && !DM_GAME_RENDER_COND ? t : 0, t}; memcpy((void *)ops[i].addr, rec, sizeof rec); }   // render_cond: a timestamp report's counter field is 0, as on the GPU (see rc_value)
    }
    free(ops); seg_clk_put(sc); r->ops = NULL; r->sc = NULL;
    atomic_store(&r->claimed, 1);
    pthread_mutex_unlock(&g_post_mu);
}
// Both GPU feedback and an early waiter complete records in submission order.
// Delayed callbacks carry their fence, never the identity of a recyclable slot.
static void post_complete(uint64_t fence, uint64_t c0, uint64_t c1) {
    uint64_t now = guest_ns();
    for (uint64_t f = g_done + 1; f <= fence; f++) {
        post_rec *r = &g_postr[f % POST_RING];
        if (!post_claim(r, f)) continue;
        uint64_t start = r->t_commit;
        if (r->sc && r->seg_last > 0 && r->seg_last <= SEG_CLK_N) {
            uint64_t pe = atomic_load(&r->sc->t[r->seg_last - 1][1]); if (pe > start) start = pe;
        }
        if (start > now) start = now;
        post_run(r, f == fence && c1 ? c0 : seg_ts() ? start : 0, f == fence && c1 ? c1 : seg_ts() ? now : 0);
    }
    pthread_mutex_lock(&g_mu); if (fence > g_done) g_done = fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
}
uint64_t mtl_sub_commit(void) {
    lc_land();   // clears still waiting (the command list ends with them)
    defer_fallback();   // Deferred merged pass: at the end of the submission there are still P draws not replayed (the normal path never gets here)
    rp_flush(); ubd_land();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    g_cut_copy = 0;
    treg_finish_submission();   // Late descriptor views retire behind this submission's fence.
    uint64_t fence = ++g_fence;
    gw_commit(fence);
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    // CPU-visible effects after completion: copy the op table into the callback
    int n = g_npost; post_op *ops = n ? malloc(n * sizeof *ops) : NULL; post_rec *pr = &g_postr[fence % POST_RING];
    if (ws_on()) for (int i = 0; i < n; i++) { int k = g_post[i].kind == 2 ? (g_post[i].type == 0 || g_post[i].type == 0x10 ? 3 : 4) : g_post[i].kind == 3 ? 2 : 1; atomic_fetch_add(&g_ws_post[k], 1); }   // 1 event, 2 samples passed, 3 timestamp, 4 other counter
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; for (dispatch_block_t block in commit_blocks) block(); }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    seg_clk *sc = g_sclk; uint32_t seg_last = g_seg; g_sclk = NULL;
    pthread_mutex_lock(&g_post_mu);
    if (!atomic_load(&pr->claimed) && pr->fence) MDIE("post ring: the writes of fence %llu never ran", (unsigned long long)pr->fence);
    pr->n = n; pr->ops = ops; pr->sc = sc; pr->seg_last = seg_last; pr->t_commit = guest_ns(); atomic_store(&pr->claimed, 0); atomic_store(&pr->fence, fence);
    pthread_mutex_unlock(&g_post_mu);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (ws_on()) ws_fb(fence, cb.GPUEndTime);
        uint64_t c0 = 0, c1 = 0; if (seg_ts()) { c0 = gpu_s_guest_ns(cb.GPUStartTime); c1 = gpu_s_guest_ns(cb.GPUEndTime); }
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int n, nv; int victim = [cb.error.description containsString:@"InnocentVictim"]; if (victim ? nv++ < 1 : n++ < 8) MLOG("[RED] command buffer error details fence=%llu: %s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime); if (!victim) MLOG("[RED] state of the failed submission fence=%llu: %s", (unsigned long long)fence, cctx.UTF8String); MLOG("[RED] command buffer execution error: %s", cb.error.localizedDescription.UTF8String); }
        post_complete(fence, c0, c1);
        defer_drain();
    }];
    void arena_commit(uint64_t fence); arena_commit(fence);
    res_commit();
    if (g_gp_state == 1 && !gp_active()) { int gn = g_gp_n; gp_lab *gl = malloc(sizeof(gp_lab) * (gn ? gn : 1)); memcpy(gl, g_gp_lab, sizeof(gp_lab) * gn); uint64_t gf = g_gp_frame; g_gp_state = 2; atomic_store(&g_gp_busy, 1);
        [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; gp_report(gn, gl, gf); }]; }
    [g_cb endCommandBuffer];
    rec_commit(g_cb, o);
    rec_signal(g_ev, fence);
    rec_drain();
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    g_commit_blocks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

static int fast_wake(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_FAST_WAKE"); v = !(e && !strcmp(e, "0")); MLOG("game SyncWaits wake on the GPU's signal of the fence%s (DM_NVN_FAST_WAKE)", v ? "" : ": off, on the feedback handler"); } return v; }
// A SyncWait woken by the GPU's signal: the CPU-side writes of every submission up to the fence that no feedback handler has run yet, run here. The fence's last segment ended
// now (c1); it started at the later of its commit and the previous segment's end (c0), as near as the CPU can tell without its feedback
static void post_early(uint64_t fence) {
    post_complete(fence, 0, 0);
}
static int mtl_wait_at(uint64_t fence, uint64_t timeout_ns, int site) {
    if (g_done >= fence) { if (ws_on()) ws_put(site, 0, 0); return 1; }
    uint64_t w0 = ws_t0();
    if ((site == WS_SYNC || site == WS_SYNC_NEWEST || site == WS_ACQ) && fast_wake() && g_ev && fence <= g_fence) {   // the game's own wait: wake on the GPU's signal (rec_signal(g_ev, fence) after each submission)
        uint64_t ms = timeout_ns / 1000000ull; if (ms > 3600000ull) ms = 3600000ull;
        if ([g_ev waitUntilSignaledValue:fence timeoutMS:ms]) post_early(fence);
        int ok = g_done >= fence;
        if (ws_on()) { ws_put(site, w0, 1); ws_wake(fence); }
        if (ok || timeout_ns < 1000000ull) return ok;
        // The event and feedback waits share a finite budget; a timeout must not buy another full wait.
        if (timeout_ns != UINT64_MAX) {
            uint64_t used = ws_t0() - w0;
            if (used >= timeout_ns) return 0;
            timeout_ns -= used;
        }
    }
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    while (g_done < fence) if ((r = pthread_cond_timedwait(&g_cv, &g_mu, &dl))) break;
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    if (ws_on()) { ws_put(site, w0, 1); if (site == WS_SYNC_NEWEST || site == WS_SYNC || site == WS_ACQ) ws_wake(fence); }
    return ok;
}
// The game's SyncWait (hle_nvn.c), with what it waits for in the wait ledger
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : fence >= g_fence ? WS_SYNC_NEWEST : WS_SYNC); }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : WS_ACQ); }

static void mtl_finish_gpu_at(int site) {
    if (g_fence) mtl_wait_at(g_fence, UINT64_MAX, site);
    if (on_sub_thread()) rec_publish();   // a batch's commit may still be a record in the encode thread's current block (rec_commit_upload), as in up_begin and q_cb
    up_wait_zero(&g_up_pending, site);
}
#define mtl_finish_gpu() mtl_finish_gpu_at(WS_DIAG)
void mtl_present_flush(void);
// nvnQueueFinish waits for the GPU work submitted so far, not for queued presents to reach the screen: with a window, the display link takes one present per tick, so waiting for them
// made every QueueFinish cost one or two display frames. Fire Emblem: Three Houses' loader submits a copy and calls QueueFinish for each resource it uploads, and its first loading screen
// took 78 s on the iPhone (5 s windowless on the Mac). A swapchain texture is not handed back before its previous present is processed (AcquireTexture waits for that ticket)
void mtl_finish(void) { mtl_finish_gpu_at(WS_FINISH); }
static void present_drop_for_suspend(void);
static void snap_drain(void);
void mtl_drain(void) { present_drop_for_suspend(); mtl_present_flush(); snap_drain(); mtl_init(); id<MTLCommandBuffer> cb = q_cb(); [cb commit]; [cb waitUntilCompleted]; mtl_finish_gpu(); }

