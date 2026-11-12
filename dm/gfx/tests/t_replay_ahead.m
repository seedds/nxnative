// Replay ahead (DM_NVN_REPLAY_AHEAD, nvn_rec.h hold_*): a queue parked on an event the game's CPU signals is translated meanwhile, and nothing of it reaches the GPU before the signal.
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
enum { W = 8, H = 8, BYTES = W * H * 4, POOL_BYTES = 1 << 20, TEX_A = 0x80000, OUTPUT = 0xa0000, EVENT = 0xa1000, GPU_EVENT = 0xa2000 };
static NVNdevice device; static NVNqueue queues[2]; static NVNmemoryPool pool; static NVNcommandBuffer command;
static NVNevent event, gpu_event; static NVNtexture target; static uint8_t *memory; static NVNbufferAddress output;
static const NVNcopyRegion region = {0, 0, 0, W, H, 1};

static int landed(NVNsync *sync, uint64_t timeout) {
    NVNsyncWaitResult r = P(nvnSyncWait)(sync, timeout);
    return r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
}
static void submit(int queue, NVNcommandHandle handle, NVNsync *sync) {
    P(nvnSyncInitialize)(sync, &device); P(nvnQueueSubmitCommands)(&queues[queue], 1, &handle);
    P(nvnQueueFenceSync)(&queues[queue], sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
}
static int pixels(unsigned r, unsigned g, unsigned b) {
    const uint8_t want[] = {r, g, b, 255};
    for (int i = 0; i < W * H; i++) if (memcmp(memory + OUTPUT + i * 4, want, 4)) return 0;
    return 1;
}
static int untouched(void) { for (int i = 0; i < BYTES; i++) if (memory[OUTPUT + i] != 0x5a) return 0; return 1; }
// A clear to the colour and its copy into the readback buffer, behind a wait on ev (NULL: none), signalling sig afterwards (NULL: none)
static NVNcommandHandle record(NVNevent *ev, const float *colour, NVNevent *sig) {
    P(nvnCommandBufferBeginRecording)(&command);
    if (ev) P(nvnCommandBufferWaitEvent)(&command, ev, NVN_EVENT_WAIT_MODE_EQUAL, 1);
    const NVNtexture *colors[] = {&target}; const float viewport[] = {0, 0, W, H};
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, NULL, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferClearColor)(&command, 0, colour, 15);
    P(nvnCommandBufferCopyTextureToBuffer)(&command, &target, NULL, &region, output, 0);
    if (sig) P(nvnCommandBufferSignalEvent)(&command, sig, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 1);
    return P(nvnCommandBufferEndRecording)(&command);
}
static int wait_until(int (^cond)(void), int ms) { for (int i = 0; i < ms * 10; i++) { if (cond()) return 1; usleep(100); } return cond(); }
int main(void) { @autoreleasepool {
    if (setvbuf(stdout, NULL, _IONBF, 0)) abort(); mtl_init();
    int on = mtl_replay_ahead();
    printf("replay ahead %s\n", on ? "on" : "off");
    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    for (int i = 0; i < 2; i++) { NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device); require(P(nvnQueueInitialize)(&queues[i], &qb), "queue initialized"); }
    require(!posix_memalign((void **)&memory, getpagesize(), POOL_BYTES), "pool storage allocated"); memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24); require(P(nvnMemoryPoolInitialize)(&pool, &pb), "pool initialized");
    NVNbufferBuilder bb = {0}; NVNbuffer buffer; P(nvnBufferBuilderSetDefaults)(&bb); P(nvnBufferBuilderSetDevice)(&bb, &device);
    P(nvnBufferBuilderSetStorage)(&bb, &pool, OUTPUT, BYTES); require(P(nvnBufferInitialize)(&buffer, &bb), "readback buffer initialized"); output = P(nvnBufferGetAddress)(&buffer);
    NVNeventBuilder eb = {0}; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb, &pool, EVENT); require(P(nvnEventInitialize)(&event, &eb), "event initialized");
    P(nvnEventBuilderSetStorage)(&eb, &pool, GPU_EVENT); require(P(nvnEventInitialize)(&gpu_event, &eb), "second event initialized");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, TEX_A);
    NVNtextureBuilder tb = {0}; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
    P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&tb, W, H);
    P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX_A); require(P(nvnTextureInitialize)(&target, &tb), "texture initialized");
    const float red[] = {1, 0, 0, 1}, green[] = {0, 1, 0, 1}, blue[] = {0, 0, 1, 1};
    NVNsync sync, sync2;
    submit(0, record(NULL, red, NULL), &sync); require(landed(&sync, 5000000000ull), "an ungated submission lands"); P(nvnSyncFinalize)(&sync);
    CHECK(pixels(255, 0, 0), "it cleared to red");

    for (int i = 0; i < 40; i++) P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, i & 1);   // the game's CPU has signalled it many times, its commands never (nvn_queue.inc NQ_AHEAD_SIGNALS)
    puts("CASE a queue parked on a CPU-signalled event");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES);
    uint64_t holds = g_hold_n;
    submit(0, record(&event, green, NULL), &sync);
    if (on) {
        CHECK(wait_until(^{ return (int)(atomic_load(&g_hold_on) && atomic_load(&g_hold_recs) >= 2); }, 2000), "its commands are translated before the signal: a commit and its fence signal are held");
    } else CHECK(!atomic_load(&g_hold_on), "no hold with the switch off");
    CHECK(!landed(&sync, 100000000ull), "the submission has not run 100 ms later");
    CHECK(untouched(), "the GPU wrote nothing of it");
    CHECK(!on || atomic_load(&g_hold_on), "a wait that gives up within the limit leaves the hold");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    require(landed(&sync, 5000000000ull), "it lands after the signal"); P(nvnSyncFinalize)(&sync);
    CHECK(pixels(0, 255, 0), "and cleared to green");
    CHECK(!atomic_load(&g_hold_on) && g_hold_n == holds + (on ? 1 : 0) && !g_hold_forced, on ? "one hold, ended by the signal" : "no hold was counted");

    puts("CASE work submitted behind the held queue keeps its order");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES);
    submit(0, record(&event, green, NULL), &sync); submit(0, record(NULL, blue, NULL), &sync2);
    CHECK(!landed(&sync2, 100000000ull) && untouched(), "the later submission of the same queue waits behind the parked one");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    require(landed(&sync2, 5000000000ull) && landed(&sync, 0), "both land after the signal"); P(nvnSyncFinalize)(&sync); P(nvnSyncFinalize)(&sync2);
    CHECK(pixels(0, 0, 255), "the later one ran last: blue");

    puts("CASE a signal by a plain store into the event's word");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES); holds = g_hold_n;
    submit(0, record(&event, red, NULL), &sync);
    CHECK(!on || wait_until(^{ return (int)(atomic_load(&g_hold_on) && atomic_load(&g_hold_recs) >= 2); }, 2000), "held again");
    CHECK(untouched(), "nothing on the GPU yet");
    atomic_store_explicit((_Atomic uint32_t *)(memory + EVENT), 1, memory_order_release);   // no API call: the encode thread's poll and the queue worker's find it
    require(landed(&sync, 5000000000ull), "it lands after the store"); P(nvnSyncFinalize)(&sync);
    CHECK(pixels(255, 0, 0) && g_hold_n == holds + (on ? 1 : 0) && !g_hold_forced, "red, and the hold ended unforced");

    puts("CASE an event the GPU signals is not held past");
    for (int i = 0; i < 40; i++) P(nvnEventSignal)(&gpu_event, NVN_EVENT_SIGNAL_MODE_WRITE, i & 1);   // as many CPU signals as the other: only the commands' signal keeps it out
    // gpu_event was last written by commands: its wait parks as before, and the commands that signal it run
    P(nvnEventSignal)(&gpu_event, NVN_EVENT_SIGNAL_MODE_WRITE, 0);
    submit(1, record(NULL, red, &gpu_event), &sync); require(landed(&sync, 5000000000ull), "the signalling commands land"); P(nvnSyncFinalize)(&sync);
    holds = g_hold_n; memset(memory + OUTPUT, 0x5a, BYTES);
    P(nvnCommandBufferBeginRecording)(&command); P(nvnCommandBufferSignalEvent)(&command, &gpu_event, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 0);
    NVNcommandHandle reset = P(nvnCommandBufferEndRecording)(&command);
    submit(1, reset, &sync); require(landed(&sync, 5000000000ull), "commands reset the event"); P(nvnSyncFinalize)(&sync);
    submit(0, record(&gpu_event, green, NULL), &sync);
    CHECK(!landed(&sync, 100000000ull) && untouched() && !atomic_load(&g_hold_on), "a wait on it parks its queue with no hold");
    submit(1, record(NULL, red, &gpu_event), &sync2);
    require(landed(&sync, 5000000000ull) && landed(&sync2, 5000000000ull), "the other queue's commands signal it and both land"); P(nvnSyncFinalize)(&sync); P(nvnSyncFinalize)(&sync2);
    CHECK(pixels(0, 255, 0) && g_hold_n == holds, "the waiting queue ran after its signal, with no hold");

    puts("CASE a thread stuck on held work");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES);
    uint64_t forced = g_hold_forced;
    submit(0, record(&event, red, NULL), &sync);
    if (on) {
        CHECK(landed(&sync, 2000000000ull), "a wait of more than 200 ms for a held fence gets the work to the GPU");
        CHECK(g_hold_forced == forced + 1, "counted as forced");
    } else CHECK(!landed(&sync, 400000000ull), "without the switch the queue stays parked");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    require(landed(&sync, 5000000000ull), "the queue is free again"); P(nvnSyncFinalize)(&sync);

    puts("CASE the event of a forced hold is left out for a while");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES); holds = g_hold_n;
    submit(0, record(&event, green, NULL), &sync);
    CHECK(!landed(&sync, 100000000ull) && untouched() && !atomic_load(&g_hold_on), "its next wait parks with no hold");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    require(landed(&sync, 5000000000ull), "and lands after the signal"); P(nvnSyncFinalize)(&sync);
    for (int i = 0; i < 40; i++) P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, i & 1);
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 0); memset(memory + OUTPUT, 0x5a, BYTES);
    submit(0, record(&event, blue, NULL), &sync);
    CHECK(!on || wait_until(^{ return (int)atomic_load(&g_hold_on); }, 2000), "after 30 more signals of the CPU's it is taken again");
    P(nvnEventSignal)(&event, NVN_EVENT_SIGNAL_MODE_WRITE, 1);
    require(landed(&sync, 5000000000ull), "the last submission lands"); P(nvnSyncFinalize)(&sync);
    CHECK(pixels(0, 0, 255), "blue");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
} }
