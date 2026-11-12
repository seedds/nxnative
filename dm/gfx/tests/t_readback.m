// NVN readback contract: a pending clear must reach both GPU buffer copies and CPU reads.
// Expected 8x4 RGBA values are literal bytes, independent of the Metal replay's state.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int cases, failed;
#define EXPECT(n,w,v) do { int a=(w), b=(v); cases++; printf("%s expected %d -> got %d %s\n",n,a,b,a==b?"PASS":"FAIL"); failed+=a!=b; } while (0)

// Observe the real readback buffer without retaining it. The caller's outer pool stays open,
// as on a long-lived guest thread; ReadTexels must drain its own temporary Metal objects.
@interface ReadbackDevice : NSObject
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, weak) id<MTLBuffer> lastBuffer;
@property(nonatomic) NSUInteger allocations;
@property(nonatomic) NSUInteger observedLength;   // 0 observes every buffer; otherwise only the readback's exact size.
@end
@implementation ReadbackDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    id<MTLBuffer> buffer = [_device newBufferWithLength:length options:options];
    if (!_observedLength || length == _observedLength) { _lastBuffer = buffer; _allocations++; }
    return buffer;
}
- (id)forwardingTargetForSelector:(SEL)selector { (void)selector; return _device; }
@end

@interface ReadbackCompletion : NSObject {
@public
    const NVNsync *sync;
    const NVNtexture *texture;
    const NVNevent *event;
    NVNcopyRegion region;
    dispatch_semaphore_t feedbackEntered, feedbackRelease, waiterStarted, waiterDone, poolRelease, waiterExited;
    _Atomic int gateTimedOut;
    NVNsyncWaitResult waitResult;
    int badPixels;
    uint32_t eventValue;
}
@end
@implementation ReadbackCompletion @end

static void readback_wait_for(const char *name, dispatch_semaphore_t semaphore) {
    int ok = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5000000000ll)) == 0;
    EXPECT(name, 1, ok);
    if (!ok) exit(1);   // Never join a stuck waiter or leave a failed gate hanging the test process.
}
static void readback_feedback_idle(void) {
    dispatch_semaphore_t idle = dispatch_semaphore_create(0);
    dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(idle); });
    readback_wait_for("readback feedback queue reached its bounded checkpoint", idle);
}
static void *readback_guest_waiter(void *arg) {
    ReadbackCompletion *c = (__bridge ReadbackCompletion *)arg;
    @autoreleasepool {
        dispatch_semaphore_signal(c->waiterStarted);
        c->waitResult = h_nvnSyncWait(c->sync, 5000000000ull);
        c->badPixels = 32; c->eventValue = 0;
        if (c->waitResult == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || c->waitResult == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED) {
            static const uint8_t expected[] = {51, 102, 153, 255};
            uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu);
            h_nvnTextureReadTexels(c->texture, NULL, &c->region, cpu);
            c->badPixels = 0;
            for (int i = 0; i < 32; i++) c->badPixels += memcmp(cpu + i * 4, expected, 4) != 0;
            c->eventValue = h_nvnEventGetValue(c->event);
        }
        dispatch_semaphore_signal(c->waiterDone);
        // Keep this same guest pool alive through callback delivery, deferred retirement and the weak observation.
        if (dispatch_semaphore_wait(c->poolRelease, dispatch_time(DISPATCH_TIME_NOW, 20000000000ll))) {
            fprintf(stderr, "readback guest pool was not released within 20 seconds\n"); _Exit(1);
        }
    }
    dispatch_semaphore_signal(c->waiterExited);
    return NULL;
}

// A texture in a sparse virtual pool takes its content from what the pool's page table maps (nvn_sparse_copy), unmapped pages as 0. ReadTexels reads it there on whatever thread asks:
// Tears of the Kingdom reads such textures on its own threads (bake::ModuleMgr), and resolving the host texture there uploaded it outside the submit thread (up_owner_check aborted, #471)
typedef struct { const NVNtexture *t; const NVNcopyRegion *r; uint8_t *dst; } read_job;
static void *read_on_thread(void *p) { read_job *j = p; h_nvnTextureReadTexels(j->t, NULL, j->r, j->dst); return NULL; }
static void sparse_reads(const NVNcopyRegion *region, const uint8_t expected[4]) {
    size_t len = 4 * (size_t)getpagesize();
    uint8_t *phys = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    pool_t physical, sparse; pool_builder_t pb = {.mem=phys, .size=len, .flags=0x24};
    EXPECT("physical pool init", 1, h_nvnMemoryPoolInitialize((NVNmemoryPool *)&physical, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size=DM_GAME_APP_POOL + 4 * len, .flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL};   // larger than the application pool: sparse
    EXPECT("sparse pool init", 1, h_nvnMemoryPoolInitialize((NVNmemoryPool *)&sparse, (const NVNmemoryPoolBuilder *)&pb));
    map_req map = {.phys=(NVNmemoryPool *)&physical, .size=len};   // [0, len) mapped, the rest not
    EXPECT("sparse mapping", 1, h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sparse, 1, (const NVNmappingRequest *)&map));
    EXPECT("pool reads through its page table", 1, nvn_sparse_pool(&sparse));
    for (size_t i = 0; i < len / 4; i++) memcpy(phys + i * 4, expected, 4);   // every texel the same, whatever the block-linear order
    NVNtextureBuilder tb; NVNtexture mapped, unmapped; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&sparse, 0); EXPECT("mapped sparse texture init", 1, h_nvnTextureInitialize(&mapped, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&sparse, 2 * len); EXPECT("unmapped sparse texture init", 1, h_nvnTextureInitialize(&unmapped, &tb));
    static const uint8_t zero[4];
    for (int other = 1; other >= 0; other--) for (int m = 0; m < 2; m++) {   // a game thread first (the texture not resolved yet), then the submit thread (this one: mtl_sub_begin ran here)
        uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu); read_job j = {m ? &unmapped : &mapped, region, cpu};
        if (other) { pthread_t th; pthread_create(&th, NULL, read_on_thread, &j); pthread_join(th, NULL); } else read_on_thread(&j);
        int bad = 0; for (int i = 0; i < 32; i++) bad += memcmp(cpu + i * 4, m ? zero : expected, 4) != 0;
        char name[128]; snprintf(name, sizeof name, "sparse texture, %s pages, ReadTexels on %s thread, 32 literal pixels", m ? "unmapped" : "mapped", other ? "a game" : "the submit");
        EXPECT(name, 0, bad);
    }
    h_nvnTextureFinalize(&unmapped); h_nvnTextureFinalize(&mapped);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&sparse); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&physical); munmap(phys, len);
}

// The RGB565 family in a plain texture (not packaged, sampled with no red / blue swap: tex_by_id) is in Metal's order, red at bit 11 (bit 10 for RGB5A1), the guest's order has red at bit 0.
// Copies between such a texture and guest memory convert: a buffer → texture copy samples as the guest wrote it, a texture → buffer copy and ReadTexels give back the guest's bits, also of
// what Metal itself cleared into it. A packaged texture holds Metal's order too: uploads convert, so what Metal clears or copies into one samples right (GitHub issue #183)
static void sample2(int tid, float out[8]) {
    static id<MTLComputePipelineState> p;
    if (!p) { NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = t.read(uint2(i, 0)); }\n" options:nil error:&e];
        p = [g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; }
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = q_cb(); id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
    [e setComputePipelineState:p]; [e setTexture:v atIndex:0]; [e setBuffer:b offset:0 atIndex:0]; [e dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [e endEncoding]; [c commit]; [c waitUntilCompleted]; memcpy(out, b.contents, 32);
}
static void packed_copies(int format, int tid) {
    int b565 = format == NVN_FORMAT_RGB565, a1 = format == NVN_FORMAT_RGB5A1, rgba4 = format == NVN_FORMAT_RGBA4;
    uint16_t red = rgba4 ? 0x800f : a1 ? 0x801f : 0x001f, blue = rgba4 ? 0x0f00 : b565 ? 0xf800 : 0x7c00, guest[2] = {red, blue};   // RGBA4: red with alpha 8/15, so that no word reads the same reversed
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144}; mtl_pool_add(mem, 262144); nvn_dirty_add_range(mem, 262144);
    NVNtextureBuilder tb; NVNtexture copied, cleared; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 2);
    h_nvnTextureBuilderSetHeight(&tb, 1); h_nvnTextureBuilderSetFormat(&tb, format);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("packed texture init", 1, h_nvnTextureInitialize(&copied, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 98304); EXPECT("packed texture 2 init", 1, h_nvnTextureInitialize(&cleared, &tb));
    // Packaged textures (uploaded from their storage, NvPkg): one only sampled, one with its blue texel cleared to red, one a copy of the cleared plain texture (stored into its storage on the GPU)
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtexture up, upclr, upcopy; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    memcpy((uint8_t *)mem + 163840, guest, 4); memcpy((uint8_t *)mem + 172032, guest, 4);   // two texels of a GOB's first row: linear
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&up, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 172032); EXPECT("packaged texture 2 init", 1, h_nvnTextureInitialize(&upclr, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 180224); EXPECT("packaged texture 3 init", 1, h_nvnTextureInitialize(&upcopy, &tb));
    uint8_t *src = (uint8_t *)mem + 131072, *out = src + 4096, *out2 = src + 8192; memcpy(src, guest, 4); memset(out, 0xEE, 4); memset(out2, 0xEE, 4);
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion region = {0, 0, 0, 2, 1, 1};
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src, &copied, NULL, &region, 0);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &copied, NULL, &region, (NVNbufferAddress)out, 0);
    float rgba[] = {1, 0, 0, rgba4 ? 8/15.f : 1}; h_nvnCommandBufferClearTexture(&cmd, &cleared, NULL, &region, rgba, 15);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &cleared, NULL, &region, (NVNbufferAddress)out2, 0);
    NVNcopyRegion second = {1, 0, 0, 1, 1, 1}; h_nvnCommandBufferClearTexture(&cmd, &upclr, NULL, &second, rgba, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &cleared, NULL, &region, &upcopy, NULL, &region, 0);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("packed copies fence wait", 1, wait == 0 || wait == 1);
    char n[160]; const char *f = rgba4 ? "RGBA4" : b565 ? "RGB565" : a1 ? "RGB5A1" : "RGB5";
    mtl_reg_tex(tid, TX(&copied), NULL); float s[8]; sample2(tid, s);
    snprintf(n, sizeof n, "%s buffer -> plain texture, sampled red then blue", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] < .01f && s[6] > .99f);
    uint16_t got[2]; memcpy(got, out, 4); snprintf(n, sizeof n, "%s plain texture -> buffer, the guest's bits", f); EXPECT(n, 1, got[0] == red && got[1] == blue);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&copied, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s plain texture ReadTexels, the guest's bits", f); EXPECT(n, 1, got[0] == red && got[1] == blue);
    uint16_t used = b565 || a1 || rgba4 ? 0xffff : 0x7fff;   // RGB5's top bit is unused (Metal's BGR5A1 stores alpha 1 there)
    memcpy(got, out2, 4); snprintf(n, sizeof n, "%s cleared to red -> buffer, guest red", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&cleared, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s cleared to red, ReadTexels guest red", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    mtl_reg_tex(tid + 10, TX(&up), NULL); sample2(tid + 10, s);
    snprintf(n, sizeof n, "%s packaged texture, sampled red then blue", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] < .01f && s[6] > .99f);
    mtl_reg_tex(tid + 20, TX(&upclr), NULL); sample2(tid + 20, s);
    snprintf(n, sizeof n, "%s packaged texture, its blue texel cleared to red: red twice", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] > .99f && s[6] < .01f);
    mtl_reg_tex(tid + 30, TX(&upcopy), NULL); sample2(tid + 30, s);
    snprintf(n, sizeof n, "%s cleared plain texture -> packaged texture, sampled red", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] > .99f && s[6] < .01f);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&upcopy, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s copy into a packaged texture, guest red in its storage", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    h_nvnTextureFinalize(&upcopy); h_nvnTextureFinalize(&upclr); h_nvnTextureFinalize(&up);
    h_nvnTextureFinalize(&cleared); h_nvnTextureFinalize(&copied); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
}

// A real GPU copy into packaged storage, stored there on the GPU (gsb_store), with completion on whichever owner wins. Gate the submission's feedback, not its GPU event:
// FAST_WAKE=1 must do the CPU effects on the guest waiter before feedback can run; FAST_WAKE=0 exercises the real callback owner instead. Either way the copied bytes are in the
// storage when a successful wait returns, the event effect is visible then, and late feedback applies nothing twice; the guest's outer pool stays open throughout
static void store_completion(void) {
    int early = fast_wake();
    printf("store completion: %s wins\n", early ? "guest waiter" : "GPU feedback");
    readback_feedback_idle();
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144}; mtl_pool_add(mem, 262144); nvn_dirty_add_range(mem, 262144);
    NVNtextureBuilder tb; NVNtexture source, packaged; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 8); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 131072);
    EXPECT("completion source texture init", 1, h_nvnTextureInitialize(&source, &tb));
    NVNpackagedTextureLayout layout = {{1, 0, 0, 0, 7, 0, 1, 0}};
    h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &layout);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840);
    EXPECT("completion packaged texture init", 1, h_nvnTextureInitialize(&packaged, &tb));
    memset((uint8_t *)mem + 163840, 0xEE, h_nvnTextureGetStorageSize(&packaged));
    NVNeventBuilder eb; NVNevent event; h_nvnEventBuilderSetDefaults(&eb);
    h_nvnEventBuilderSetStorage(&eb, (const NVNmemoryPool *)&pool, 196608);
    EXPECT("completion event init", 1, h_nvnEventInitialize(&event, &eb));
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    NVNcommandBuffer cmd; EXPECT("completion command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; float color[] = {0.2f, 0.4f, 0.6f, 1.f};
    h_nvnCommandBufferClearTexture(&cmd, &source, NULL, &region, color, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &source, NULL, &region, &packaged, NULL, &region, 0);
    h_nvnCommandBufferSignalEvent(&cmd, &event, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 47);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    ReadbackCompletion *c = [ReadbackCompletion new]; c->sync = &sync; c->texture = &packaged; c->event = &event; c->region = region;
    c->feedbackEntered = dispatch_semaphore_create(0); c->feedbackRelease = dispatch_semaphore_create(0);
    c->waiterStarted = dispatch_semaphore_create(0); c->waiterDone = dispatch_semaphore_create(0);
    c->poolRelease = dispatch_semaphore_create(0); c->waiterExited = dispatch_semaphore_create(0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    uint64_t stores = g_gsb_stores;
    mtl_exec(nvn_rec_linear(handle, NULL));
    EXPECT("the clear of the source (a GPU texture) and the copy into the packaged texture are each stored on the GPU", 2, (int)(g_gsb_stores - stores));
    [g_commit_blocks addObject:^{
        dispatch_semaphore_signal(c->feedbackEntered);
        if (dispatch_semaphore_wait(c->feedbackRelease, dispatch_time(DISPATCH_TIME_NOW, 10000000000ll))) atomic_store(&c->gateTimedOut, 1);
    }];
    mtl_sub_commit();
    readback_wait_for("real store feedback reached its gate", c->feedbackEntered);
    EXPECT("GPU completion alone does not publish the pending fence", NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, h_nvnSyncWait(&sync, 0));
    EXPECT("pending fence has not applied its event effect", 0, h_nvnEventGetValue(&event));
    pthread_t thread; int created = pthread_create(&thread, NULL, readback_guest_waiter, (__bridge void *)c) == 0;
    EXPECT("long-lived guest waiter started", 1, created);
    if (!created) { dispatch_semaphore_signal(c->feedbackRelease); exit(1); }
    readback_wait_for("guest waiter entered SyncWait", c->waiterStarted);
    int completed = 0;
    if (!early) {
        completed = dispatch_semaphore_wait(c->waiterDone, dispatch_time(DISPATCH_TIME_NOW, 50000000ll)) == 0;
        EXPECT("callback-owned waiter cannot return while feedback is gated", 0, completed);
        dispatch_semaphore_signal(c->feedbackRelease);
    }
    if (!completed) readback_wait_for(early ? "fast-wake guest completed before feedback was released" : "callback-owned guest completed after feedback was released", c->waiterDone);
    EXPECT("store completion guest fence wait succeeded", 1, c->waitResult == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || c->waitResult == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    EXPECT("copied bytes are in guest storage at successful wait return, 32 literal pixels", 0, c->badPixels);
    EXPECT("event effect is visible at successful guest wait return", 47, c->eventValue);
    if (early) dispatch_semaphore_signal(c->feedbackRelease);
    readback_feedback_idle();
    EXPECT("store feedback gate did not time out", 0, atomic_load(&c->gateTimedOut));
    EXPECT("late feedback does not apply the event effect twice", 47, h_nvnEventGetValue(&event));
    mtl_sub_begin(); uint64_t retirement = mtl_sub_commit();
    EXPECT("retirement submission completed", 1, mtl_wait_at(retirement, 5000000000ull, WS_FINISH));
    readback_feedback_idle();
    EXPECT("retirement GPU success", 0, mtl_last_commit_error());
    dispatch_semaphore_signal(c->poolRelease);
    readback_wait_for("guest waiter drained its outer pool and exited", c->waiterExited); pthread_join(thread, NULL);
    h_nvnCommandBufferFinalize(&cmd); h_nvnSyncFinalize(&sync); h_nvnEventFinalize(&event);
    h_nvnTextureFinalize(&packaged); h_nvnTextureFinalize(&source);
    nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
}

// A texture ID outlives the game's memory pool object: the ID stays registered, the object is freed and its memory reused. What was there reads as anything, as x8 = pool->vp =
// 0x2c33398156a0e722 did in nvn_sparse_protect after 22 minutes of FINAL FANTASY RESONANCE (GitHub issue #934). Nothing may be made or refreshed from such a texture's pool or storage.
static void pool_object_reused(pool_t *o) { memset(o, 0x22, sizeof(NVNmemoryPool)); o->vp = (struct vpool *)0x2c33398156a0e722ull; }
static id<MTLTexture> by_id(int tid) { os_unfair_lock_lock(&g_rmu); id<MTLTexture> t = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu); return t; }
static void stale_pools(void) {
    size_t len = 4 * (size_t)getpagesize();
    uint8_t *phys = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    NVNmemoryPool *mp = calloc(1, sizeof *mp), *sp = calloc(1, sizeof *sp), *mp2 = calloc(1, sizeof *mp2);   // the game's objects, on its heap
    pool_builder_t pb = {.mem=phys, .size=len, .flags=0x24}, vb = {.size=DM_GAME_APP_POOL + 4 * len, .flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    EXPECT("stale: physical pool init", 1, h_nvnMemoryPoolInitialize(mp, (const NVNmemoryPoolBuilder *)&pb));
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, mp, 0);

    // The texture finalized and its ID left registered, its pool then finalized and the object reused; a late read has no host texture to fall back on, so it would upload (the crash)
    NVNtexture late, live, unfinalized; EXPECT("stale: late texture init", 1, h_nvnTextureInitialize(&late, &tb));
    EXPECT("stale: live texture init", 1, h_nvnTextureInitialize(&live, &tb));
    EXPECT("stale: unfinalized texture init", 1, h_nvnTextureInitialize(&unfinalized, &tb));
    mtl_reg_tex(601, (const tex_t *)&late, NULL); mtl_reg_tex(602, (const tex_t *)&live, NULL);
    id<MTLTexture> made = by_id(602); EXPECT("stale: a texture of a live pool resolves by ID", 1, made != nil);
    h_nvnTextureFinalize(&late);
    h_nvnMemoryPoolFinalize(mp); pool_object_reused((pool_t *)mp);
    EXPECT("stale: finalized texture and pool, object reused, read by ID -> empty slot", 1, by_id(601) == nil);
    // A live texture the host already made stays what it was, with no refresh from the dead pool; one it has not made stays unmade, also through its object rather than an ID
    uint64_t uploads = g_pkg_uploads, reuploads = g_pkg_reuploads;
    EXPECT("stale: live texture of a finalized pool keeps its host texture", 1, by_id(602) == made);
    EXPECT("stale: live texture of a finalized pool, through its object", 1, tex_mtl((const tex_t *)&live) == made);
    EXPECT("stale: a texture never made is not made", 1, tex_mtl((const tex_t *)&unfinalized) == nil);
    EXPECT("stale: nothing was uploaded", 1, g_pkg_uploads == uploads && g_pkg_reuploads == reuploads);

    // A new pool at the same address (a heap reuses a freed block of this size) does not bring the old pool's IDs back: their storage is still gone, until the game registers them again
    EXPECT("stale: new pool at the old object's address", 1, h_nvnMemoryPoolInitialize(mp, (const NVNmemoryPoolBuilder *)&pb));
    EXPECT("stale: the old ID stays empty over the new pool", 1, by_id(601) == nil);
    h_nvnTextureBuilderSetStorage(&tb, mp, 0); NVNtexture again; EXPECT("stale: new texture init", 1, h_nvnTextureInitialize(&again, &tb));
    mtl_reg_tex(601, (const tex_t *)&again, NULL); EXPECT("stale: registered again -> resolves", 1, by_id(601) != nil);
    h_nvnTextureFinalize(&again); h_nvnMemoryPoolFinalize(mp); memset(mp, 0, sizeof *mp);

    // The object overwritten with no Finalize at all: no gone mark can know, and its magic is what is left to tell
    EXPECT("stale: physical pool init 2", 1, h_nvnMemoryPoolInitialize(mp2, (const NVNmemoryPoolBuilder *)&pb));
    h_nvnTextureBuilderSetStorage(&tb, mp2, 0); NVNtexture unsaid; EXPECT("stale: texture init 2", 1, h_nvnTextureInitialize(&unsaid, &tb));
    mtl_reg_tex(603, (const tex_t *)&unsaid, NULL);
    pool_t saved = *(pool_t *)mp2; pool_object_reused((pool_t *)mp2);
    EXPECT("stale: pool object overwritten with no Finalize, read by ID -> empty slot", 1, by_id(603) == nil);
    *(pool_t *)mp2 = saved; h_nvnTextureFinalize(&unsaid); h_nvnMemoryPoolFinalize(mp2);

    // A sparse texture the host uploaded: its next look would go through the dead pool's page table (nvn_sparse_since), not only a first upload's protection
    EXPECT("stale: physical pool init 3", 1, h_nvnMemoryPoolInitialize(mp2, (const NVNmemoryPoolBuilder *)&pb));
    EXPECT("stale: sparse pool init", 1, h_nvnMemoryPoolInitialize(sp, (const NVNmemoryPoolBuilder *)&vb));
    map_req map = {.phys=mp2, .size=len}; EXPECT("stale: sparse mapping", 1, h_nvnMemoryPoolMapVirtual(sp, 1, (const NVNmappingRequest *)&map));
    NVNtextureBuilder sb; NVNtexture st; h_nvnTextureBuilderSetDefaults(&sb); h_nvnTextureBuilderSetWidth(&sb, 8); h_nvnTextureBuilderSetHeight(&sb, 4);
    h_nvnTextureBuilderSetFormat(&sb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&sb, sp, 0); EXPECT("stale: sparse texture init", 1, h_nvnTextureInitialize(&st, &sb));
    mtl_reg_tex(604, (const tex_t *)&st, NULL);
    id<MTLTexture> smade = by_id(604); EXPECT("stale: sparse texture resolves", 1, smade != nil);
    h_nvnMemoryPoolFinalize(sp); pool_object_reused((pool_t *)sp);
    EXPECT("stale: sparse texture of a finalized pool keeps its host texture", 1, by_id(604) == smade);
    EXPECT("stale: and through its object", 1, tex_mtl((const tex_t *)&st) == smade);
    mtl_reg_tex(601, NULL, NULL); mtl_reg_tex(602, NULL, NULL); mtl_reg_tex(603, NULL, NULL); mtl_reg_tex(604, NULL, NULL);
    h_nvnMemoryPoolFinalize(mp2); munmap(phys, len); free(mp); free(mp2); free(sp);
}

// A texture the host uploads from its storage, filled by GPU copies a level of a layer at a time, as Star Fox streams its textures into BC7 arrays: each copy writes the host texture and,
// stored on the GPU, the storage (gsb_store_buf), which page tracking does not see, so none of them uploads the texture again; a CPU write into it still does. While the command buffer
// being encoded holds a store into it, the texture keeps its host texture (gsb_held: an upload batch runs before that command buffer), and a CPU write is uploaded at its next use after
// the commit. Every level of every layer holds one byte value, so its layout does not matter
void crash_install(void);
static void copy_stream(void) { @autoreleasepool {
    crash_install();   // the game's writes into protected storage go through the page-fault hook, as in production
    enum { W = 64, LAYERS = 4, LEVELS = 3, POOL = 1 << 20, OFF = 0x11000, SRC = 0x80000, OUT = 0xC0000 };   // OFF: 4 KB into a 16 KB host page, whose first 4 KB is other data
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), POOL)) abort(); memset(mem, 0, POOL);
    uint8_t *m = mem;
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=POOL}; mtl_pool_add(mem, POOL); nvn_dirty_add_range(mem, POOL);
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D_ARRAY);
    h_nvnTextureBuilderSetWidth(&tb, W); h_nvnTextureBuilderSetHeight(&tb, W); h_nvnTextureBuilderSetDepth(&tb, LAYERS); h_nvnTextureBuilderSetLevels(&tb, LEVELS);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, OFF);
    NVNtexture arr; EXPECT("streamed array init", 1, h_nvnTextureInitialize(&arr, &tb));
    const tex_t *t = (const tex_t *)&arr;
    EXPECT("streamed array fits before the copy source", 1, OFF + t->L.size <= SRC);
    memset(m + OFF, 0x11, t->L.size);
    uint64_t lo[LAYERS][LEVELS], hi[LAYERS][LEVELS];
    for (int z = 0; z < LAYERS; z++) for (int l = 0; l < LEVELS; l++) nvn_tex_level_range(t, (unsigned)l, (unsigned)z, &lo[z][l], &hi[z][l]);
    tex_mtl(t);
    uint64_t r0 = g_pkg_reuploads, s0 = g_gsb_stores;
    memset(m + OFF + lo[3][0], 0x77, hi[3][0] - lo[3][0]);   // the game writes layer 3's level 0 before the copies: uploaded once, at the first copy's lookup
    NVNcommandBuffer cmd; EXPECT("stream command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 0); h_nvnCommandBufferSetCopyImageStride(&cmd, 0);
    size_t so = SRC, oo = OUT, out_at[LAYERS][LEVELS];
    for (int z = 0; z < 3; z++) for (int l = 0; l < LEVELS; l++) {   // layers 0-2, every level, one copy each
        int w = W >> l; NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, l, 1);
        memset(m + so, 0x40 + z * 8 + l, (size_t)w * w * 4);
        NVNcopyRegion r = {0, 0, z, w, w, 1}; h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + so), &arr, &v, &r, 0);
        so += (size_t)w * w * 4;
    }
    for (int z = 0; z < LAYERS; z++) for (int l = 0; l < LEVELS; l++) {   // the host texture's content, read on the GPU
        int w = W >> l; NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, l, 1);
        NVNcopyRegion r = {0, 0, z, w, w, 1}; out_at[z][l] = oo; memset(m + oo, 0xEE, (size_t)w * w * 4);
        h_nvnCommandBufferCopyTextureToBuffer(&cmd, &arr, &v, &r, (NVNbufferAddress)(m + oo), 0);
        oo += (size_t)w * w * 4;
    }
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("stream fence wait", 1, wait == 0 || wait == 1);
    EXPECT("stream GPU success", 0, mtl_last_commit_error());
    EXPECT("9 copies into a texture uploaded from its storage: one re-upload (the CPU write before them)", 1, (int)(g_pkg_reuploads - r0));
    EXPECT("... and the 9 copies stored into the storage on the GPU", 9, (int)(g_gsb_stores - s0));
    int bad = 0;
    for (int z = 0; z < LAYERS; z++) for (int l = 0; l < LEVELS; l++) {
        int w = W >> l; uint8_t want = z < 3 ? (uint8_t)(0x40 + z * 8 + l) : l ? 0x11 : 0x77;
        for (size_t i = 0; i < (size_t)w * w * 4; i++) bad += m[out_at[z][l] + i] != want;
        for (uint64_t i = lo[z][l]; i < hi[z][l]; i++) bad += m[OFF + i] != want;   // the storage too (each level holds one value: layout padding as well)
    }
    EXPECT("host texture and storage hold every copy and the CPU write, every byte", 0, bad);
    uint64_t r1 = g_pkg_reuploads; m[OFF + lo[0][2] + 5] = 0x99; tex_mtl(t);
    EXPECT("a CPU write after the copies: uploaded again", 1, (int)(g_pkg_reuploads - r1));
    // A store into level 1 of layer 0 in the command buffer being encoded, then a CPU write into layer 2 (another thread, during the replay): the texture keeps its host texture until the
    // command buffer is committed, then the write is uploaded, and both the storage and the new host texture hold the store and the write
    NvPkg *pk = tex_get(t->serial); id<MTLTexture> host = tex_mtl(t); EXPECT("streamed array is a packaged record", 1, [pk isKindOfClass:[NvPkg class]] && host == pk->tex);
    memset(m + SRC, 0x55, (size_t)(W / 2) * (W / 2) * 4);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    void *sb; uint64_t sbo, sgpu; EXPECT("copy source in a pool buffer", 1, mtl_gpu_addr((uint64_t)(uintptr_t)(m + SRC), (W / 2) * (W / 2) * 4, &sb, &sbo, &sgpu));
    NVNcopyRegion r1v = {0, 0, 0, W / 2, W / 2, 1};
    enc_b_copy_buffer_texture(blit(), (__bridge id<MTLBuffer>)sb, sbo, (W / 2) * 4, (W / 2) * (W / 2) * 4, MTLSizeMake(W / 2, W / 2, 1), host, 0, 1, MTLOriginMake(0, 0, 0));
    gsb_store_buf(t, 1, 0, 1, &r1v, sgpu, (W / 2) * 4, (W / 2) * (W / 2) * 4);
    EXPECT("the store holds the texture for the command buffer being encoded", 1, gsb_held(t->serial));
    uint64_t r2 = g_pkg_reuploads; m[OFF + lo[2][0] + 3] = 0x66;
    EXPECT("a CPU write while it is held: the texture keeps its host texture", 1, tex_mtl(t) == host && g_pkg_reuploads == r2);
    mtl_wait(mtl_sub_commit(), 5000000000ull); EXPECT("held store GPU success", 0, mtl_last_commit_error());
    EXPECT("committed: no longer held", 0, gsb_held(t->serial));
    id<MTLTexture> again = tex_mtl(t); EXPECT("... and the CPU write is uploaded at the next use", 1, (int)(g_pkg_reuploads - r2));
    bad = 0; for (uint64_t i = lo[0][1]; i < hi[0][1]; i++) bad += m[OFF + i] != 0x55;
    EXPECT("the store is in the storage, every byte of the level", 0, bad);
    EXPECT("and the CPU write", 0x66, m[OFF + lo[2][0] + 3]);
    mtl_finish_gpu();   // the re-upload's batch, before another queue reads its texture
    {   // the new host texture, read on the GPU: the stored level
        id<MTLBuffer> rb = [g_dev newBufferWithLength:(size_t)W * W * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromTexture:again sourceSlice:0 sourceLevel:1 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W / 2, W / 2, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:(W / 2) * 4 destinationBytesPerImage:(W / 2) * (W / 2) * 4];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        bad = 0; for (int i = 0; i < (W / 2) * (W / 2) * 4; i++) bad += ((uint8_t *)rb.contents)[i] != 0x55;
        EXPECT("the re-uploaded host texture holds the stored level", 0, bad);
    }
    // A store into the storage's first host page, which other data shares (pkg_edges_gpu): a CPU write beside the texture there afterwards is a write beside it, not into it, so it
    // keeps its host texture; a CPU write into its own bytes there is still uploaded
    {
        EXPECT("layer 0's level 0 begins in the shared first page", 1, lo[0][0] == 0 && ((uintptr_t)(m + OFF) & (uintptr_t)(getpagesize() - 1)) != 0);
        memset(m + SRC, 0x5A, (size_t)W * W * 4);
        h_nvnCommandBufferBeginRecording(&cmd);
        NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, 0, 1);
        NVNcopyRegion r = {0, 0, 0, W, W, 1}; h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + SRC), &arr, &v, &r, 0);
        h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        NVNcommandHandle he = h_nvnCommandBufferEndRecording(&cmd);
        uint64_t s3 = g_gsb_stores;
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(he, NULL)); mtl_sub_commit();
        wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("edge store fence wait", 1, wait == 0 || wait == 1);
        EXPECT("edge store GPU success", 0, mtl_last_commit_error());
        EXPECT("the copy into the first page is stored", 1, (int)(g_gsb_stores - s3));
        bad = 0; for (uint64_t i = lo[0][0]; i < hi[0][0]; i++) bad += m[OFF + i] != 0x5A;
        EXPECT("the store is in the storage, the shared page's part too", 0, bad);
        uint64_t r3 = g_pkg_reuploads, k3 = g_pkg_edge_skips; id<MTLTexture> before = tex_mtl(t);
        m[OFF - 16] = 0xAB;   // the other data in the shared page
        EXPECT("a CPU write beside it after the store: no re-upload", 1, tex_mtl(t) == before && g_pkg_reuploads == r3);
        EXPECT("... judged by the edge bytes as stored", 1, (int)(g_pkg_edge_skips - k3));
        m[OFF + 8] ^= 0xff;   // its own byte in the shared page
        tex_mtl(t); EXPECT("a CPU write into its bytes in the shared page: uploaded again", 1, (int)(g_pkg_reuploads - r3));
    }
    h_nvnTextureFinalize(&arr); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
} }

// The same into a 3D texture, a slice per copy, as LEGO Star Wars fills its light-probe volumes (GitHub #1856): each slice is stored into its plane of the level's block-linear order
// (the planes interleave by the level's block depth), and none uploads the volume again
static void copy_stream_3d(void) { @autoreleasepool {
    enum { W = 32, D = 8, POOL = 1 << 20, OFF = 0x11000, SRC = 0x80000, OUT = 0xC0000 };
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), POOL)) abort(); memset(mem, 0, POOL);
    uint8_t *m = mem;
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=POOL}; mtl_pool_add(mem, POOL); nvn_dirty_add_range(mem, POOL);
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_3D);
    h_nvnTextureBuilderSetWidth(&tb, W); h_nvnTextureBuilderSetHeight(&tb, W); h_nvnTextureBuilderSetDepth(&tb, D); h_nvnTextureBuilderSetLevels(&tb, 2);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, OFF);
    NVNtexture vol; EXPECT("streamed volume init", 1, h_nvnTextureInitialize(&vol, &tb));
    const tex_t *t = (const tex_t *)&vol;
    EXPECT("streamed volume fits before the copy source", 1, OFF + t->L.size <= SRC);
    memset(m + OFF, 0x11, t->L.size);
    tex_mtl(t);
    uint64_t r0 = g_pkg_reuploads, s0 = g_gsb_stores;
    NVNcommandBuffer cmd; EXPECT("volume command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 0); h_nvnCommandBufferSetCopyImageStride(&cmd, 0);
    NVNtextureView v; h_nvnTextureViewSetDefaults(&v); h_nvnTextureViewSetLevels(&v, 0, 1);
    for (int z = 0; z < D; z++) {   // level 0, one slice per copy, each its own value
        memset(m + SRC + (size_t)z * W * W * 4, 0x40 + z, (size_t)W * W * 4);
        NVNcopyRegion r = {0, 0, z, W, W, 1}; h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + SRC + (size_t)z * W * W * 4), &vol, &v, &r, 0);
    }
    memset(m + OUT, 0xEE, (size_t)W * W * D * 4);
    NVNcopyRegion all = {0, 0, 0, W, W, D}; h_nvnCommandBufferCopyTextureToBuffer(&cmd, &vol, &v, &all, (NVNbufferAddress)(m + OUT), 0);   // the host texture's level 0, read on the GPU
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("volume fence wait", 1, wait == 0 || wait == 1);
    EXPECT("volume GPU success", 0, mtl_last_commit_error());
    EXPECT("8 slice copies into a volume uploaded from its storage: no re-upload", 0, (int)(g_pkg_reuploads - r0));
    EXPECT("... the 8 copies stored on the GPU", D, (int)(g_gsb_stores - s0));
    uint8_t *lin = malloc((size_t)W * W * D * 4); int bad = 0;
    EXPECT("volume storage deswizzles", 0, nvn_deswizzle(&t->d, 0, 0, t->mem, lin, (size_t)W * 4));
    for (int z = 0; z < D; z++) for (size_t i = 0; i < (size_t)W * W * 4; i++) { size_t k = (size_t)z * W * W * 4 + i; bad += m[OUT + k] != 0x40 + z; bad += lin[k] != 0x40 + z; }
    EXPECT("host texture and storage hold every slice, every byte", 0, bad);
    free(lin);
    uint64_t lo, hi; nvn_tex_level_range(t, 1, 0, &lo, &hi);
    uint64_t r1 = g_pkg_reuploads; m[OFF + lo + 3] ^= 0xff; tex_mtl(t);
    EXPECT("a CPU write into level 1 after the copies: uploaded again", 1, (int)(g_pkg_reuploads - r1));
    h_nvnTextureFinalize(&vol); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
} }

// A GPU texture in storage the CPU can read (no packaged layout, never CPU-written; a CPU_CACHED | GPU_CACHED pool): what a render pass, a texture copy and a texture clear write
// into it reaches its storage too, as on the hardware (gsb_store). Ocarina of Time moves the terrain normal maps the GPU drew with memcpy and re-creates them at the new address
static void gpu_storage(void) {
    enum { N = 1 << 20, W = 128 };
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), N)) abort(); memset(mem, 0, N);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=N, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}; mtl_pool_add(mem, N); nvn_dirty_add_range(mem, N);
    NVNtextureBuilder tb; NVNtexture drawn, copied, cleared, moved, pkc; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, W); h_nvnTextureBuilderSetHeight(&tb, W);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 0x40000); EXPECT("GPU texture init", 1, h_nvnTextureInitialize(&drawn, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 0x60000); EXPECT("GPU texture 2 init", 1, h_nvnTextureInitialize(&copied, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 0x80000); EXPECT("GPU texture 3 init", 1, h_nvnTextureInitialize(&cleared, &tb));
    NVNtextureBuilder pb = tb; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; h_nvnTextureBuilderSetPackagedTextureLayout(&pb, &lay); h_nvnTextureBuilderSetWidth(&pb, 8); h_nvnTextureBuilderSetHeight(&pb, 4);
    h_nvnTextureBuilderSetStorage(&pb, (const NVNmemoryPool *)&pool, 0xc0000); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&pkc, &pb));
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    const NVNtexture *targets[] = {&drawn}; const NVNtextureView *views[] = {NULL};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets, views, NULL, NULL);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color, 15);
    NVNcopyRegion region = {0, 0, 0, W, W, 1}, small = {0, 0, 0, 8, 4, 1};
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &drawn, NULL, &region, &copied, NULL, &region, 0);
    float color2[] = {0.8f, 0.6f, 0.4f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &cleared, NULL, &region, color2, 15);
    h_nvnCommandBufferClearTexture(&cmd, &pkc, NULL, &small, color2, 15);   // a texture uploaded from its storage: its clear is stored as its render passes are
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("GPU storage fence wait", 1, wait == 0 || wait == 1);
    EXPECT("GPU success", 0, mtl_last_commit_error());
    static const uint8_t expected[] = {51, 102, 153, 255}, expected2[] = {204, 153, 102, 255};
    uint8_t *lin = malloc(W * W * 4); const tex_t *td = TX(&drawn), *tc = TX(&copied), *tl = TX(&cleared), *tp = TX(&pkc); int bad;
    memset(lin, 0xEE, W * W * 4); bad = nvn_deswizzle(&td->d, 0, 0, td->mem, lin, W * 4) ? 1 << 20 : 0; for (int i = 0; i < W * W; i++) bad += memcmp(lin + i * 4, expected, 4) != 0;
    EXPECT("render pass into a GPU texture -> its storage, every pixel", 0, bad);
    memset(lin, 0xEE, W * W * 4); bad = nvn_deswizzle(&tc->d, 0, 0, tc->mem, lin, W * 4) ? 1 << 20 : 0; for (int i = 0; i < W * W; i++) bad += memcmp(lin + i * 4, expected, 4) != 0;
    EXPECT("texture copy into a GPU texture -> its storage, every pixel", 0, bad);
    memset(lin, 0xEE, W * W * 4); bad = nvn_deswizzle(&tl->d, 0, 0, tl->mem, lin, W * 4) ? 1 << 20 : 0; for (int i = 0; i < W * W; i++) bad += memcmp(lin + i * 4, expected2, 4) != 0;
    EXPECT("texture clear of a GPU texture -> its storage, every pixel", 0, bad);
    memset(lin, 0xEE, W * W * 4); bad = nvn_deswizzle(&tp->d, 0, 0, tp->mem, lin, 32) ? 1 << 20 : 0; for (int i = 0; i < 32; i++) bad += memcmp(lin + i * 4, expected2, 4) != 0;
    EXPECT("texture clear of a packaged texture -> its storage, 32 pixels", 0, bad);
    uint64_t up = 0, reup = 0, up2 = 0, reup2 = 0; mtl_pkg_stats(&up, &reup);
    nvn_ft_poll(); mtl_sub_begin(); id<MTLTexture> host = tex_mtl(td); tex_mtl(tc); tex_mtl(tl); tex_mtl(tp); mtl_wait(mtl_sub_commit(), 5000000000ull); mtl_pkg_stats(&up2, &reup2);
    EXPECT("they stay GPU textures: nothing is uploaded from the storage the host wrote", 1, host && up == up2 && reup == reup2 && mtl_tex_gpu_owned(td->serial) && mtl_tex_gpu_owned(tc->serial) && mtl_tex_gpu_owned(tl->serial)
           && !tex_cpu_written(td->serial) && !tex_cpu_written(tc->serial) && !tex_cpu_written(tl->serial) && gws_owned((uintptr_t)td->mem, (uintptr_t)td->mem + td->L.size));
    EXPECT("nothing held once committed", 1, !gsb_held(td->serial) && !gsb_held(tp->serial));
    memcpy((uint8_t *)mem + 0xa0000, td->mem, td->L.size);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 0xa0000); EXPECT("moved texture init", 1, h_nvnTextureInitialize(&moved, &tb));
    memset(lin, 0xEE, W * W * 4); h_nvnTextureReadTexels(&moved, NULL, &region, lin); bad = 0; for (int i = 0; i < W * W; i++) bad += memcmp(lin + i * 4, expected, 4) != 0;
    EXPECT("storage moved with memcpy -> a texture there holds the drawing (ReadTexels), every pixel", 0, bad);
    EXPECT("and is taken from its storage", 1, tex_cpu_written(TX(&moved)->serial));
    free(lin);
    h_nvnTextureFinalize(&moved); h_nvnTextureFinalize(&pkc); h_nvnTextureFinalize(&cleared); h_nvnTextureFinalize(&copied); h_nvnTextureFinalize(&drawn);
    mtl_alias_drop_pool(&pool); nvn_dirty_remove_range(mem); mtl_pool_del(mem); mtl_drain(); free(mem);
}

// Storage of the kinds the store takes besides the plain RGBA8 2D ones above (gsb_store_buf): a buffer copy into a block region of layer 1 of a packaged BC1 array (block rows), and into
// a packaged RGB565 texture (stored from the source bytes, the guest's order, with no red / blue exchange). And the order of writes into one storage, all on the GPU: a texture clear of a
// packaged texture in one submission, not waited for, a buffer copy over its right half in the next, then a copy from it into another packaged texture; each lands after the earlier
static int all_px(const uint8_t *lin, int w, int x0, int x1, int h, const uint8_t px[4]) { int bad = 0; for (int y = 0; y < h; y++) for (int x = x0; x < x1; x++) bad += memcmp(lin + (y * w + x) * 4, px, 4) != 0; return bad; }
static void store_kinds(void) { @autoreleasepool {
    enum { POOL = 1 << 20, BC = 0x10000, RGB = 0x30000, P = 0x40000, Q = 0x50000, SRC = 0x80000, SRC2 = 0x90000, SRC3 = 0xA0000 };
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), POOL)) abort(); memset(mem, 0, POOL); uint8_t *m = mem;
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=POOL}; mtl_pool_add(mem, POOL); nvn_dirty_add_range(mem, POOL);
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D_ARRAY); h_nvnTextureBuilderSetSize3D(&tb, 64, 64, 2); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA_DXT1);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, BC); NVNtexture bc; EXPECT("packaged BC1 array init", 1, h_nvnTextureInitialize(&bc, &tb));
    EXPECT("BC1 array fits", 1, BC + TX(&bc)->L.size <= RGB);
    memset(m + BC, 0x11, TX(&bc)->L.size);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize3D(&tb, 16, 4, 1); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGB565);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, RGB); NVNtexture rgb; EXPECT("packaged RGB565 texture init", 1, h_nvnTextureInitialize(&rgb, &tb));
    h_nvnTextureBuilderSetSize3D(&tb, 8, 4, 1); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, P); NVNtexture pt; EXPECT("packaged texture P init", 1, h_nvnTextureInitialize(&pt, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, Q); NVNtexture qt; EXPECT("packaged texture Q init", 1, h_nvnTextureInitialize(&qt, &tb));
    for (int i = 0; i < 4 * 2 * 8; i++) m[SRC + i] = (uint8_t)(0x80 ^ i);   // 4x2 BC1 blocks (16x8 texels)
    for (int i = 0; i < 16 * 4 * 2; i++) m[SRC2 + i] = (uint8_t)(i * 7 + 3);
    memset(m + SRC3, 0x99, 4 * 4 * 4);
    NVNcommandBuffer cmd; EXPECT("kinds command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd); h_nvnCommandBufferSetCopyRowStride(&cmd, 0); h_nvnCommandBufferSetCopyImageStride(&cmd, 0);
    NVNcopyRegion rbc = {8, 4, 1, 16, 8, 1}, rrgb = {0, 0, 0, 16, 4, 1}, whole = {0, 0, 0, 8, 4, 1}, right = {4, 0, 0, 4, 4, 1};
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + SRC), &bc, NULL, &rbc, 0);
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + SRC2), &rgb, NULL, &rrgb, 0);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &pt, NULL, &whole, color, 15);
    NVNcommandHandle h1 = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h1, NULL)); mtl_sub_commit();   // not waited for
    h_nvnCommandBufferBeginRecording(&cmd); h_nvnCommandBufferSetCopyRowStride(&cmd, 0); h_nvnCommandBufferSetCopyImageStride(&cmd, 0);
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)(m + SRC3), &pt, NULL, &right, 0);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pt, NULL, &whole, &qt, NULL, &whole, 0);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL); h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h2 = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h2, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("kinds fence wait", 1, wait == 0 || wait == 1);
    EXPECT("kinds GPU success", 0, mtl_last_commit_error());
    {   // BC1 layer 1: 16 block columns of 8 bytes, 16 block rows
        uint8_t lin[16 * 8 * 16]; int bad = nvn_deswizzle(&TX(&bc)->d, 0, 1, TX(&bc)->mem, lin, 16 * 8) ? 1 << 20 : 0;
        for (int by = 0; by < 16; by++) for (int bx = 0; bx < 16; bx++) for (int k = 0; k < 8; k++) {
            int in = bx >= 2 && bx < 6 && by >= 1 && by < 3;
            bad += lin[(by * 16 + bx) * 8 + k] != (in ? (uint8_t)(0x80 ^ (((by - 1) * 4 + (bx - 2)) * 8 + k)) : 0x11); }
        EXPECT("buffer copy into a block region of a packaged BC1 array's layer 1 -> those blocks of its storage, the rest untouched", 0, bad);
        bad = nvn_deswizzle(&TX(&bc)->d, 0, 0, TX(&bc)->mem, lin, 16 * 8) ? 1 << 20 : 0; for (size_t i = 0; i < sizeof lin; i++) bad += lin[i] != 0x11;
        EXPECT("... and layer 0 untouched", 0, bad);
    }
    {   uint8_t lin[16 * 4 * 2]; int bad = nvn_deswizzle(&TX(&rgb)->d, 0, 0, TX(&rgb)->mem, lin, 16 * 2) ? 1 << 20 : 0;
        for (int i = 0; i < 16 * 4 * 2; i++) bad += lin[i] != (uint8_t)(i * 7 + 3);
        EXPECT("buffer copy into a packaged RGB565 texture -> its storage holds the source's bytes", 0, bad);
    }
    {   static const uint8_t a[] = {51, 102, 153, 255}, b[] = {0x99, 0x99, 0x99, 0x99}; uint8_t lin[8 * 4 * 4];
        int bad = nvn_deswizzle(&TX(&pt)->d, 0, 0, TX(&pt)->mem, lin, 32) ? 1 << 20 : 0; bad += all_px(lin, 8, 0, 4, 4, a) + all_px(lin, 8, 4, 8, 4, b);
        EXPECT("a clear, then a copy over its right half in the next submission: the storage holds both, in that order", 0, bad);
        bad = nvn_deswizzle(&TX(&qt)->d, 0, 0, TX(&qt)->mem, lin, 32) ? 1 << 20 : 0; bad += all_px(lin, 8, 0, 4, 4, a) + all_px(lin, 8, 4, 8, 4, b);
        EXPECT("then a copy from it into another packaged texture: its storage holds both too", 0, bad);
    }
    h_nvnCommandBufferFinalize(&cmd); h_nvnSyncFinalize(&sync);
    h_nvnTextureFinalize(&qt); h_nvnTextureFinalize(&pt); h_nvnTextureFinalize(&rgb); h_nvnTextureFinalize(&bc);
    nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
} }

int main(void) { @autoreleasepool {
    alarm(60); if (setvbuf(stdout, NULL, _IONBF, 0)) abort();   // GPU, callback and pthread failures must not leave the check target hanging.
    mtl_init();
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 524288)) abort(); memset(mem, 0, 524288);
    // Registered with the dirty tracker as nvnMemoryPoolInitialize does: a packaged texture in an untracked pool is re-uploaded from its storage at every use, which would discard what the GPU wrote into it
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=524288}; mtl_pool_add(mem, 524288); nvn_dirty_add_range(mem, 524288);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 131072); EXPECT("texture init", 1, h_nvnTextureInitialize(&tex, &tb));
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    const NVNtexture *targets[] = {&tex}; const NVNtextureView *views[] = {NULL};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets, views, NULL, NULL);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color, 15);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; uint8_t *out = (uint8_t *)mem + 196608; memset(out, 0xEE, 256);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 64);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (NVNbufferAddress)out, 0);
    // A copy from the cleared (GPU-written) texture into a packaged texture, whose storage is where the host takes its content from: the copy's bytes are stored into the storage on
    // the GPU (gsb_store), so ReadTexels, which reads a packaged texture's storage, sees the clear colour
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtexture pk; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&pk, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &tex, NULL, &region, &pk, NULL, &region, 0);
    // A copy from that packaged texture while its store is in the same command buffer: the copy reads the host texture on the GPU, and its own store lands after the first
    NVNtexture pk2; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 262144); EXPECT("packaged texture 2 init", 1, h_nvnTextureInitialize(&pk2, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk, NULL, &region, &pk2, NULL, &region, 0);
    // A buffer a copy queued in this submission writes (out → src2 on the GPU) holds stale bytes on the CPU: the store reads it on the GPU, after that copy
    uint8_t *src2 = (uint8_t *)mem + 200704; memset(src2, 0xEE, 256);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)out, (NVNbufferAddress)src2, 256, 0);
    NVNtexture pk4; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 294912); EXPECT("packaged texture 4 init", 1, h_nvnTextureInitialize(&pk4, &tb));
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src2, &pk4, NULL, &region, 0);
    // A format-converting copy (RGBA8 → RGBA8 sRGB, the same bytes per texel) from a packaged texture the CPU wrote is drawn on the GPU: the storage takes the converted bytes (stored from
    // the host texture), not the source's raw bytes. Every texel of the source is the same, whatever its block-linear order
    static const uint8_t expected[] = {51, 102, 153, 255};
    NVNtexture pk7; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 425984); EXPECT("packaged texture 7 init", 1, h_nvnTextureInitialize(&pk7, &tb));
    for (int i = 0; i < 1024; i++) memcpy((uint8_t *)mem + 425984 + i * 4, expected, 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    NVNtexture pk3; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 327680); EXPECT("packaged sRGB texture init", 1, h_nvnTextureInitialize(&pk3, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk7, NULL, &region, &pk3, NULL, &region, 0);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    // A packaged texture the GPU cleared (the clear gives its host texture render-target use): a copy from it takes the clear, from the host texture
    NVNtexture pk5, pk6; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 360448); EXPECT("packaged texture 5 init", 1, h_nvnTextureInitialize(&pk5, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 393216); EXPECT("packaged texture 6 init", 1, h_nvnTextureInitialize(&pk6, &tb));
    float color2[] = {0.8f, 0.6f, 0.4f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &pk5, NULL, &region, color2, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk5, NULL, &region, &pk6, NULL, &region, 0);
    // A buffer copy into slices 1 and 2 of a packaged 3D texture from a buffer this submission writes on the GPU: the region is stored from the source bytes into those slices of the
    // storage (GitHub #1856: LEGO Star Wars fills its light-probe volumes a slice at a time and then moves them by copying their storage); slices 0 and 3 stay as they were
    uint8_t *src3 = (uint8_t *)mem + 204800; memset(src3, 0xEE, 512);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)out, (NVNbufferAddress)src3, 256, 0);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)out, (NVNbufferAddress)(src3 + 256), 256, 0);
    NVNtexture vol3; h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_3D); h_nvnTextureBuilderSetSize3D(&tb, 8, 4, 4);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 229376); EXPECT("packaged 3D texture init", 1, h_nvnTextureInitialize(&vol3, &tb));
    EXPECT("packaged 3D texture fits the pool", 1, 229376 + ((const tex_t *)&vol3)->L.size <= 262144);
    NVNcopyRegion region3 = {0, 0, 1, 8, 4, 2};
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src3, &vol3, NULL, &region3, 0);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize3D(&tb, 8, 4, 1);
    // A render pass into a packaged texture writes its storage, as the hardware's would: what the pass drew is stored into the storage on the GPU when the pass ends (rt_mirror_flush).
    // Tears of the Kingdom moves such textures with memcpy (its resource defragmentation) and re-creates them at the new address, which then has to carry the drawing
    NVNtexture pk8; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 458752); EXPECT("packaged render target init", 1, h_nvnTextureInitialize(&pk8, &tb));
    const NVNtexture *targets8[] = {&pk8};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets8, views, NULL, NULL);
    float color3[] = {0.4f, 0.2f, 0.8f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color3, 15);
    // The same into depth plane 2 of a packaged 3D texture (a view's first layer selects the plane): that plane is stored into plane 2 of the storage, the others stay as they were
    NVNtexture vol3r; h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_3D); h_nvnTextureBuilderSetSize3D(&tb, 8, 4, 4);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 245760); EXPECT("packaged 3D render target init", 1, h_nvnTextureInitialize(&vol3r, &tb));
    EXPECT("packaged 3D render target fits before the next", 1, 245760 + ((const tex_t *)&vol3r)->L.size <= 262144);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetSize3D(&tb, 8, 4, 1);
    NVNtextureView plane2; h_nvnTextureViewSetDefaults(&plane2); h_nvnTextureViewSetLayers(&plane2, 2, 1);
    const NVNtexture *targets3[] = {&vol3r}; const NVNtextureView *views3[] = {&plane2};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets3, views3, NULL, NULL); h_nvnCommandBufferClearColor(&cmd, 0, color3, 15);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(nvn_rec_linear(h, NULL)); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("recorded fence wait", 1, wait == 0 || wait == 1);
    EXPECT("GPU success", 0, mtl_last_commit_error());
    int bad = 0;
    for (int y=0; y<4; y++) { for (int x=0; x<8; x++) bad += memcmp(out+y*64+x*4, expected, 4) != 0;
        for (int x=32; x<64; x++) bad += out[y*64+x] != 0xEE; }
    EXPECT("pending clear -> GPU copy, literal pixels and untouched row padding", 0, bad);
    ReadbackDevice *device = [ReadbackDevice new]; device.device = g_dev; g_dev = (id)device;
    uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&tex, NULL, &region, cpu); bad = 0;
    g_dev = device.device;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU clear -> CPU ReadTexels, 32 literal pixels", 0, bad);
    EXPECT("ReadTexels used the GPU readback path", 1, device.allocations);
    EXPECT("ReadTexels releases its readback buffer before the caller's pool drains", 1, device.lastBuffer == nil);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU copy into a packaged texture -> its storage (ReadTexels), 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk2, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a packaged texture with a store in the same command buffer -> the first copy, 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk4, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a buffer a queued copy writes -> that copy's bytes, 32 literal pixels", 0, bad);
    static const uint8_t expected_srgb[] = {124, 170, 203, 255};   // the clear colour sRGB-encoded (within 1: the GPU's encoding curve), not the source's 51, 102, 153
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk3, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) for (int k=0; k<4; k++) bad += abs((int)cpu[i*4+k] - expected_srgb[k]) > 1;
    EXPECT("format-converting copy from a CPU-written packaged texture -> converted bytes in its storage, 32 pixels within 1", 0, bad);
    static const uint8_t expected2[] = {204, 153, 102, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk6, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected2, 4) != 0;
    EXPECT("copy from a packaged texture the GPU cleared -> the clear, 32 literal pixels", 0, bad);
    static const uint8_t expected3[] = {102, 51, 204, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk8, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected3, 4) != 0;
    EXPECT("render pass into a packaged texture -> its storage (ReadTexels), 32 literal pixels", 0, bad);
    uint64_t reuploads = g_pkg_reuploads; tex_mtl((const tex_t *)&pk8);
    EXPECT("its own store is not uploaded again", 1, g_pkg_reuploads == reuploads);
    NVNtexture pk9; memcpy((uint8_t *)mem + 491520, (uint8_t *)mem + 458752, ((const tex_t *)&pk8)->L.size);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 491520); EXPECT("moved texture init", 1, h_nvnTextureInitialize(&pk9, &tb));
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk9, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected3, 4) != 0;
    EXPECT("storage moved with memcpy -> a texture there holds the drawing, 32 literal pixels", 0, bad);
    {   // the 3D copy's slices in the storage: decode the whole level (4 slices of 8x4 RGBA8)
        const tex_t *v3 = (const tex_t *)&vol3; uint8_t lin3[4 * 128]; memset(lin3, 0xEE, sizeof lin3); bad = nvn_deswizzle(&v3->d, 0, 0, v3->mem, lin3, 32) ? 1000 : 0;
        for (int z = 0; z < 4; z++) for (int i = 0; i < 32; i++) bad += z == 1 || z == 2 ? memcmp(lin3 + z * 128 + i * 4, expected, 4) != 0 : (lin3[z * 128 + i * 4] | lin3[z * 128 + i * 4 + 1] | lin3[z * 128 + i * 4 + 2] | lin3[z * 128 + i * 4 + 3]) != 0;
        EXPECT("buffer copy into slices 1-2 of a packaged 3D texture -> stored into those slices of its storage, the others untouched", 0, bad);
        const tex_t *r3 = (const tex_t *)&vol3r; memset(lin3, 0xEE, sizeof lin3); bad = nvn_deswizzle(&r3->d, 0, 0, r3->mem, lin3, 32) ? 1000 : 0;
        for (int z = 0; z < 4; z++) for (int i = 0; i < 32; i++) bad += z == 2 ? memcmp(lin3 + z * 128 + i * 4, expected3, 4) != 0 : (lin3[z * 128 + i * 4] | lin3[z * 128 + i * 4 + 1] | lin3[z * 128 + i * 4 + 2] | lin3[z * 128 + i * 4 + 3]) != 0;
        EXPECT("render pass into depth plane 2 of a packaged 3D texture -> stored into that plane of its storage, the others untouched", 0, bad);
    }
    h_nvnTextureFinalize(&pk9); h_nvnTextureFinalize(&pk8); h_nvnTextureFinalize(&vol3); h_nvnTextureFinalize(&vol3r);
    h_nvnTextureFinalize(&pk6); h_nvnTextureFinalize(&pk5); h_nvnTextureFinalize(&pk3); h_nvnTextureFinalize(&pk7); h_nvnTextureFinalize(&pk4); h_nvnTextureFinalize(&pk2);
    h_nvnTextureFinalize(&pk); h_nvnTextureFinalize(&tex); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
    sparse_reads(&region, expected);
    stale_pools();
    packed_copies(NVN_FORMAT_RGB565, 500); packed_copies(NVN_FORMAT_RGB5A1, 501); packed_copies(NVN_FORMAT_RGB5, 502); packed_copies(NVN_FORMAT_RGBA4, 503);
    store_completion();
    copy_stream();
    copy_stream_3d();
    gpu_storage();
    store_kinds();
    printf("READBACK cases=%d failed=%d\n", cases, failed); return failed != 0;
} }
