// Delayed CPU feedback must not hold an allocator after its GPU fence signals.
// A real Metal queue is gated first, then released while feedback stays withheld.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static dispatch_semaphore_t waiter_done;
static int waited;
static void *waiter(void *unused) { (void)unused; @autoreleasepool {
    waited = mtl_wait_at(1, UINT64_MAX, WS_ALLOC);
    dispatch_semaphore_signal(waiter_done);
} return NULL; }
int main(void) { @autoreleasepool {
    alarm(15); setbuf(stdout, NULL); setenv("DM_NVN_ENC_THREAD", "0", 1);
    mtl_init();
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    dispatch_semaphore_t feedback_entered = dispatch_semaphore_create(0);
    dispatch_semaphore_t feedback_release = dispatch_semaphore_create(0);
    dispatch_semaphore_t feedback_done = dispatch_semaphore_create(0);
    id<MTL4CommandAllocator> alloc = [g_dev newCommandAllocator];
    id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:alloc]; [cb endCommandBuffer];
    g_fence = 1; g_done = 0;
    post_rec *r = &g_postr[1]; r->n = 0; r->ops = NULL; r->sc = NULL;
    atomic_store(&r->claimed, 0); atomic_store(&r->fence, 1);
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        (void)f; dispatch_semaphore_signal(feedback_entered);
        dispatch_semaphore_wait(feedback_release, DISPATCH_TIME_FOREVER);
        post_complete(1, 0, 0); dispatch_semaphore_signal(feedback_done);
    }];
    [g_q4 waitForEvent:gate value:1];
    id<MTL4CommandBuffer> buffers[] = {cb}; [g_q4 commit:buffers count:1 options:o];
    [g_q4 signalEvent:g_ev value:1];
    waiter_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, waiter, NULL);
    int early = dispatch_semaphore_wait(waiter_done, dispatch_time(DISPATCH_TIME_NOW, 50*NSEC_PER_MSEC)) == 0;
    printf("blocked before GPU completion: %s\n", early ? "FAIL" : "PASS");
    gate.signaledValue = 1;
    int signaled = [g_ev waitUntilSignaledValue:1 timeoutMS:2000];
    int entered = dispatch_semaphore_wait(feedback_entered, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0;
    int prompt = early || dispatch_semaphore_wait(waiter_done, dispatch_time(DISPATCH_TIME_NOW, 100*NSEC_PER_MSEC)) == 0;
    printf("GPU completed while feedback withheld: %s\n", signaled && entered ? "PASS" : "FAIL");
    printf("allocator wakes before feedback: %s\n", prompt ? "PASS" : "FAIL");
    int retired = g_done == 1 && atomic_load(&r->claimed);
    dispatch_semaphore_signal(feedback_release);
    dispatch_semaphore_wait(feedback_done, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC));
    pthread_join(th, NULL);
    printf("completion safely published once: %s\n", retired && waited && g_done == 1 && atomic_load(&r->claimed) ? "PASS" : "FAIL");
    return early || !signaled || !entered || !prompt || !retired || !waited;
} }
