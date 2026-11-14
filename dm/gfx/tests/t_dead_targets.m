// Render targets another one drew over (nvn_mtl.m's tu_pass, nvn_owner.m's own_alias_dead_locked): a shared render target goes once another target of its pool wrote its bytes after
// its last use and it has stayed unused for TU_IDLE. One read every frame stays, as does one nothing overlaps, one whose overlapping neighbour wrote before its last use, and one a pin
// holds. A released target's serial no longer resolves, its texture ID drops its view (a pool walk reads it as the dummy until a pass writes it), and its next use makes it again.
// Last, a pass over 4,000 covered targets (it prints the time).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static tex_t target(pool_t *pool, uint64_t serial, int64_t off, NVNformat format, unsigned w) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .pool = pool, .mem = pool->mem + off, .off = off};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width = w, .height = w, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static int shared(id o) { pthread_mutex_lock(&g_smu); int r = own_shared_locked(o); pthread_mutex_unlock(&g_smu); return r; }
static int covered(id o) { own_use u; return tu_use_of(o, &u); }
static void at(uint32_t now) { g_tu_now = now; }
static void submissions(void) { for (int k = 0; k <= ALIAS_IDLE; k++) { CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "submission completes"); mtl_sub_begin(); } }   // past ALIAS_IDLE: no lookup found them lately
static void pass(uint32_t now) { submissions(); at(now); g_tu_last = now - 10; tu_pass(); }
static void drawn(id<MTLTexture> t) { tu_wrote(t); }

int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin(); g_tu_manual = 1;
    void *mem = NULL; posix_memalign(&mem, 16384, 64 << 20); memset(mem, 0, 64 << 20);
    pool_t pool = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED | NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem = mem, .size = 64 << 20};
    at(100);
    tex_t a = target(&pool, 97001, 0, NVN_FORMAT_RGBA8, 64), b = target(&pool, 97002, 0, NVN_FORMAT_R32F, 64);           // b over a's bytes
    tex_t c = target(&pool, 97003, 1 << 20, NVN_FORMAT_RGBA8, 64);                                                         // nothing over it
    tex_t d = target(&pool, 97004, 2 << 20, NVN_FORMAT_RGBA8, 64), e = target(&pool, 97005, 2 << 20, NVN_FORMAT_R32F, 64); // e over d, d read every frame
    tex_t f = target(&pool, 97006, 3 << 20, NVN_FORMAT_RGBA8, 64), g = target(&pool, 97007, 3 << 20, NVN_FORMAT_R32F, 64); // g over f, f pinned
    tex_t h = target(&pool, 97008, 4 << 20, NVN_FORMAT_RGBA8, 64), i = target(&pool, 97009, 4 << 20, NVN_FORMAT_R32F, 64); // i wrote before h was made, h drew over i
    id<MTLTexture> ta = tex_mtl(&a), tc = tex_mtl(&c), td = tex_mtl(&d), tf = tex_mtl(&f), ti = tex_mtl(&i);
    CHECK(ta && (ta.usage & MTLTextureUsageRenderTarget) && covered(ta), "a compressible render target of a GPU-only pool is covered");
    drawn(ta); drawn(tc); drawn(td); drawn(tf); drawn(ti);
    at(110);
    id<MTLTexture> tb = tex_mtl(&b), te = tex_mtl(&e), tg = tex_mtl(&g), th = tex_mtl(&h);
    CHECK(tb && tb != ta && shared(ta) && shared(tb), "another description over the bytes leaves the target a live object resolves to (the takeover)");
    drawn(tb); drawn(te); drawn(tg); drawn(th);
    mtl_tex_pin(f.serial);
    for (uint32_t now = 120; now <= 160; now += 10) { at(now); CHECK(tex_mtl(&d) == td, "the target read every frame resolves to its texture"); }
    {   // c's texture ID, bound (a use) and then not
        mtl_reg_tex(5, &a, NULL); mtl_reg_tex(6, &c, NULL);
        os_unfair_lock_lock(&g_rmu); void *va = NULL, *vc = NULL; at(100); tex_fast_rid(5, &va); at(160); tex_fast_rid(6, &vc); os_unfair_lock_unlock(&g_rmu);
        CHECK(va && vc, "the IDs have views");
    }
    pass(160);
    CHECK(!shared(ta) && !tex_get(a.serial), "a target drawn over and unused for TU_IDLE goes, with its serial entry");
    CHECK(pool_unwritten_target(&a), "a pool walk reads it as the dummy until a pass writes it");
    os_unfair_lock_lock(&g_rmu); CHECK(!g_treg[5].view, "its texture ID dropped its view"); CHECK(g_treg[6].view != NULL, "an ID of a target that stays keeps its view"); os_unfair_lock_unlock(&g_rmu);
    CHECK(shared(tb) && tex_get(b.serial) == tb, "the target that drew over it stays (written after nothing over it)");
    CHECK(shared(tc), "a target nothing overlaps stays");
    CHECK(shared(td), "a target read every frame stays");
    CHECK(shared(tf) && tex_get(f.serial) == tf, "a pinned target stays");
    CHECK(shared(th), "a target whose neighbour wrote before its last use stays");
    CHECK(!shared(ti), "and the neighbour it drew over goes");
    CHECK(g_tu_n == 2 && g_tu_pinned == 1, "two released, one kept by a pin");
    mtl_tex_unpin(f.serial);
    at(170);
    id<MTLTexture> ta2 = tex_mtl(&a); drawn(ta2);
    CHECK(ta2 && ta2 != ta && shared(ta2) && tex_get(a.serial) == ta2 && covered(ta2), "drawn into again, it is made again and covered");
    CHECK(g_tu_remade == 1, "and counted as made again");
    pass(175);
    CHECK(!shared(tb) && shared(ta2), "b goes in turn: a2 drew over its bytes after its last use");
    CHECK(!shared(tf) && shared(tg), "unpinned, the target drawn over goes");
    pthread_mutex_lock(&g_smu); char why[256] = ""; int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
    CHECK(ok, why);
    {   // The cost: 4,000 covered targets in 2,000 overlapping pairs, half of them released
        enum { N = 2000 }; static tex_t ts[2 * N];
        at(300);
        for (int k = 0; k < N; k++) { ts[2 * k] = target(&pool, 98000 + 2 * k, (8 << 20) + ((int64_t)k << 14), NVN_FORMAT_RGBA8, 32); drawn(tex_mtl(&ts[2 * k])); }
        at(310);
        for (int k = 0; k < N; k++) { ts[2 * k + 1] = target(&pool, 98001 + 2 * k, (8 << 20) + ((int64_t)k << 14), NVN_FORMAT_R32F, 32); drawn(tex_mtl(&ts[2 * k + 1])); }
        uint64_t n0 = g_tu_n, ns0 = g_tu_ns;
        int64_t rounds = 0; for (uint32_t now = 350; g_tu_n - n0 < N && rounds < 20; now += 10, rounds++) pass(now);
        double ms = (g_tu_ns - ns0) / 1e6 / (double)rounds;
        printf("a pass over %u covered targets: %.3f ms (%lld passes released %llu)\n", g_tu.n + (unsigned)(g_tu_n - n0), ms, (long long)rounds, (unsigned long long)(g_tu_n - n0));
        CHECK(g_tu_n - n0 == N, "every target drawn over goes, TU_MAX a pass");
        CHECK(ms < 20, "a pass stays short");
        for (int k = 0; k < 2 * N; k++) mtl_tex_release(&ts[k]);
    }
    mtl_tex_release(&a); mtl_tex_release(&b); mtl_tex_release(&c); mtl_tex_release(&d); mtl_tex_release(&e); mtl_tex_release(&f); mtl_tex_release(&g); mtl_tex_release(&h); mtl_tex_release(&i);
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "no GPU error");
    printf("t_dead_targets: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
