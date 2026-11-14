// A finalized pool's buffer between submissions (nvn_mtl.m mtl_pool_del, defer_kick): with no submission open it waits only for the last committed fence, and goes with no
// submission after it, as a game that stops submitting out of focus (Ocarina of Time) still remakes virtual pool runs. A submission open when the pool goes keeps it until that
// submission completes, and one in flight when it goes keeps it until its feedback. The residency set holds what a drain took out of it until its next commit, which with no
// submission to come is made off the submit thread (defer_idle_commit). A commit that lands between pool_gone_release's read of the open flag and its read of g_fence leaves the
// buffer at that submission's own fence, neither the one before it nor the one after (a hook between the two reads stands in for it; PR #2328).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

enum { LEN = 1 << 20 };
static uint8_t *pages(void) { uint8_t *m = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (m == MAP_FAILED) abort(); return m; }
static __weak id<MTLBuffer> g_w;
static void hold(const void *mem) { @autoreleasepool {   // the pool's buffer, weakly
    g_w = nil;
    pthread_mutex_lock(&g_mu); for (int i = 0; i < g_npool; i++) if (g_pools[i].key == mem) g_w = g_pools[i].buf; pthread_mutex_unlock(&g_mu);
} }
static int alive(void) { @autoreleasepool { return g_w != nil; } }
static int gone_within(uint64_t ms) { for (uint64_t t = 0; t < ms; t++) { if (!alive()) return 1; usleep(1000); } return !alive(); }
static void commit_lands(void) { atomic_fetch_add(&g_fence, 1); atomic_store(&g_sub_open, 0); }   // what a commit does between the reads: its ++g_fence, later its flag cleared
static void settle(void) { mtl_drain(); mtl_wait(g_fence, 5000000000ull); defer_drain(); res_commit(); }

int main(void) { @autoreleasepool {
    mtl_init();
    uint8_t *m = pages();

    // Between submissions: the buffer goes without another submission
    settle();
    mtl_pool_add(m, LEN); hold(m);
    CHECK(alive(), "the pool has its buffer");
    uint64_t f0 = g_fence;
    mtl_pool_del(m);
    CHECK(gone_within(1000), "between submissions a finalized pool's buffer goes with no submission after it");
    CHECK(g_fence == f0, "no submission was made");

    // A submission open when the pool goes: the buffer stays until that submission completes
    settle();
    mtl_pool_add(m, LEN); hold(m);
    mtl_sub_begin();
    { void *mb = NULL; uint64_t mo = 0, gpu = 0; CHECK(mtl_gpu_addr((uint64_t)(uintptr_t)m, 256, &mb, &mo, &gpu), "the open submission looks the pool up"); }
    mtl_pool_del(m);
    usleep(50000);
    CHECK(alive(), "with a submission open the buffer stays");
    uint64_t f = mtl_sub_commit();
    CHECK(mtl_wait(f, 5000000000ull), "the submission completed");
    CHECK(gone_within(1000), "and goes behind that submission, with no submission after it to commit the residency set");

    // A commit that lands between the open flag's read and g_fence's: the buffer waits for exactly that submission's fence, and goes with no submission after it
    settle();
    mtl_pool_add(m, LEN); hold(m);
    mtl_sub_begin();
    { void *mb = NULL; uint64_t mo = 0, gpu = 0; CHECK(mtl_gpu_addr((uint64_t)(uintptr_t)m, 256, &mb, &mo, &gpu), "the open submission looks the pool up"); }
    uint64_t fo = g_fence + 1;   // the open submission's fence
    CHECK(atomic_load(&g_sub_open) == fo, "the open flag holds the open submission's fence");
    g_pool_gone_between = commit_lands;
    mtl_pool_del(m);
    g_pool_gone_between = NULL;
    atomic_fetch_sub(&g_fence, 1); atomic_store(&g_sub_open, fo);   // the real commit below makes both
    { uint64_t w = 0; pthread_mutex_lock(&g_deferred_mu); for (NvDeferredRelease *d in g_deferred) if (d->object == g_w) w = d->fence; pthread_mutex_unlock(&g_deferred_mu);
      CHECK(w == fo, "a commit between the two reads: the buffer waits for that submission's fence, not the one before it or after it"); }
    usleep(50000);
    CHECK(alive(), "and stays while that submission is open");
    f = mtl_sub_commit();
    CHECK(f == fo, "the submission took the fence the open flag held");
    CHECK(mtl_wait(f, 5000000000ull), "the submission completed");
    CHECK(gone_within(1000), "and the buffer goes behind it, with no submission after it");

    // A submission in flight, none open: the buffer waits for its feedback
    settle();
    mtl_pool_add(m, LEN); hold(m);
    mtl_sub_begin();
    { void *mb = NULL; uint64_t mo = 0, gpu = 0; mtl_gpu_addr((uint64_t)(uintptr_t)m, 256, &mb, &mo, &gpu); }
    f = mtl_sub_commit();
    mtl_pool_del(m);
    pthread_mutex_lock(&g_deferred_mu); uint64_t waits = 0; for (NvDeferredRelease *d in g_deferred) if (d->object == g_w) waits = d->fence; pthread_mutex_unlock(&g_deferred_mu);
    CHECK(!alive() || waits == f, "retired after the commit, it waits for that submission's fence, not the next one");
    CHECK(mtl_wait(f, 5000000000ull), "the submission completed");
    CHECK(gone_within(1000), "and the buffer goes with no submission after it");

    CHECK(!mtl_last_commit_error(), "no GPU error");
    printf("t_pool_retire: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
