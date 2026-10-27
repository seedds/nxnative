// Queue-level regression for split retirement. The production completion
// handler is compiled with this translation unit; this test needs no Metal
// device and checks which records that handler is allowed to receive.
#define DM_NVN_LEGACY_RENDERER_TEST 1
#include "../nvn/nvn_mtl.m"

uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
int front_screen_max_fps(void) { return 60; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *retire_on_foreign_thread(void *p) { @autoreleasepool { defer_obj_release((__bridge id)p); } return NULL; }

int main(void) { @autoreleasepool {
    g_fence = 41; atomic_store(&g_done, 40); atomic_store(&g_sub_serial, 7);
    atomic_store(&g_sub_pending_fence, 42); atomic_store(&g_sub_active, 1);
    uint64_t fence0 = g_fence, done0 = atomic_load(&g_done);
    g_defer_cut_active = 1;
    NSObject *local = [NSObject new];
    defer_obj_release(local);
    NvDeferredRelease *local_retirement = g_deferred.lastObject;
    local = nil;
    CHECK(local_retirement->cut_eligible, "replay-thread retirement is eligible for this segment");
    CHECK(local_retirement->submission_active && local_retirement->fence == 42,
          "active replay publishes its pending fence to retirement records");
    CHECK(local_retirement->site > 0 && local_retirement->sub_serial == 7 && local_retirement->kind == 0,
          "retirement diagnostics record source, submission and object class");

    NSObject *foreign = [NSObject new];
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, retire_on_foreign_thread, (__bridge void *)foreign), "foreign retirement thread starts");
    pthread_join(thread, NULL);
    NvDeferredRelease *foreign_retirement = g_deferred.lastObject;
    foreign = nil;
    CHECK(!foreign_retirement->cut_eligible, "other-thread retirement keeps the whole-submission fence");

    defer_diag_summary summary; pthread_mutex_lock(&g_deferred_mu); defer_diag_collect_locked(&summary); pthread_mutex_unlock(&g_deferred_mu);
    CHECK(summary.count == 2 && summary.cut == 1 && summary.active == 2 && summary.other == 2 &&
          summary.first_sub == 7 && summary.last_sub == 7 && summary.first_fence == 42 && summary.last_fence == 42,
          "diagnostic aggregation classifies replay and foreign records without a Metal device");

    NSArray<NvDeferredRelease *> *cut = defer_take_cut_batch();
    CHECK(cut.count == 1 && cut.firstObject == local_retirement,
          "cut takes only replay-thread records for its pending fence");
    CHECK(g_deferred.count == 1 && g_deferred.firstObject == foreign_retirement,
          "conservative record stays on the global deferred queue");
    CHECK(g_fence == fence0, "taking a split batch does not advance the guest-visible fence");
    CHECK(atomic_load(&g_done) == done0, "taking a split batch does not signal guest completion");
    defer_release_batch(cut);
    CHECK(!local_retirement->object, "split completion retires replay-thread objects");
    CHECK(foreign_retirement->object != nil, "split completion preserves other-thread objects");

    atomic_store(&g_done, 41); defer_drain();
    CHECK(foreign_retirement->object != nil, "record stays alive before its terminal fence");
    atomic_store(&g_done, 42); defer_drain();
    CHECK(!foreign_retirement->object, "terminal completion retires the conservative remainder");
    CHECK(g_deferred.count == 0, "terminal drain empties the global deferred queue");

    // A CPU-only loader can retire a replay-only derived descriptor while no
    // replay exists. Enqueue must not destroy it under g_rmu; the post-unlock
    // safe point drains it against the already-completed current fence.
    atomic_store(&g_sub_active, 0); g_defer_cut_active = 0;
    atomic_store(&g_fence, 50); atomic_store(&g_done, 50); atomic_store(&g_sub_serial, 8);
    NSObject *idle = [NSObject new]; __weak NSObject *idle_weak = idle;
    defer_obj_release_ex(idle, 0, 1, 1234); idle = nil;
    CHECK(g_deferred.count == 1 && idle_weak != nil && atomic_load(&g_defer_replay_view_drain_needed),
          "idle descriptor retirement requests a drain without releasing under the registry lock");
    defer_replay_view_drain_safe_point();
    CHECK(g_deferred.count == 0 && idle_weak == nil && !atomic_load(&g_defer_replay_view_drain_needed),
          "post-unlock safe point drains an idle descriptor on the completed current fence");

    // With a prior command buffer still in flight, the same derived view waits for
    // that real fence, not an invented next one.
    atomic_store(&g_fence, 60); atomic_store(&g_done, 59);
    NSObject *prior = [NSObject new]; defer_obj_release_ex(prior, 1, 1, 2345);
    NvDeferredRelease *prior_retirement = g_deferred.lastObject; prior = nil;
    CHECK(prior_retirement->fence == 60 && !prior_retirement->submission_active,
          "idle replay-view retirement targets the last committed in-flight fence");
    pthread_mutex_lock(&g_deferred_mu); defer_diag_collect_locked(&summary); pthread_mutex_unlock(&g_deferred_mu);
    CHECK(summary.count == 1 && summary.keep == 1 && summary.replay_view == 1 && summary.sites[0].site == 2345,
          "diagnostic aggregation preserves keep-resident, replay-view and call-site fields");
    defer_drain(); CHECK(prior_retirement->object != nil, "prior in-flight fence still protects its derived view");
    atomic_store(&g_done, 60); defer_drain();
    CHECK(!prior_retirement->object && g_deferred.count == 0, "prior fence completion releases the idle derived view");

    // A derived view replaced while a replay is active is still protected by that
    // replay's pending terminal fence, even if the previous fence is done.
    atomic_store(&g_fence, 70); atomic_store(&g_done, 70);
    atomic_store(&g_sub_pending_fence, 71); atomic_store(&g_sub_active, 1);
    NSObject *active_view = [NSObject new]; defer_obj_release_ex(active_view, 0, 1, 3456);
    NvDeferredRelease *active_retirement = g_deferred.lastObject; active_view = nil;
    CHECK(active_retirement->fence == 71 && active_retirement->submission_active && active_retirement->object != nil,
          "active replay keeps derived-view retirement on its pending fence");
    atomic_store(&g_sub_active, 0); defer_drain();
    CHECK(active_retirement->object != nil, "completed previous fence cannot release an active replay view");
    atomic_store(&g_done, 71); defer_drain();
    CHECK(!active_retirement->object && g_deferred.count == 0, "active replay view releases at its terminal fence");

    // Production begin/commit helpers serialize the state pair with enqueue.
    // A replay-view retirement between those transitions must use exactly the
    // pending fence, and closing the state must happen only after commit.
    atomic_store(&g_fence, 75); atomic_store(&g_done, 75); atomic_store(&g_sub_active, 0);
    defer_submission_begin_state();
    CHECK(atomic_load(&g_sub_active) && atomic_load(&g_sub_pending_fence) == 76,
          "submission begin publishes one locked active/pending state");
    NSObject *state_view = [NSObject new]; defer_obj_release_ex(state_view, 1, 1, 4000);
    NvDeferredRelease *state_retirement = g_deferred.lastObject; state_view = nil;
    CHECK(state_retirement->fence == 76 && state_retirement->submission_active,
          "retirement serialized after begin uses the active pending fence");
    uint64_t prepared = defer_submission_prepare_commit();
    CHECK(prepared == 76 && atomic_load(&g_sub_active),
          "prepare publishes the fence but keeps replay active until queue commit");
    defer_submission_did_commit(prepared);
    CHECK(!atomic_load(&g_sub_active), "did-commit closes the locked replay state");
    defer_drain(); CHECK(state_retirement->object != nil, "prepared fence is not complete early");
    atomic_store(&g_done, 76); defer_drain();
    CHECK(!state_retirement->object && g_deferred.count == 0,
          "locked replay state retirement releases at completion");

    // Pool/serial/base-view objects are deliberately not on the replay-view
    // allowlist. An independent upload or present CB can still refer to them
    // while g_done == g_fence; the next main-queue fence is the conservative
    // same-queue completion gate.
    atomic_store(&g_fence, 80); atomic_store(&g_done, 80);
    NSObject *upload_ref = [NSObject new], *present_ref = [NSObject new];
    __weak NSObject *upload_weak = upload_ref;
    __weak NSObject *present_weak = present_ref;
    defer_obj_release_ex(upload_ref, 0, 0, 4567);
    defer_obj_release_ex(present_ref, 1, 0, 5678);
    NvDeferredRelease *upload_retirement = g_deferred.firstObject;
    NvDeferredRelease *present_retirement = g_deferred.lastObject;
    upload_ref = present_ref = nil;
    CHECK(upload_retirement->fence == 81 && present_retirement->fence == 81 &&
          !upload_retirement->replay_view_retirement && !present_retirement->replay_view_retirement,
          "upload/present-capable objects remain gated by a future same-queue submission");
    defer_replay_view_drain_safe_point(); defer_drain();
    CHECK(upload_weak != nil && present_weak != nil && g_deferred.count == 2,
          "completed replay fence cannot release independent-CB references");
    atomic_store(&g_done, 81); defer_drain();
    CHECK(upload_weak == nil && present_weak == nil && g_deferred.count == 0,
          "future same-queue completion releases conservative independent-CB references");

    // Pool finalization must also detach replay-only derived views when the
    // ordinary descriptor view is already empty. Those views retain their
    // parent storage and otherwise pin a finalized pool indefinitely.
    atomic_store(&g_fence, 90); atomic_store(&g_done, 90); atomic_store(&g_sub_active, 0);
    pool_t *pool = (pool_t *)(uintptr_t)0x12340000;
    g_treg = calloc(1, sizeof *g_treg); g_ntreg = 1;
    g_treg[0].ok = 1; g_treg[0].t.pool = pool;
    NSObject *alt = [NSObject new], *ar = [NSObject new];
    __weak NSObject *alt_weak = alt;
    __weak NSObject *ar_weak = ar;
    g_treg[0].alt = (void *)CFBridgingRetain(alt);
    g_treg[0].ar = (void *)CFBridgingRetain(ar);
    alt = ar = nil;
    treg_drop_pool_views(pool);
    CHECK(!g_treg[0].alt && !g_treg[0].ar,
          "pool finalization detaches alt/ar without a base descriptor view");
    CHECK(alt_weak == nil && ar_weak == nil && g_deferred.count == 0,
          "idle replay-only pool views drain after the registry lock is released");
    free(g_treg); g_treg = NULL; g_ntreg = 0;

    printf("cut retirement: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
