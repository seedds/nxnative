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

// The ledger does not dereference these addresses. Choose collisions with the
// production hash, but derive the expected hazards from the writes alone.
static void colliders(uint64_t *pages, unsigned n, unsigned slot) {
    for (uint64_t pg = 1; n; pg++) if (gw_slot(pg) == slot) { *pages++ = pg; n--; }
}
static void ledger_reset(uint64_t done) {
    memset(g_gw, 0, sizeof g_gw); memset(g_gwx, 0, sizeof g_gwx);
    memset(g_gww, 0, sizeof g_gww); g_gw_all = 0; g_done = done;
}
static void ledger_write(gw_ent *t, uint64_t pg, uint64_t fence) { gw_put_in(t, pg << 12, (pg << 12) + 4, fence, g_done); }
static int ledger_live(const gw_ent *t, uint64_t pg) { return gw_live_in(t, pg << 12, (pg << 12) + 4); }
static void ledger_cases(void) {
    uint64_t p[4]; colliders(p, 4, GW_N - 1);   // displaced entries wrap past the end of the table
    uint64_t crowded[17]; colliders(crowded, 17, GW_N - 1);
    for (int which = 0; which < 2; which++) {
        gw_ent *t = which ? g_gwx : g_gw;
        ledger_reset(10);
        ledger_write(t, p[0], 20); ledger_write(t, p[1], 30);
        CHECK(ledger_live(t, p[0]) && ledger_live(t, p[1]), "both colliding live writes remain hazards");
        CHECK(!ledger_live(t, p[2]), "an untouched colliding page is not a hazard");
        CHECK(gw_live_in(t, (p[1] << 12) + 4095, (p[1] << 12) + 4096), "the whole written page stays protected");
        g_done = 20;
        CHECK(!ledger_live(t, p[0]) && ledger_live(t, p[1]), "one colliding writer completes independently");
        ledger_write(t, p[1], 40); ledger_write(t, p[1], 25);
        ledger_write(t, p[2], 35);   // reuse the completed entry before p[1], without hiding it
        g_done = 35;
        CHECK(ledger_live(t, p[1]) && !ledger_live(t, p[2]), "a displaced page keeps its newest fence across an expired hole");
        CHECK(!ledger_live(t, p[3]), "reusing an expired collision does not poison another page");
        g_done = 40;
        CHECK(!ledger_live(t, p[1]), "the displaced writer expires at its newest fence");

        ledger_reset(10);
        for (int i = 0; i < 16; i++) ledger_write(t, crowded[i], 20);
        int all_live = 1; for (int i = 0; i < 16; i++) all_live &= ledger_live(t, crowded[i]);
        CHECK(all_live && !g_gw_all && !ledger_live(t, crowded[16]), "all sixteen probes retain separate identities");
        ledger_write(t, crowded[15], 40); ledger_write(t, crowded[16], 30);
        CHECK(g_gw_all == 30, "the seventeenth collider uses the conservative fallback");
        g_done = 30;
        CHECK(ledger_live(t, crowded[15]) && !ledger_live(t, crowded[16]), "the last probe remains visible after overflow completes");

        // More distinct live writes than the ledger can hold: keep a global
        // fallback fence, then prove its completion does not hide a newer writer.
        ledger_reset(50);
        uint64_t overflow = 0;
        for (uint64_t pg = 1; pg <= GW_N + 1; pg++) { ledger_write(t, pg, 60); if (!overflow && g_gw_all) overflow = pg; }
        CHECK(g_gw_all == 60 && ledger_live(t, GW_N + 2), "saturation conservatively protects unlisted pages");
        if (overflow) { ledger_write(t, overflow, 70); ledger_write(t, overflow, 65); }
        CHECK(g_gw_all == 70, "further saturation keeps the newest overflow fence");
        ledger_write(t, 1, 80);
        g_done = 60;
        CHECK(ledger_live(t, GW_N + 2), "completing older overflow does not release newer overflow");
        g_done = 70;
        CHECK(ledger_live(t, 1), "overflow completion preserves a newer tracked writer");
        CHECK(!ledger_live(t, GW_N + 2), "the global saturation fence expires");
        ledger_write(t, GW_N + 2, 75);
        CHECK(ledger_live(t, GW_N + 2) && !ledger_live(t, GW_N + 3), "completed capacity can be reused without another overflow");
        g_done = 80;
        CHECK(!ledger_live(t, 1) && !ledger_live(t, GW_N + 2), "all completed writes stop blocking");
    }
    ledger_reset(0);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    ledger_cases();
    mtl_init();
    mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "memory allocated"); if (mem == MAP_FAILED) return 1;
    mtl_pool_add(mem, LEN);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    running = g_done + 1000;
    memset(mem, 0x11, LEN);

    // The first case: every frame the same fields, with a hole (word 1) nothing updates
    update(0, 0xa0, 4); update(8, 0xa2, 4); commit_as_running();
    update(0, 0xb0, 4); update(8, 0xb2, 4);
    { const uint32_t want[] = {0xb0, 0x11111111, 0xb2}; words(made_shadow(0, 12), want, 3, "a hole beside fields updated by a running submission reads memory"); }
    commit_as_running();

    // Two real running writes elsewhere used to make this untouched page look
    // GPU-written solely because all three hash alike. Its partial shadow must
    // still contain memory plus the actual inline update.
    uint64_t pg[3]; colliders(pg, 3, gw_slot(((uint64_t)mem + 0x14000) >> 12));
    uint64_t own = ((uint64_t)mem + 0x14000) >> 12;
    for (int i = 0, written = 0; i < 3 && written < 2; i++) if (pg[i] != own) { gw_put(pg[i] << 12, (pg[i] << 12) + 4, running, g_done); written++; }
    update(0x14008, 0xabc, 4);
    uint64_t refused = g_ubp_why[2];
    { const uint32_t want[] = {0x11111111, 0x11111111, 0xabc}; words(made_shadow(0x14000, 12), want, 3, "colliding writes do not refuse a safe partial shadow"); }
    CHECK(g_ubp_why[2] == refused, "collisions do not count as running-writer refusals");
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
    g_done = running; running += 1000;
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

    // A saturated probe range must still refuse a shadow, report the running
    // writer reason, and release it once the conservative fence completes.
    uint64_t crowded[17]; colliders(crowded, 17, GW_N - 1);
    for (int i = 0; i < 17; i++) gw_put(crowded[i] << 12, (crowded[i] << 12) + 4, running, g_done);
    update(0x16004, 0xc5, 4); refused = g_ubp_why[2];
    CHECK(!made_shadow(0x16000, 12) && g_ubp_why[2] == refused + 1, "saturation refuses a shadow and counts the running writer");
    g_done = running;
    { const uint32_t want[] = {0x22222222, 0xc5, 0x22222222}; words(made_shadow(0x16000, 12), want, 3, "completion releases the saturated shadow with its pending update"); }

    pend_reset(); g_nubd = 0;
    printf("t_ubp_running: %d checks, %d failed\n", checks, failed); fflush(stdout);
    _exit(failed != 0);
}}
