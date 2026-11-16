// Depth bounds (nvnCommandBufferSetDepthBounds → OP_DEPTH_BOUNDS → Metal's depth bounds test) through public NVN recording and the real replay. A Mac GPU of the Apple9 family
// (the M4 Max this was written on) has no depth bounds test, and Metal's validation stops the process at the encoder call there, so the GPU half cannot run on such a Mac. What runs:
// how nvn_dbnd.h reads bounds (clamped to [0, 1], off at (0, 1), nothing passing for min above max or a NaN); on this GPU, that a game turning the test on makes no encoder call and
// draws as if it were off; then, with the GPU taken as having it (g_dbnd_hw) and the encoder call taken by g_dbnd_test_enc, the calls the replay makes: the bounds at the first draw,
// none for a draw with the same bounds, again after a copy splits the pass, (0, 1) when the game turns the test off and around a clear inside the pass, none in a pass without depth;
// draws whose bounds pass nothing are skipped (their pixels checked) where the GPU has the test and drawn where it has not, and DM_DEPTH_BOUNDS=0 ignores all of it. Every submission
// runs under MTL_DEBUG_LAYER=1 (make GAME=<id> check_t_depth_bounds), which would stop at a depth bounds call reaching this GPU's encoder
#include "game_profile.h"
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
#define CHECK(c, name) do { int check_ok = !!(c); checks++; failures += !check_ok; if (!check_ok) printf("FAIL: %s (line %d)\n", name, __LINE__); } while (0)
static void require(int ok, const char *name) { CHECK(ok, name); if (!ok) exit(2); }
enum { W = 8, H = 8, BYTES = W * H * 4, POOL_BYTES = 1 << 20, COLOR = 0x80000, DEPTH = 0x90000, OUTPUT = 0xa0000 };
static NVNdevice device; static NVNqueue queue; static NVNmemoryPool pool; static NVNcommandBuffer command;
static NVNtexture color, depth; static NVNprogram green; static uint8_t *memory; static NVNbufferAddress output;
static const NVNcopyRegion region = {0, 0, 0, W, H, 1};

static float calls[64][2]; static int ncalls;   // the bounds the replay sent (g_dbnd_test_enc)
static void take(float lo, float hi) { if (ncalls < 64) { calls[ncalls][0] = lo; calls[ncalls][1] = hi; } ncalls++; }
static int call_is(int i, float lo, float hi) { return i < ncalls && calls[i][0] == lo && calls[i][1] == hi; }

static void run(NVNcommandHandle handle) {
    NVNsync sync; P(nvnSyncInitialize)(&sync, &device); P(nvnQueueSubmitCommands)(&queue, 1, &handle);
    P(nvnQueueFenceSync)(&queue, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNsyncWaitResult r = P(nvnSyncWait)(&sync, 5000000000ull);
    require(r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "submission completes");
    P(nvnSyncFinalize)(&sync); require(!mtl_last_commit_error(), "no command buffer error");
}
static int pixels(unsigned offset, unsigned r, unsigned g, unsigned b) {
    const uint8_t want[] = {r, g, b, 255};
    for (int i = 0; i < W * H; i++) if (memcmp(memory + OUTPUT + offset + i * 4, want, 4)) return 0;
    return 1;
}
// Targets (with or without the depth texture) cleared to blue and depth 0.5, the green program bound. with_depth 2: both cleared in a submission of their own, then attached
// as it left them with no clear in the recording, so the first draw opens the pass
static void run(NVNcommandHandle handle);
static void rec_begin(int with_depth) {
    if (with_depth == 2) { rec_begin(1); run(P(nvnCommandBufferEndRecording)(&command)); }
    P(nvnCommandBufferBeginRecording)(&command);
    const NVNtexture *colors[] = {&color}; const float viewport[] = {0, 0, W, H}, blue[] = {0, 0, 1, 1};
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, with_depth ? &depth : NULL, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    if (with_depth != 2) P(nvnCommandBufferClearColor)(&command, 0, blue, 15);
    if (with_depth == 1) P(nvnCommandBufferClearDepthStencil)(&command, 0.5f, 1, 0, 0);
    P(nvnCommandBufferBindProgram)(&command, &green, 3);
}
static void tri(void) { P(nvnCommandBufferDrawArrays)(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3); }
static void bounds(int on, float lo, float hi) { P(nvnCommandBufferSetDepthBounds)(&command, on, lo, hi); }
static void copy_out(unsigned offset) { P(nvnCommandBufferCopyTextureToBuffer)(&command, &color, NULL, &region, output + offset, 0); }
static void init_texture(NVNtexture *texture, NVNformat format, unsigned offset) {
    NVNtextureBuilder tb = {0}; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
    P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&tb, W, H);
    P(nvnTextureBuilderSetFormat)(&tb, format); P(nvnTextureBuilderSetStorage)(&tb, &pool, offset);
    require(P(nvnTextureInitialize)(texture, &tb), "texture initialized");
}

int main(void) { @autoreleasepool {
    if (setvbuf(stdout, NULL, _IONBF, 0)) abort();
    // How the bounds read (nvn_dbnd.h)
    float b[2];
    CHECK(dbnd_resolve(0, .25f, .75f, b) == DBND_OFF && b[0] == 0 && b[1] == 1, "off: (0, 1) whatever the bounds");
    CHECK(dbnd_resolve(1, .25f, .75f, b) == DBND_TEST && b[0] == .25f && b[1] == .75f, "bounds inside [0, 1] are kept");
    CHECK(dbnd_resolve(1, -1, 2, b) == DBND_OFF && b[0] == 0 && b[1] == 1, "bounds wider than [0, 1] reject nothing: off");
    CHECK(dbnd_resolve(1, 0, 1, b) == DBND_OFF, "(0, 1) is Metal's off");
    CHECK(dbnd_resolve(1, .5f, 3, b) == DBND_TEST && b[0] == .5f && b[1] == 1, "max clamped to 1");
    CHECK(dbnd_resolve(1, -3, .5f, b) == DBND_TEST && b[0] == 0 && b[1] == .5f, "min clamped to 0");
    CHECK(dbnd_resolve(1, -1, -0.f, b) == DBND_TEST && b[0] == 0 && b[1] == 0 && !signbit(b[1]), "max -0 is +0, which Metal takes; only a stored 0 passes");
    CHECK(dbnd_resolve(1, .5f, .5f, b) == DBND_TEST && b[0] == .5f && b[1] == .5f, "min equal to max passes that depth only");
    CHECK(dbnd_resolve(1, .75f, .25f, b) == DBND_EMPTY && b[0] == 0 && b[1] == 1, "min above max passes nothing");
    CHECK(dbnd_resolve(1, 1.5f, 2, b) == DBND_EMPTY, "both above 1: nothing, not a clamp to (1, 1) that a cleared far plane would pass");
    CHECK(dbnd_resolve(1, -2, -1, b) == DBND_EMPTY, "both below 0: nothing");
    CHECK(dbnd_resolve(1, NAN, .5f, b) == DBND_EMPTY && dbnd_resolve(1, 0, NAN, b) == DBND_EMPTY, "a NaN bound passes nothing");
    CHECK([DmRecEnc instancesRespondToSelector:@selector(setDepthTestMinBound:maxBound:)], "the deferred encoder's stand-in records the call (DM_NVN_ENC_THREAD)");

    mtl_init(); NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "fragment float4 g() { return float4(0,1,0,1); }\n" options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "test shaders compile");
    NvProg *compiled = [NvProg new]; compiled->sh[0] = [NvShader new]; compiled->sh[0]->fn = [library newFunctionWithName:@"v"];
    NvShader *fs = [NvShader new]; fs->stage = 1; fs->rf.color_mask = 1; fs->fn = [library newFunctionWithName:@"g"]; compiled->sh[1] = fs;
    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device); require(P(nvnQueueInitialize)(&queue, &qb), "queue initialized");
    require(!posix_memalign((void **)&memory, getpagesize(), POOL_BYTES), "pool storage allocated"); memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24); require(P(nvnMemoryPoolInitialize)(&pool, &pb), "pool initialized");
    NVNbufferBuilder bb = {0}; NVNbuffer buffer; P(nvnBufferBuilderSetDefaults)(&bb); P(nvnBufferBuilderSetDevice)(&bb, &device);
    P(nvnBufferBuilderSetStorage)(&bb, &pool, OUTPUT, BYTES * 8); require(P(nvnBufferInitialize)(&buffer, &bb), "readback buffer initialized"); output = P(nvnBufferGetAddress)(&buffer);
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, COLOR);
    init_texture(&color, NVN_FORMAT_RGBA8, COLOR); init_texture(&depth, NVN_FORMAT_DEPTH32F, DEPTH);
    require(P(nvnProgramInitialize)(&green, &device), "program initialized");
    prog_t *pp = (prog_t *)&green; pp->serial = nvn_new_serial(); serial_put(pp->serial, compiled);

    // This GPU as it is: a game turning the test on asks the GPU once; without the test (Apple9) no call is made and the draw goes through as if it were off
    int apple10 = [g_dev supportsFamily:MTLGPUFamilyApple10];
    g_dbnd_test_enc = take;   // counts the calls here too: on this GPU there must be none
    CHECK(g_dbnd_hw == -1, "nothing asks the GPU before a game uses the test");
    rec_begin(1); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(g_dbnd_hw == -1 && ncalls == 0, "a game that never turns it on: no question, no call");
    rec_begin(1); bounds(1, .25f, .75f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(S.dbnd.on && S.dbnd.lo == .25f && S.dbnd.hi == .75f, "SetDepthBounds is recorded and replayed into the queue's state (no longer an empty function)");
    CHECK(g_dbnd_hw == apple10, "the GPU is asked at the first draw with bounds to test");
    if (!apple10) { CHECK(ncalls == 0 && g_red_once[173] == 1, "Apple9: no encoder call, one red line"); CHECK(pixels(0, 0, 255, 0), "Apple9: drawn as if the test were off"); }
    else CHECK(call_is(0, .25f, .75f) && ncalls == 1, "Apple10: the bounds sent once");
    if (!apple10) {   // bounds that pass nothing are drawn too on a GPU without the test, as before it was applied
        rec_begin(1); bounds(1, .9f, .1f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
        CHECK(ncalls == 0 && g_dbnd_empty == 0 && pixels(0, 0, 255, 0), "Apple9: bounds that pass nothing drawn as if the test were off, no call");
    }

    // From here the GPU is taken as having the test; g_dbnd_test_enc takes the calls
    g_dbnd_hw = 1; ncalls = 0; uint64_t empty0 = g_dbnd_empty;
    rec_begin(1);
    bounds(1, .25f, .75f); tri();   // 0: the bounds
    tri();                          // the same bounds on the same encoder: no call
    copy_out(0);                     // a copy of the target ends the render pass
    tri();                          // 1: a new encoder starts with the test off: the bounds again
    bounds(0, .25f, .75f); tri();   // 2: turned off: (0, 1)
    bounds(1, -1, 2); tri();        // wider than [0, 1]: off, and the encoder has it off already
    bounds(1, .5f, 3); tri();       // 3: clamped to (0.5, 1)
    bounds(1, .9f, .1f); tri();     // passes nothing: skipped, no call
    copy_out(BYTES);
    run(P(nvnCommandBufferEndRecording)(&command));
    for (int i = 0; i < ncalls && i < 64; i++) printf("call %d: %g %g\n", i, calls[i][0], calls[i][1]);
    CHECK(ncalls == 4, "four calls: first draw, after the split, off, clamped");
    CHECK(call_is(0, .25f, .75f), "the bounds at the first draw");
    CHECK(call_is(1, .25f, .75f), "the bounds again on the encoder after the pass split");
    CHECK(call_is(2, 0, 1), "turning the test off sends (0, 1)");
    CHECK(call_is(3, .5f, 1), "bounds past 1 are clamped");
    CHECK(g_dbnd_empty == empty0 + 1, "the draw with bounds that pass nothing is skipped");
    CHECK(S.dbnd.on && S.dbnd.lo == .9f && S.dbnd.hi == .1f, "the queue keeps the bounds as the game set them");

    // A clear inside the pass is not tested: the clear triangle turns the test off, and the next draw sends the bounds again
    ncalls = 0;
    rec_begin(1); bounds(1, .25f, .75f); tri();
    { const float red[] = {1, 0, 0, 1}; P(nvnCommandBufferSetScissor)(&command, 0, 0, W / 2, H); P(nvnCommandBufferClearColor)(&command, 0, red, 15); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H); }
    tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    for (int i = 0; i < ncalls && i < 64; i++) printf("clear call %d: %g %g\n", i, calls[i][0], calls[i][1]);
    CHECK(ncalls == 3 && call_is(0, .25f, .75f) && call_is(1, 0, 1) && call_is(2, .25f, .75f), "a clear inside the pass runs with the test off, the next draw has the bounds again");

    // Bounds that pass nothing: the draw leaves the target as it was
    ncalls = 0;
    rec_begin(1); bounds(1, .9f, .1f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(pixels(0, 0, 0, 255) && ncalls == 0, "min above max: nothing drawn, no call");
    // A pass without depth: nothing to test, so neither the bounds nor the skip apply
    rec_begin(0); bounds(1, .25f, .75f); tri(); bounds(1, .9f, .1f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(ncalls == 0 && pixels(0, 0, 255, 0), "no depth attachment: no call, drawn");
    // The depth attached but neither tested nor written, as a light volume's stencil pass: the bounds read it all the same
    NVNdepthStencilState no_depth, defaults; P(nvnDepthStencilStateSetDefaults)(&no_depth); P(nvnDepthStencilStateSetDefaults)(&defaults);
    P(nvnDepthStencilStateSetDepthTestEnable)(&no_depth, 0); P(nvnDepthStencilStateSetDepthWriteEnable)(&no_depth, 0);
    rec_begin(2); P(nvnCommandBufferBindDepthStencilState)(&command, &no_depth); bounds(1, .25f, .75f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(ncalls == 1 && call_is(0, .25f, .75f) && pixels(0, 0, 255, 0), "depth neither tested nor written: the bounds sent, drawn");
    ncalls = 0; uint64_t empty1 = g_dbnd_empty;
    rec_begin(2); P(nvnCommandBufferBindDepthStencilState)(&command, &no_depth); bounds(1, .9f, .1f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(ncalls == 0 && g_dbnd_empty == empty1 + 1 && pixels(0, 0, 0, 255), "depth neither tested nor written: bounds that pass nothing skip the draw");
    // A same-target continuation (SetRenderTargets with the same targets) goes on in the open pass; a draw with bounds after it tests them against the depth
    ncalls = 0;
    rec_begin(2); P(nvnCommandBufferBindDepthStencilState)(&command, &no_depth); bounds(0, 0, 1); tri();
    { const NVNtexture *colors[] = {&color}; P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, &depth, NULL); }
    bounds(1, .25f, .75f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(ncalls == 1 && call_is(0, .25f, .75f) && pixels(0, 0, 255, 0), "a draw with bounds after a same-target continuation tests them against the depth");
    P(nvnCommandBufferBeginRecording)(&command); P(nvnCommandBufferBindDepthStencilState)(&command, &defaults); run(P(nvnCommandBufferEndRecording)(&command));
    // DM_DEPTH_BOUNDS=0: ignored as before
    g_dbnd_env = 0; ncalls = 0;
    rec_begin(1); bounds(1, .25f, .75f); tri(); bounds(1, .9f, .1f); tri(); copy_out(0); run(P(nvnCommandBufferEndRecording)(&command));
    CHECK(ncalls == 0 && pixels(0, 0, 255, 0), "DM_DEPTH_BOUNDS=0: no call, bounds that pass nothing drawn anyway");
    g_dbnd_env = -1;
    // A new queue starts with the test off, and so does its first encoder
    NVNqueue fresh; require(P(nvnQueueInitialize)(&fresh, &qb), "second queue initialized");
    mtl_queue_activate(((queue_t *)&fresh)->serial); CHECK(!S.dbnd.on, "a new queue's state has the test off");
    mtl_queue_activate(((queue_t *)&queue)->serial); CHECK(S.dbnd.on && S.dbnd.hi == .1f, "the first queue keeps its bounds");

    printf("depth bounds: %d checks, %d failed\n", checks, failures);
    return failures != 0;
} }
