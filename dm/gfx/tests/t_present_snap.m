// Snapshot lifecycle: real NVN acquire/present/suspend, real Metal copies, and controlled encoder/GPU delays.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static NVNdevice fixture_device;
static NVNqueue fixture_queue;
static void queue_encoded(void) {
    NVNsync sync; h_nvnSyncInitialize(&sync, &fixture_device);
    h_nvnQueueFenceSync(&fixture_queue, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNsyncWaitResult result = h_nvnSyncWait(&sync, 2000000000ull);
    CHECK("queued present reaches physical encoding", result == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || result == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    h_nvnSyncFinalize(&sync);
}
static tex_t source_n(unsigned i, int n) {   // n x n texels of 73 + i (n at most 16)
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:n height:n mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t bytes[1024]; memset(bytes, 73 + i, sizeof bytes);
    [mt replaceRegion:MTLRegionMake2D(0, 0, n, n) mipmapLevel:0 withBytes:bytes bytesPerRow:n * 4];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=90001+i};
    t.d.width = t.d.height = n; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static tex_t source(unsigned i) { return source_n(i, 8); }
static void pixels(id<MTLTexture> t, int value) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bad = 0; for (int i = 0; i < 256; i++) bad += ((uint8_t *)out.contents)[i] != value;
    CHECK("snapshot has the expected pixels", bad == 0);
}
static _Atomic uint64_t gate_note;
static void gate_encoder(void) {
    pthread_mutex_lock(&g_note_mu);
    rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&gate_note; r->a[1] = 1; rec_publish();
}
static dispatch_semaphore_t suspend_started, suspend_done;
static void *suspend_thread(void *arg) {
    (void)arg; @autoreleasepool {
        dispatch_semaphore_signal(suspend_started); dm_gfx_suspend(1); dispatch_semaphore_signal(suspend_done);
    } return NULL;
}
static void suspend_case(void) {
    tex_t t = source(0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent];
    [g_q4 waitForEvent:gpu_gate value:1];
    gate_encoder();
    // The OS has stopped the display link before notifying the app to suspend.
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    present_enqueue(&t, 1. / 60, NULL, 0, NULL); mtl_present_discard(1);
    suspend_started = dispatch_semaphore_create(0); suspend_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, suspend_thread, NULL);
    dispatch_semaphore_wait(suspend_started, DISPATCH_TIME_FOREVER);
    int returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend waits for the queued snapshot commit", !returned);
    pthread_mutex_unlock(&g_note_mu); snap_commit_wait(g_snap_v);
    if (!returned) returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend also waits for snapshot GPU completion", !returned);
    gpu_gate.signaledValue = 1;
    CHECK("snapshot completes after the GPU gate opens", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    if (!returned) CHECK("suspend finishes after the snapshot", dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 2000000000)) == 0);
    pthread_join(th, NULL); dm_gfx_suspend(0);
}
static void pacing_case(void) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    tex_t textures[4]; win_t win = {.ntex=4}; int indices[4];
    for (int i = 0; i < 4; i++) { textures[i] = source(i); win.tex[i] = (NVNtexture *)&textures[i]; }
    // Several acquires can precede their presents. A full snapshot ring must fall back to texture tickets.
    for (int i = 0; i < 4; i++) h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &indices[i]);
    mtl_present_test_stall(150000000); double longest = 0; uint64_t copies = g_snap_v;
    for (int i = 0; i < 4; i++) {
        uint64_t start = now_ns(); h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, indices[i]);
        double ms = (now_ns() - start) / 1e6; if (ms > longest) longest = ms;
        printf("present %d: %.3f ms, ticket %llu\n", i, ms, (unsigned long long)win.ticket[indices[i]]);
    }
    CHECK("present does not wait for the stalled display", longest < 50);
    // Window tickets now own logical queue receipts even for snapshots. Wait
    // until the queue has encoded them and observe actual copies, not receipt IDs.
    queue_encoded();
    CHECK("full ring copies only its two available snapshot slots", g_snap_v == copies + 2);
    uint64_t start = now_ns(); int idx; h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    double ms = (now_ns() - start) / 1e6; printf("acquire: %.3f ms\n", ms);
    CHECK("acquire waits for snapshot capacity", ms >= 50);
    copies = g_snap_v; h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, idx); queue_encoded();
    CHECK("snapshot resumes after acquire frees capacity", g_snap_v == copies + 1);
    mtl_present_flush(); mtl_present_test_stall(0);
    for (int i = 0; i < 4; i++) {
        h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
        h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, idx); queue_encoded();
        mtl_present_flush(); snap_commit_wait(g_snap_v);
        CHECK("reused snapshot completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
        int slot = (g_snap_next - 1) % g_snap_n; pixels(g_snap[slot].tex, 73 + idx);
    }
}
static void pending_copy_case(void) {
    tex_t t = source(0);
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent]; if (!gpu_gate) abort(); [g_q4 waitForEvent:gpu_gate value:1];
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    uint64_t v0 = g_snap_v;   // a snapshot keeps its present ticket (LNPANDY/NX2Apple#1117): the copy count tells a snapshot from a fallback
    CHECK("first snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0, NULL) != 0 && g_snap_v == v0 + 1);
    CHECK("second snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0, NULL) != 0 && g_snap_v == v0 + 2);
    present_drop_for_suspend();   // Tickets are free, but the GPU still owns both allocators.
    uint64_t v = g_snap_v, start = now_ns(), ticket = present_enqueue(&t, 1. / 60, NULL, 0, NULL);
    CHECK("pending copy falls back without waiting", ticket != 0 && now_ns() - start < 50000000);
    CHECK("pending copy's allocator was not reused", g_snap_v == v);
    gpu_gate.signaledValue = 1; mtl_drain();
    CHECK("discarded copies finish before drain returns", g_snap_ev.signaledValue >= v);
    pixels(g_snap[0].tex, 73); pixels(g_snap[1].tex, 73);
}
static void snap_present(const tex_t *t) { present_enqueue(t, 1. / 60, NULL, 0, NULL); mtl_present_flush(); snap_commit_wait(g_snap_v); }
// A new presented size replaces the ring slot's texture while a present that sampled the old one may still be queued on the GPU (a gate holds the queue): the old texture stays
// resident until the slot's next use, which comes only after the replacing copy, and with it everything queued before, has run
static void resize_case(void) {
    tex_t small = source_n(0, 8), big = source_n(26, 16);
    snap_present(&small); snap_present(&small);
    CHECK("both ring slots copied", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    id<MTLTexture> old0 = g_snap[0].tex, old1 = g_snap[1].tex;
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent]; if (!gpu_gate) abort(); [g_q4 waitForEvent:gpu_gate value:1];   // stands for display command buffers still queued
    snap_present(&big);
    CHECK("slot 0 has a texture of the new size", g_snap[0].tex != old0 && g_snap[0].tex.width == 16);
    CHECK("slot 0's old texture is still resident", [g_snap_res containsAllocation:old0]);
    snap_present(&big);
    CHECK("slot 1's old texture is still resident", [g_snap_res containsAllocation:old1]);
    uint64_t copies = g_snap_v; present_enqueue(&big, 1. / 60, NULL, 0, NULL); mtl_present_flush();
    CHECK("a present while slot 0's copy is queued falls back", g_snap_v == copies);
    CHECK("the old texture stays resident while the copy is queued", [g_snap_res containsAllocation:old0]);
    gpu_gate.signaledValue = 1;
    CHECK("the replacing copies complete", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    snap_present(&big);
    CHECK("slot 0's old texture leaves at the slot's next use", ![g_snap_res containsAllocation:old0] && !g_snap[0].old);
    CHECK("slot 0's new texture is resident", [g_snap_res containsAllocation:g_snap[0].tex]);
    CHECK("slot 1's old texture waits for its own slot", [g_snap_res containsAllocation:old1]);
    CHECK("the copy completes", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    pixels(g_snap[0].tex, 99);
    snap_present(&big);
    CHECK("slot 1's old texture leaves at its next use", ![g_snap_res containsAllocation:old1] && !g_snap[1].old);
}
typedef struct { win_t *window; int index; dispatch_semaphore_t started, done; } acquire_job;
static void *acquire_thread(void *arg) { @autoreleasepool {
    acquire_job *job = arg; dispatch_semaphore_signal(job->started);
    h_nvnWindowAcquireTexture((NVNwindow *)job->window, NULL, &job->index);
    dispatch_semaphore_signal(job->done);
} return NULL; }
// A snapshot present keeps its ticket (LNPANDY/NX2Apple#1117): acquiring that texture again waits until its copy, and with it the frame drawn into it, has run on the GPU,
// though not for the screen. Before, the ticket was 0 and only the next ring slot paced the acquire; with more ring slots than window textures, or a game that
// acquires its next texture before presenting the current frame, the game ran frames further ahead of the GPU and rewrote per-frame data queued draws still read
// (Xenoblade Chronicles 3: the scene one flat colour in a third to a half of the frames)
static void ticket_case(void) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    g_snap_n = 4;   // more ring slots than window textures: ring capacity alone does not hold the acquire
    tex_t textures[2]; win_t win = {.ntex=2};
    for (int i = 0; i < 2; i++) { textures[i] = source(i); win.tex[i] = (NVNtexture *)&textures[i]; }
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent]; if (!gpu_gate) abort();
    mtl_present_test_stall(400000000); uint64_t copies = g_snap_v; int idx;
    h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx); h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, idx);
    queue_encoded(); snap_commit_wait(g_snap_v);
    // nvnQueueFinish after a snapshot present waits for the copy, not for the screen: by the present's ticket it waited for the stalled display link (400 ms here),
    // which paces a game that finishes its queue every frame by the screen again
    uint64_t f0 = now_ns(); h_nvnQueueFinish(&fixture_queue); double fms = (now_ns() - f0) / 1e6; printf("finish: %.3f ms\n", fms);
    CHECK("finish waits for the snapshot's copy, not the stalled display", fms < 200 && g_snap_ev.signaledValue >= copies + 1);
    [g_q4 waitForEvent:gpu_gate value:1];   // holds the queue: the next copy stays unrun until the gate opens
    h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx); h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, idx);
    for (uint64_t t0 = now_ns(); g_snap_v < copies + 2 && now_ns() - t0 < 2000000000ull;) usleep(1000);   // the queue worker takes the second snapshot (its copy waits behind the gate)
    CHECK("both presents took snapshots", g_snap_v == copies + 2);
    // The window holds the queue's receipts; the ticket the next acquire waits for is the present ticket the receipt was published with (0: nothing to wait for)
    nq_receipt *r0 = (nq_receipt *)(uintptr_t)win.ticket[0];
    CHECK("snapshot presents keep their present tickets", r0 && r0->published && r0->present);
    uint64_t ev0 = r0 ? r0->copy : 0;
    CHECK("the first texture's receipt names its copy", ev0 == copies + 1);
    h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);   // (it also takes the stalled display link's pacing wait, as any acquire with snapshots does)
    CHECK("acquiring the first texture again waited for its copy", idx == 0 && g_snap_ev.signaledValue >= ev0);
    // the second texture's copy is still behind the gate: its acquire must hold until the gate opens
    h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, idx);
    acquire_job job = {.window = &win, .started = dispatch_semaphore_create(0), .done = dispatch_semaphore_create(0)};
    pthread_t thread; if (pthread_create(&thread, NULL, acquire_thread, &job)) { gpu_gate.signaledValue = 1; exit(2); }
    dispatch_semaphore_wait(job.started, DISPATCH_TIME_FOREVER);
    int returned = dispatch_semaphore_wait(job.done, dispatch_time(DISPATCH_TIME_NOW, 100000000ll)) == 0;
    CHECK("acquiring the second texture waits for its unrun copy", !returned);
    gpu_gate.signaledValue = 1;
    if (!returned) returned = dispatch_semaphore_wait(job.done, dispatch_time(DISPATCH_TIME_NOW, 2000000000ll)) == 0;
    CHECK("that acquire returns once the copy has run", returned && job.index == 1);
    if (!returned) exit(2); pthread_join(thread, NULL);
    h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, job.index); queue_encoded();
    mtl_present_flush(); mtl_present_test_stall(0); snap_commit_wait(g_snap_v);
    CHECK("the last snapshot completes", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
}
static void queued_case(void) {
    uint8_t *memory = NULL; if (posix_memalign((void **)&memory, getpagesize(), 65536)) exit(2); memset(memory, 0, 65536);
    NVNmemoryPool pool; NVNmemoryPoolBuilder pb = {0}; h_nvnMemoryPoolBuilderSetDefaults(&pb); h_nvnMemoryPoolBuilderSetDevice(&pb, &fixture_device);
    h_nvnMemoryPoolBuilderSetStorage(&pb, memory, 65536); h_nvnMemoryPoolBuilderSetFlags(&pb, 0x24);
    CHECK("event pool initialized", h_nvnMemoryPoolInitialize(&pool, &pb));
    NVNevent event; NVNeventBuilder eb = {0}; h_nvnEventBuilderSetDefaults(&eb); h_nvnEventBuilderSetStorage(&eb, &pool, 32768);
    CHECK("event initialized", h_nvnEventInitialize(&event, &eb));
    NVNcommandBuffer command; h_nvnCommandBufferInitialize(&command, &fixture_device); h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 32768);
    h_nvnCommandBufferBeginRecording(&command); h_nvnCommandBufferWaitEvent(&command, &event, NVN_EVENT_WAIT_MODE_EQUAL, 1);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&command);
    tex_t texture = source(0); id<MTLTexture> original = serial_get(texture.serial);   // retain the original GPU backing throughout descriptor reuse
    win_t win = {.ntex = 1}; win.tex[0] = (NVNtexture *)&texture;
    int index; h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &index);
    dispatch_semaphore_t cancel = dispatch_semaphore_create(0), rescued = dispatch_semaphore_create(0);
    NVNevent *event_ptr = &event;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (dispatch_semaphore_wait(cancel, dispatch_time(DISPATCH_TIME_NOW, 5000000000ll))) h_nvnEventSignal(event_ptr, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
        dispatch_semaphore_signal(rescued);
    });
    h_nvnQueueSubmitCommands(&fixture_queue, 1, &handle);
    uint64_t copies = g_snap_v, start = now_ns(); h_nvnQueuePresentTexture(&fixture_queue, (NVNwindow *)&win, index);
    CHECK("present returns while its queue waits for an event", now_ns() - start < 50000000);
    acquire_job job = {.window = &win, .started = dispatch_semaphore_create(0), .done = dispatch_semaphore_create(0)};
    pthread_t thread; if (pthread_create(&thread, NULL, acquire_thread, &job)) { h_nvnEventSignal(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1); exit(2); }
    dispatch_semaphore_wait(job.started, DISPATCH_TIME_FOREVER);
    int returned = dispatch_semaphore_wait(job.done, dispatch_time(DISPATCH_TIME_NOW, 100000000ll)) == 0;
    CHECK("acquire waits for the blocked queued present", !returned);
    CHECK("no snapshot copy crosses the unresolved event", g_snap_v == copies);
    h_nvnTextureFinalize((NVNtexture *)&texture); texture = source(100);
    // source() installs no pool alias or retired-serial cache entry. Finalizing
    // it removes that lookup; the queued present must retain its own source.
    h_nvnEventSignal(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1); dispatch_semaphore_signal(cancel);
    CHECK("event rescue worker retired", dispatch_semaphore_wait(rescued, dispatch_time(DISPATCH_TIME_NOW, 2000000000ll)) == 0);
    if (!returned) returned = dispatch_semaphore_wait(job.done, dispatch_time(DISPATCH_TIME_NOW, 2000000000ll)) == 0;
    CHECK("acquire finishes after event release", returned);
    if (!returned) exit(2); pthread_join(thread, NULL);
    queue_encoded(); mtl_present_flush(); snap_commit_wait(g_snap_v);
    CHECK("queued source snapshot completes", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    CHECK("queued present produced exactly one snapshot", g_snap_v == copies + 1);
    pixels(g_snap[(g_snap_next - 1) % g_snap_n].tex, 73); pixels(original, 73);
    h_nvnWindowFinalize((NVNwindow *)&win); h_nvnQueueFinish(&fixture_queue);
    h_nvnCommandBufferFinalize(&command); h_nvnEventFinalize(&event); h_nvnMemoryPoolFinalize(&pool); free(memory);
}
int main(int argc, char **argv) { @autoreleasepool {
    alarm(15); setbuf(stdout, NULL);
    if (argc != 2) return 2;
    setenv("DM_PRESENT_SNAP", "1", 1); setenv("DM_PRESENT_HZ", "0", 1);
    mtl_init();
    NVNdeviceBuilder db = {0}; h_nvnDeviceBuilderSetDefaults(&db); CHECK("fixture device initialized", h_nvnDeviceInitialize(&fixture_device, &db));
    NVNqueueBuilder qb = {0}; h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &fixture_device);
    CHECK("fixture queue initialized", h_nvnQueueInitialize(&fixture_queue, &qb));
    if (!strcmp(argv[1], "suspend")) suspend_case();
    else if (!strcmp(argv[1], "pacing")) pacing_case();
    else if (!strcmp(argv[1], "copy")) pending_copy_case();
    else if (!strcmp(argv[1], "resize")) resize_case();
    else if (!strcmp(argv[1], "queued")) queued_case();
    else if (!strcmp(argv[1], "ticket")) ticket_case();
    else return 2;
    printf("present snapshots: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
