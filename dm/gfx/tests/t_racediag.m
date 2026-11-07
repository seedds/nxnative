// race-diag (nvn_racediag.inc + hle_nvn.c, 2026-11-07), CPU only: no command buffer is committed.
//  1. DM_NVN_WRITEWATCH: an armed range's first CPU store faults, is carried out through the page's second mapping (the value lands), and is recorded with its pc, the
//     reader's state (in flight / completed) and the thread's last wait; other bytes of the page are written silently; the range rests after its first write; an atomic store
//     opens the page; a page the dirty tracking holds is skipped; ww_unprot_span (zero-copy buffer creation) opens the page; the KEY form arms on a binding's address.
//  2. DM_NVN_SUBMIT_AT_FLUSH=3: frame N's submissions replay at frame N+1's present, in order; a SyncWait on a sync fenced behind held work replays early (counted);
//     EventGetValue / QueueFinish likewise; DM_NVN_SUBMIT_AT_FLUSH_KEYS holds only from the learned place on. DM_NVN_SERIALIZE=1 waits after each replay.
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
void crash_install(void);
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
static pool_t g_pool;
static int g_order[64], g_norder;
static int g_key_handle = -1;
static void exec_hook(NVNqueue *q, int n, const NVNcommandHandle *h) { (void)q;
    if (n < 0) { g_order[g_norder++] = -1; return; }   // a QueueWaitSync SAF held (XQ): nothing is encoded, so the KEYS flag is the previous submission's (as with sub_exec -> qe_sync_wait)
    g_order[g_norder++] = (int)h[0]; g_safk_hit = (int)h[0] == g_key_handle; }
static void submit(NVNqueue *q, int tag) { NVNcommandHandle h = (NVNcommandHandle)tag; h_nvnQueueSubmitCommands(q, 1, &h); }
static int order_is(const int *want, int n) { if (g_norder != n) return 0; for (int i = 0; i < n; i++) if (g_order[i] != want[i]) return 0; return 1; }
static void present(win_t *w) { h_nvnQueuePresentTexture(NULL, (NVNwindow *)w, -1); }
int main(void) { @autoreleasepool {
    setenv("DM_NVN_SUBMIT_AT_FLUSH", "3", 1); setenv("DM_NVN_SERIALIZE", "1", 1); setenv("DM_NVN_SUBMIT_AT_FLUSH_KEYS", "f1b16a8a", 1);
    size_t plen = 4u << 20;
    uint8_t *mem = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    uint64_t A = (uint64_t)(uintptr_t)(mem + 0x10000 + 0x600), B = (uint64_t)(uintptr_t)(mem + 0x40000 + 0x200);
    char ws[128]; snprintf(ws, sizeof ws, "%llx:0x100,%llx:0x80", (unsigned long long)A, (unsigned long long)B);
    setenv("DM_NVN_WRITEWATCH", ws, 1); setenv("DM_NVN_WRITEWATCH_KEY", "f1b16a8a:s0,s2;abcd:p3", 1);
    crash_install();
    mtl_init();
    pool_builder_t pb = {.mem = mem, .size = plen, .flags = 0x24};
    CHECK("pool", h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_pool, (const NVNmemoryPoolBuilder *)&pb));
    CHECK("writewatch on", WW_ON() && atomic_load(&g_wwr_n) == 2 && g_wwk_n == 2 && g_wwk[0].nb == 2 && !strcmp(g_wwk[1].b[0], "p3"));
    CHECK("pages with a second mapping", atomic_load(&g_wwp_n) == 2 && g_wwp[0].alias && g_wwp[1].alias);
    ww_range *rA = &g_wwr[0], *rB = &g_wwr[1];
    // 1a. armed, GPU reader in flight (its fence above the completed one): the first store is caught, lands, and rests the range
    pthread_mutex_lock(&g_ww_mu); ww_arm(rA, g_done + 5); pthread_mutex_unlock(&g_ww_mu);
    ww_note_wait(WWT_SYNC, 77, 1);
    volatile uint32_t *pa = (volatile uint32_t *)(uintptr_t)A;
    pa[3] = 0x10008;
    CHECK("store landed through the alias", pa[3] == 0x10008);
    CHECK("one write, in flight", atomic_load(&rA->hits) == 1 && atomic_load(&rA->hits_inflight) == 1 && atomic_load(&rA->hits_after) == 0);
    CHECK("range rests after its first write", !atomic_load(&rA->armed));
    CHECK("writer pc recorded", atomic_load(&rA->pcs[0].pc) != 0 && atomic_load(&rA->pcs[0].n) == 1);
    ww_ev *e = &g_wwe[0];
    CHECK("event: range, offset, length", atomic_load(&e->seq) == 1 && e->r == 0 && e->ea == A + 12 && e->len == 4 && !e->atomic_);
    CHECK("event: thread's last wait", e->wkind == WWT_SYNC && e->wfence == 77 && e->wblocked == 1);
    pa[4] = 5; CHECK("later store: no fault, no hit", pa[4] == 5 && atomic_load(&rA->hits) == 1);
    CHECK("page opened when no armed range is on it", !atomic_load(&g_wwp[0].prot));
    // 1b. armed, reader completed; a store to other bytes of the page first (silent, emulated), then a pair store into the range
    pthread_mutex_lock(&g_ww_mu); ww_arm(rA, 0); pthread_mutex_unlock(&g_ww_mu);
    uint64_t f0 = atomic_load(&g_ww_faults), em0 = atomic_load(&g_ww_emul);
    volatile uint32_t *other = (volatile uint32_t *)(uintptr_t)(A - 0x400); other[0] = 0xabcd;
    CHECK("other bytes: emulated, no hit", other[0] == 0xabcd && atomic_load(&rA->hits) == 1 && atomic_load(&g_ww_faults) == f0 + 1 && atomic_load(&g_ww_emul) == em0 + 1 && atomic_load(&g_wwp[0].prot));
    uint64_t v2[2] = {0x1111222233334444ull, 0x5555666677778888ull}; __uint128_t q; memcpy(&q, v2, 16);
    *(volatile __uint128_t *)(uintptr_t)(A + 0x20) = q;
    CHECK("16-byte store landed", !memcmp((void *)(uintptr_t)(A + 0x20), v2, 16));
    CHECK("second write: after the reader completed", atomic_load(&rA->hits) == 2 && atomic_load(&rA->hits_after) == 1);
    // 1c. an atomic store opens the page and is still recorded
    pthread_mutex_lock(&g_ww_mu); ww_arm(rB, 0); pthread_mutex_unlock(&g_ww_mu);
    _Atomic uint32_t *pb_ = (_Atomic uint32_t *)(uintptr_t)(B + 8); atomic_fetch_add(pb_, 3);
    CHECK("atomic: value and record", atomic_load(pb_) == 3 && atomic_load(&rB->hits) == 1 && atomic_load(&rB->atomics) == 1 && !atomic_load(&g_wwp[1].prot));
    // 1d. ww_unprot_span (a zero-copy buffer over the page) opens it: the next store is not caught
    pthread_mutex_lock(&g_ww_mu); ww_arm(rB, 0); pthread_mutex_unlock(&g_ww_mu);
    ww_unprot_span((uintptr_t)B, (uintptr_t)B + 1);
    ((volatile uint32_t *)(uintptr_t)B)[0] = 9;
    CHECK("opened page: no record", atomic_load(&rB->hits) == 1 && ((volatile uint32_t *)(uintptr_t)B)[0] == 9);
    // 1e. a page the dirty tracking holds is skipped; its store goes to the dirty tracking
    nvn_dirty_protect((const void *)(uintptr_t)(B & ~(uint64_t)16383), 16384);
    uint64_t ds0 = atomic_load(&rB->dirty_skip);
    pthread_mutex_lock(&g_ww_mu); ww_arm(rB, 0); pthread_mutex_unlock(&g_ww_mu);
    int dprot = nvn_dirty_page_protected((const void *)(uintptr_t)B);
    CHECK("dirty-tracked page skipped", dprot && atomic_load(&rB->dirty_skip) == ds0 + 1);
    ((volatile uint32_t *)(uintptr_t)B)[1] = 11;
    CHECK("its store went to the dirty tracking", ((volatile uint32_t *)(uintptr_t)B)[1] == 11 && atomic_load(&rB->hits) == 1 && !nvn_dirty_page_protected((const void *)(uintptr_t)B));
    // 1f. the drain prints the events (no crash) and the summary
    ww_drain(); ww_summary();
    CHECK("drained", g_wwe_r == atomic_load(&g_wwe_w));
    // 1g. KEY form: a storage binding p3 of shader abcd... adds a range at its address, read by the next fence; committing that fence arms it
    NvShader *sh = [NvShader new]; snprintf(sh->key, sizeof sh->key, "abcd000011112222");
    uint64_t C = (uint64_t)(uintptr_t)(mem + 0x80000);
    ww_bind(5, sh, 0, 3, C, 0x40, 1);
    int nr = atomic_load(&g_wwr_n); CHECK("key range added", nr == 3 && g_wwr[2].lo == C && g_wwr[2].n == 0x40 && g_wwr[2].src == 1);
    CHECK("read by the next submission", atomic_load(&g_wwr[2].pend) == g_fence + 1);
    ww_commit(g_fence + 1); CHECK("armed at its commit", atomic_load(&g_wwr[2].armed) && atomic_load(&g_wwr[2].reader) == g_fence + 1);
    ((volatile uint32_t *)(uintptr_t)C)[0] = 1; CHECK("key range caught", atomic_load(&g_wwr[2].hits) == 1);
    ww_bind(5, sh, 0, 2, C + 0x1000, 0x40, 1); CHECK("other binding: no range", atomic_load(&g_wwr_n) == 3);
    // 1h. auto: a detection adds a range
    ww_auto_hit((uint64_t)(uintptr_t)(mem + 0xc0000), 0x200, "1c844d9cb64ad632"); CHECK("auto range", atomic_load(&g_wwr_n) == 4 && g_wwr[3].src == 2 && !strcmp(g_wwr[3].key, "1c844d9c"));

    // 2. SUBMIT_AT_FLUSH=3
    g_saf_exec_hook = exec_hook;
    queue_t qs = {0}; NVNqueue *Q = (NVNqueue *)&qs; win_t w = {0};
    CHECK("saf 3, serialize 1", saf_on() == 3 && ser_on() == 1);
    submit(Q, 1); submit(Q, 2); present(&w);   // frame 1 held
    CHECK("frame 1 held at its present", g_norder == 0);
    submit(Q, 3); present(&w);   // frame 2: frame 1 replays
    { int want[] = {1, 2}; CHECK("frame 1 replayed at frame 2's present", order_is(want, 2)); }
    sync_t sy = {0}; submit(Q, 4); h_nvnQueueFenceSync(Q, (NVNsync *)&sy, 0, 0);
    CHECK("sync pending behind held work", sy.fenced == 3);
    submit(Q, 5);
    uint64_t early0 = g_saf3_early[0];
    h_nvnSyncWait((NVNsync *)&sy, 0);
    { int want[] = {1, 2, 3, 4, 5}; CHECK("SyncWait replayed everything early, in order", order_is(want, 5)); }
    CHECK("early replay counted", g_saf3_early[0] == early0 + 1 && sy.fenced == 1);
    CHECK("serialize waited per replay", g_ser_waits >= 5);
    submit(Q, 6); ev_t ev = {MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(uintptr_t)(mem + 0x100), NULL, 0};
    h_nvnEventGetValue((NVNevent *)&ev); { int want[] = {1, 2, 3, 4, 5, 6}; CHECK("EventGetValue replays early", order_is(want, 6) && g_saf3_early[1] == 1); }
    submit(Q, 7); h_nvnQueueFinish(Q); CHECK("QueueFinish replays early", g_norder == 7 && g_order[6] == 7 && g_saf3_early[2] == 1);
    // partial replay: a sync fenced after submission 8 of 2 held is fenced when 8 (and before) replay
    present(&w); submit(Q, 8); sync_t s2 = {0}; h_nvnQueueFenceSync(Q, (NVNsync *)&s2, 0, 0); present(&w); submit(Q, 9); sync_t s3 = {0}; h_nvnQueueFenceSync(Q, (NVNsync *)&s3, 0, 0);
    CHECK("two syncs pending", s2.fenced == 3 && s3.fenced == 3);
    present(&w);
    CHECK("frame of 8 replayed, its sync fenced; 9 and its sync held", g_norder == 8 && g_order[7] == 8 && s2.fenced == 1 && s3.fenced == 3);
    present(&w); CHECK("then 9", g_norder == 9 && g_order[8] == 9 && s3.fenced == 1);
    // KEYS: the frame's submission #1 (tag 11) dispatches the key shader: learned when it replays; from then on #0 replays at its own frame's present, #1 on are held
    g_norder = 0; g_key_handle = 11;
    submit(Q, 10); submit(Q, 11); submit(Q, 12); present(&w); present(&w);
    { int want[] = {10, 11, 12}; CHECK("first frame: whole frame held, then replayed", order_is(want, 3)); }
    CHECK("place learned", g_saf[saf_slot(Q, 0)].kidx == 1);
    g_norder = 0; submit(Q, 20); g_key_handle = 21; submit(Q, 21); submit(Q, 22); present(&w);
    { int want[] = {20}; CHECK("KEYS: only the part before the key shader replays at its own present", order_is(want, 1)); }
    present(&w); { int want[] = {20, 21, 22}; CHECK("KEYS: the rest at the next present", order_is(want, 3)); }
    // trip: a loop-cap hit ends the lag; the held frame replays at the next present, and the held present runs before the current one
    g_norder = 0; g_key_handle = -1; g_saf[saf_slot(Q, 0)].kidx = -1; g_saf[saf_slot(Q, 0)].kgen = 0; g_safk_n = 0; submit(Q, 30); present(&w); CHECK("held before the trip", g_norder == 0 && g_saf3_pw == &w);
    atomic_store(&g_gpu_trouble, 1); CHECK("tripped: runs as 2", saf_on() == 2);
    submit(Q, 31); present(&w); { int want[] = {30, 31}; CHECK("after the trip: everything replays at the present", order_is(want, 2) && g_saf3_pw == NULL); }
    submit(Q, 32); CHECK("and is not held across presents any more", g_norder == 2); present(&w); CHECK("replayed at its own present", g_norder == 3 && g_order[2] == 32);
    // SAF=4 (KEYS, the present not held): the frame's key submission (#1) waits for the next present and runs first there; the rest runs at its own present
    present(&w); g_norder = 0; atomic_store(&g_gpu_trouble, 0); g_saf3_tripped = 0; g_saf_force = 4; g_safk_n = -1; g_saf[saf_slot(Q, 0)].kidx = 1; g_key_handle = 41;
    CHECK("mode 4", saf_on() == 4);
    submit(Q, 40); submit(Q, 41); submit(Q, 42); present(&w);
    { int want[] = {40, 42}; CHECK("key submission held, the rest replayed, present not held", order_is(want, 2) && g_saf3_pw == NULL); }
    g_key_handle = 51; submit(Q, 50); submit(Q, 51); submit(Q, 52); present(&w);
    { int want[] = {40, 42, 41, 50, 52}; CHECK("previous key first, then this frame without its key", order_is(want, 5)); }
    sync_t s4 = {0}; submit(Q, 60); h_nvnQueueFenceSync(Q, (NVNsync *)&s4, 0, 0); h_nvnSyncWait((NVNsync *)&s4, 0);
    { int want[] = {40, 42, 41, 50, 52, 51, 60}; CHECK("a wait on a pending sync replays all, the held key too", order_is(want, 7) && s4.fenced == 1); }
    // 3. review of the PR #1245 port (2026-11-07), DM_NVN_XQ_EVENTS forced on (no scheduler yet, no command buffer committed)
    {   extern int g_mtl_xq_force; g_mtl_xq_force = 1;
        // 3a. a sync DM_NVN_SUBMIT_AT_FLUSH still holds (fenced 3) is not a satisfied GPU wait (PR #1245's 3 was QUEUE_PREFIX)
        sync_t held = {MAGIC('S', 'Y', 'N', 'C'), NVN_SYNC_FENCE_SAF_PENDING, 0, 0}; uint32_t v = 7;
        CHECK("scheduler reads a SAF-pending sync as not ready", qe_read(NULL, (uint64_t)(uintptr_t)&held | 1, &v) && v == 0);
        held.fenced = NVN_SYNC_FENCE_GPU; CHECK("a GPU-fenced sync is ready", qe_read(NULL, (uint64_t)(uintptr_t)&held | 1, &v) && v == 1);
        held.fenced = NVN_SYNC_FENCE_NONE; CHECK("an unfenced sync is not", qe_read(NULL, (uint64_t)(uintptr_t)&held | 1, &v) && v == 0);
        // 3b. SAF=3 + KEYS: a QueueWaitSync held behind the frame's work (n = -1 marker) replays in order and does not unlearn the key place
        present(&w); present(&w); g_saf_force = 3; g_saf3_tripped = 0; atomic_store(&g_gpu_trouble, 0); g_safk_n = -1;
        int k = saf_slot(Q, 0); g_saf[k].kidx = 1; g_saf[k].kgen = 0; g_norder = 0; g_key_handle = 71;
        submit(Q, 70); submit(Q, 71); present(&w);
        { int want[] = {70}; CHECK("XQ+SAF3: the part before the key place replays at its present", order_is(want, 1)); }
        sync_t sx = {MAGIC('S', 'Y', 'N', 'C'), NVN_SYNC_FENCE_GPU, 0, 0}; uint64_t mk = atomic_load(&g_xq_n[XQ_SAFMARK]);
        h_nvnQueueWaitSync(Q, (NVNsync *)&sx); submit(Q, 80); submit(Q, 81);
        CHECK("XQ+SAF: QueueWaitSync behind held work is held (marker)", atomic_load(&g_xq_n[XQ_SAFMARK]) == mk + 1 && g_saf[k].ns == 4);
        present(&w);
        { int want[] = {70, 71, -1, 80}; CHECK("XQ+SAF3: the held wait replays after the work before it", order_is(want, 4)); }
        CHECK("XQ+SAF3: a marker after the key submission does not unlearn the key place", g_saf[k].kidx == 1);
        present(&w); present(&w); g_norder = 0; g_key_handle = -1;
        // 3c. SAF=2 + XQ: q2 waits (GPU) on a sync q1's held work fences; QueueFinish(q2) must replay q1 rather than wait forever
        //     (before the review: q2 only was replayed; with the SAF-pending fix alone it hung until the forced-wait escape)
        setenv("DM_NVN_XQ_WAIT_MS", "400", 1); g_saf_force = 2;
        queue_t q1s = {.serial = nvn_new_serial()}, q2s = {.serial = nvn_new_serial()}, q3s = {.serial = nvn_new_serial()};
        NVNqueue *Q1 = (NVNqueue *)&q1s, *Q2 = (NVNqueue *)&q2s, *Q3 = (NVNqueue *)&q3s;
        sync_t sp = {MAGIC('S', 'Y', 'N', 'C'), 0, 0, 0};
        submit(Q1, 90); h_nvnQueueFenceSync(Q1, (NVNsync *)&sp, 0, 0); CHECK("XQ+SAF2: q1's sync pending behind its held work", sp.fenced == NVN_SYNC_FENCE_SAF_PENDING);
        h_nvnQueueWaitSync(Q2, (NVNsync *)&sp); CHECK("XQ: q2's wait went to the scheduler", g_qs && nvn_qs_unscheduled(g_qs) == 1);
        uint64_t forced0 = atomic_load(&g_xq_n[XQ_FORCED]), t0 = now_ns();
        h_nvnQueueFinish(Q2);
        CHECK("XQ+SAF2: QueueFinish(q2) replays q1's held work and returns", now_ns() - t0 < 300000000ull && atomic_load(&g_xq_n[XQ_FORCED]) == forced0);
        CHECK("XQ+SAF2: q1's sync fenced at that replay, q2 scheduled", sp.fenced == NVN_SYNC_FENCE_GPU && nvn_qs_unscheduled(g_qs) == 0);
        // 3d. the escape: a GPU wait on a sync nobody ever fences stops q3 for DM_NVN_XQ_WAIT_MS, then is forced with a [RED] line
        sync_t never = {MAGIC('S', 'Y', 'N', 'C'), 0, 0, 0};
        h_nvnQueueWaitSync(Q3, (NVNsync *)&never); t0 = now_ns();
        CHECK("XQ: SyncWait on q3's prefix times out while the wait is blocked", h_nvnSyncWait((NVNsync *)&(sync_t){0}, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        h_nvnQueueFinish(Q3); uint64_t dt = now_ns() - t0;
        CHECK("XQ: an unsatisfiable wait is forced after DM_NVN_XQ_WAIT_MS (not forever)", dt >= 350000000ull && dt < 3000000000ull && atomic_load(&g_xq_n[XQ_FORCED]) == forced0 + 1);
        CHECK("XQ: nothing left blocked", nvn_qs_unscheduled(g_qs) == 0);
        g_saf_force = 0; g_mtl_xq_force = -1;
    }
    fprintf(stderr, "%d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
