// Residency changes and their commit: res_add / res_remove only queue a change, and res_commit applies and commits what is queued under a lock of its own. A commit that adds
// hundreds of MB takes tens of milliseconds; while it held the lock res_add takes, every res_add on any thread waited for it (Switch Sports Resort's court load: the submit
// thread's res_add waited 20-43 ms in one frame for the upload batches' completion handlers). Checks, with real Metal allocations:
//  - a res_add or res_remove while another thread's commit is under way returns at once (it waited for the whole commit before);
//  - after res_commit returns, what was added before it is in the set, also when another thread's commit took the change;
//  - a removed allocation stays alive until the commit that takes it out of the set, then goes.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { BIG_BYTES = 48u << 20 };

static id<MTLBuffer> buf(size_t n) { return [g_dev newBufferWithLength:n options:MTLResourceStorageModePrivate]; }

int main(void) { @autoreleasepool {
    mtl_init();
    CHECK("a residency set", g_res != nil);

    // Added, committed, in the set
    id<MTLBuffer> a = buf(1u << 20);
    res_add(a); res_commit();
    CHECK("an allocation added and committed is in the set", [g_res containsAllocation:a]);

    // Removed: alive until its commit, gone after it
    __weak id<MTLBuffer> weak_r = nil;
    @autoreleasepool { id<MTLBuffer> r = buf(1u << 20); weak_r = r; res_add(r); res_commit(); res_remove(r); }
    CHECK("a removed allocation stays alive until its removal is committed", weak_r != nil);
    @autoreleasepool { res_commit(); }
    CHECK("and goes once it is", weak_r == nil);

    // A res_add while another thread's commit is under way: a commit alone takes well under a millisecond here, but tens of milliseconds in a game whose GPU is busy (10-25 ms
    // each in Switch Sports Resort's court load), so one is stood in for by holding the commit's lock for 100 ms, as a commit holds it
    NSMutableArray *small = [NSMutableArray array];
    dispatch_semaphore_t held = dispatch_semaphore_create(0), released = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        pthread_mutex_lock(&g_res_commit_mu); dispatch_semaphore_signal(held); usleep(100000); pthread_mutex_unlock(&g_res_commit_mu); dispatch_semaphore_signal(released);
    });
    dispatch_semaphore_wait(held, DISPATCH_TIME_FOREVER);
    uint64_t worst = 0, t_all = bp_now();
    for (int i = 0; i < 200; i++) { id<MTLBuffer> s = buf(4096); [small addObject:s]; uint64_t t0 = bp_now(); res_add(s); res_remove(s); res_add(s); uint64_t d = bp_now() - t0; if (d > worst) worst = d; }
    uint64_t adds = bp_now() - t_all;
    printf("200 res_add / res_remove while a commit holds its lock: %.2f ms in all, the slowest %.3f ms\n", adds / 1e6, worst / 1e6);
    CHECK("res_add and res_remove do not wait for a commit under way", adds < 50000000ull);
    uint64_t t_c = bp_now(); res_commit(); uint64_t waited = bp_now() - t_c;
    CHECK("res_commit waits for the commit under way, then commits its own changes", waited > 20000000ull && dispatch_semaphore_wait(released, DISPATCH_TIME_NOW) == 0);
    int all = 1; for (id<MTLBuffer> s in small) all &= [g_res containsAllocation:s];
    CHECK("everything added is in the set after the commit", all);

    // A change another thread's commit took is in the set when this thread's res_commit returns
    for (int round = 0; round < 50; round++) {
        id<MTLBuffer> x = buf(BIG_BYTES / 4), y = buf(4096);
        res_add(x);
        dispatch_semaphore_t go = dispatch_semaphore_create(0), fin = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ dispatch_semaphore_signal(go); res_commit(); dispatch_semaphore_signal(fin); });
        dispatch_semaphore_wait(go, DISPATCH_TIME_FOREVER);
        res_add(y); res_commit();
        int ok = [g_res containsAllocation:x] && [g_res containsAllocation:y];
        dispatch_semaphore_wait(fin, DISPATCH_TIME_FOREVER);
        if (!ok) { CHECK("a change committed by another thread is in the set when res_commit returns", 0); break; }
        res_remove(x); res_remove(y); res_commit();
    }
    CHECK("racing commits leave nothing queued", !res_pending());
    for (id<MTLBuffer> s in small) res_remove(s);
    res_remove(a); res_commit();
    printf("t_res_commit: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
