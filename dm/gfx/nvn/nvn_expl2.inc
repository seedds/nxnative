// expl2 (PRAGMATA geometry "explosions", 2026-11-07): three diagnostics / candidate fixes, all off by default. Included by nvn_mtl.m after nvn_geometry.inc; the hooks are
// declared near nvn_rec.h (x2_bound from mtl_gpu_addr / labelled mtl_addr, x2_buf_made from nocopy_buf, af_begin / af_end around the MTL4 encoders, af_write from sbw_note / pend_add).
// Sources: emu-scan2-2026-11-07.md items 2 (KyTx metalOrder.h encoder fences), 3 (citron-neo #372 / #449 non-contiguous host backing), 5 (citron-neo #368 remap without
// cache invalidation); explosions-2026-11-07.md §6 (bindings resolved at encode, never refreshed after a sparse remap).
//
// DM_NVN_VA_AUDIT=1: every binding the GPU will use (mtl_gpu_addr; mtl_addr for copies, fills and inline updates) inside a sparse pool is walked page by page against the page
//   table (hle_nvn.c nvn_sparse_va_audit): the host memory it reads must follow the guest pages in order. 'o' a page read at the wrong host address (pieces mapped out of order, a
//   physical pool's buffer past its piece), 's' a zero-copy buffer over the virtual range made before a page under it was remapped (it wired the old pages), 'h' a hole read through
//   another range. Log only: [va-audit] lines (key, stage, lookup class, call site, range, first discontinuity) and a summary every 600 submissions. Audits are cached by
//   (address, length, host address, sparse generation), so a binding is walked again only after a mapping changed. DM_NVN_VA_AUDIT_MAX_MB (default 64) caps the walk.
// DM_NVN_REMAP_REFRESH=1: a sparse request that replaces mapped pages (remap A→B, or unmap) is noted by vpool_map once complete (mtl_remap_note): the pool and registration
//   generations are bumped at once, so every cached resolution keyed by them (the per-thread pool cache pc_get, the vertex buffer cache, pool_run, vbuf_unaligned, argument-buffer
//   reuse, render-pass reuse, the pool table) is resolved again at the latest at the next encode; the encoding thread then (x2_rr_drain) marks its resolved bindings that cover the
//   range, drops kept index ranges there (ix_range), and counts: covered (refreshed), re-resolved to a different buffer (changed), and the ones encoded in the open submission
//   before the remap (in flight: not fixable here, Metal has them already). [remap-refresh] lines.
// DM_NVN_ALIAS_FENCE=1 (KyTx metalOrder.h, MTL4): each draw pass / compute / copy encoder of a submission keeps the guest ranges it binds with the Metal buffer it binds them
//   through (and whether a write was noted on them). When an encoder reaches a range an earlier encoder of the submission reached through a DIFFERENT buffer and one of the two
//   writes it, the earlier encoder's MTLFence (updated at its end) is waited for before the later one's next commands (MTL4 waitForFence:beforeEncoderStages:). The host's
//   queue barriers already order encoders; this adds the fence edge KyTx needed with aliased views. Not with DM_NVN_ENC_THREAD (proxies). [alias-fence] lines.
static int va_audit_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VA_AUDIT"); v = e && atoi(e) > 0; if (v) MLOG("DM_NVN_VA_AUDIT=1: sparse bindings are checked against the page table at bind time ([va-audit] lines; log only)"); } return v; }
static int rr_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_REMAP_REFRESH"); v = e && atoi(e) > 0; if (v) MLOG("DM_NVN_REMAP_REFRESH=1: a sparse remap of mapped pages invalidates the cached resolved bindings over it ([remap-refresh] lines)"); } return v; }
static int af_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ALIAS_FENCE"); v = e && atoi(e) > 0;
    if (v && rec_on()) { MLOG("DM_NVN_ALIAS_FENCE=1 ignored: the encoders are recorded for the encoding thread (DM_NVN_ENC_THREAD)"); v = 0; }
    else if (v) MLOG("DM_NVN_ALIAS_FENCE=1: encoders reaching a guest range an earlier encoder of the submission reached through another Metal buffer wait for its fence ([alias-fence] lines)"); } return v; }
static int x2_any(void) { int v = va_audit_on() || rr_on() || af_on(); g_x2_any = v; return v; }
int mtl_remap_wanted(void) { return rr_on() || va_audit_on(); }

// ---------------- DM_NVN_REMAP_REFRESH ----------------
enum { RR_N = 4096, RRT = 8192 };
static pthread_mutex_t g_rr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_rr_lo[RR_N], g_rr_hi[RR_N]; static _Atomic uint64_t g_rr_seq;   // the ring of noted ranges (under g_rr_mu); seq: noted so far
static _Atomic uint64_t g_rr_remaps, g_rr_unmaps, g_rr_pages;
static uint64_t g_rr_seen, g_rr_covered, g_rr_inflight, g_rr_changed, g_rr_same, g_rr_ix, g_rr_overrun, g_rr_lines;   // encoding thread
static struct { uint64_t addr, len, off, sub; const void *buf; uint8_t flagged, inflight; } g_rrt[RRT];   // bindings resolved on the encoding thread (direct-mapped)
static inline unsigned rrt_slot(uint64_t a, uint64_t n) { return (unsigned)(((a >> 4) ^ (n * 0x9E3779B97F4A7C15ull)) * 0x9E3779B97F4A7C15ull >> 51) & (RRT - 1); }
void mtl_remap_note(uint64_t lo, uint64_t n, int kind, uint64_t pages) {   // any thread; hle_nvn.c vpool_map under g_vp_mu, after the request is complete
    if (!rr_on() && !va_audit_on()) return;
    atomic_fetch_add(kind == 'u' ? &g_rr_unmaps : &g_rr_remaps, 1); atomic_fetch_add(&g_rr_pages, pages);
    if (!rr_on()) return;
    atomic_fetch_add(&g_pool_gen, 1); atomic_fetch_add(&g_reg_gen, 1);   // every cache keyed by them resolves again at the next lookup (see the head of this file)
    pthread_mutex_lock(&g_rr_mu); uint64_t s = atomic_load(&g_rr_seq); g_rr_lo[s % RR_N] = lo; g_rr_hi[s % RR_N] = lo + n; atomic_store(&g_rr_seq, s + 1); pthread_mutex_unlock(&g_rr_mu);
}
static void rr_line(const char *why) {
    MLOG("[remap-refresh] %s: sparse requests replacing mapped pages %llu remaps + %llu unmaps (%llu pages); resolved bindings covering them %llu refreshed, %llu of them re-resolved to another buffer, %llu to the same; %llu encoded in the open submission before the remap (in flight, not refreshable); kept index ranges dropped %llu; ring overruns %llu",
         why, (unsigned long long)atomic_load(&g_rr_remaps), (unsigned long long)atomic_load(&g_rr_unmaps), (unsigned long long)atomic_load(&g_rr_pages), (unsigned long long)g_rr_covered,
         (unsigned long long)g_rr_changed, (unsigned long long)g_rr_same, (unsigned long long)g_rr_inflight, (unsigned long long)g_rr_ix, (unsigned long long)g_rr_overrun);
}
static void rr_cover(uint64_t lo, uint64_t hi) {   // encoding thread: one noted range
    for (unsigned i = 0; i < RRT; i++) { if (!g_rrt[i].buf || g_rrt[i].flagged || g_rrt[i].addr >= hi || g_rrt[i].addr + (g_rrt[i].len ? g_rrt[i].len : 1) <= lo) continue;
        g_rrt[i].flagged = 1; g_rr_covered++;
        if (g_rrt[i].sub == g_sub_serial) { g_rrt[i].inflight = 1; g_rr_inflight++; } }
    if (g_ir.n && vc_rgn_hit(lo, hi - lo)) for (uint32_t i = 0; i < g_ir.cap; i++) { ir_ent *e = g_ir.vals[i]; if (e && !e->gpuw && lo < e->lo + e->n && hi > e->lo) { e->gpuw = 1; g_rr_ix++; } }
}
static void x2_rr_drain(void) {   // encoding thread: the ranges noted since the last drain
    uint64_t s = atomic_load(&g_rr_seq); if (s == g_rr_seen) return;
    uint64_t c0 = g_rr_covered;
    if (s - g_rr_seen > RR_N) { g_rr_overrun++; rr_cover(0, UINT64_MAX); g_rr_seen = s; }
    else { pthread_mutex_lock(&g_rr_mu); for (; g_rr_seen < s; g_rr_seen++) { uint64_t lo = g_rr_lo[g_rr_seen % RR_N], hi = g_rr_hi[g_rr_seen % RR_N]; pthread_mutex_unlock(&g_rr_mu); rr_cover(lo, hi); pthread_mutex_lock(&g_rr_mu); } pthread_mutex_unlock(&g_rr_mu); }
    if (g_rr_covered != c0) { uint64_t n = ++g_rr_lines; if (n <= 8 || (n & (n - 1)) == 0) rr_line("covered bindings"); }
}
static void rr_record(uint64_t addr, uint64_t len, const void *buf, uint64_t off) {
    unsigned i = rrt_slot(addr, len);
    if (g_rrt[i].buf && g_rrt[i].addr == addr && g_rrt[i].len == len && g_rrt[i].flagged) { if (g_rrt[i].buf != buf || g_rrt[i].off != off) g_rr_changed++; else g_rr_same++; }
    g_rrt[i].addr = addr; g_rrt[i].len = len; g_rrt[i].off = off; g_rrt[i].buf = buf; g_rrt[i].sub = g_sub_serial; g_rrt[i].flagged = g_rrt[i].inflight = 0;
}

// ---------------- DM_NVN_VA_AUDIT ----------------
static char g_va_gen_key;   // associated object key: the sparse generation a zero-copy buffer was made at
static void x2_buf_made(id<MTLBuffer> b) { if (va_audit_on()) objc_setAssociatedObject(b, &g_va_gen_key, @(nvn_sparse_gen()), OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
static uint64_t va_buf_gen(id<MTLBuffer> b) { NSNumber *n = objc_getAssociatedObject(b, &g_va_gen_key); return n ? n.unsignedLongLongValue : UINT64_MAX; }
enum { VAC = 16384 };
static struct { uint64_t addr, len, h, gen; } g_vac[VAC];
static uint64_t g_va_seen, g_va_walked, g_va_sparse, g_va_multi, g_va_desc, g_va_bad[3], g_va_bad_pages, g_va_lines, g_va_desc_lines, g_va_stat_sub;
static void va_site(const void *site, char *out, size_t cap) {
    Dl_info di; if (site && dladdr(site, &di) && di.dli_fbase) { const char *nm = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL; snprintf(out, cap, "%s+%#lx", nm ? nm + 1 : "?", (unsigned long)((uintptr_t)site - (uintptr_t)di.dli_fbase)); }
    else snprintf(out, cap, "%p", site);
}
static void va_stat_line(const char *why) {
    MLOG("[va-audit] %s: bindings seen %llu, walked %llu, in sparse pools %llu; in several physical pieces %llu (with descending pieces %llu); BAD: order %llu, stale %llu, hole %llu (%llu pages); sparse requests replacing mapped pages %llu remaps + %llu unmaps",
         why, (unsigned long long)g_va_seen, (unsigned long long)g_va_walked, (unsigned long long)g_va_sparse, (unsigned long long)g_va_multi, (unsigned long long)g_va_desc,
         (unsigned long long)g_va_bad[0], (unsigned long long)g_va_bad[1], (unsigned long long)g_va_bad[2], (unsigned long long)g_va_bad_pages,
         (unsigned long long)atomic_load(&g_rr_remaps), (unsigned long long)atomic_load(&g_rr_unmaps));
}
static void va_audit(uint64_t addr, uint64_t len, void *buf, uint64_t off, const char *what, const void *site, int gpu) {
    static uint64_t maxl; if (!maxl) { const char *e = getenv("DM_NVN_VA_AUDIT_MAX_MB"); maxl = (uint64_t)(e && atol(e) > 0 ? atol(e) : 64) << 20; }
    g_va_seen++;
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)buf; uint64_t blen = b.length, n = len > 1 ? len : (blen > off ? MIN(blen - off, 1u << 20) : 1);   // address only: up to 1 MB of the buffer
    if (n > maxl) n = maxl;
    uint64_t h = (uint64_t)(uintptr_t)b.contents + off, gen = nvn_sparse_gen();
    unsigned i = (unsigned)(((addr >> 4) ^ (n * 0x9E3779B97F4A7C15ull) ^ (h >> 4) * 0xC2B2AE3D27D4EB4Full) * 0x9E3779B97F4A7C15ull >> 50) & (VAC - 1);
    if (g_vac[i].addr == addr && g_vac[i].len == n && g_vac[i].h == h && g_vac[i].gen == gen) return;
    g_vac[i].addr = addr; g_vac[i].len = n; g_vac[i].h = h; g_vac[i].gen = gen;
    if (!b.contents) return;
    g_va_walked++;
    nvn_va_audit_t r; int bad = nvn_sparse_va_audit(addr, n, h, va_buf_gen(b), &r);
    if (bad < 0) return;
    g_va_sparse++; if (r.pieces > 1) g_va_multi++; if (r.desc) g_va_desc++;
    if (!bad) {
        if (r.desc && g_va_desc_lines < 16) { g_va_desc_lines++; char st[96]; va_site(site, st, sizeof st);
            MLOG("[va-audit] ok-desc key=%s st=%d bind=%s%s site=%s range=%#llx+%#llx: %d physical pieces, %d descending, %d holes; the binding reads the virtual range (host %#llx), so in order",
                 g_enc_key ? g_enc_key : "-", g_enc_stage, what, gpu ? "" : "(blit)", st, (unsigned long long)addr, (unsigned long long)n, r.pieces, r.desc, r.holes, (unsigned long long)h); }
        return; }
    int k = r.kind == 'o' ? 0 : r.kind == 's' ? 1 : 2; g_va_bad[k]++; g_va_bad_pages += (uint64_t)bad;
    uint64_t nb = g_va_bad[0] + g_va_bad[1] + g_va_bad[2];
    if (nb <= 64 || (nb & (nb - 1)) == 0) { char st[96]; va_site(site, st, sizeof st);
        MLOG("[va-audit] BAD %s key=%s st=%d bind=%s%s how=%c site=%s range=%#llx+%#llx buf=%p+%#llx(len %#llx, made at sparse gen %lld) first discontinuity at +%#llx: guest page -> host %#llx, binding reads %#llx (page gen %llu); %d bad pages, %d physical pieces, %d descending, %d holes (%llu bad so far)",
             r.kind == 'o' ? (r.first ? "order" : "foreign") : r.kind == 's' ? "stale" : "hole", g_enc_key ? g_enc_key : "-", g_enc_stage, what, gpu ? "" : "(blit)", t_res_how ? t_res_how : '-', st,
             (unsigned long long)addr, (unsigned long long)n, buf, (unsigned long long)off, (unsigned long long)blen, (long long)(va_buf_gen(b) == UINT64_MAX ? -1 : (long long)va_buf_gen(b)),
             (unsigned long long)r.first, (unsigned long long)r.expect, (unsigned long long)r.got, (unsigned long long)r.pgen, bad, r.pieces, r.desc, r.holes, (unsigned long long)nb); }
}

// ---------------- DM_NVN_ALIAS_FENCE ----------------
enum { AF_ACC = 256, AF_HIST = 16, AF_FN = 64 };
typedef struct { uint64_t lo, hi; const void *obj; uint8_t w; } af_acc;
typedef struct { af_acc a[AF_ACC]; int n, over; int fence; char type; uint64_t seq, bloom[4]; } af_enc;
static af_enc g_af_hist[AF_HIST]; static int g_af_nhist; static unsigned g_af_hnext;   // the ended encoders of this submission (newest at g_af_hnext - 1)
static af_enc g_af_rec;   // the open encoder's (or, between encoders, the next one's) accesses
static __unsafe_unretained id g_af_cur; static char g_af_type;   // the open encoder this file saw begin (nil: none, or one it does not hook)
static uint64_t g_af_seq, g_af_waited[AF_HIST]; static int g_af_nwaited;   // fences the open encoder waited for (encoder seqs)
static uint64_t g_af_pend[AF_HIST]; static int g_af_npend;   // fences to wait for when the next encoder begins (conflicts found between encoders)
static __strong id<MTLFence> g_af_fence[AF_FN];
static uint64_t g_af_conf_raw, g_af_conf_war, g_af_waits, g_af_waits_begin, g_af_updates, g_af_over, g_af_lines, g_af_encs;
static inline uint64_t af_bit(uint64_t r) { return 1ull << (((r * 0x9E3779B97F4A7C15ull) >> 56) & 63); }
static void af_bloom(uint64_t *bl, uint64_t lo, uint64_t hi) { if (((hi - 1) >> 20) - (lo >> 20) > 64) { bl[0] = bl[1] = bl[2] = bl[3] = ~0ull; return; } for (uint64_t r = lo >> 20; r <= (hi - 1) >> 20; r++) bl[r & 3] |= af_bit(r); }   // 1 MB regions
static int af_bloom_hit(const uint64_t *bl, uint64_t lo, uint64_t hi) { if (((hi - 1) >> 20) - (lo >> 20) > 64) return 1; for (uint64_t r = lo >> 20; r <= (hi - 1) >> 20; r++) if (bl[r & 3] & af_bit(r)) return 1; return 0; }
static int af_mtl4(id e) { return [e respondsToSelector:@selector(waitForFence:beforeEncoderStages:)]; }   // (t_expl2 passes plain objects)
static MTLStages af_stages(char t) { return t == 'R' ? MTLStageVertex | MTLStageObject | MTLStageMesh | MTLStageFragment : MTLStageDispatch | MTLStageBlit; }
static void af_line(const char *why) {
    MLOG("[alias-fence] %s: encoders tracked %llu; conflicts through different Metal buffers: write->access %llu, access->write %llu; fence waits inserted %llu (%llu at an encoder's begin), fence updates %llu; access lists full %llu",
         why, (unsigned long long)g_af_encs, (unsigned long long)g_af_conf_raw, (unsigned long long)g_af_conf_war, (unsigned long long)(g_af_waits + g_af_waits_begin), (unsigned long long)g_af_waits_begin,
         (unsigned long long)g_af_updates, (unsigned long long)g_af_over);
}
static void af_wait(const af_enc *p) {   // the open encoder (or the next one) waits for p's fence
    for (int i = 0; i < g_af_nwaited; i++) if (g_af_waited[i] == p->seq) return;
    if (g_af_cur) { if (p->fence >= 0 && g_af_fence[p->fence] && af_mtl4(g_af_cur)) { [(id<MTL4CommandEncoder>)g_af_cur waitForFence:g_af_fence[p->fence] beforeEncoderStages:af_stages(g_af_type)]; g_af_waits++; } }
    else { int dup = 0; for (int i = 0; i < g_af_npend; i++) dup |= g_af_pend[i] == p->seq; if (!dup && g_af_npend < AF_HIST) g_af_pend[g_af_npend++] = p->seq; }
    if (g_af_nwaited < AF_HIST) g_af_waited[g_af_nwaited++] = p->seq;
    uint64_t n = ++g_af_lines; if (n <= 4 || (n & (n - 1)) == 0) af_line("fence");
}
static void af_check(const af_acc *x) {   // against the ended encoders, newest first
    for (int k = 0; k < g_af_nhist; k++) { const af_enc *p = &g_af_hist[(g_af_hnext - 1 - (unsigned)k) % AF_HIST];
        if (!af_bloom_hit(p->bloom, x->lo, x->hi)) continue;
        for (int j = 0; j < p->n; j++) { const af_acc *y = &p->a[j];
            if (y->lo >= x->hi || x->lo >= y->hi || y->obj == x->obj || !y->obj || !x->obj || !(y->w || x->w)) continue;
            if (y->w) g_af_conf_raw++; else g_af_conf_war++;
            af_wait(p); break; } }
}
static void af_access(uint64_t lo, uint64_t len, const void *obj) {
    uint64_t hi = lo + (len > 1 ? len : 1);
    for (int i = g_af_rec.n - 1; i >= 0 && i >= g_af_rec.n - 8; i--) { af_acc *y = &g_af_rec.a[i];   // the same buffer again (or adjacent): one entry
        if (y->obj == obj && lo <= y->hi && y->lo <= hi) { if (lo >= y->lo && hi <= y->hi) return; y->lo = MIN(lo, y->lo); y->hi = MAX(hi, y->hi); af_bloom(g_af_rec.bloom, y->lo, y->hi); af_check(y); return; } }
    if (g_af_rec.n == AF_ACC) { if (!g_af_rec.over++) g_af_over++; return; }
    af_acc *x = &g_af_rec.a[g_af_rec.n++]; *x = (af_acc){lo, hi, obj, 0}; af_bloom(g_af_rec.bloom, lo, hi); af_check(x);
}
static void af_write(uint64_t lo, uint64_t n) {   // a write noted on [lo, lo + n): the open encoder's accesses there write
    if (!af_on() || !on_sub_thread()) return;
    uint64_t hi = lo + n; if (hi < lo) hi = UINT64_MAX;
    for (int i = 0; i < g_af_rec.n; i++) { af_acc *y = &g_af_rec.a[i]; if (y->w || y->lo >= hi || lo >= y->hi) continue; y->w = 1; af_check(y); }
}
static void af_reset(void) { g_af_nhist = 0; g_af_hnext = 0; g_af_rec.n = g_af_rec.over = 0; memset(g_af_rec.bloom, 0, sizeof g_af_rec.bloom); g_af_cur = nil; g_af_nwaited = g_af_npend = 0; }
static void af_begin(id e, char type) {
    if (!af_on() || !on_sub_thread()) return;
    if (!g_af_fence[0]) for (int i = 0; i < AF_FN; i++) g_af_fence[i] = [g_dev newFence];
    g_af_cur = e; g_af_type = type; g_af_nwaited = 0; g_af_encs++;
    for (int i = 0; i < g_af_npend; i++) for (int k = 0; k < g_af_nhist; k++) { const af_enc *p = &g_af_hist[(g_af_hnext - 1 - (unsigned)k) % AF_HIST];   // conflicts found before it began
        if (p->seq != g_af_pend[i] || p->fence < 0 || !af_mtl4(e)) continue;
        [(id<MTL4CommandEncoder>)e waitForFence:g_af_fence[p->fence] beforeEncoderStages:af_stages(type)]; g_af_waits_begin++; if (g_af_nwaited < AF_HIST) g_af_waited[g_af_nwaited++] = p->seq; }
    g_af_npend = 0;
}
static void af_end(id e) {
    if (!af_on() || !on_sub_thread() || !g_af_cur || e != g_af_cur) return;
    af_enc *h = &g_af_hist[g_af_hnext % AF_HIST]; *h = g_af_rec; h->seq = ++g_af_seq; h->type = g_af_type; h->fence = -1;
    if (g_af_rec.n) { h->fence = (int)(h->seq % AF_FN); if (af_mtl4(e)) { [(id<MTL4CommandEncoder>)e updateFence:g_af_fence[h->fence] afterEncoderStages:af_stages(g_af_type)]; g_af_updates++; }
        g_af_hnext++; if (g_af_nhist < AF_HIST) g_af_nhist++; }
    g_af_rec.n = g_af_rec.over = 0; memset(g_af_rec.bloom, 0, sizeof g_af_rec.bloom); g_af_cur = nil; g_af_nwaited = 0;
}

// ---------------- common ----------------
static void x2_bound(uint64_t addr, uint64_t len, void *buf, uint64_t off, const char *what, const void *site, int gpu) {
    if (!buf || !on_sub_thread()) return;
    if (rr_on()) { x2_rr_drain(); rr_record(addr, len, buf, off); }
    if (va_audit_on()) va_audit(addr, len, buf, off, what, site, gpu);
    if (af_on()) af_access(addr, len, buf);
}
static void x2_sub_begin(void) {
    if (rr_on()) x2_rr_drain();
    if (af_on()) af_reset();
    if (g_sub_serial - g_va_stat_sub >= 600) { g_va_stat_sub = g_sub_serial;
        if (va_audit_on()) va_stat_line("summary"); if (rr_on()) rr_line("summary"); if (af_on()) af_line("summary"); }
}
