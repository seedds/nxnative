// race-diag (2026-11-07; included by nvn_mtl.m; every switch off by default). Diagnostics for PRAGMATA's light flicker (f1b16a8a's CPU-written light list s0 / shadow records s2)
// and the other CPU-written read-only storage ranges SB_FREEZE_AUTO caught changing under a submission (ex_sba: 1c844d9c, 917eeb4a, 5aab38bb, 4f11e16c, ...):
//
// DM_NVN_WRITEWATCH=<guest addr>:<len>[,<addr>:<len>...] | 1 (= f1b16a8a's s0 0x721e9f6600:0x400 and s2 0x72375f2200:0x800 of the shB1 run; addresses can move between runs)
// DM_NVN_WRITEWATCH_KEY=<key prefix>:<binding>[,<binding>...][;<key prefix>:...]   binding: s0 / s1 / s2 (f1b16a8a's names: push +64 / +80 / +16), +<push offset> (any
//     uniform-addressed storage binding, NVN_RB_SBUFU) or p<reg> (a storage binding, NVN_RB_SBUFP). The range is armed on whatever address the dispatch binds; its length is
//     f1b16a8a's exact extent from this dispatch's s0 (as DM_NVN_SBU_SNAP computes it), the binding's size, or 4 KB
// DM_NVN_WRITEWATCH_AUTO=1   read-only storage ranges caught changing on the CPU while a submission reading them was in flight (SB_FREEZE_AUTO's probation, detect only:
//     nothing is frozen unless DM_NVN_SB_FREEZE_AUTO=1 is set too) are watched from then on
// DM_NVN_WRITEWATCH_ALL=1    keep a range armed after its first write (every store faults; default: the first write after each arm is caught, then the range rests until the next arm)
//
// What it does: the host pages (16 KB) under each range are write-protected ("armed") when the submission that read the range has been committed to Metal, and again at each
// present. The first CPU store into the range after that faults; the handler (crash.c's page-fault hook chain, after the dirty tracking's) logs the writer's pc / lr (module
// offsets: guest code runs natively, so the host pc is the guest pc), its thread (guest handle and Mach id), the frame, how long after the arm, and whether the GPU work that read
// the range had completed (its fence <= the completed fence) or was still in flight: a store while it is in flight is a CPU/GPU race on the host. It adds the writer thread's last
// CPU wait on the GPU (SyncWait / QueueFinish / EventGetValue: which fence or value, how long before the store). The store itself is carried out through a second writable
// mapping of the page (mach_vm_remap) and the page stays protected for the next store; stores the decoder does not know (atomics, exclusives) unprotect the page instead.
// Stores to other bytes of an armed page are carried out the same way, silently; a page with more than 4096 of them since its arm is unprotected until the next arm (counted "noisy").
// Pages the dirty tracking holds protected are never touched. A new zero-copy Metal buffer over an armed page, and a host write into guest memory (dm_guest_write_prepare),
// unprotect the page first.
// Log: [ww] lines (each new writer pc of a range, the first 64 events, every in-flight store for the first 256), and a per-range summary every 600 frames: arms, writes after the
// arm (in flight / after the reader completed / same frame), and the writer pcs with counts.
//
// DM_NVN_SERIALIZE (hle_nvn.c) waits with mtl_serialize_wait; DM_NVN_SUBMIT_AT_FLUSH_KEYS=<key prefix>[,...] (with SUBMIT_AT_FLUSH=3) asks mtl_saf_keys_hit whether the
// submission just encoded dispatched such a shader; DM_NVN_WAITS_EVERY=<frames> sets the wait ledger's period (default 300; 1 = every frame), and nvnEventGetValue polls are
// counted in it ("EventGetValue polls").
#include <mach/mach_vm.h>

// ---------- SERIALIZE ----------
void mtl_serialize_wait(void) {
    if (g_fence) mtl_wait_at(g_fence, UINT64_MAX, WS_SER);
    if (on_sub_thread()) rec_publish();
    up_wait_zero(&g_up_pending, WS_SER);
    if (g_fence && g_done < g_fence) mtl_wait_at(g_fence, UINT64_MAX, WS_SER);
}

// ---------- SUBMIT_AT_FLUSH_KEYS ----------
static char g_safk[8][24]; static int g_safk_n = -1; static int g_safk_hit;
static int safk_on(void) {
    if (g_safk_n < 0) { g_safk_n = 0; const char *e = getenv("DM_NVN_SUBMIT_AT_FLUSH_KEYS");
        for (const char *q = e; q && *q && g_safk_n < 8;) { size_t L = strcspn(q, ","); snprintf(g_safk[g_safk_n], sizeof g_safk[0], "%.*s", (int)(L < 23 ? L : 23), q); if (L) g_safk_n++; q += L; if (*q == ',') q++; }
        if (g_safk_n) MLOG("DM_NVN_SUBMIT_AT_FLUSH_KEYS: %d shader prefix(es) (first %s): with SUBMIT_AT_FLUSH=3 only a frame's submissions from the first one dispatching them are held a frame", g_safk_n, g_safk[0]); }
    return g_safk_n;
}
static void safk_note(const char *key) { if (!safk_on() || !key) return; for (int i = 0; i < g_safk_n; i++) if (!strncmp(key, g_safk[i], strlen(g_safk[i]))) g_safk_hit = 1; }
int mtl_saf_keys_hit(void) { return safk_on() && g_safk_hit; }

// ---------- Wait ledger additions ----------
static int ws_every(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_WAITS_EVERY"); v = e && atoi(e) > 0 ? atoi(e) : 300; } return v; }
// The calling thread's last CPU wait on the GPU (read by the write-watch fault handler through pthread_getspecific: allocated outside it, on the thread's first wait)
typedef struct { int kind; int blocked; uint64_t fence, ns, frame; } ww_tw;
enum { WWT_NONE, WWT_SYNC, WWT_POLL, WWT_ACQ, WWT_FINISH, WWT_EVENT, WWT_SER };
static const char *const ww_wait_name[] = {"none", "SyncWait", "SyncWait poll", "acquire SyncWait", "QueueFinish", "EventGetValue", "serialize wait"};
static pthread_key_t g_ww_twkey; static _Atomic int g_ww_twkey_ok; static pthread_once_t g_ww_twonce = PTHREAD_ONCE_INIT;
static void ww_twkey_init(void) { if (!pthread_key_create(&g_ww_twkey, free)) atomic_store(&g_ww_twkey_ok, 1); }
static void ww_note_wait(int kind, uint64_t fence, int blocked) {
    pthread_once(&g_ww_twonce, ww_twkey_init);
    if (!atomic_load(&g_ww_twkey_ok)) return;
    ww_tw *t = pthread_getspecific(g_ww_twkey); if (!t) { t = calloc(1, sizeof *t); if (!t) return; pthread_setspecific(g_ww_twkey, t); }
    t->kind = kind; t->blocked = blocked; t->fence = fence; t->ns = now_ns(); t->frame = atomic_load(&g_cur_frame);
}
void mtl_ws_event_poll(const void *ev, uint32_t val) {
    (void)ev; if (ws_on()) ws_put(WS_EVPOLL, 0, 0);
    if (WW_ON()) ww_note_wait(WWT_EVENT, val, 0);
}

// ---------- WRITEWATCH ----------
enum { WW_R = 64, WW_PG = 1024, WW_PCS = 8, WW_EV = 4096, WW_NOISE = 4096, WW_MAXLEN = 1u << 20 };
typedef struct { _Atomic uint64_t pc, lr, n, inflight; _Atomic int logged; } ww_pc;
typedef struct { uint64_t lo, n; char key[17]; char what[12]; int src;   // src: 0 static, 1 key, 2 auto
    _Atomic uint64_t pend, reader, arm_frame, arm_ns; _Atomic int armed;
    _Atomic uint64_t arms, hits, hits_inflight, hits_after, hits_same, atomics, noisy, dirty_skip; ww_pc pcs[WW_PCS];
    _Atomic int freeze_req, lfx, dirty; _Atomic uint64_t lfx_faults, lfx_pc; } ww_range;   // DM_NVN_SB_FREEZE_WW (lightfix): freeze requested by an in-flight CPU store / frozen with versions / CPU-written since its last copy
typedef struct { uintptr_t pg; uint8_t *alias; _Atomic int prot; _Atomic uint32_t noise; } ww_page;
typedef struct { _Atomic uint64_t seq; int r, atomic_, wkind, wblocked; uint32_t thr, mtid, len; uint64_t pc, lr, ea, frame, reader, done, committed, arm_frame, since_ns, wfence, wago_ns, wframe; } ww_ev;
static ww_range g_wwr[WW_R]; static _Atomic int g_wwr_n;
static ww_page g_wwp[WW_PG]; static _Atomic int g_wwp_n;
static ww_ev g_wwe[WW_EV]; static _Atomic uint64_t g_wwe_w, g_wwe_dropped; static uint64_t g_wwe_r;
static pthread_mutex_t g_ww_mu = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t g_ww_psz = 16384; static int g_ww_all, g_ww_auto;
static struct { char key[24]; int nb; char b[8][8]; } g_wwk[4]; static int g_wwk_n;
static _Atomic uint64_t g_ww_faults, g_ww_emul, g_ww_open;
typedef uint32_t (*ww_thr_fn)(void); static ww_thr_fn g_ww_thr;   // hle_thread.c thr_cur_handle, found at run time (absent in the unit tests)
static int ww_fault(void *addr, int is_write, void *ucv);
static void ww_prep(void *addr, size_t len);
static int ww_add(uint64_t lo, uint64_t n, int src, const char *key, const char *what);
static int ww_auto_on(void) { return WW_ON() && g_ww_auto; }
static int ww_any(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
    if (g_ww_any >= 0) { pthread_mutex_unlock(&mu); return g_ww_any; }
    const char *s = getenv("DM_NVN_WRITEWATCH"), *k = getenv("DM_NVN_WRITEWATCH_KEY"), *a = getenv("DM_NVN_WRITEWATCH_AUTO"), *al = getenv("DM_NVN_WRITEWATCH_ALL");
    int on_s = s && *s && strcmp(s, "0"), on_k = k && *k, on_a = (a && *a && strcmp(a, "0")) || lfx_on();
    g_ww_all = al && *al && strcmp(al, "0"); g_ww_auto = on_a; g_ww_psz = (uintptr_t)getpagesize();
    for (const char *q = on_k ? k : NULL; q && *q && g_wwk_n < 4;) {   // key:b,b;key:b
        size_t L = strcspn(q, ";"); char t[128]; snprintf(t, sizeof t, "%.*s", (int)(L < 127 ? L : 127), q); q += L; if (*q == ';') q++;
        char *c = strchr(t, ':'); if (c) *c++ = 0; snprintf(g_wwk[g_wwk_n].key, sizeof g_wwk[0].key, "%s", t); int nb = 0;
        for (char *b = c; b && *b && nb < 8;) { size_t m = strcspn(b, ","); snprintf(g_wwk[g_wwk_n].b[nb], 8, "%.*s", (int)(m < 7 ? m : 7), b); nb++; b += m; if (*b == ',') b++; }
        if (!nb) { snprintf(g_wwk[g_wwk_n].b[0], 8, "s0"); snprintf(g_wwk[g_wwk_n].b[1], 8, "s2"); nb = 2; }
        g_wwk[g_wwk_n].nb = nb; g_wwk_n++;
    }
    g_ww_any = on_s || g_wwk_n || on_a;
    pthread_mutex_unlock(&mu);
    if (!g_ww_any) return 0;
    g_ww_thr = (ww_thr_fn)dlsym(RTLD_DEFAULT, "thr_cur_handle");
    { static int reg; if (!reg++) { dm_fault_hook_register(ww_fault); dm_guest_write_prep_register(ww_prep); } }
    MLOG("DM_NVN_WRITEWATCH: static %s, keys %d (first %s), auto %d, all %d: CPU stores into the ranges after the reading submission's commit / a present are logged with the writer ([ww])",
         on_s ? s : "-", g_wwk_n, g_wwk_n ? g_wwk[0].key : "-", on_a, g_ww_all);
    if (on_s) {
        if (!strcmp(s, "1")) { ww_add(0x721e9f6600ull, 0x400, 0, "f1b16a8a", "s0 (default)"); ww_add(0x72375f2200ull, 0x800, 0, "f1b16a8a", "s2 (default)"); }
        else for (const char *q = s; *q;) { char *e; uint64_t lo = strtoull(q, &e, 16), n = 0x1000; if (*e == ':') n = strtoull(e + 1, &e, 0); if (lo) ww_add(lo, n, 0, "-", "static"); q = e; while (*q && *q != ',') q++; if (*q == ',') q++; }
    }
    return 1;
}
static int ww_page_find(uintptr_t pg) { int n = atomic_load(&g_wwp_n); for (int i = 0; i < n; i++) if (g_wwp[i].pg == pg) return i; return -1; }
// A range's pages, with a second writable mapping of each (caller holds g_ww_mu)
static int ww_pages_add(uint64_t lo, uint64_t n) {
    uintptr_t a = (uintptr_t)lo & ~(g_ww_psz - 1), b = ((uintptr_t)(lo + n) + g_ww_psz - 1) & ~(g_ww_psz - 1); int add = 0;
    for (uintptr_t p = a; p < b; p += g_ww_psz) {
        if (ww_page_find(p) >= 0) continue;
        int k = atomic_load(&g_wwp_n); if (k == WW_PG) return -1;
        mach_vm_address_t al = 0; vm_prot_t cur, max;
        if (mach_vm_remap(mach_task_self(), &al, g_ww_psz, 0, VM_FLAGS_ANYWHERE, mach_task_self(), p, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS || !(cur & VM_PROT_WRITE)) al = 0;
        g_wwp[k].pg = p; g_wwp[k].alias = (uint8_t *)(uintptr_t)al; atomic_store(&g_wwp[k].prot, 0); atomic_store(&g_wwp[k].noise, 0);
        atomic_store(&g_wwp_n, k + 1); add++;
        if (!al) MLOG("[ww] [RED] page %#lx: no second mapping; a store to it unprotects it (only the first write after each arm is caught)", (unsigned long)p);
    }
    return add;
}
static int ww_add(uint64_t lo, uint64_t n, int src, const char *key, const char *what) {
    if (!lo || !n) return -1; if (n > WW_MAXLEN) n = WW_MAXLEN;
    if (!nvn_host_readable(lo, n)) { static int nl; if (nl++ < 8) MLOG("[ww] [RED] %#llx+%#llx (%s %s) is not host memory: not watched", (unsigned long long)lo, (unsigned long long)n, key, what); return -1; }
    pthread_mutex_lock(&g_ww_mu);
    int nr = atomic_load(&g_wwr_n);
    for (int i = 0; i < nr; i++) if (g_wwr[i].lo == lo) { if (n > g_wwr[i].n && ww_pages_add(lo, n) >= 0) g_wwr[i].n = n; pthread_mutex_unlock(&g_ww_mu); return i; }
    if (nr == WW_R || ww_pages_add(lo, n) < 0) { pthread_mutex_unlock(&g_ww_mu); static int nl; if (nl++ < 4) MLOG("[ww] [RED] table full (%d ranges, %d pages): %#llx+%#llx (%s %s) not watched", WW_R, WW_PG, (unsigned long long)lo, (unsigned long long)n, key, what); return -1; }
    ww_range *r = &g_wwr[nr]; memset(r, 0, sizeof *r); r->lo = lo; r->n = n; r->src = src; snprintf(r->key, sizeof r->key, "%s", key ? key : "-"); snprintf(r->what, sizeof r->what, "%s", what ? what : "-");
    atomic_store(&g_wwr_n, nr + 1);
    pthread_mutex_unlock(&g_ww_mu);
    MLOG("[ww] R%d %s %s %#llx+%#llx watched (%s), frame %llu", nr, r->key, r->what, (unsigned long long)lo, (unsigned long long)n, src == 0 ? "static" : src == 1 ? "key binding" : "SB_FREEZE_AUTO detection", (unsigned long long)atomic_load(&g_cur_frame));
    return nr;
}
static int ww_overlaps(const ww_range *r, uintptr_t pg) { return r->lo < pg + g_ww_psz && r->lo + r->n > pg; }
static void ww_unprotect(ww_page *P) {   // also from the fault handler: atomics and mprotect only; a page the dirty tracking holds stays read-only (its own hook opens it)
    if (!atomic_exchange(&P->prot, 0)) return;
    atomic_fetch_add(&g_ww_open, 1);
    if (!nvn_dirty_page_protected((const void *)P->pg)) mprotect((void *)P->pg, g_ww_psz, PROT_READ | PROT_WRITE);
}
static void ww_arm(ww_range *r, uint64_t reader) {   // caller holds g_ww_mu
    atomic_store(&r->reader, reader); atomic_store(&r->arm_frame, atomic_load(&g_cur_frame)); atomic_store(&r->arm_ns, now_ns()); atomic_store(&r->armed, 1); atomic_fetch_add(&r->arms, 1);
    int n = atomic_load(&g_wwp_n);
    for (int i = 0; i < n; i++) { ww_page *P = &g_wwp[i]; if (!ww_overlaps(r, P->pg)) continue;
        if (nvn_dirty_page_protected((const void *)P->pg)) { atomic_fetch_add(&r->dirty_skip, 1); continue; }
        atomic_store(&P->noise, 0); atomic_store(&P->prot, 1); mprotect((void *)P->pg, g_ww_psz, PROT_READ); }   // always: the dirty tracking may have opened it since
}
static void ww_unprot_span(uintptr_t a, uintptr_t b) { int n = atomic_load(&g_wwp_n); for (int i = 0; i < n; i++) if (g_wwp[i].pg < b && g_wwp[i].pg + g_ww_psz > a) ww_unprotect(&g_wwp[i]); }
static void ww_prep(void *addr, size_t len) { if (len) ww_unprot_span((uintptr_t)addr, (uintptr_t)addr + len); }
// The store at pc: address, size, data, base write-back (the same decoder as host/probe.c's DM_PROBE_WATCH, which this layer does not link)
static int ww_decode(ucontext_t *uc, uint32_t in, uint64_t *ea, unsigned *len, uint8_t *data, int *wb, uint64_t *wbv) {
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
#define XN(r) ((r) == 31 ? ss->__sp : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])
#define XT(r) ((r) == 31 ? 0 : (r) == 29 ? ss->__fp : (r) == 30 ? ss->__lr : ss->__x[r])
    unsigned rt = in & 31, rn = (in >> 5) & 31, v = (in >> 26) & 1; *wb = -1;
    if ((in & 0xffffffe0) == 0xd50b7420) { *ea = XT(rt) & ~63ull; *len = 64; memset(data, 0, 64); return 1; }   // DC ZVA
    if ((in & 0x3a000000) == 0x28000000) {   // STP / STNP
        if (in & (1u << 22)) return 0;
        unsigned opc = in >> 30, type = (in >> 23) & 3, sz = v ? 4u << opc : (opc & 2 ? 8 : 4);
        if ((!v && opc == 1) || opc == 3) return 0;
        int64_t imm = ((int64_t)(int32_t)(in << 10) >> 25) * (int64_t)sz; uint64_t base = XN(rn);
        *ea = type == 1 ? base : base + imm; *len = 2 * sz; unsigned r2 = (in >> 10) & 31;
        if (v) { memcpy(data, &ns->__v[rt], sz); memcpy(data + sz, &ns->__v[r2], sz); } else { uint64_t x = XT(rt), y = XT(r2); memcpy(data, &x, sz); memcpy(data + sz, &y, sz); }
        if (type == 1 || type == 3) { *wb = (int)rn; *wbv = base + imm; }
        return 1;
    }
    if ((in & 0x3b000000) == 0x39000000 || (in & 0x3b200000) == 0x38000000 || (in & 0x3b200c00) == 0x38200800) {   // STR forms
        unsigned size = in >> 30, opc = (in >> 22) & 3, sz;
        if (v) { if (opc == 0) sz = 1u << size; else if (opc == 2 && size == 0) sz = 16; else return 0; } else { if (opc != 0) return 0; sz = 1u << size; }
        uint64_t base = XN(rn), addr;
        if ((in & 0x3b000000) == 0x39000000) addr = base + (uint64_t)((in >> 10) & 0xfff) * sz;
        else if (!(in & (1u << 21))) { int64_t imm = (int64_t)(int32_t)(in << 11) >> 23; unsigned k = (in >> 10) & 3; if (k == 2) return 0; addr = k == 1 ? base : base + imm; if (k == 1 || k == 3) { *wb = (int)rn; *wbv = base + imm; } }
        else { unsigned rm = (in >> 16) & 31, opt = (in >> 13) & 7, s = (in >> 12) & 1; uint64_t x = XT(rm);
            if (opt == 2) x = (uint32_t)x; else if (opt == 6) x = (uint64_t)(int64_t)(int32_t)x; else if (opt != 3 && opt != 7) return 0;
            addr = base + (s ? x << __builtin_ctz(sz) : x); }
        *ea = addr; *len = sz;
        if (v) memcpy(data, &ns->__v[rt], sz); else { uint64_t x = XT(rt); memcpy(data, &x, sz); }
        return 1;
    }
    return 0;
#undef XN
#undef XT
}
static void ww_record(int ri, ucontext_t *uc, uint64_t ea, unsigned len, int atomic_) {   // signal context: atomics only
    ww_range *r = &g_wwr[ri]; uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
    uint64_t reader = atomic_load(&r->reader), done = atomic_load(&g_done), fr = atomic_load(&g_cur_frame), af = atomic_load(&r->arm_frame);
    int inflight = reader > done;
    if (inflight && lfx_on() && !atomic_load(&r->lfx)) { atomic_store(&r->lfx_pc, pc); atomic_store(&r->freeze_req, 1); }   // DM_NVN_SB_FREEZE_WW: a proven CPU store under a GPU reader
    atomic_fetch_add(&r->hits, 1); atomic_fetch_add(inflight ? &r->hits_inflight : &r->hits_after, 1); if (fr == af) atomic_fetch_add(&r->hits_same, 1); if (atomic_) atomic_fetch_add(&r->atomics, 1);
    for (int i = 0; i < WW_PCS; i++) { uint64_t z = 0, cur = atomic_load(&r->pcs[i].pc);
        if (cur == pc || (!cur && atomic_compare_exchange_strong(&r->pcs[i].pc, &z, pc)) || atomic_load(&r->pcs[i].pc) == pc) { atomic_store(&r->pcs[i].lr, lr); atomic_fetch_add(&r->pcs[i].n, 1); if (inflight) atomic_fetch_add(&r->pcs[i].inflight, 1); break; } }
    uint64_t w = atomic_fetch_add(&g_wwe_w, 1); if (w - g_wwe_r >= WW_EV) { atomic_fetch_add(&g_wwe_dropped, 1); return; }
    ww_ev *e = &g_wwe[w % WW_EV];
    e->r = ri; e->atomic_ = atomic_; e->thr = g_ww_thr ? g_ww_thr() : 0; e->mtid = pthread_mach_thread_np(pthread_self()); e->len = len;
    e->pc = pc; e->lr = lr; e->ea = ea; e->frame = fr; e->reader = reader; e->done = done; e->committed = atomic_load(&g_fence); e->arm_frame = af; e->since_ns = now_ns() - atomic_load(&r->arm_ns);
    ww_tw *t = atomic_load(&g_ww_twkey_ok) ? pthread_getspecific(g_ww_twkey) : NULL;
    e->wkind = t ? t->kind : 0; e->wblocked = t ? t->blocked : 0; e->wfence = t ? t->fence : 0; e->wago_ns = t ? now_ns() - t->ns : 0; e->wframe = t ? t->frame : 0;
    atomic_store(&e->seq, w + 1);
}
static int ww_fault(void *addr, int is_write, void *ucv) {
    if (!is_write || g_ww_any <= 0) return 0;
    uintptr_t a = (uintptr_t)addr; int pi = ww_page_find(a & ~(g_ww_psz - 1)); if (pi < 0) return 0;
    ww_page *P = &g_wwp[pi];
    if (nvn_dirty_page_protected((const void *)P->pg)) { atomic_store(&P->prot, 0); return 0; }   // the dirty tracking's page (whichever hook runs first): its handler opens it and stamps the write
    atomic_fetch_add(&g_ww_faults, 1);
    if (!atomic_load(&P->prot)) return 1;   // opened meanwhile by another thread: the store runs again on the writable page
    ucontext_t *uc = ucv; uint64_t pc = uc->uc_mcontext->__ss.__pc, ea = 0, wbv = 0; unsigned len = 0; int wb = -1; uint8_t data[64]; uint32_t in = *(uint32_t *)pc;
    int dec = P->alias && ww_decode(uc, in, &ea, &len, data, &wb, &wbv), atomic_ = 0;
    if (!dec) {   // exclusive / atomic (address in [Xn]) or a form the decoder does not know: open the page; it is re-armed later
        int ex = (in & 0x3f000000) == 0x08000000, at = (in & 0x3f200c00) == 0x38200000;
        if (ex || at) { unsigned rn = (in >> 5) & 31; _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss; ea = rn == 31 ? ss->__sp : rn == 29 ? ss->__fp : rn == 30 ? ss->__lr : ss->__x[rn]; len = 1u << (in >> 30); if ((in & 0xbfa00000) == 0x88200000) len *= 2; atomic_ = 1; }
        else { ea = a; len = 1; }
    }
    int nr = atomic_load(&g_wwr_n), hit = 0, lfxpg = 0;
    for (int i = 0; i < nr; i++) { ww_range *r = &g_wwr[i]; if (!atomic_load(&r->armed) || ea >= r->lo + r->n || ea + len <= r->lo) continue;
        if (g_ww_all ? 1 : atomic_exchange(&r->armed, 0)) { ww_record(i, uc, ea, len, atomic_); hit = 1; } }
    // DM_NVN_SB_FREEZE_WW: a range frozen with versions is marked CPU-written (its next reader takes a new copy) and its page is opened until the next arm: one fault
    // per page per version, no store emulation (the store runs again on the writable page)
    for (int i = 0; i < nr; i++) { ww_range *r = &g_wwr[i]; if (!atomic_load(&r->lfx) || !ww_overlaps(r, P->pg)) continue; lfxpg = 1;
        if (!(ea >= r->lo + r->n || ea + len <= r->lo)) { atomic_store(&r->dirty, 1); atomic_fetch_add(&r->lfx_faults, 1); } }
    if (lfxpg) { for (int i = 0; i < nr; i++) if (ww_overlaps(&g_wwr[i], P->pg) && !atomic_load(&g_wwr[i].lfx)) atomic_store(&g_wwr[i].dirty, 1); ww_unprotect(P); return 1; }
    if (dec) {
        for (unsigned i = 0; i < len; i++) { uintptr_t b = (uintptr_t)ea + i, bp = b & ~(g_ww_psz - 1);
            if (bp == P->pg) { P->alias[b - bp] = data[i]; continue; }
            int pj = ww_page_find(bp);
            if (pj >= 0 && atomic_load(&g_wwp[pj].prot)) { if (g_wwp[pj].alias) { g_wwp[pj].alias[b - bp] = data[i]; continue; } ww_unprotect(&g_wwp[pj]); }
            *(volatile uint8_t *)b = data[i]; }
        if (wb >= 0) { if (wb == 31) uc->uc_mcontext->__ss.__sp = wbv; else if (wb == 29) uc->uc_mcontext->__ss.__fp = wbv; else if (wb == 30) uc->uc_mcontext->__ss.__lr = wbv; else uc->uc_mcontext->__ss.__x[wb] = wbv; }
        uc->uc_mcontext->__ss.__pc = pc + 4; atomic_fetch_add(&g_ww_emul, 1);
    } else { ww_unprotect(P); return 1; }
    int armed = 0; for (int i = 0; i < nr; i++) if (atomic_load(&g_wwr[i].armed) && ww_overlaps(&g_wwr[i], P->pg)) armed = 1;
    if (!armed) ww_unprotect(P);
    else if (!hit && atomic_fetch_add(&P->noise, 1) + 1 == WW_NOISE) { for (int i = 0; i < nr; i++) if (ww_overlaps(&g_wwr[i], P->pg)) atomic_fetch_add(&g_wwr[i].noisy, 1); ww_unprotect(P); }
    return 1;
}
static void ww_where(char *o, size_t n, uint64_t a) {
    module_t *m = module_of((void *)a); Dl_info di;
    if (m) snprintf(o, n, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)(uintptr_t)m->base));
    else if (dladdr((void *)a, &di) && di.dli_sname) snprintf(o, n, "%s+%#lx", di.dli_sname, (unsigned long)(a - (uint64_t)(uintptr_t)di.dli_saddr));
    else snprintf(o, n, "%#llx", (unsigned long long)a);
}
static void ww_drain(void) {   // the events since the last drain, as [ww] lines (any thread but a signal handler)
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static int lines, inflight_lines; pthread_mutex_lock(&mu);
    uint64_t w = atomic_load(&g_wwe_w);
    while (g_wwe_r < w) { ww_ev *e = &g_wwe[g_wwe_r % WW_EV]; if (atomic_load(&e->seq) != g_wwe_r + 1) { if (w - g_wwe_r > WW_EV) { g_wwe_r++; continue; } break; }
        ww_range *r = &g_wwr[e->r]; int inflight = e->reader > e->done, newpc = 0;
        for (int i = 0; i < WW_PCS; i++) if (atomic_load(&r->pcs[i].pc) == e->pc) { newpc = !atomic_exchange(&r->pcs[i].logged, 1); break; }
        if (newpc || lines < 64 || (inflight && inflight_lines < 256)) { lines++; if (inflight) inflight_lines++;
            char p[96], l[96]; ww_where(p, sizeof p, e->pc); ww_where(l, sizeof l, e->lr);
            MLOG("[ww] F%llu R%d %s %s %#llx+%#llx: %s%u B at +%#llx by T=%#x (mach %u) pc=%s lr=%s; %.3f ms after the arm at F%llu; reader fence %llu, completed %llu, committed %llu: %s%s; thread's last wait: %s %llu (%s) %.3f ms before, F%llu",
                (unsigned long long)e->frame, e->r, r->key, r->what, (unsigned long long)r->lo, (unsigned long long)r->n, e->atomic_ ? "atomic " : "", e->len, (unsigned long long)(e->ea - r->lo), e->thr, e->mtid, p, l,
                e->since_ns / 1e6, (unsigned long long)e->arm_frame, (unsigned long long)e->reader, (unsigned long long)e->done, (unsigned long long)e->committed,
                inflight ? "GPU READER IN FLIGHT (race)" : "reader completed", e->frame == e->arm_frame ? ", same frame" : "", ww_wait_name[e->wkind < 7 ? e->wkind : 0], (unsigned long long)e->wfence,
                e->wblocked ? "blocked" : "no block", e->wkind ? e->wago_ns / 1e6 : 0.0, (unsigned long long)e->wframe); }
        g_wwe_r++; }
    pthread_mutex_unlock(&mu);
}
static void ww_summary(void) {
    int nr = atomic_load(&g_wwr_n);
    MLOG("[ww] summary F%llu: %d ranges, %d pages, faults %llu, emulated stores %llu, page opens %llu, events dropped %llu", (unsigned long long)atomic_load(&g_cur_frame), nr, (int)atomic_load(&g_wwp_n),
         (unsigned long long)atomic_load(&g_ww_faults), (unsigned long long)atomic_load(&g_ww_emul), (unsigned long long)atomic_load(&g_ww_open), (unsigned long long)atomic_load(&g_wwe_dropped));
    for (int i = 0; i < nr; i++) { ww_range *r = &g_wwr[i]; char pcs[700]; size_t o = 0; pcs[0] = 0;
        for (int k = 0; k < WW_PCS && atomic_load(&r->pcs[k].pc) && o < sizeof pcs - 120; k++) { char p[96], l[96]; ww_where(p, sizeof p, atomic_load(&r->pcs[k].pc)); ww_where(l, sizeof l, atomic_load(&r->pcs[k].lr));
            o += snprintf(pcs + o, sizeof pcs - o, " %s (lr %s) x%llu (%llu in flight);", p, l, (unsigned long long)atomic_load(&r->pcs[k].n), (unsigned long long)atomic_load(&r->pcs[k].inflight)); }
        MLOG("[ww] R%d %s %s %#llx+%#llx: arms %llu, first writes after the arm %llu (GPU reader in flight %llu, after it completed %llu, same frame %llu, atomic %llu), noisy-page opens %llu, dirty-tracked pages skipped %llu; writers:%s",
             i, r->key, r->what, (unsigned long long)r->lo, (unsigned long long)r->n, (unsigned long long)atomic_load(&r->arms), (unsigned long long)atomic_load(&r->hits), (unsigned long long)atomic_load(&r->hits_inflight),
             (unsigned long long)atomic_load(&r->hits_after), (unsigned long long)atomic_load(&r->hits_same), (unsigned long long)atomic_load(&r->atomics), (unsigned long long)atomic_load(&r->noisy), (unsigned long long)atomic_load(&r->dirty_skip), o ? pcs : " none"); }
}
// f1b16a8a's binding extents (as sbu_snap): s0 4 B per thread, s1 max (s0 & 0xfffc) + 8, s2 max id * 16 + 16
static uint64_t ww_f1b_extent(int stage, const NvShader *sh, uint32_t off) {
    uint8_t tmp[DMSH_PUSH_SIZE]; const uint8_t *pv = push_view(stage, sh, tmp); uint32_t cnt; uint64_t s0; memcpy(&cnt, pv, 4); memcpy(&s0, pv + 64, 8);
    if (cnt > (1u << 20)) cnt = 1u << 20;
    if (off == 64) return cnt ? 4ull * cnt : 16;
    if (!s0 || !cnt || !nvn_host_readable(s0, 4ull * cnt)) return 4096;
    const uint32_t *w = (const uint32_t *)(uintptr_t)s0; uint64_t m1 = 0, m2 = 0;
    for (uint32_t i = 0; i < cnt; i++) { uint32_t v = w[i]; uint64_t e1 = (uint64_t)(v & 0xfffcu) + 8, e2 = (uint64_t)((v >> 16) & 0x7fffu) * 16 + 16; if (e1 > m1) m1 = e1; if (e2 > m2) m2 = e2; }
    uint64_t n = off == 80 ? m1 : m2; return n ? n : 16;
}
// A read-only storage binding of a dispatch (sbufp: NVN_RB_SBUFP slot reg, size known; else NVN_RB_SBUFU at push offset off): marks the ranges it reads as read by this
// submission (armed at its commit), and adds a DM_NVN_WRITEWATCH_KEY range
static void ww_bind(int stage, const NvShader *sh, uint32_t off, int reg, uint64_t addr, uint64_t size, int sbufp) {
    if (!addr || !sh) return;
    for (int k = 0; k < g_wwk_n; k++) { if (strncmp(sh->key, g_wwk[k].key, strlen(g_wwk[k].key))) continue;
        for (int j = 0; j < g_wwk[k].nb; j++) { const char *b = g_wwk[k].b[j]; int want = -1, isp = 0;
            if (!strcmp(b, "s0")) want = 64; else if (!strcmp(b, "s1")) want = 80; else if (!strcmp(b, "s2")) want = 16; else if (b[0] == '+') want = (int)strtol(b + 1, NULL, 0); else if (b[0] == 'p') { want = atoi(b + 1); isp = 1; }
            if (isp != sbufp || (isp ? reg : (int)off) != want) continue;
            uint64_t n = sbufp ? (size ? size : 4096) : !strncmp(sh->key, "f1b16a8ab9ff9b99", 16) && (off == 64 || off == 80 || off == 16) ? ww_f1b_extent(stage, sh, off) : 4096;
            int found = 0, nr = atomic_load(&g_wwr_n); for (int i = 0; i < nr; i++) if (g_wwr[i].lo == addr) { found = 1; if (n > g_wwr[i].n) ww_add(addr, n, 1, sh->key, b); }
            if (!found) { char k8[17]; snprintf(k8, sizeof k8, "%.8s", sh->key); ww_add(addr, n, 1, k8, b); } } }
    uint64_t hi = addr + (size ? size : 1); int nr = atomic_load(&g_wwr_n);
    for (int i = 0; i < nr; i++) if (g_wwr[i].lo < hi && g_wwr[i].lo + g_wwr[i].n > addr) atomic_store(&g_wwr[i].pend, g_fence + 1);
}
static void ww_auto_hit(uint64_t lo, uint64_t n, const char *key) { char k8[17]; snprintf(k8, sizeof k8, "%.8s", key ? key : "-"); ww_add(lo, n, 2, k8, "auto"); }
static void lfx_process(void);
static void ww_commit(uint64_t fence) {   // the submission with this fence has been committed: arm the ranges it read
    int nr = atomic_load(&g_wwr_n); if (!nr) return;
    if (lfx_on()) lfx_process();
    pthread_mutex_lock(&g_ww_mu); for (int i = 0; i < nr; i++) if (atomic_load(&g_wwr[i].pend) == fence) ww_arm(&g_wwr[i], fence); pthread_mutex_unlock(&g_ww_mu);
    ww_drain();
}
void mtl_ww_present(void) {   // each present: arm every range not armed (its last reader), drain, and the summary every 600 frames
    if (!WW_ON()) return;
    int nr = atomic_load(&g_wwr_n);
    pthread_mutex_lock(&g_ww_mu); for (int i = 0; i < nr; i++) if (!atomic_load(&g_wwr[i].armed)) ww_arm(&g_wwr[i], atomic_load(&g_wwr[i].pend)); pthread_mutex_unlock(&g_ww_mu);
    ww_drain();
    static uint64_t f0; uint64_t f = atomic_load(&g_cur_frame); if (!f0) f0 = f; else if (f >= f0 + 600) { ww_summary(); lfx_summary(); f0 = f; }
}
// ---------- DM_NVN_SB_FREEZE_WW (lightfix agent, 2026-11-07; off by default) ----------
// The CPU-written read-only storage ranges that a CPU store PROVES racing a GPU reader are frozen, and only those; their copy is versioned per CPU write, not per frame.
// Why: (1) SB_FREEZE_AUTO / SBU_SNAP_CHECK / sbucheck call a range "CPU-written" when an encode-time copy differs from memory at completion: GPU stores (raw pointer stores
// that no binding records) differ too. dg_ww proved it: 30 of the 32 AUTO ranges (and f1b16a8a's s0 / s2) got NO CPU store after 2400+ arms, so they are GPU-produced, and
// copying them drew the black rectangles / blue flicker. Only a write fault is CPU evidence. (2) The per-frame copy at the first binding goes stale when the CPU writes the range
// later in the frame. Here: the probation (sbf_note) still nominates ranges, write-watch confirms them: the first CPU store after a reading submission's commit while that
// submission is still on the GPU requests the freeze (ww_record). A frozen range keeps its write-watch pages: every CPU store after a copy marks it written (one fault per page,
// the page opens until the next arm); the next read-only binding of it takes a new copy (re-armed first, so a store during the copy marks it again), so each submission reads the
// range as it was at that submission's encode (the game's submit) and never a CPU write made after it. Without CPU writes the copy is reused. A writable binding keeps the old
// rule (memory this frame). Ranges larger than SB_FREEZE's 256 KB stay on memory. Log: [lfix] lines.
static int lfx_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_SB_FREEZE_WW"); v = e && *e && strcmp(e, "0"); if (v) MLOG("DM_NVN_SB_FREEZE_WW=1: read-only storage ranges a CPU store proves racing a GPU reader are read from a copy made at each submission's encode after a CPU write ([lfix])"); } return v; }
static uint64_t g_lfx_frozen, g_lfx_versions;
static void lfx_process(void) {   // the encoding thread (commit): freeze the ranges whose in-flight CPU store was caught
    int nr = atomic_load(&g_wwr_n);
    for (int i = 0; i < nr; i++) { ww_range *r = &g_wwr[i]; if (!atomic_exchange(&r->freeze_req, 0) || atomic_load(&r->lfx)) continue;
        char p[96]; ww_where(p, sizeof p, atomic_load(&r->lfx_pc));
        if (r->n > SBF_MAX) { static int nl; if (nl++ < 8) MLOG("[lfix] [RED] %#llx+%#llx (%s): a CPU store under a GPU reader (pc %s), but longer than %u KB: left on memory", (unsigned long long)r->lo, (unsigned long long)r->n, r->key, p, SBF_MAX >> 10); continue; }
        sbf_range *s = sbf_add(r->lo, r->n, "[lfix] a CPU store under a GPU reader proves it CPU-written:", r->key);
        if (!s) continue;
        s->wwi1 = i + 1; atomic_store(&r->dirty, 1); atomic_store(&r->lfx, 1); g_lfx_frozen++;
        MLOG("[lfix] R%d %s %#llx+%#llx frozen with versions at F%llu (writer pc %s; reader fence %llu)", i, r->key, (unsigned long long)r->lo, (unsigned long long)r->n, (unsigned long long)atomic_load(&g_cur_frame), p, (unsigned long long)atomic_load(&r->reader)); }
}
static int lfx_take(int wwi1) {   // sbf_read: this range was written on the CPU since its last copy (and re-arms it before the new copy is made)
    if (!wwi1) return 0; ww_range *r = &g_wwr[wwi1 - 1];
    if (!atomic_load(&r->dirty)) return 0;
    pthread_mutex_lock(&g_ww_mu); atomic_store(&r->dirty, 0); ww_arm(r, g_fence + 1); pthread_mutex_unlock(&g_ww_mu);
    g_lfx_versions++; return 1;
}
static void lfx_summary(void) {
    if (!lfx_on()) return; int nr = atomic_load(&g_wwr_n);
    MLOG("[lfix] summary F%llu: %llu ranges frozen with versions, %llu new versions", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_lfx_frozen, (unsigned long long)g_lfx_versions);
    for (int i = 0; i < nr; i++) if (atomic_load(&g_wwr[i].lfx)) MLOG("[lfix] R%d %s %#llx+%#llx: CPU-store faults %llu", i, g_wwr[i].key, (unsigned long long)g_wwr[i].lo, (unsigned long long)g_wwr[i].n, (unsigned long long)atomic_load(&g_wwr[i].lfx_faults));
}
