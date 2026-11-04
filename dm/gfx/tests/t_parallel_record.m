// Concurrent record producers finish out of order; only the coordinator publishes GPU command order.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { MARKERS = REC_BLK + 17 };
static int seen[2 * MARKERS]; static _Atomic int nseen, finished, gpu_errors, token_freed, discarded_ran;
@interface RecordToken : NSObject @end
@implementation RecordToken
- (void)dealloc { atomic_fetch_add(&token_freed, 1); }
@end
typedef struct {
    int context, number, finish;
    id<MTL4CommandAllocator> allocator;
    id<MTLBuffer> buffer;
    dispatch_semaphore_t ready, go, second_done, gpu_done;
} record_job;
static void *record_worker(void *arg) { @autoreleasepool {
    record_job *job = arg; rx_select(job->context);
    dispatch_semaphore_signal(job->ready); dispatch_semaphore_wait(job->go, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < MARKERS; i++) {
        int tag = job->number * MARKERS + i;
        rec_queue_block(^{ int k = atomic_fetch_add(&nseen, 1); if (k < 2 * MARKERS) seen[k] = tag; });
    }
    id cb = rec_cb_new(); [cb beginCommandBufferWithAllocator:job->allocator];
    id<MTL4ComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e fillBuffer:job->buffer range:NSMakeRange(0, 16) value:(uint8_t)(job->number ? 0x22 : 0x11)];
    [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding]; [cb endCommandBuffer];
    MTL4CommitOptions *options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> f) { if (f.error) atomic_fetch_add(&gpu_errors, 1); dispatch_semaphore_signal(job->gpu_done); }];
    rec_commit(cb, options); rec_publish();
    if (!job->number) dispatch_semaphore_wait(job->second_done, DISPATCH_TIME_FOREVER);
    job->finish = atomic_fetch_add(&finished, 1) + 1;
    if (job->number) dispatch_semaphore_signal(job->second_done);
    return NULL;
} }
enum { COPY_COUNT = UBC_N + 17, COPY_BYTES = 16 };
typedef struct { uint8_t *bytes; uint64_t *seq; dispatch_semaphore_t ready, go; } copy_job;
static void *copy_producer(void *arg) {
    copy_job *job = arg;
    dispatch_semaphore_signal(job->ready); dispatch_semaphore_wait(job->go, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < COPY_COUNT; i++) job->seq[i] = ubc_push(job->bytes + (i + 1) * COPY_BYTES, job->bytes + i * COPY_BYTES, COPY_BYTES);
    ubc_wait(job->seq[COPY_COUNT - 1]); return NULL;
}
static void check_copy_producers(void) {
    copy_job jobs[2] = {0}; pthread_t threads[2]; uint64_t start = ubc_head();
    uint64_t *sequences = malloc(2 * COPY_COUNT * sizeof *sequences);
    CHECK(sequences != NULL, "allocate copy sequence ledger"); if (!sequences) return;
    for (int i = 0; i < 2; i++) {
        jobs[i].bytes = calloc(COPY_COUNT + 1, COPY_BYTES); jobs[i].seq = sequences + i * COPY_COUNT;
        CHECK(jobs[i].bytes != NULL, "allocate independent dependent-copy chain"); if (!jobs[i].bytes) abort();
        memset(jobs[i].bytes, 0x30 + i, COPY_BYTES);
        jobs[i].ready = dispatch_semaphore_create(0); jobs[i].go = dispatch_semaphore_create(0);
        int err = pthread_create(&threads[i], NULL, copy_producer, &jobs[i]);
        CHECK(!err, "start concurrent uniform-copy producer"); if (err) abort();
    }
    for (int i = 0; i < 2; i++) dispatch_semaphore_wait(jobs[i].ready, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < 2; i++) dispatch_semaphore_signal(jobs[i].go);
    for (int i = 0; i < 2; i++) pthread_join(threads[i], NULL);
    CHECK(ubc_head() == start + 2 * COPY_COUNT, "both producers publish every operation through ring wraparound");
    qsort(sequences, 2 * COPY_COUNT, sizeof *sequences, par_u64cmp);
    int unique = 1; for (int i = 0; i < 2 * COPY_COUNT; i++) if (sequences[i] != start + i + 1) unique = 0;
    CHECK(unique, "copy completion sequences are unique and have no gaps");
    for (int i = 0; i < 2; i++) {
        int intact = 1; for (int b = COPY_BYTES; b < (COPY_COUNT + 1) * COPY_BYTES; b++) if (jobs[i].bytes[b] != 0x30 + i) intact = 0;
        CHECK(intact, "each dependent copy observes its predecessor across ring reuse"); free(jobs[i].bytes);
    }
    free(sequences);
}
typedef struct {
    int context, failed; uint64_t fence; id<MTLBuffer> first;
    dispatch_semaphore_t ready, go;
} arena_job;
static void *arena_producer(void *arg) { @autoreleasepool {
    arena_job *job = arg; rx_select(job->context);
    for (int i = 0; i < 1000; i++) {
        arena_begin(); id<MTLBuffer> buf; size_t off = arena_alloc(128, &buf);
        if (!i) { job->first = buf; dispatch_semaphore_signal(job->ready); dispatch_semaphore_wait(job->go, DISPATCH_TIME_FOREVER); }
        uint8_t *bytes = (uint8_t *)buf.contents + off;
        memset(bytes, job->context, 128);
        for (int k = 0; k < 128; k++) if (bytes[k] != job->context) job->failed = 1;
        // These allocations are CPU-only, so an already completed GPU fence is sufficient for reuse.
        arena_commit(job->fence);
    }
    return NULL;
} }
static void check_arena_producers(void) {
    mtl_sub_begin(); uint64_t fence = mtl_sub_commit(); mtl_drain();
    uint64_t made = atomic_load(&g_arena_made); arena_job jobs[2] = {0}; pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        jobs[i].context = i + 1; jobs[i].fence = fence;
        jobs[i].ready = dispatch_semaphore_create(0); jobs[i].go = dispatch_semaphore_create(0);
        int err = pthread_create(&threads[i], NULL, arena_producer, &jobs[i]);
        CHECK(!err, "start independent arena producer"); if (err) abort();
    }
    for (int i = 0; i < 2; i++) dispatch_semaphore_wait(jobs[i].ready, DISPATCH_TIME_FOREVER);
    CHECK(jobs[0].first != jobs[1].first, "simultaneously owned arenas cannot alias");
    for (int i = 0; i < 2; i++) dispatch_semaphore_signal(jobs[i].go);
    for (int i = 0; i < 2; i++) { pthread_join(threads[i], NULL); CHECK(!jobs[i].failed, "arena payload survives concurrent claim and retirement"); }
    CHECK(atomic_load(&g_arena_made) <= made + 2, "completed arenas are reused rather than leaked per context");
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_ENC_THREAD", "1", 1); mtl_init();
    CHECK(!g_rb_started, "exercise concurrent cold startup of the recording queue");
    id<MTLBuffer> buffer = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    memset(buffer.contents, 0, 16); res_add(buffer); res_commit();
    record_job jobs[2] = {0}; pthread_t threads[2];
    dispatch_semaphore_t second_done = dispatch_semaphore_create(0);
    for (int i = 0; i < 2; i++) {
        jobs[i].context = i + 1; jobs[i].number = i; jobs[i].buffer = buffer;
        jobs[i].ready = dispatch_semaphore_create(0); jobs[i].go = dispatch_semaphore_create(0);
        jobs[i].gpu_done = dispatch_semaphore_create(0); jobs[i].second_done = second_done;
        NSError *error = nil;
        jobs[i].allocator = [g_dev newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:&error];
        CHECK(jobs[i].allocator != nil, "allocate independent worker command storage");
        if (!jobs[i].allocator) return 1;
        rec_stream_hold(&g_rec_streams[jobs[i].context]);
        int err = pthread_create(&threads[i], NULL, record_worker, &jobs[i]);
        CHECK(!err, "start concurrent record producer"); if (err) return 1;
    }
    for (int i = 0; i < 2; i++) dispatch_semaphore_wait(jobs[i].ready, DISPATCH_TIME_FOREVER);
    for (int i = 0; i < 2; i++) dispatch_semaphore_signal(jobs[i].go);
    for (int i = 0; i < 2; i++) pthread_join(threads[i], NULL);
    CHECK(jobs[1].finish == 1 && jobs[0].finish == 2, "workers finish in reverse command order");
    rec_drain();
    CHECK(!atomic_load(&nseen) && ((uint32_t *)buffer.contents)[0] == 0, "held streams publish neither callbacks nor GPU writes");
    for (int i = 0; i < 2; i++) rec_stream_submit(&g_rec_streams[jobs[i].context]);
    rec_drain();
    for (int i = 0; i < 2; i++) CHECK(!dispatch_semaphore_wait(jobs[i].gpu_done, dispatch_time(DISPATCH_TIME_NOW, 5000000000ll)), "GPU command stream completes");
    CHECK(!atomic_load(&gpu_errors), "GPU feedback has no validation error");
    CHECK(atomic_load(&nseen) == 2 * MARKERS, "every callback executes exactly once across block boundaries");
    int ordered = 1; for (int i = 0; i < 2 * MARKERS; i++) if (seen[i] != i) ordered = 0;
    CHECK(ordered, "publication preserves command order despite reverse worker completion");
    CHECK(((uint32_t *)buffer.contents)[0] == 0x22222222u, "dependent GPU writes execute in submission order");
    rx_select(3); rec_stream_hold(&g_rec_streams[3]);
    @autoreleasepool {
        RecordToken *token = [RecordToken new];
        for (int i = 0; i < MARKERS; i++) rec_queue_block(^{ (void)[token class]; atomic_fetch_add(&discarded_ran, 1); });
    }
    CHECK(!atomic_load(&token_freed), "held records retain their captured objects");
    rec_stream_discard(&g_rec_streams[3]); rx_select(0); rec_drain();
    CHECK(atomic_load(&token_freed) == 1, "discard releases all captured objects");
    CHECK(!atomic_load(&discarded_ran), "discard executes no recorded side effects");
    rx_select(3); rec_stream_hold(&g_rec_streams[3]);
    rec_queue_block(^{ atomic_fetch_add(&discarded_ran, 1); });
    rec_stream_submit(&g_rec_streams[3]); rx_select(0); rec_drain();
    CHECK(atomic_load(&discarded_ran) == 1, "discarded stream can be reused");
    check_copy_producers();
    check_arena_producers();
    printf("parallel recording: %d checks, %d failures\n", checks, failed); return failed != 0;
} }
