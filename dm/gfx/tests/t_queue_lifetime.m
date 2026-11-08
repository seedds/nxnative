// Submitted commands own their CPU descriptors. Pool storage and shader code stay
// alive while a wait parks the queue; only program/texture objects are reused.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
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
static int checks, failures;
#define CHECK(c, name) do { int check_ok = !!(c); checks++; failures += !check_ok; printf("%s: %s\n", check_ok ? "PASS" : "FAIL", name); } while (0)
static void require(int ok, const char *name) { CHECK(ok, name); if (!ok) exit(2); }
enum { W = 8, H = 8, BYTES = W * H * 4, POOL_BYTES = 1 << 20, TEX_A = 0x80000, TEX_B = 0x90000, OUTPUT = 0xa0000, EVENT = 0xa1000 };
static NVNdevice device; static NVNqueue queues[2]; static NVNmemoryPool pool; static NVNcommandBuffer command;
static NVNevent event; static NVNtexture target, replacement; static uint8_t *memory; static NVNbufferAddress output;
static dispatch_semaphore_t cancel_rescue, rescue_done;
static const NVNcopyRegion region = {0, 0, 0, W, H, 1};

static int landed(NVNsync *sync, uint64_t timeout) {
    NVNsyncWaitResult r = P(nvnSyncWait)(sync, timeout);
    return r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
}
static void submit(int queue, NVNcommandHandle handle, NVNsync *sync) {
    P(nvnSyncInitialize)(sync, &device); P(nvnQueueSubmitCommands)(&queues[queue], 1, &handle);
    // The queue-tail receipt includes retiring this job's CPU descriptors.
    P(nvnQueueFenceSync)(&queues[queue], sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
}
static void finish(NVNsync *sync) {
    require(landed(sync, 5000000000ull), "queue tail and GPU work complete");
    P(nvnSyncFinalize)(sync);
}
static void park(void) {
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0);
    cancel_rescue = dispatch_semaphore_create(0); rescue_done = dispatch_semaphore_create(0);
    dispatch_semaphore_t cancel = cancel_rescue, done = rescue_done;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (dispatch_semaphore_wait(cancel, dispatch_time(DISPATCH_TIME_NOW, 2000000000ll))) P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
        dispatch_semaphore_signal(done);
    });
}
static void release(void) {
    // Every assertion path releases the event. The watchdog also releases a
    // broken implementation that blocks QueueSubmitCommands on the CPU.
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1); dispatch_semaphore_signal(cancel_rescue);
    require(!dispatch_semaphore_wait(rescue_done, dispatch_time(DISPATCH_TIME_NOW, 3000000000ll)), "event rescue worker retired");
}
static int pixels(unsigned offset, unsigned r, unsigned g, unsigned b) {
    const uint8_t want[] = {r, g, b, 255};
    for (int i = 0; i < W * H; i++) if (memcmp(memory + OUTPUT + offset + i * 4, want, 4)) return 0;
    return 1;
}
static void init_texture(NVNtexture *texture, unsigned offset) {
    NVNtextureBuilder tb = {0}; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
    P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&tb, W, H);
    P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb, &pool, offset);
    require(P(nvnTextureInitialize)(texture, &tb), "texture initialized on retained backing storage");
}
static void init_program(NVNprogram *program, NvProg *compiled) {
    require(P(nvnProgramInitialize)(program, &device), "program object initialized");
    // Synthetic MSL replaces only game shader loading; recording and submission
    // use the public NVN entry points and the real serial ownership table.
    prog_t *p = (prog_t *)program; p->serial = nvn_new_serial(); serial_put(p->serial, compiled);
}
static NVNcommandHandle record(NVNtexture *texture, NVNprogram *program, int wait, int draw, unsigned offset) {
    P(nvnCommandBufferBeginRecording)(&command);
    if (wait) P(nvnCommandBufferWaitEvent)(&command, &event, NVN_EVENT_WAIT_MODE_EQUAL, 1);
    const NVNtexture *colors[] = {texture}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, NULL, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    if (program) P(nvnCommandBufferBindProgram)(&command, program, 3);
    if (draw) P(nvnCommandBufferDrawArrays)(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, texture, NULL, &region, output + offset, 0);
    return P(nvnCommandBufferEndRecording)(&command);
}
int main(void) { @autoreleasepool {
    setbuf(stdout, NULL); mtl_init(); NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); } fragment float4 green() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "synthetic shaders compile");
    NvShader *vs = [NvShader new]; vs->fn = [library newFunctionWithName:@"v"];
    NvProg *red = [NvProg new], *green = [NvProg new]; red->sh[0] = green->sh[0] = vs;
    for (int i = 0; i < 2; i++) {
        NvShader *fs = [NvShader new]; fs->stage = 1; fs->rf.color_mask = 1; fs->fn = [library newFunctionWithName:i ? @"green" : @"red"];
        (i ? green : red)->sh[1] = fs;
    }
    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    for (int i = 0; i < 2; i++) { NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device); require(P(nvnQueueInitialize)(&queues[i], &qb), "queue initialized"); }
    require(!posix_memalign((void **)&memory, getpagesize(), POOL_BYTES), "pool storage allocated"); memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24); require(P(nvnMemoryPoolInitialize)(&pool, &pb), "pool initialized");
    NVNbufferBuilder bb = {0}; NVNbuffer buffer; P(nvnBufferBuilderSetDefaults)(&bb); P(nvnBufferBuilderSetDevice)(&bb, &device);
    P(nvnBufferBuilderSetStorage)(&bb, &pool, OUTPUT, BYTES * 2); require(P(nvnBufferInitialize)(&buffer, &bb), "readback buffer initialized"); output = P(nvnBufferGetAddress)(&buffer);
    NVNeventBuilder eb = {0}; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb, &pool, EVENT); require(P(nvnEventInitialize)(&event, &eb), "event initialized");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, TEX_A);
    init_texture(&target, TEX_A); init_texture(&replacement, TEX_B);
    NVNprogram reused, stable_red, stable_green; init_program(&reused, red); init_program(&stable_red, red); init_program(&stable_green, green);
    NVNsync sync;
    submit(0, record(&target, &reused, 0, 1, 0), &sync); finish(&sync); CHECK(pixels(0, 255, 0, 0), "original program draws red before reuse");

    puts("CASE submitted program object reused behind an event");
    park(); memset(memory + OUTPUT, 0x5a, BYTES);
    submit(0, record(&target, &reused, 1, 1, 0), &sync);
    CHECK(!landed(&sync, 100000000ull), "submitted draw is still parked before object reuse");
    P(nvnProgramFinalize)(&reused); init_program(&reused, green);
    release(); finish(&sync); CHECK(pixels(0, 255, 0, 0), "parked draw retains its original red program");
    submit(0, record(&target, &reused, 0, 1, 0), &sync); finish(&sync); CHECK(pixels(0, 0, 255, 0), "later bind sees the reused green program object");

    puts("CASE bound program survives retiring its owning job");
    submit(0, record(&target, &stable_red, 0, 1, 0), &sync); finish(&sync); CHECK(pixels(0, 255, 0, 0), "job A binds and draws red");
    submit(1, record(&target, &stable_green, 0, 1, 0), &sync); finish(&sync); CHECK(pixels(0, 0, 255, 0), "another queue binds and draws green after job A retires");
    submit(0, record(&target, NULL, 0, 1, 0), &sync); finish(&sync); CHECK(pixels(0, 255, 0, 0), "job B draws with its queue's red program without another bind");

    puts("CASE submitted texture object reused behind an event");
    submit(0, record(&replacement, NULL, 0, 0, BYTES), &sync); finish(&sync); CHECK(pixels(BYTES, 0, 0, 255), "replacement texture backing starts blue");
    park(); submit(0, record(&target, &stable_red, 1, 1, 0), &sync);
    CHECK(!landed(&sync, 100000000ull), "submitted draw is still parked before texture object reuse");
    P(nvnTextureFinalize)(&target); init_texture(&target, TEX_B);
    release(); finish(&sync); CHECK(pixels(0, 255, 0, 0), "parked draw and copy retain the original texture descriptor");
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &replacement, NULL, &region, output + BYTES, 0);
    submit(0, P(nvnCommandBufferEndRecording)(&command), &sync); finish(&sync);
    CHECK(pixels(BYTES, 0, 0, 255), "queued draw did not write the replacement texture's backing");

    P(nvnQueueFinish)(&queues[0]); P(nvnQueueFinish)(&queues[1]);
    P(nvnProgramFinalize)(&reused); P(nvnProgramFinalize)(&stable_red); P(nvnProgramFinalize)(&stable_green);
    P(nvnTextureFinalize)(&target); P(nvnTextureFinalize)(&replacement); P(nvnCommandBufferFinalize)(&command);
    P(nvnBufferFinalize)(&buffer); P(nvnEventFinalize)(&event); P(nvnMemoryPoolFinalize)(&pool);
    P(nvnQueueFinalize)(&queues[0]); P(nvnQueueFinalize)(&queues[1]); P(nvnDeviceFinalize)(&device); free(memory);
    printf("queue lifetime: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
