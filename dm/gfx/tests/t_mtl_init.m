// Race the first Metal device request with graphics suspend or another classic device request.
#import <Metal/Metal.h>
static id<MTLDevice> gated_device(void);
#define MTLCreateSystemDefaultDevice gated_device
#include "../nvn/nvn_mtl.m"
#undef MTLCreateSystemDefaultDevice

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void *front_metal_layer(void) { return NULL; }
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static dispatch_semaphore_t requesting_device, allow_device, second_started, second_returned;
static _Atomic int device_requests;

static id<MTLDevice> gated_device(void) {
    if (atomic_fetch_add(&device_requests, 1)) {
        fputs("FAIL: two threads requested the Metal device during initialization\n", stderr);
        exit(1);
    }
    dispatch_semaphore_signal(requesting_device);
    dispatch_semaphore_wait(allow_device, DISPATCH_TIME_FOREVER);
    return MTLCreateSystemDefaultDevice();
}

static void await(dispatch_semaphore_t semaphore, const char *message) {
    if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC))) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main(void) {
    @autoreleasepool {
        alarm(15);
        requesting_device = dispatch_semaphore_create(0);
        allow_device = dispatch_semaphore_create(0);
        second_started = dispatch_semaphore_create(0);
        second_returned = dispatch_semaphore_create(0);
        dispatch_queue_t workers = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
        dispatch_group_t work = dispatch_group_create();
        int classic = getenv("DM_TEST_NO_METAL4") != NULL;
        dispatch_group_async(work, workers, ^{
            if (classic) mtl_device_classic();
            else mtl_init();
        });
        await(requesting_device, "Metal initialization did not request a device");
        dispatch_group_async(work, workers, ^{
            dispatch_semaphore_signal(second_started);
            if (classic) mtl_device_classic();
            else dm_gfx_suspend(1);
            dispatch_semaphore_signal(second_returned);
        });
        await(second_started, "the second caller did not start");
        if (dispatch_semaphore_wait(second_returned, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC)) == 0) {
            fputs("FAIL: the second caller returned before initialization completed\n", stderr);
            return 1;
        }
        dispatch_semaphore_signal(allow_device);
        if (dispatch_group_wait(work, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
            atomic_load(&device_requests) != 1 || !g_q || !g_ev || g_classic != classic ||
            (!classic && (!g_q4 || !g_feedback_q))) {
            fputs("FAIL: the callers did not finish with one fully initialized device\n", stderr);
            return 1;
        }
        if (classic) dm_gfx_suspend(1);   // The classic fallback has no residency sets to attach during the drain.
        printf("Metal initialization passed with concurrent %s\n", classic ? "classic device requests" : "graphics suspend");
        return 0;
    }
}
