// With Metal encoding on its own thread (DM_NVN_ENC_THREAD), a submission's end hands its records to the encode thread and returns: the replay translates the next submission while
// the encode thread finishes this one. It used to wait for the encode thread (rec_drain) at the end of every submission, and Ocarina of Time makes about 30 a frame (one at each event
// signal), so on the iPhone the GPU sat idle before most of them (13.6 of 14.8 ms of gaps in a 35 ms frame). What needs the work committed waits for the submission's note
// (g_sub_cmt): a present of the game's own texture (no snapshot), before its display command buffer. The encode thread is held inside a submission by a queue block here.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)

static dispatch_semaphore_t g_gate, g_sub_done, g_wait_done;
static _Atomic uint64_t g_fence_out;
static void *submit_thread(void *arg) {   // one submission whose encoding the gate holds
    (void)arg; @autoreleasepool {
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        dispatch_semaphore_t gate = g_gate; rec_queue_block(^{ dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        atomic_store(&g_fence_out, mtl_sub_commit()); dispatch_semaphore_signal(g_sub_done);
    } return NULL;
}
static void *present_wait_thread(void *arg) { sub_commit_wait((uint64_t)(uintptr_t)arg); dispatch_semaphore_signal(g_wait_done); return NULL; }

int main(void) { @autoreleasepool {
    alarm(20); setbuf(stdout, NULL);
    setenv("DM_NVN_ENC_THREAD", "1", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    mtl_init();
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t f0 = mtl_sub_commit();
    CHECK("a submission completes on the GPU", mtl_wait(f0, 2000000000ull));
    CHECK("its commit note is passed", atomic_load(&g_sub_cmt) >= f0);

    g_gate = dispatch_semaphore_create(0); g_sub_done = dispatch_semaphore_create(0); g_wait_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, submit_thread, NULL);
    long returned = dispatch_semaphore_wait(g_sub_done, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)) == 0;
    CHECK("the submission returns while the encode thread is still encoding it", returned);
    uint64_t f1 = returned ? atomic_load(&g_fence_out) : f0 + 1;
    CHECK("its commit note is not passed before the encode thread commits it", atomic_load(&g_sub_cmt) < f1);
    pthread_t pw; pthread_create(&pw, NULL, present_wait_thread, (void *)(uintptr_t)f1);
    CHECK("a present of the game's texture waits for that commit", dispatch_semaphore_wait(g_wait_done, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) != 0);
    dispatch_semaphore_signal(g_gate);
    if (!returned) dispatch_semaphore_wait(g_sub_done, DISPATCH_TIME_FOREVER);
    pthread_join(th, NULL);
    CHECK("the present goes on once the encode thread has committed it", dispatch_semaphore_wait(g_wait_done, dispatch_time(DISPATCH_TIME_NOW, 2000 * NSEC_PER_MSEC)) == 0);
    pthread_join(pw, NULL);
    CHECK("the held submission completes on the GPU", mtl_wait(atomic_load(&g_fence_out), 2000000000ull));
    CHECK("its commit note is passed", atomic_load(&g_sub_cmt) >= atomic_load(&g_fence_out));
    printf("t_sub_overlap: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
