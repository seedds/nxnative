// The suspend's drain with a limit (nvn_mtl.m mtl_drain_by; hle_nvn.c dm_gfx_suspend runs it on its own thread going to the background), with real Metal. Going to the background
// waited with no limit for the GPU, so a GPU or a feedback queue that stopped answering kept the main thread from ever returning; every wait on the drain's path now ends at the
// limit and the drain names the step. Each case fakes one step that never completes: a present on the GPU the display link owns (present_drop_for_suspend), a snapshot copy the
// encode thread never notes (snap_drain's rec_note_wait; it no longer stops the process), a one-off command buffer queued behind work that waits for an event nobody signals
// (drain_queue_wait), a fence nothing completes and an upload batch never reported back (mtl_finish_gpu). Each returns within the limit plus 150 ms with its step's name and leaves
// no limit on the thread; once let go, the plain mtl_drain finishes, and mtl_drain_by with nothing stuck returns NULL. A limit already past names the one-off command buffer's step
// without waiting. t_gfx_suspend checks dm_gfx_suspend's own bound and lines
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; printf("FAIL: %s (line %d)\n", label, __LINE__); } else printf("ok: %s\n", label); } while (0)
enum { LIMIT_MS = 300 };
static const char *g_what; static double g_took;
static void drain_limited(void) {
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    g_what = mtl_drain_by(t0 + LIMIT_MS * 1000000ull);
    g_took = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
    printf("  mtl_drain_by: %s after %.0f ms\n", g_what ? g_what : "drained", g_took);
}
static int said(const char *step) { return g_what && strstr(g_what, step) && g_took >= LIMIT_MS - 5 && g_took < LIMIT_MS + 150 && !t_drain_by; }

int main(void) { @autoreleasepool {
    mtl_init();
    drain_limited();
    CHECK(!g_what && g_took < LIMIT_MS, "nothing stuck: drained, before the limit");

    // A present on the GPU that never reports back, with the display link owning the layer
    pthread_mutex_lock(&g_pq_mu); int owned = g_link_owns_layer; g_link_owns_layer = 1; g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    drain_limited();
    CHECK(said("the present on the GPU (present_drop_for_suspend)"), "a present on the GPU: the drain ends at its limit and names it");
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight--; g_link_owns_layer = owned; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);

    // A snapshot copy the encode thread never notes: no 10 s stop of the process, the step named
    uint64_t snap_v = g_snap_v; g_snap_v = atomic_load(&g_snap_cmt) + 1;
    drain_limited();
    CHECK(said("the present snapshot copies (snap_drain)"), "a snapshot copy never noted: the drain ends at its limit and names it");
    g_snap_v = snap_v;

    // A one-off command buffer queued behind a command buffer that waits for an event nobody signals
    id<MTLSharedEvent> hold = [g_dev newSharedEvent];
    id<MTLCommandBuffer> held = [g_q commandBuffer]; [held encodeWaitForEvent:hold value:1]; [held commit];
    drain_limited();
    CHECK(said("a one-off command buffer behind all earlier work (drain_queue_wait)"), "a one-off command buffer held behind a wait: the drain ends at its limit and names it");
    hold.signaledValue = 1;   // the wait ends, and the late completion of the one-off command buffer comes into the wait that has gone
    mtl_drain();
    CHECK(1, "the late completion is taken, and the plain drain finishes after it");

    // A limit already past when the drain reaches the one-off command buffer: it commits none, and the step is named at once
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const char *past = mtl_drain_by(1);
    double past_ms = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6;
    printf("  mtl_drain_by with its limit past: %s after %.1f ms\n", past ? past : "drained", past_ms);
    CHECK(past && strstr(past, "(drain_queue_wait)") && past_ms < 100 && !t_drain_by, "a limit already past: the one-off command buffer's step is named without waiting");

    // A fence nothing completes
    atomic_fetch_add(&g_fence, 1);
    drain_limited();
    CHECK(said("the GPU work up to the last fence (mtl_finish_gpu)"), "a fence never completed: the drain ends at its limit and names it");
    atomic_fetch_sub(&g_fence, 1);

    // An upload batch never reported back
    atomic_fetch_add(&g_up_pending, 1);
    drain_limited();
    CHECK(said("the upload batches (mtl_finish_gpu)"), "an upload batch never reported back: the drain ends at its limit and names it");
    atomic_fetch_sub(&g_up_pending, 1); up_wake();

    mtl_drain();
    drain_limited();
    CHECK(!g_what && g_took < LIMIT_MS, "everything let go: drained, before the limit");
    printf("t_suspend_drain: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
