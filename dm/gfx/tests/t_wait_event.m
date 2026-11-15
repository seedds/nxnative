// Contract fixture: real public NVN lookup -> recording -> QueueSubmit -> Metal replay.
// An event wait gates GPU work, but never blocks CPU submission or another NVN queue.
#define nvn_game_fps nvn_game_fps_mtl_default
#define nvn_event_publish_gpu event_test_publish
#include "../nvn/nvn_mtl.m"
#undef nvn_event_publish_gpu
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#define P(name) ((NVNT_##name)nvn_lookup(#name))
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c,label) do { int ok=!!(c); checks++; fails+=!ok; printf("%s: %s\n",ok?"PASS":"FAIL",label); fflush(stdout); } while(0)
// Observe the exact release boundary, then call the real event publisher. This
// avoids a timing race between a CPU polling thread and a very small store into storage.
static _Atomic uintptr_t event_test_storage;
static _Atomic uint32_t event_test_pixel;
void event_test_publish(volatile uint32_t *word,unsigned mode,uint32_t value,uint64_t fence) {
    uintptr_t storage=atomic_load(&event_test_storage);
    if(storage && value==11) {
        atomic_store(&event_test_pixel,*(volatile uint32_t *)storage);
    }
    nvn_event_publish_gpu(word,mode,value,fence);
}

typedef struct job { NVNqueue *queue; NVNcommandHandle handle; dispatch_semaphore_t done; struct job *next; } job;
static pthread_mutex_t jobs_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cv=PTHREAD_COND_INITIALIZER;
static job *jobs_first,*jobs_last; static int jobs_stop;
static void *submit_worker(void *unused) { (void)unused; @autoreleasepool {
    for (;;) {
        pthread_mutex_lock(&jobs_mu);
        while (!jobs_first && !jobs_stop) pthread_cond_wait(&jobs_cv,&jobs_mu);
        if (!jobs_first) { pthread_mutex_unlock(&jobs_mu); break; }
        job *j=jobs_first; jobs_first=j->next; if (!jobs_first) jobs_last=NULL;
        pthread_mutex_unlock(&jobs_mu);
        P(nvnQueueSubmitCommands)(j->queue,1,&j->handle);
        dispatch_semaphore_signal(j->done);
    }
} return NULL; }
static void enqueue(job *j,NVNqueue *queue,NVNcommandHandle handle) {
    *j=(job){.queue=queue,.handle=handle,.done=dispatch_semaphore_create(0)};
    pthread_mutex_lock(&jobs_mu); if(jobs_last) jobs_last->next=j; else jobs_first=j;
    jobs_last=j; pthread_cond_signal(&jobs_cv); pthread_mutex_unlock(&jobs_mu);
}
static int returned(job *j,uint64_t ns) { return dispatch_semaphore_wait(j->done,dispatch_time(DISPATCH_TIME_NOW,(int64_t)ns))==0; }
static int landed(NVNsync *s,uint64_t ns) {
    NVNsyncWaitResult r=P(nvnSyncWait)(s,ns);
    return r==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
}
static void require_returned(job *j,int seen) {
    if (!seen && !returned(j,5000000000ull)) { fprintf(stderr,"submission did not return after rescue release\n"); _Exit(2); }
}
static NVNdevice device; static NVNqueue queues[2]; static NVNmemoryPool pool;
static NVNevent event; static uint8_t *memory; static volatile uint32_t *source,*result;
static NVNbufferAddress source_addr,result_addr; static NVNcommandBuffer commands[2];
static NVNcommandHandle begin_cmd(int k) {
    P(nvnCommandBufferInitialize)(&commands[k],&device);
    P(nvnCommandBufferAddCommandMemory)(&commands[k],&pool,k*32768,32768);
    P(nvnCommandBufferBeginRecording)(&commands[k]); return 0;
}
static NVNcommandHandle consumer(int mode,uint32_t target,NVNsync *fence,int wait) {
    begin_cmd(0);
    if(wait) P(nvnCommandBufferWaitEvent)(&commands[0],&event,(NVNeventWaitMode)mode,target);
    P(nvnCommandBufferCopyBufferToBuffer)(&commands[0],source_addr,result_addr,4,0);
    P(nvnCommandBufferFenceSync)(&commands[0],fence,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    return P(nvnCommandBufferEndRecording)(&commands[0]);
}
static const uint32_t sentinel=0x13579bdf, before=0x01020304, after=0xa1b2c3d4;
static void case_wait(const char *label,int mode,uint32_t initial,uint32_t target,int ready) {
    printf("CASE %s mode=%d initial=%#x target=%#x ready=%d\n",label,mode,initial,target,ready);
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,initial);
    NVNsync fence; P(nvnSyncInitialize)(&fence,&device); job j;
    enqueue(&j,&queues[0],consumer(mode,target,&fence,1));
    int cpu_returned=returned(&j,250000000ull);
    CHECK(cpu_returned,"QueueSubmitCommands returns while the event is unresolved");
    int completed=landed(&fence,ready?1000000000ull:100000000ull);
    CHECK(completed==ready,"GPU completion follows the event predicate before CPU release");
    CHECK(*result==(ready?before:sentinel),"GPU output before CPU release");
    // Release on every path, including failed assertions and a CPU-blocking implementation.
    if(!ready) *source=after;
    P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,target);
    require_returned(&j,cpu_returned);
    int finished=landed(&fence,5000000000ull); CHECK(finished,"GPU work finishes after CPU rescue/release");
    if(!finished) { fprintf(stderr,"GPU did not finish after unconditional event release\n"); _Exit(2); }
    CHECK(*result==(ready?before:after),"GPU reads payload from the correct side of the event release");
    P(nvnSyncFinalize)(&fence);
}
static void case_cross_queue(void) {
    printf("CASE forward cross-queue producer\n");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    NVNsync consumed,produced; P(nvnSyncInitialize)(&consumed,&device); P(nvnSyncInitialize)(&produced,&device);
    NVNcommandHandle ch=consumer(0,1,&consumed,1);
    begin_cmd(1);
    uint32_t payload=after;
    P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&payload);
    P(nvnCommandBufferSignalEvent)(&commands[1],&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,1);
    P(nvnCommandBufferFenceSync)(&commands[1],&produced,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    NVNcommandHandle ph=P(nvnCommandBufferEndRecording)(&commands[1]);
    job cj,pj; enqueue(&cj,&queues[0],ch); int cr=returned(&cj,250000000ull);
    CHECK(cr,"consumer submit returns before a later producer is submitted");
    CHECK(!landed(&consumed,100000000ull) && *result==sentinel,"consumer is gated before producer submission");
    enqueue(&pj,&queues[1],ph); int pr=returned(&pj,250000000ull);
    CHECK(pr,"producer on a different NVN queue can be submitted");
    int done=landed(&consumed,1000000000ull);
    CHECK(done,"later producer releases the consumer without CPU intervention");
    CHECK(done && *result==after,"consumer sees the later producer's GPU payload");
    // Even a single-Metal-queue deadlock is released before joining/draining.
    P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1);
    require_returned(&cj,cr); require_returned(&pj,pr);
    int drained=landed(&consumed,5000000000ull) && landed(&produced,5000000000ull);
    CHECK(drained,"both queues drain after unconditional cleanup release");
    if(!drained) _Exit(2);
    P(nvnSyncFinalize)(&consumed); P(nvnSyncFinalize)(&produced);
}
// Receipt tests observe public sync completion. This helper only waits until the scheduler has encoded the logical fence, so a negative GPU assertion cannot pass merely because its CPU worker has not run yet.
static int receipt_encoded(NVNsync *sync,uint64_t timeout) {
    uint64_t start=now_ns(),fence=0; int ready=0;
    pthread_mutex_lock(&nq_mu);
    while(!(ready=nq_sync_encoded((const sync_t *)sync,&fence))) {
        uint64_t elapsed=now_ns()-start; if(elapsed>=timeout) break;
        uint64_t left=timeout-elapsed; struct timespec ts={(time_t)(left/1000000000ull),(long)(left%1000000000ull)};
        pthread_cond_timedwait_relative_np(&nq_cv,&nq_mu,&ts);
    }
    pthread_mutex_unlock(&nq_mu); return ready;
}
static void receipt_unrelated_completion(void) {
    printf("CASE unrelated physical completion cannot release a parked logical fence\n");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    NVNsync command_fence,logical,unrelated; P(nvnSyncInitialize)(&command_fence,&device); P(nvnSyncInitialize)(&logical,&device); P(nvnSyncInitialize)(&unrelated,&device);
    job blocked,other; enqueue(&blocked,&queues[0],consumer(0,1,&command_fence,1)); int br=returned(&blocked,250000000ull);
    CHECK(br,"parked consumer submission returns before unrelated work");
    P(nvnQueueFenceSync)(&queues[0],&logical,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    begin_cmd(1); uint32_t payload=after; P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&payload);
    P(nvnCommandBufferFenceSync)(&commands[1],&unrelated,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    enqueue(&other,&queues[1],P(nvnCommandBufferEndRecording)(&commands[1])); int or_=returned(&other,250000000ull);
    CHECK(or_,"unrelated queue submission returns"); int progressed=landed(&unrelated,1000000000ull);
    CHECK(progressed,"unrelated queue physically completes while consumer is parked");
    CHECK(!landed(&logical,0) && !landed(&command_fence,0),"neither consumer fence inherits an unrelated physical completion");
    CHECK(*result==sentinel,"parked consumer has not copied its payload");
    P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1);   // unconditional rescue before joining or reporting a fatal fixture failure
    require_returned(&blocked,br); require_returned(&other,or_);
    int drained=landed(&logical,5000000000ull) && landed(&command_fence,5000000000ull) && landed(&unrelated,5000000000ull);
    CHECK(drained && *result==after,"logical and physical work drain after the actual dependency is released"); if(!drained) _Exit(2);
    P(nvnSyncFinalize)(&command_fence); P(nvnSyncFinalize)(&logical); P(nvnSyncFinalize)(&unrelated);
}
static void receipt_snapshot(int command_wait) {
    printf("CASE %s wait retains its original receipt through retarget and finalize\n",command_wait?"command-buffer":"queue");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    NVNqueue completed_queue; NVNqueueBuilder qb={0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb,&device);
    CHECK(P(nvnQueueInitialize)(&completed_queue,&qb),"completed retarget queue initializes");
    NVNsync target_sync,consumed,producer_tail; P(nvnSyncInitialize)(&target_sync,&device); P(nvnSyncInitialize)(&consumed,&device); P(nvnSyncInitialize)(&producer_tail,&device);
    begin_cmd(0); P(nvnCommandBufferWaitEvent)(&commands[0],&event,NVN_EVENT_WAIT_MODE_EQUAL,1); uint32_t payload=after;
    P(nvnCommandBufferUpdateUniformBuffer)(&commands[0],source_addr,4,0,4,&payload);
    job producer,consumer_job,tail_job; enqueue(&producer,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); int pr=returned(&producer,250000000ull);
    CHECK(pr,"original producer is enqueued before its receipt is captured");
    P(nvnQueueFenceSync)(&queues[0],&target_sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    if(!command_wait) CHECK(P(nvnQueueWaitSync)(&queues[1],&target_sync),"queue wait captures the original producer receipt");
    begin_cmd(1); if(command_wait) P(nvnCommandBufferWaitSync)(&commands[1],&target_sync);
    P(nvnCommandBufferCopyBufferToBuffer)(&commands[1],source_addr,result_addr,4,0);
    P(nvnCommandBufferFenceSync)(&commands[1],&consumed,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    enqueue(&consumer_job,&queues[1],P(nvnCommandBufferEndRecording)(&commands[1])); int cr=returned(&consumer_job,250000000ull);
    CHECK(cr,"consumer recording has captured its sync dependency");
    // Replace the producer queue's tail too: after the original job retires, the queued wait must own the old receipt itself.
    begin_cmd(0); enqueue(&tail_job,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); int tr=returned(&tail_job,250000000ull);
    P(nvnQueueFenceSync)(&queues[0],&producer_tail,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    P(nvnQueueFenceSync)(&completed_queue,&target_sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    CHECK(!landed(&consumed,100000000ull) && *result==sentinel,"retargeting the source sync to a completed queue does not release the queued wait");
    P(nvnSyncFinalize)(&target_sync);
    P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1);   // release even if a stale raw pointer has already let the consumer run
    require_returned(&producer,pr); require_returned(&consumer_job,cr); require_returned(&tail_job,tr);
    int drained=landed(&consumed,5000000000ull) && landed(&producer_tail,5000000000ull);
    CHECK(drained && *result==after,"queued wait survives sync finalization and observes the original producer payload"); if(!drained) _Exit(2);
    P(nvnSyncFinalize)(&consumed); P(nvnSyncFinalize)(&producer_tail); P(nvnQueueFinalize)(&completed_queue);
}
static void receipt_wait_only(int command_wait) {
    printf("CASE %s wait-only queue inherits the producer GPU fence\n",command_wait?"command-buffer":"queue");
    *source=before;
    NVNsync produced,consumed; P(nvnSyncInitialize)(&produced,&device); P(nvnSyncInitialize)(&consumed,&device);
    id<MTLSharedEvent> gate=[g_dev newSharedEvent];
    if (!gate) abort();
    pthread_mutex_lock(&g_sub_mu); rec_drain(); [g_q4 waitForEvent:gate value:1]; pthread_mutex_unlock(&g_sub_mu);
    begin_cmd(1); uint32_t payload=after; P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&payload);
    job producer,waiter; enqueue(&producer,&queues[1],P(nvnCommandBufferEndRecording)(&commands[1])); int pr=returned(&producer,250000000ull);
    P(nvnQueueFenceSync)(&queues[1],&produced,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    int encoded=receipt_encoded(&produced,1000000000ull); CHECK(pr && encoded,"producer receipt publishes while its GPU work is held by a shared-event gate");
    int wr=1;
    if(command_wait) {
        begin_cmd(0); P(nvnCommandBufferWaitSync)(&commands[0],&produced);
        P(nvnCommandBufferFenceSync)(&commands[0],&consumed,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        enqueue(&waiter,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); wr=returned(&waiter,250000000ull);
    } else {
        CHECK(P(nvnQueueWaitSync)(&queues[0],&produced),"wait-only queue accepts the producer dependency");
        P(nvnQueueFenceSync)(&queues[0],&consumed,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    }
    int wait_encoded=receipt_encoded(&consumed,1000000000ull); CHECK(wr && wait_encoded,"wait-only logical receipt publishes without adding GPU commands");
    CHECK(!landed(&produced,0) && !landed(&consumed,0),"published wait-only receipt remains pending until the producer GPU fence completes");
    gate.signaledValue=1;   // always release the GPU before cleanup, including assertion failures
    require_returned(&producer,pr); if(command_wait) require_returned(&waiter,wr);
    int drained=landed(&produced,5000000000ull) && landed(&consumed,5000000000ull);
    CHECK(drained && *source==after,"wait-only fence completes after the actual producer GPU work"); if(!drained) _Exit(2);
    P(nvnSyncFinalize)(&produced); P(nvnSyncFinalize)(&consumed);
}
// The stale initial value must not satisfy a wait behind an earlier GPU reset.
static void event_stale_gpu_reset(void) {
    printf("CASE queued GPU reset precedes equality evaluation\n");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1);
    id<MTLSharedEvent> gate=[g_dev newSharedEvent];
    if (!gate) abort();
    pthread_mutex_lock(&g_sub_mu); pthread_mutex_lock(&g_res_mu); [g_q4 waitForEvent:gate value:1]; pthread_mutex_unlock(&g_res_mu); pthread_mutex_unlock(&g_sub_mu);
    NVNsync reset,consumed; P(nvnSyncInitialize)(&reset,&device); P(nvnSyncInitialize)(&consumed,&device);
    begin_cmd(0);
    P(nvnCommandBufferSignalEvent)(&commands[0],&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,0);
    P(nvnCommandBufferFenceSync)(&commands[0],&reset,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    P(nvnCommandBufferWaitEvent)(&commands[0],&event,NVN_EVENT_WAIT_MODE_EQUAL,1);
    P(nvnCommandBufferCopyBufferToBuffer)(&commands[0],source_addr,result_addr,4,0);
    P(nvnCommandBufferFenceSync)(&commands[0],&consumed,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    job j; enqueue(&j,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); int returned_=returned(&j,250000000ull);
    CHECK(returned_ && receipt_encoded(&reset,1000000000ull),"GPU reset prefix encodes without blocking submit");
    CHECK(!receipt_encoded(&consumed,100000000ull),"stale initial event value cannot release a wait behind an in-flight reset");
    gate.signaledValue=1;
    int reset_done=landed(&reset,5000000000ull);
    CHECK(reset_done && P(nvnEventGetValue)(&event)==0,"GPU reset reaches storage before the later CPU release");
    CHECK(!landed(&consumed,0) && *result==sentinel,"consumer remains parked after the reset");
    *source=after; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1);
    require_returned(&j,returned_); int done=landed(&consumed,5000000000ull);
    CHECK(done && *result==after,"later CPU release resumes the consumer with its new payload");
    if(!done) _Exit(2);
    P(nvnSyncFinalize)(&reset); P(nvnSyncFinalize)(&consumed);
}
// A wait whose event a still-running WRITE signal satisfies is encoded without that writer's completion (the one Metal queue runs it after the writer, nq_event_write), and its GPU work
// still sees the writer's payload; an ADD signal's result is not known before it lands, so a wait on it parks until then
static void event_queued_write(int mode) {
    printf("CASE wait on an in-flight %s signal\n",mode==NVN_EVENT_SIGNAL_MODE_WRITE?"WRITE":"ADD");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    id<MTLSharedEvent> gate=[g_dev newSharedEvent];
    if (!gate) abort();
    pthread_mutex_lock(&g_sub_mu); pthread_mutex_lock(&g_res_mu); [g_q4 waitForEvent:gate value:1]; pthread_mutex_unlock(&g_res_mu); pthread_mutex_unlock(&g_sub_mu);
    NVNsync produced,consumed; P(nvnSyncInitialize)(&produced,&device); P(nvnSyncInitialize)(&consumed,&device);
    begin_cmd(1); uint32_t payload=after;
    P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&payload);
    P(nvnCommandBufferSignalEvent)(&commands[1],&event,(NVNeventSignalMode)mode,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,1);
    P(nvnCommandBufferFenceSync)(&commands[1],&produced,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    job pj,cj; enqueue(&pj,&queues[1],P(nvnCommandBufferEndRecording)(&commands[1])); int pr=returned(&pj,250000000ull);
    CHECK(pr && receipt_encoded(&produced,1000000000ull),"producer encodes while its GPU work is held by a shared-event gate");
    enqueue(&cj,&queues[0],consumer(NVN_EVENT_WAIT_MODE_EQUAL,1,&consumed,1)); int cr=returned(&cj,250000000ull);
    int early=receipt_encoded(&consumed,mode==NVN_EVENT_SIGNAL_MODE_WRITE?1000000000ull:100000000ull);
    CHECK(early==(mode==NVN_EVENT_SIGNAL_MODE_WRITE),mode==NVN_EVENT_SIGNAL_MODE_WRITE?"the consumer is encoded behind the in-flight WRITE before it completes"
                                                                                 :"the consumer waits for an in-flight ADD to land");
    CHECK(!landed(&produced,0) && !landed(&consumed,0) && *result==sentinel,"nothing has run while the gate holds the GPU");
    gate.signaledValue=1;   // always release the GPU before cleanup, including assertion failures
    require_returned(&pj,pr); require_returned(&cj,cr);
    int done=landed(&produced,5000000000ull) && landed(&consumed,5000000000ull);
    CHECK(done && *result==after,"the consumer's copy reads the producer's payload"); if(!done) { P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1); _Exit(2); }
    P(nvnSyncFinalize)(&produced); P(nvnSyncFinalize)(&consumed);
}
// A signal does not cut its recording (issue #2112): two signals with commands between them are one Metal submission, and a wait on the first of them, which is not the
// recording's last, is still encoded behind the in-flight write
static void event_signal_no_cut(void) {
    printf("CASE signals stay in their recording's submission\n");
    *source=before; *result=sentinel; P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    NVNevent second; NVNeventBuilder eb={0}; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb,&pool,65548); CHECK(P(nvnEventInitialize)(&second,&eb),"second event initialized");
    P(nvnEventSignal)(&second,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    id<MTLSharedEvent> gate=[g_dev newSharedEvent];
    if (!gate) abort();
    pthread_mutex_lock(&g_sub_mu); pthread_mutex_lock(&g_res_mu); [g_q4 waitForEvent:gate value:1]; uint64_t first_fence=mtl_last_fence(); pthread_mutex_unlock(&g_res_mu); pthread_mutex_unlock(&g_sub_mu);
    NVNsync produced,consumed; P(nvnSyncInitialize)(&produced,&device); P(nvnSyncInitialize)(&consumed,&device);
    begin_cmd(1); uint32_t payload=after, other=before;
    P(nvnCommandBufferSignalEvent)(&commands[1],&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,1);
    P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&other);
    P(nvnCommandBufferUpdateUniformBuffer)(&commands[1],source_addr,4,0,4,&payload);
    P(nvnCommandBufferSignalEvent)(&commands[1],&second,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,7);
    P(nvnCommandBufferFenceSync)(&commands[1],&produced,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    job pj,cj; enqueue(&pj,&queues[1],P(nvnCommandBufferEndRecording)(&commands[1])); int pr=returned(&pj,250000000ull);
    CHECK(pr && receipt_encoded(&produced,1000000000ull),"producer encodes while its GPU work is held by a shared-event gate");
    pthread_mutex_lock(&g_sub_mu); uint64_t submissions=mtl_last_fence()-first_fence; pthread_mutex_unlock(&g_sub_mu);
    CHECK(submissions==1,"both signals and the commands between them are one Metal submission");
    enqueue(&cj,&queues[0],consumer(NVN_EVENT_WAIT_MODE_EQUAL,1,&consumed,1)); int cr=returned(&cj,250000000ull);
    CHECK(receipt_encoded(&consumed,1000000000ull),"a wait on the first signal is encoded behind the in-flight submission");
    CHECK(!landed(&produced,0) && !landed(&consumed,0) && *result==sentinel && P(nvnEventGetValue)(&event)==0,"nothing has run while the gate holds the GPU");
    gate.signaledValue=1;   // always release the GPU before cleanup, including assertion failures
    require_returned(&pj,pr); require_returned(&cj,cr);
    int done=landed(&produced,5000000000ull) && landed(&consumed,5000000000ull);
    CHECK(done && *result==after,"the consumer's copy reads what the producer wrote after the first signal"); if(!done) { P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,1); _Exit(2); }
    CHECK(P(nvnEventGetValue)(&event)==1 && P(nvnEventGetValue)(&second)==7,"both events hold their signalled values");
    P(nvnSyncFinalize)(&produced); P(nvnSyncFinalize)(&consumed); P(nvnEventFinalize)(&second);
}
// The submissions of one frame share a command allocator, and one that counts no samples takes no visibility buffer (PR #2090): 24 of them with the GPU held, three times
// the pool, all encode without a wait for the GPU, which would never return here
static void frame_shares_allocator(void) {
    enum { N=24 };
    printf("CASE a frame's submissions share a command allocator\n");
    *source=before; *result=sentinel;
    id<MTLSharedEvent> gate=[g_dev newSharedEvent];
    if (!gate) abort();
    pthread_mutex_lock(&g_sub_mu); pthread_mutex_lock(&g_res_mu); [g_q4 waitForEvent:gate value:1];
    uint64_t waits=g_alloc_waits, shares=g_alloc_shares; int pool=g_alloc_n, ring=g_vis_ring_n; pthread_mutex_unlock(&g_res_mu); pthread_mutex_unlock(&g_sub_mu);
    static NVNsync fences[N]; static job jobs[N]; int submitted=0, encoded=0;
    for(int i=0;i<N;i++) {
        P(nvnSyncInitialize)(&fences[i],&device); enqueue(&jobs[i],&queues[0],consumer(0,0,&fences[i],0));
        if(!returned(&jobs[i],2000000000ull) || !receipt_encoded(&fences[i],2000000000ull)) break;
        submitted++; encoded++;
    }
    CHECK(submitted==N && encoded==N,"24 submissions encode while the GPU is held");
    pthread_mutex_lock(&g_sub_mu);
    CHECK(g_alloc_waits==waits,"none waited for the GPU to free an allocator");
    CHECK(g_alloc_n<=pool+1 && g_alloc_shares-shares>=N-2,"they share the frame's allocator");
    CHECK(g_vis_ring_n==ring,"none took a visibility buffer");
    pthread_mutex_unlock(&g_sub_mu);
    CHECK(*result==sentinel,"nothing has run while the gate holds the GPU");
    gate.signaledValue=1;   // always release the GPU before cleanup, including assertion failures
    if(submitted<N) { fprintf(stderr,"a submission waited for the held GPU\n"); _Exit(2); }
    int done=1; for(int i=0;i<N;i++) done&=landed(&fences[i],5000000000ull);
    CHECK(done && *result==before,"all of them complete once the GPU runs"); if(!done) _Exit(2);
    for(int i=0;i<N;i++) P(nvnSyncFinalize)(&fences[i]);
}
// Counting submissions in flight each take a buffer of the visibility ring, which grows, instead of a one-off buffer that joins the residency set and leaves it after the
// submission: 24 of them, three times the old ring of 8, with none complete; once they are, the next one reuses a ring buffer and the ring does not grow
static void vis_ring_grows(void) {
    enum { N=24 };
    printf("CASE counting submissions in flight take buffers of the visibility ring\n");
    pthread_mutex_lock(&g_sub_mu);
    const uint64_t far=g_done+(1ull<<40); __strong id<MTLBuffer> taken[N]; int took=0;
    for(int i=0;i<N;i++) {
        g_vis_ring_i=-1; g_vis=nil; vis_take(); taken[i]=g_vis;
        if(g_vis && g_vis_ring_i>=0 && vis_in_ring(g_vis)) { took++; g_vis_ring[g_vis_ring_i].fence=far+(uint64_t)i; }   // in flight: its fence is far ahead of g_done
    }
    CHECK(took==N,"24 counting submissions in flight each take a ring buffer");
    int distinct=1; for(int i=0;i<N;i++) for(int k=0;k<i;k++) if(taken[i]==taken[k]) distinct=0;
    CHECK(distinct,"no two of them share a buffer");
    const int grown=g_vis_ring_n;
    for(int i=0;i<grown;i++) if(g_vis_ring[i].fence>=far) g_vis_ring[i].fence=0;   // their submissions complete
    g_vis_ring_i=-1; g_vis=nil; vis_take();
    CHECK(g_vis && g_vis_ring_n==grown && vis_in_ring(g_vis),"once they complete, the next one reuses a ring buffer");
    if(g_vis_ring_i>=0) g_vis_ring[g_vis_ring_i].fence=0;
    g_vis_ring_i=-1; g_vis=nil; g_vis_n=0;
    pthread_mutex_unlock(&g_sub_mu);
}
// A wait on a sync no fence has reached when it is submitted is already satisfied: the queue's own later work, its fence included, runs (issue #1808)
static void receipt_unfenced(int command_wait) {
    printf("CASE %s wait on a never-fenced sync does not park its queue\n",command_wait?"command-buffer":"queue");
    *source=before; *result=sentinel;
    NVNsync never,consumed; P(nvnSyncInitialize)(&never,&device); P(nvnSyncInitialize)(&consumed,&device);
    if(!command_wait) CHECK(P(nvnQueueWaitSync)(&queues[0],&never),"queue accepts a wait on a never-fenced sync");
    begin_cmd(0); if(command_wait) P(nvnCommandBufferWaitSync)(&commands[0],&never);
    P(nvnCommandBufferCopyBufferToBuffer)(&commands[0],source_addr,result_addr,4,0);
    P(nvnCommandBufferFenceSync)(&commands[0],&consumed,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    job j; enqueue(&j,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); int r=returned(&j,250000000ull);
    int done=landed(&consumed,5000000000ull);
    CHECK(r && done && *result==before,"the work after the wait runs and its fence lands"); if(!done) _Exit(2);
    P(nvnSyncFinalize)(&never); P(nvnSyncFinalize)(&consumed);
}
static void receipt_tests(void) {
    receipt_unrelated_completion(); receipt_snapshot(0); receipt_snapshot(1); receipt_wait_only(0); receipt_wait_only(1); receipt_unfenced(0); receipt_unfenced(1);
}
static void event_mirror_publication(void) {
    printf("CASE terminal event publishes after the render pass's store into storage\n");
    NVNtextureBuilder tb; NVNtexture texture; P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetDevice)(&tb,&device); P(nvnTextureBuilderSetWidth)(&tb,8); P(nvnTextureBuilderSetHeight)(&tb,4);
    P(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb,&pool,98304);
    NVNpackagedTextureLayout layout={{1,0,0,0,7,0,1,0}}; P(nvnTextureBuilderSetPackagedTextureLayout)(&tb,&layout);
    CHECK(P(nvnTextureInitialize)(&texture,&tb),"storage-backed render target initializes");
    memset(memory+98304,0xee,(size_t)P(nvnTextureGetStorageSize)(&texture));
    atomic_store(&event_test_pixel,0); atomic_store(&event_test_storage,(uintptr_t)(memory+98304));
    P(nvnEventSignal)(&event,NVN_EVENT_SIGNAL_MODE_WRITE,0);
    NVNsync fence; P(nvnSyncInitialize)(&fence,&device);
    begin_cmd(0); const NVNtexture *colors[]={&texture}; float rgba[]={0.2f,0.4f,0.6f,1.f};
    P(nvnCommandBufferSetRenderTargets)(&commands[0],1,colors,NULL,NULL,NULL);
    P(nvnCommandBufferClearColor)(&commands[0],0,rgba,15);
    P(nvnCommandBufferSignalEvent)(&commands[0],&event,NVN_EVENT_SIGNAL_MODE_WRITE,NVN_EVENT_SIGNAL_LOCATION_BOTTOM,0,11);
    P(nvnCommandBufferFenceSync)(&commands[0],&fence,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    job submitted; enqueue(&submitted,&queues[0],P(nvnCommandBufferEndRecording)(&commands[0])); int sr=returned(&submitted,250000000ull);
    require_returned(&submitted,sr); int done=landed(&fence,5000000000ull);
    CHECK(done,"render clear and terminal signal complete"); if(!done) _Exit(2);
    CHECK(atomic_load(&event_test_pixel)==0xff996633u,"rendered bytes are already in guest storage when the event is published");
    CHECK(P(nvnEventGetValue)(&event)==11,"the terminal event publishes once the storage is visible");
    atomic_store(&event_test_storage,0); P(nvnSyncFinalize)(&fence); P(nvnTextureFinalize)(&texture);
}
int main(void) { @autoreleasepool {
    if (setvbuf(stdout, NULL, _IONBF, 0)) abort(); setenv("DM_PSO_PREPARE","0",1);
    mtl_init();
    NVNdeviceBuilder db={0}; P(nvnDeviceBuilderSetDefaults)(&db); CHECK(P(nvnDeviceInitialize)(&device,&db),"device initialized");
    for(int i=0;i<2;i++) { NVNqueueBuilder qb={0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb,&device); CHECK(P(nvnQueueInitialize)(&queues[i],&qb),"NVN queue initialized"); }
    CHECK(!posix_memalign((void **)&memory,getpagesize(),131072),"pool storage allocated"); if(!memory) return 2; memset(memory,0,131072);
    NVNmemoryPoolBuilder pb={0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb,&device); P(nvnMemoryPoolBuilderSetStorage)(&pb,memory,131072); P(nvnMemoryPoolBuilderSetFlags)(&pb,0x24);
    CHECK(P(nvnMemoryPoolInitialize)(&pool,&pb),"memory pool initialized");
    source=(volatile uint32_t *)(memory+65536); result=(volatile uint32_t *)(memory+65540);
    NVNbufferBuilder bb={0}; NVNbuffer buffer; P(nvnBufferBuilderSetDefaults)(&bb); P(nvnBufferBuilderSetDevice)(&bb,&device); P(nvnBufferBuilderSetStorage)(&bb,&pool,65536,16); CHECK(P(nvnBufferInitialize)(&buffer,&bb),"buffer initialized");
    source_addr=P(nvnBufferGetAddress)(&buffer); result_addr=source_addr+4;
    NVNeventBuilder eb={0}; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb,&pool,65544); CHECK(P(nvnEventInitialize)(&event,&eb),"event initialized");
    pthread_t worker; if(pthread_create(&worker,NULL,submit_worker,NULL)) return 2;
    // Warm the actual copy/fence path before making bounded negative assertions.
    *source=before; *result=sentinel; NVNsync warm; P(nvnSyncInitialize)(&warm,&device); job w; enqueue(&w,&queues[0],consumer(0,0,&warm,0));
    require_returned(&w,0); int warm_ok=landed(&warm,5000000000ull); CHECK(warm_ok && *result==before,"real GPU copy and fence warm-up"); if(!warm_ok) _Exit(2); P(nvnSyncFinalize)(&warm);
    case_wait("CPU release equality",0,0,1,0);
    case_wait("equality rejects a larger value",0,2,1,0);
    case_wait("wrap accepts zero after ffffffff",1,0,0xffffffff,1);
    case_wait("wrap rejects ffffffff before zero",1,0xffffffff,0,0);
    case_wait("wrap accepts half-range minus one",1,0x7fffffff,0,1);
    case_wait("wrap rejects exactly half-range",1,0x80000000,0,0);
    case_cross_queue();
    event_stale_gpu_reset();
    event_queued_write(NVN_EVENT_SIGNAL_MODE_WRITE);
    event_queued_write(NVN_EVENT_SIGNAL_MODE_ADD);
    event_signal_no_cut();
    frame_shares_allocator();
    vis_ring_grows();
    receipt_tests();
    event_mirror_publication();
    pthread_mutex_lock(&jobs_mu); jobs_stop=1; pthread_cond_signal(&jobs_cv); pthread_mutex_unlock(&jobs_mu); pthread_join(worker,NULL);
    printf("wait-event fixture: %d checks, %d failed\n",checks,fails);
    // The process owns the fixture pool until all callbacks have retired.
    return fails?1:0;
} }
