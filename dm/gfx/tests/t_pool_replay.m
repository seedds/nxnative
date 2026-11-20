// A patched pool table goes into another buffer, as draws already encoded read the old one (nvn_mtl.m pool_patch_take): a finished spare of the same table's build, brought up to date
// from the table's log, or a copy of the whole table when the table has no spare of its own, none of them finished, or the log has moved past them. Each case holds what the table
// holds; a table built anew takes nothing from the spares of its earlier build, and the spares stay within their byte cap
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
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

enum { LEN = 1 << 20 };
// The 2D float texture table at base 0, as a draw that reads it would ask, in the submission open now
static id<MTLBuffer> lookup(void) {
    __unsafe_unretained id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b); os_unfair_lock_unlock(&g_rmu);
    return b;
}
static id<MTLBuffer> table(void) {   // in a submission of its own, completed
    mtl_sub_begin(); id<MTLBuffer> held = lookup();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    return held;
}
static pool_snap *snap(void) { for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf && !g_psnap[k].samp && g_psnap[k].dtt == MTLTextureType2D && g_psnap[k].dpf == NVN_DPF_FLOAT && !g_psnap[k].base) return &g_psnap[k]; return NULL; }
static uint64_t *ents(id<MTLBuffer> b) { return (uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD); }
static uint32_t count(id<MTLBuffer> b) { return *(const uint32_t *)b.contents; }
static int same(id<MTLBuffer> a, id<MTLBuffer> b) { return a.length == b.length && !memcmp(a.contents, b.contents, DMSH_POOL_HEAD + 8 * (size_t)(count(a) + 1)); }
static uint64_t spare_sum(void) { uint64_t s = 0; for (int i = 0; i < g_npool_spare; i++) s += g_pool_spare[i].buf.length; return s; }
static uint64_t copied(int why) { return g_stat_pool_copied[why]; }
// The table built whole in the next submission (a bump the registration log holds no note for) holds what the patched one does
static void same_as_whole(id<MTLBuffer> patched, const char *label) {
    id<MTLBuffer> keep = patched; uint64_t builds = g_stat_pool_builds; atomic_fetch_add(&g_reg_gen, 1);
    id<MTLBuffer> w = table();
    CHECK(g_stat_pool_builds == builds + 1, "a change without a note builds the table whole");
    CHECK(same(keep, w), label);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_RT_RECYCLE", "0", 1);
    mtl_init();
    uint8_t *ma = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (ma == MAP_FAILED) abort();
    pool_t a = {0}; pool_builder_t pb = {.mem = ma, .size = LEN, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&a, (const NVNmemoryPoolBuilder *)&pb), "pool A initialized");
    NVNtextureBuilder tb = {0}; NVNtexture t1 = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&a, 0);
    CHECK(h_nvnTextureInitialize(&t1, &tb), "texture initialized");
    mtl_reg_tex(1, (const tex_t *)&t1, NULL); mtl_reg_tex(7, (const tex_t *)&t1, NULL);   // IDs 1 and 7 hold the texture, 2-6 nothing
    #define TOGGLE(on) mtl_reg_tex(2, (on) ? (const tex_t *)&t1 : NULL, NULL)   // ID 2 takes the texture, or gives it up: one entry a patch changes

    id<MTLBuffer> b0 = table(); pool_snap *s = snap(); int k = (int)(s - g_psnap);
    uint64_t dummy = ents(b0)[count(b0)], rid1 = ents(b0)[1];
    CHECK(s && rid1 != dummy && ents(b0)[2] == dummy, "the first walk builds the table: ID 1 holds its texture, ID 2 the dummy");
    CHECK(g_plog[k].cap >= 64 && g_plog[k].n == 0, "the table's log starts empty");

    // The first patch: the table has no spare of its own, so it copies the whole table; the buffer it leaves is the table's spare at the log position before the patch's entry
    uint64_t c0 = copied(0), rep = g_stat_pool_replayed;
    TOGGLE(1); id<MTLBuffer> b1 = table();
    CHECK(b1 != b0 && ents(b1)[2] == rid1 && copied(0) == c0 + 1 && g_stat_pool_replayed == rep, "a first patch copies the table whole: no spare of its own");
    CHECK(g_plog[k].n == 1, "its entry is logged");
    int own = 0; for (int i = 0; i < g_npool_spare; i++) own += g_pool_spare[i].buf == b0 && g_pool_spare[i].k == k && g_pool_spare[i].lpos == 0;
    CHECK(own == 1, "the old buffer is the table's spare, at log position 0");

    // The next patch takes that spare, finished, and writes the entries logged since: its own and the patch's
    TOGGLE(0); uint64_t n0 = g_stat_pool_replay_n; rep = g_stat_pool_replayed;
    id<MTLBuffer> b2 = table();
    CHECK(b2 == b0 && g_stat_pool_replayed == rep + 1 && g_stat_pool_replay_n == n0 + 1, "the next patch takes the table's finished spare and writes the one entry logged since");
    CHECK(ents(b2)[2] == dummy && ents(b2)[1] == rid1, "it holds the table as it is");

    // In one submission: the first patch takes the finished spare, and the second finds only the buffer the first left, which the open submission may read: a whole copy
    uint64_t c1 = copied(1); rep = g_stat_pool_replayed;
    mtl_sub_begin(); TOGGLE(1); id<MTLBuffer> b3 = lookup(); TOGGLE(0); id<MTLBuffer> b4 = lookup();
    CHECK(g_stat_pool_replayed == rep + 1 && b3 == b1, "the first patch of the submission takes the finished spare");
    CHECK(copied(1) == c1 + 1 && b4 != b3 && b4 != b2, "the second copies the table whole: no spare of its own has finished");
    for (int i = 0; i < g_npool_spare; i++) if (g_pool_spare[i].buf == b2) CHECK(g_pool_spare[i].fence > g_done, "the buffer the submission read waits for its fence");
    CHECK(ents(b3)[2] == rid1 && ents(b4)[2] == dummy, "each holds the table as it was at its lookup");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission completed");
    same_as_whole(b4, "the table copied in the open submission holds what a table built whole does");

    // The rebuild started the table's log over and gave it a new build: no spare of the earlier one is brought up to date from it
    s = snap(); k = (int)(s - g_psnap); c0 = copied(0); rep = g_stat_pool_replayed;
    CHECK(g_plog[k].n == 0, "a table built anew starts its log over");
    TOGGLE(1); id<MTLBuffer> b5 = table();
    CHECK(copied(0) == c0 + 1 && g_stat_pool_replayed == rep && ents(b5)[2] == rid1, "after a rebuild the first patch copies whole: the spares are of the earlier build");

    // The log moves past every spare: in-place writes (as pool_seen_scan's) of more entries than the log holds, then a patch, which copies whole
    for (uint32_t i = 0; i <= g_plog[k].cap; i++) plog_add(s, 1, ents(s->buf)[1]);
    uint64_t c2 = copied(2); rep = g_stat_pool_replayed;
    TOGGLE(0); id<MTLBuffer> b6 = table();
    CHECK(copied(2) == c2 + 1 && g_stat_pool_replayed == rep && ents(b6)[2] == dummy, "a patch past the log copies the table whole");

    // Entries written in place across the end of the log's ring: the patch after them writes each, in order, into the spare it takes
    uint32_t cap = g_plog[k].cap; while (g_plog[k].n % cap != cap - 8) plog_add(s, 1, ents(s->buf)[1]);   // the next spare starts 8 entries before the ring's end
    TOGGLE(1); id<MTLBuffer> b7 = table(); (void)b7;
    uint64_t from = g_plog[k].n - 1;   // where the buffer that patch left stands: before its own entry
    int found = 0; for (int i = 0; i < g_npool_spare; i++) found |= g_pool_spare[i].k == k && g_pool_spare[i].build == g_plog[k].build && g_pool_spare[i].lpos == from;
    CHECK(found && from % cap == cap - 8, "the patch's spare stands 8 entries before the ring's end");
    for (int i = 0; i < 20; i++) { uint64_t v = i & 1 ? rid1 : dummy; ents(s->buf)[5] = v; plog_add(s, 5, v); }   // ID 5, written in place 20 times, ends at rid1
    ents(s->buf)[6] = rid1; plog_add(s, 6, rid1);
    id<MTLBuffer> cur = s->buf; id<MTLBuffer> want = [g_dev newBufferWithLength:cur.length options:MTLResourceStorageModeShared]; memcpy(want.contents, cur.contents, cur.length); ents(want)[2] = dummy;
    rep = g_stat_pool_replayed; n0 = g_stat_pool_replay_n;
    TOGGLE(0); id<MTLBuffer> b8 = table();
    CHECK(g_stat_pool_replayed == rep + 1 && g_stat_pool_replay_n - n0 == g_plog[k].n - 1 - from && (from % cap) + (g_plog[k].n - 1 - from) > cap, "the patch takes that spare and writes the entries logged since, across the ring's end");
    CHECK(same(b8, want), "it holds the table as it is: the in-place writes, the last of each ID, and the patch's own");
    ents(s->buf)[5] = dummy; plog_add(s, 5, dummy); ents(s->buf)[6] = dummy; plog_add(s, 6, dummy);   // back to what a walk gives
    TOGGLE(1); id<MTLBuffer> b9 = table();
    same_as_whole(b9, "after the replays the table holds what a table built whole does");

    // The spares stay within their byte cap, counting what they hold
    CHECK(g_pool_spare_bytes == spare_sum() && g_pool_spare_bytes <= g_pool_spare_cap, "the spares' bytes are counted, within the cap");
    size_t cap0 = g_pool_spare_cap; g_pool_spare_cap = 2 * b9.length;
    for (int i = 0; i < 6; i++) { TOGGLE(i & 1); table(); }
    CHECK(g_pool_spare_bytes == spare_sum() && g_pool_spare_bytes <= g_pool_spare_cap && g_npool_spare <= 2, "with a cap of two tables, two spares at most");
    g_pool_spare_cap = cap0;

    printf("t_pool_replay: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
