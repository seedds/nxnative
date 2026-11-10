// A partly covered uniform shadow while an earlier submission still runs: that submission's inline updates are known to the CPU, so they do not refuse the shadow (the pass would end for it); its other GPU writes do.
// Splatoon Raiders updates 0x00..0x14 and 0x18..0x70 of its ink query block before each of about 150 queries a frame while the shader reads 0x00..0x70, and the page is updated again every frame
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { LEN = 0x20000 };
static uint8_t *mem;
static uint64_t running;   // the fence of the submission that stays unfinished

// What the open submission has queued goes into the running one
static void commit_as_running(void) { gw_commit(running); g_nubd = 0; memset(g_ubd_pf, 0, sizeof g_ubd_pf); g_ubd_wide = 0; pend_reset(); memset(g_ubpc, 0, sizeof g_ubpc); }
static void update(uint64_t off, uint32_t value, uint64_t n) { uint32_t v[16]; for (int i = 0; i < 16; i++) v[i] = value; mtl_sub_update(v, (uint64_t)mem + off, n); }
static const uint32_t *made_shadow(uint64_t off, uint64_t n) {
    uint64_t g; const uint8_t *c; if (!ubp_get((uint64_t)mem + off, (uint64_t)mem + off + n, &g, &c)) return NULL;
    ubc_wait(g_ubp_ready); return (const uint32_t *)c;
}
static void words(const uint32_t *got, const uint32_t *want, int n, const char *label) {
    CHECK(got && !memcmp(got, want, 4 * (size_t)n), label);
    if (got) for (int i = 0; i < n; i++) if (got[i] != want[i]) fprintf(stderr, "  word %d: %#x, want %#x\n", i, got[i], want[i]);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init();
    mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "memory allocated"); if (mem == MAP_FAILED) return 1;
    mtl_pool_add(mem, LEN);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    running = g_done + 1000; g_fence = running;   // committed, as a running submission's fence is: g_done never passes g_fence, and objects deferred to g_fence + 1 (the open submission's update buffers) would be freed at once (defer_obj_at)
    memset(mem, 0x11, LEN);

    // The first case: every frame the same fields, with a hole (word 1) nothing updates
    update(0, 0xa0, 4); update(8, 0xa2, 4); commit_as_running();
    update(0, 0xb0, 4); update(8, 0xb2, 4);
    { const uint32_t want[] = {0xb0, 0x11111111, 0xb2}; words(made_shadow(0, 12), want, 3, "a hole beside fields updated by a running submission reads memory"); }
    commit_as_running();

    // A field only the running submission updated: memory does not hold it yet
    update(0x40, 0xc0, 4); commit_as_running();
    update(0x48, 0xc2, 4);
    { const uint32_t want[] = {0xc0, 0x11111111, 0xc2}; words(made_shadow(0x40, 12), want, 3, "a running submission's update shows through"); }
    commit_as_running();

    // Newest wins across running submissions and a page boundary
    update(0xff8, 0xd0, 16); commit_as_running();
    update(0xffc, 0xd1, 8); commit_as_running();
    update(0xff8, 0xd2, 4);
    { const uint32_t want[] = {0xd2, 0xd1, 0xd1, 0xd0}; words(made_shadow(0xff8, 16), want, 4, "several running updates in order, over a page boundary"); }
    commit_as_running();

    // A running copy or fill the CPU cannot see refuses, and only on its page
    pend_add((uint64_t)mem + 0x8000, 16, 3); commit_as_running();
    update(0x8010, 0xe0, 4);
    CHECK(!made_shadow(0x8000, 32), "a running fill refuses a shadow of its page");
    update(0x10000, 0xe1, 4);
    CHECK(made_shadow(0x10000, 32) != NULL, "and not of another page");
    commit_as_running();

    // Once it completes, memory holds the truth: what the CPU wrote after it is not overwritten by what was remembered
    g_done = running; running += 1000; g_fence = running;
    memset(mem, 0x22, LEN);
    update(0x48, 0xf2, 4);
    { const uint32_t want[] = {0x22222222, 0x22222222, 0xf2}; words(made_shadow(0x40, 12), want, 3, "a finished update is no longer laid over memory"); }
    CHECK(made_shadow(0x8000, 32) == NULL ? (update(0x8010, 0xf0, 4), made_shadow(0x8000, 32) != NULL) : 1, "a finished fill no longer refuses");
    commit_as_running();

    // A running update on a page that shares its table slot with another running page: refused rather than lost
    uint64_t other = 0;
    for (uint64_t off = 0x1000; off < LEN && !other; off += 0x1000) if (off != 0x18000 && um_slot(((uint64_t)mem + off) >> 12) == um_slot(((uint64_t)mem + 0x18000) >> 12)) other = off;
    if (other) { update(0x18000, 0xa5, 4); update(other, 0xa6, 4); commit_as_running();
        update(other + 8, 0xa7, 4);
        const uint32_t *got = made_shadow(other, 12);
        CHECK(!got || got[0] == 0xa6, "a slot collision refuses or shows the update"); commit_as_running(); }

    // Splatoon Raiders' third boss stage: about 1,550 queries in one submission, 7 updates of the block before each (about 11,000 writes). Past the pending list's old 8,192 entries the
    // submission counted as writing everywhere, and every partly covered shadow was refused while it ran
    for (int q = 0; q < 1550; q++) for (int k = 0; k < 7; k++) update(0x1c000 + 16 * k, (uint32_t)q, 4);
    CHECK(!g_pend_over, "a submission of 10,850 inline updates fits the pending list");
    commit_as_running();
    update(0x1c004, 0xb5, 4);
    { const uint32_t want[] = {1549, 0xb5}; words(made_shadow(0x1c000, 8), want, 2, "after it, a shadow of the block shows its last update while it runs"); }
    CHECK(made_shadow(0x10000, 32) != NULL || (update(0x10004, 0xb6, 4), made_shadow(0x10000, 32) != NULL), "and another block's shadow is not refused");
    commit_as_running();

    // The list has no fixed size: four times that (43,400 updates, past the 32,768 a fixed list would have held next) still fits, and the shadow after it shows the last update
    for (int q = 0; q < 6200; q++) for (int k = 0; k < 7; k++) update(0x1c000 + 16 * k, (uint32_t)q, 4);
    CHECK(!g_pend_over, "a submission of 43,400 inline updates fits the pending list");
    { const uint32_t want[] = {6199}; words(made_shadow(0x1c000, 4), want, 1, "and a shadow in it shows the last one"); }
    commit_as_running();

    // A shadow of 512 bytes or more that a running submission's update is laid over (um_overlay) is copied here: queued to the copy thread it was waited for at once, on
    // every such shadow (Ocarina of Time: half the replay thread's busy time in ubc_wait on one build). One without such bytes still goes to the copy thread
    if (ubc_on()) {
        update(0x1e010, 0xd5, 4); commit_as_running();
        update(0x1e020, 0xd6, 4);
        uint64_t h0 = ubc_head(); const uint32_t *got = made_shadow(0x1e000, 1024);
        CHECK(got && ubc_head() == h0, "a large shadow with a running update over it queues no copy");
        { const uint32_t want[] = {0x22222222, 0x22222222, 0x22222222, 0x22222222, 0xd5, 0x22222222, 0x22222222, 0x22222222, 0xd6}; words(got, want, 9, "and holds memory, the running update and the new one"); }
        update(0x12020, 0xd7, 4);
        h0 = ubc_head(); got = made_shadow(0x12000, 1024);
        CHECK(got && ubc_head() > h0, "a large shadow without running updates still goes to the copy thread");
        { const uint32_t want[] = {0x22222222, 0x22222222, 0x22222222, 0x22222222, 0x22222222, 0x22222222, 0x22222222, 0x22222222, 0xd7}; words(got, want, 9, "and holds memory and the new update"); }
        commit_as_running();
    }

    // Ori's two 16-byte reports share a page with uniform blocks. Only their bytes are unknown,
    // not the rest of that page; the report timestamp is protected from CPU source reads too.
    g_done = running; running += 1000; g_fence = running;
    memset(mem + 0x14000, 0x55, 0x3000);
    uint64_t base = (uint64_t)mem + 0x14000;
    mtl_sub_counter(base + 0x40, 0); mtl_sub_counter(base + 0x80, 0); commit_as_running();
    update(0x14004, 0x76543210, 4);
    { const uint32_t want[] = {0x55555555, 0x76543210, 0x55555555}; words(made_shadow(0x14000, 12), want, 3, "a disjoint uniform beside two unfinished reports stays eligible"); }
    CHECK(!made_shadow(0x14038, 32), "a uniform intersecting a report is refused");
    CHECK(gw_live(base + 0x40, base + 0x41) && gw_live(base + 0x8f, base + 0x90), "both complete report records remain live");
    CHECK(!gw_live(base + 0x30, base + 0x40) && !gw_live(base + 0x50, base + 0x60), "touching either report boundary is disjoint");
    { uint8_t out[8]; int unreliable = 0;
      CHECK(rc_source(base + 0x48, 8, out, &unreliable) == 0, "a report timestamp cannot be read as completed CPU memory");
      CHECK(rc_source(base + 0x60, 8, out, &unreliable) == 255, "disjoint CPU source bytes on the report page are readable"); }
    commit_as_running();
    uint64_t first_report = running;
    running++; g_fence = running;
    mtl_sub_counter(base + 0xc0, 0); commit_as_running();
    g_done = first_report;
    CHECK(!gw_live(base + 0x40, base + 0x50), "the earlier report retires at its own fence");
    CHECK(gw_live(base + 0xc0, base + 0xd0), "a later report on that page stays live");
    // A report crossing a page protects both pieces, without claiming neighboring bytes.
    mtl_sub_counter(base + 0xff8, 0); commit_as_running();
    CHECK(gw_live(base + 0xff8, base + 0xff9) && gw_live(base + 0x1007, base + 0x1008), "a crossing report protects both pages");
    CHECK(!gw_live(base + 0xfe8, base + 0xff8) && !gw_live(base + 0x1008, base + 0x1018), "crossing-report neighbors stay disjoint");
    // Real GPU writes still claim their page even when a report is also there.
    sbw_note(base + 0x2040, 16, SBW_REPORT); sbw_note(base + 0x2080, 16, SBW_GPU); commit_as_running();
    CHECK(gw_live(base + 0x2000, base + 0x2010), "a mixed report/GPU page keeps its full hazard");
    g_done = running; running += 1000; g_fence = running;
    // Many reports must retain every hazard, whether listed exactly or conservatively by page.
    for (int i = 0; i < 2048; i++) mtl_sub_counter(base + (uint64_t)(i % 128) * 16, 0);
    commit_as_running();
    for (int i = 0; i < 128; i++) CHECK(gw_live(base + (uint64_t)i * 16, base + (uint64_t)i * 16 + 16), "overflow never loses an unfinished report");
    g_done = running;
    CHECK(!gw_live(base, base + 0x3000), "completed overflow and mixed-page hazards retire");
    // Fill the cross-submission ring while every fence remains live. The next submission must
    // fall back without overwriting any earlier record, then reuse only slots whose fence finished.
    uint64_t ring_first = running + 1;
    for (unsigned chunk = 0; chunk < GR_N / REP_N; chunk++) {
        running++; g_fence = running;
        for (unsigned i = 0; i < REP_N; i++) mtl_sub_counter(base + ((uint64_t)chunk * REP_N + i) * 32, 0);
        commit_as_running();
    }
    CHECK(!gw_live(base + 16, base + 32), "a full exact ring still leaves disjoint bytes readable");
    running++; g_fence = running; mtl_sub_counter(base + 0x9000, 0); commit_as_running();
    CHECK(gw_live(base + 0x9010, base + 0x9020), "a full live ring falls back to the new report's page");
    for (unsigned i = 0; i < GR_N; i++) CHECK(gw_live(base + (uint64_t)i * 32, base + (uint64_t)i * 32 + 16), "ring overflow preserves all older reports");
    g_done = ring_first;
    CHECK(!gw_live(base, base + 16), "only the first batch has retired");
    CHECK(gw_live(base + REP_N * 32, base + REP_N * 32 + 16), "the next batch is still unfinished");
    running++; g_fence = running; mtl_sub_counter(base + 0xa000, 0); commit_as_running();
    CHECK(gw_live(base + 0xa000, base + 0xa010) && !gw_live(base + 0xa010, base + 0xa020), "a retired ring slot can hold a new exact report");
    g_done = running;
    CHECK(!gw_live(base, base + 0xb000), "all report hazards retire after completion");

    pend_reset(); g_nubd = 0;
    printf("t_ubp_running: %d checks, %d failed\n", checks, failed); fflush(stdout);
    _exit(failed != 0);
}}
