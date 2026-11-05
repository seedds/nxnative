// A GPU fence can signal before Metal delivers its timing feedback. Timestamp
// reports must not replace that missing measurement with CPU queueing time.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failures;
#define CHECK(label, condition) do { \
    int passed = !!(condition); \
    checks++; failures += !passed; \
    printf("%s: %s\n", label, passed ? "PASS" : "FAIL"); \
} while (0)

typedef struct {
    uint64_t fence;
    int result;
    dispatch_semaphore_t entered, completed;
} wait_state;

static void *wait_for_submission(void *argument) {
    wait_state *state = argument;
    @autoreleasepool {
        dispatch_semaphore_signal(state->entered);
        state->result = mtl_wait(state->fence, 5 * NSEC_PER_SEC);
    }
    dispatch_semaphore_signal(state->completed);
    return NULL;
}

static int wait_bounded(dispatch_semaphore_t semaphore, uint64_t nanoseconds) {
    return dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, nanoseconds)) == 0;
}

int main(void) {
    @autoreleasepool {
        alarm(15);
        setbuf(stdout, NULL);
        setenv("DM_NVN_GPU_TS", "segment", 1);
        setenv("DM_NVN_ENC_THREAD", "0", 1);
        mtl_init();

        id<MTLBuffer> buffer = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        CHECK("GPU test storage allocated", buffer != nil);
        if (!buffer) return 1;
        res_add(buffer);

        uint64_t top[2] = {UINT64_MAX, UINT64_MAX};
        uint64_t bottom[2] = {UINT64_MAX, UINT64_MAX};
        volatile uint32_t event = 0;
        dispatch_semaphore_t feedback_entered = dispatch_semaphore_create(0);
        dispatch_semaphore_t release_feedback = dispatch_semaphore_create(0);
        dispatch_semaphore_t feedback_finished = dispatch_semaphore_create(0);

        mtl_sub_begin();
        mtl_sub_counter((uint64_t)top, NVN_COUNTER_TYPE_TIMESTAMP_TOP);
        id<MTL4ComputeCommandEncoder> encoder = [g_cb computeCommandEncoder];
        [encoder fillBuffer:buffer range:NSMakeRange(0, 4096) value:73];
        [encoder endEncoding];
        mtl_sub_counter((uint64_t)bottom, NVN_COUNTER_TYPE_TIMESTAMP);
        mtl_sub_event(&event, 0, 47);
        [g_commit_blocks addObject:^{
            dispatch_semaphore_signal(feedback_entered);
            if (!wait_bounded(release_feedback, 5 * NSEC_PER_SEC)) abort();
        }];
        uint64_t fence = mtl_sub_commit();
        int entered = wait_bounded(feedback_entered, 5 * NSEC_PER_SEC);
        CHECK("GPU completed and feedback reached its gate", entered);
        if (!entered) return 1;
        CHECK("GPU event is already signaled", [g_ev waitUntilSignaledValue:fence timeoutMS:1000]);

        wait_state waiter = {.fence = fence, .entered = dispatch_semaphore_create(0),
                             .completed = dispatch_semaphore_create(0)};
        pthread_t thread;
        if (pthread_create(&thread, NULL, wait_for_submission, &waiter)) abort();
        CHECK("guest entered its fence wait", wait_bounded(waiter.entered, NSEC_PER_SEC));
        int returned_early = wait_bounded(waiter.completed, 50 * NSEC_PER_MSEC);
        CHECK("timestamp fence stays pending until measured GPU times arrive", !returned_early);
        // Inspect the reports only when the waiter finished, or under the lock
        // that serializes CPU-visible completion writes.
        pthread_mutex_lock(&g_post_mu);
        CHECK("no CPU elapsed time is published as a GPU timestamp",
              top[1] == UINT64_MAX && bottom[1] == UINT64_MAX);
        CHECK("following event does not overtake pending timestamp reports", event == 0);
        pthread_mutex_unlock(&g_post_mu);

        dispatch_semaphore_signal(release_feedback);
        if (!returned_early) CHECK("guest completes once feedback is released", wait_bounded(waiter.completed, 5 * NSEC_PER_SEC));
        pthread_join(thread, NULL);
        dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(feedback_finished); });
        CHECK("feedback queue drained", wait_bounded(feedback_finished, 5 * NSEC_PER_SEC));
        CHECK("guest wait succeeds with all reports visible", waiter.result && event == 47);
        CHECK("both GPU timestamps have been written", top[1] != UINT64_MAX && bottom[1] != UINT64_MAX);
        CHECK("GPU interval is nonnegative", bottom[1] >= top[1]);
        CHECK("real GPU work was executed", ((const uint8_t *)buffer.contents)[0] == 73);
        printf("Timestamp feedback: %d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
}
