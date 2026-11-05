// The GPU memory guard (nvn_mtl.m guard_*): a commit past the working set is not made while removals still are, the eviction level lets go of what nothing holds and keeps what a live object or
// its sRGB view holds, the stop level gives the eviction a chance first, stops once with the report, and a stuck submit thread is stopped by the timer without its lock
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed; static volatile sig_atomic_t stops;
static void on_term(int sig) { (void)sig; stops++; }   // the stop without a front end: the host's graceful exit starts from SIGTERM (front_gpu_memory_stop's weak default)
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static tex_t rt(uint64_t serial, const pool_t *pool, int64_t off, uint32_t format) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .pool=(pool_t *)pool, .off=off};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .width=64, .height=64, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L);
    return t;
}
static int in_set(id o) { pthread_mutex_lock(&g_res_mu); int r = [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
static void settle(void) { mtl_sub_begin(); mtl_sub_commit(); mtl_drain(); defer_drain(); res_commit(); }

static void levels(void) {
    g_guard.evict = 800; g_guard.stop = 950; g_guard.wall = 1000;
    CHECK(guard_level(799) == GUARD_OK && guard_level(800) == GUARD_EVICT && guard_level(950) == GUARD_STOP && guard_level(1000) == GUARD_WALL, "the levels");
    memset(&g_guard, 0, sizeof g_guard); CHECK(guard_level(UINT64_MAX) == GUARD_OK, "off: no level");
    setenv("DM_NVN_RES_LIMIT_MB", "950", 1); guard_init();
    CHECK(g_guard.stop == 950ull << 20 && g_guard.evict == 800ull << 20 && g_guard.wall == g_dev.recommendedMaxWorkingSetSize, "DM_NVN_RES_LIMIT_MB sets the stop and the eviction level under it");
    setenv("DM_NVN_RES_LIMIT_MB", "0", 1); memset(&g_guard, 0, sizeof g_guard);
}
// The set's own size leaves out what was never committed: the device's counts it from its creation, and that is what the check before the commit needs
static void commits(void) {
    id<MTLBuffer> a = [g_dev newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate], b = [g_dev newBufferWithLength:64 << 20 options:MTLResourceStorageModePrivate];
    res_add(a); res_commit();
    uint64_t now = g_dev.currentAllocatedSize, set = g_res.allocatedSize;
    res_add(b);
    CHECK(g_res.allocatedSize == set, "an addition is not in the set's size before its commit");
    CHECK(guard_used(g_res.allocatedSize) >= now, "the device's size counts it already");
    g_guard.evict = 1; g_guard.stop = 2; g_guard.wall = 3;   // anything is past these
    res_commit();
    CHECK(g_res_dirty && g_res_grew && g_res.allocatedSize == set, "a commit that adds is not made past the working set");
    CHECK(atomic_load(&g_guard_lv) == GUARD_WALL, "and leaves the level for the submit thread");
    pthread_mutex_lock(&g_res_mu); g_res_grew = 0; pthread_mutex_unlock(&g_res_mu);   // only removals left to commit
    res_remove(a); res_commit();
    CHECK(!g_res_dirty, "removals go through past it");
    memset(&g_guard, 0, sizeof g_guard); atomic_store(&g_guard_lv, GUARD_OK);
    res_remove(b); res_commit();
}
static void eviction(void) {
    static pool_t pool = {.magic=MAGIC('P','O','O','L')};
    tex_t live = rt(99601, &pool, 0, NVN_FORMAT_RGBA8), dead = rt(99602, &pool, 1 << 20, NVN_FORMAT_RGBA8), seen = rt(99603, &pool, 2 << 20, NVN_FORMAT_RGBA8);
    tex_t base = rt(99604, &pool, 3 << 20, NVN_FORMAT_RGBA8), srgb = rt(99605, &pool, 3 << 20, NVN_FORMAT_RGBA8_SRGB);
    mtl_tex_init(&live); mtl_tex_init(&dead); mtl_tex_init(&seen); mtl_tex_init(&base); mtl_tex_init(&srgb);
    mtl_sub_begin();
    __weak id<MTLTexture> w_live = tex_mtl(&live), w_dead = tex_mtl(&dead), w_seen = tex_mtl(&seen), w_base = tex_mtl(&base); id<MTLTexture> view = tex_mtl(&srgb);
    CHECK(w_live && w_dead && w_seen && view.parentTexture == w_base, "the textures");
    mtl_sub_commit(); mtl_drain();
    mtl_tex_release(&dead); mtl_tex_release(&seen); mtl_tex_release(&base);
    for (int i = 0; i < ALIAS_IDLE; i++) settle();
    alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)&pool; k.off = seen.off; k.d = seen.d;
    pthread_mutex_lock(&g_smu); nvn_hm_put(&g_alias_seen, &k, (void *)(uintptr_t)(g_fence + 1)); pthread_mutex_unlock(&g_smu);   // a lookup just now
    id<MTLBuffer> staging = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModePrivate]; res_add(staging); g_up_slot_blk[1] = staging; __weak id<MTLBuffer> w_staging = staging; staging = nil;
    uint64_t idle = g_alias_idle, gen = g_ser_gen;
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    guard_evict(1);
    CHECK(g_alias_idle == idle + 1 && g_ser_gen == gen + 1, "one entry left, one bump");
    settle();
    CHECK(!w_dead, "the idle entry nothing holds is gone");
    CHECK(w_live && in_set(w_live), "a live object's texture stays, and stays resident");
    CHECK(w_seen && in_set(w_seen), "an entry a lookup found lately stays");
    CHECK(w_base && in_set(w_base) && view.parentTexture == w_base, "the parent of a live sRGB view stays, though no serial holds it");
    CHECK(!w_staging && !g_up_slot_blk[1], "the staging the upload slots keep is let go");
    CHECK(tex_mtl(&dead) && g_alias_idle == idle + 1, "a later object over the same key gets a texture again");
    mtl_tex_release(&live); mtl_tex_release(&srgb);
    mtl_alias_drop_pool(&pool);
}
static void stop(void) {
    g_guard.evict = 1; g_guard.stop = UINT64_MAX - 1; g_guard.wall = UINT64_MAX;
    atomic_store(&g_guard_lv, GUARD_EVICT); guard_act(g_fence);
    CHECK(!stops && !atomic_load(&g_guard_lv), "the eviction level does not stop");
    // Over the stop level, but what the eviction frees brings it under: the game goes on
    atomic_store(&g_guard_lv, GUARD_STOP); guard_act(g_fence);
    CHECK(!stops, "the stop level looks again after the eviction");
    g_guard.stop = 2; g_guard.wall = UINT64_MAX;
    atomic_store(&g_guard_lv, GUARD_STOP); guard_act(g_fence);
    CHECK(stops == 1, "still over: the game stops");
    atomic_store(&g_guard_lv, GUARD_STOP); guard_act(g_fence); guard_tick();
    CHECK(stops == 1, "once");
}
// A submit thread stuck in the driver holds the residency lock: the timer takes the game down without it. Run in a process of its own, as the stop happens once
static void stuck(void) {
    g_guard.evict = 1; g_guard.stop = 2; g_guard.wall = UINT64_MAX;
    pthread_mutex_lock(&g_res_mu);
    for (int i = 0; i < GUARD_TICKS - 1; i++) guard_tick();
    CHECK(!stops, "the submit thread gets its time");
    guard_tick();
    CHECK(stops == 1, "then the timer stops the game, with the lock held");
    pthread_mutex_unlock(&g_res_mu);
}

int main(int argc, char **argv) { @autoreleasepool {
    (void)argv; signal(SIGTERM, on_term);
    setenv("DM_NVN_RES_LIMIT_MB", "0", 1);   // this test sets the levels itself
    mtl_init();
    if (argc > 1) stuck();
    else { levels(); commits(); eviction(); stop(); }
    printf("t_res_guard: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
