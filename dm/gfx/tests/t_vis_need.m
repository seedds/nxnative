// Occlusion queries count only the draws a report can see (DM_NVN_VIS_NEED, nvn_mtl.m vis_need_scan): after a samples-passed reset or report
// whose next samples-passed op in the submission is a reset, the draws up to it are encoded with counting off. Every sequence reads the same
// values either way; with DM_NVN_VIS_NEED=0 (the check's second run) no draw is left uncounted. Recorded with the public NVN entry points.
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
#define CHECK(c, name) do { int check_ok = !!(c); checks++; failures += !check_ok; printf("%s: %s\n", check_ok ? "PASS" : "FAIL", name); } while (0)
static void require(int ok, const char *name) { CHECK(ok, name); if (!ok) exit(2); }
enum { W = 8, H = 8, FULL = W * H, POOL_BYTES = 1 << 20, TEX = 0x80000, REPORTS = 0xa0000, INDIRECT = 0xa1000, CMD2 = 0xb0000 };
static NVNdevice device; static NVNqueue queue; static NVNmemoryPool pool; static NVNcommandBuffer command, child; static NVNtexture target;
static NVNprogram red; static uint8_t *memory; static int need_on;
static const NVNtexture *colors[] = {&target}; static const float viewport[] = {0, 0, W, H};

static uint64_t *t_report(int i) { uint64_t *r = (uint64_t *)(memory + REPORTS + 64 * i); r[0] = 7; return r; }
static void t_begin(void) {
    P(nvnCommandBufferBeginRecording)(&command);
    P(nvnCommandBufferSetRenderTargets)(&command, 1, colors, NULL, NULL, NULL);
    P(nvnCommandBufferSetViewports)(&command, 0, 1, viewport); P(nvnCommandBufferSetScissor)(&command, 0, 0, W, H);
    P(nvnCommandBufferBindProgram)(&command, &red, 3);
}
static void t_draw(NVNcommandBuffer *c) { P(nvnCommandBufferDrawArrays)(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3); }   // covers the target: FULL samples
static void t_reset(void) { P(nvnCommandBufferResetCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED); }
static void t_rep(uint64_t *r) { P(nvnCommandBufferReportCounter)(&command, NVN_COUNTER_TYPE_SAMPLES_PASSED, (NVNbufferAddress)(uintptr_t)r); }
// Submits the recording and waits; returns how many draws it encoded with counting off because no report could see them
static uint64_t t_run(void) {
    NVNcommandHandle h = P(nvnCommandBufferEndRecording)(&command); NVNsync sync; uint64_t u0 = g_vis_unseen;
    P(nvnSyncInitialize)(&sync, &device); P(nvnQueueSubmitCommands)(&queue, 1, &h);
    P(nvnQueueFenceSync)(&queue, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNsyncWaitResult r = P(nvnSyncWait)(&sync, 5000000000ull);
    require(r == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || r == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "submission completes");
    P(nvnSyncFinalize)(&sync); return g_vis_unseen - u0;
}
static void t_expect(uint64_t unseen, uint64_t want, const char *name) {
    char s[200]; snprintf(s, sizeof s, "%s: %llu draws uncounted (want %llu)", name, (unsigned long long)unseen, (unsigned long long)(need_on ? want : 0)); CHECK(unseen == (need_on ? want : 0), s);
}

int main(void) { @autoreleasepool {
    if (setvbuf(stdout, NULL, _IONBF, 0)) abort(); mtl_init(); NSError *error = nil;
    const char *sw = getenv("DM_NVN_VIS_NEED"); need_on = !sw || atoi(sw);
    printf("DM_NVN_VIS_NEED=%s\n", sw ? sw : "(default, on)");
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { const float2 p[] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "fragment float4 red() { return float4(1,0,0,1); }\n" options:nil error:&error];
    if (!library) fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
    require(library != nil, "synthetic shaders compile");
    NvShader *vs = [NvShader new]; vs->fn = [library newFunctionWithName:@"v"];
    NvShader *fs = [NvShader new]; fs->stage = 1; fs->rf.color_mask = 1; fs->fn = [library newFunctionWithName:@"red"];
    NvProg *compiled = [NvProg new]; compiled->sh[0] = vs; compiled->sh[1] = fs;
    NVNdeviceBuilder db = {0}; P(nvnDeviceBuilderSetDefaults)(&db); require(P(nvnDeviceInitialize)(&device, &db), "device initialized");
    NVNqueueBuilder qb = {0}; P(nvnQueueBuilderSetDefaults)(&qb); P(nvnQueueBuilderSetDevice)(&qb, &device); require(P(nvnQueueInitialize)(&queue, &qb), "queue initialized");
    require(!posix_memalign((void **)&memory, getpagesize(), POOL_BYTES), "pool storage allocated"); memset(memory, 0, POOL_BYTES);
    NVNmemoryPoolBuilder pb = {0}; P(nvnMemoryPoolBuilderSetDefaults)(&pb); P(nvnMemoryPoolBuilderSetDevice)(&pb, &device);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, memory, POOL_BYTES); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24); require(P(nvnMemoryPoolInitialize)(&pool, &pb), "pool initialized");
    require(P(nvnCommandBufferInitialize)(&command, &device), "command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&command, &pool, 0, TEX);
    require(P(nvnCommandBufferInitialize)(&child, &device), "child command buffer initialized"); P(nvnCommandBufferAddCommandMemory)(&child, &pool, CMD2, 0x10000);
    NVNtextureBuilder tb = {0}; P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetDevice)(&tb, &device);
    P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D); P(nvnTextureBuilderSetSize2D)(&tb, W, H);
    P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetStorage)(&tb, &pool, TEX);
    require(P(nvnTextureInitialize)(&target, &tb), "target initialized");
    require(P(nvnProgramInitialize)(&red, &device), "program initialized");
    prog_t *pp = (prog_t *)&red; pp->serial = nvn_new_serial(); serial_put(pp->serial, compiled);

    // A query of its own around each queried draw, other draws between (Ocarina of Time): the in-between draw reaches no report
    uint64_t *a = t_report(0), *b = t_report(1);
    t_begin(); t_reset(); t_draw(&command); t_rep(a); t_draw(&command); t_reset(); t_draw(&command); t_rep(b);
    uint64_t u = t_run();
    CHECK(a[0] == FULL && b[0] == FULL, "reset, draw, report around each queried draw: each report reads one draw");
    t_expect(u, 1, "report then reset");

    // Report, draw, report: the second report reads the running total, the draw between included
    a = t_report(0); b = t_report(1);
    t_begin(); t_reset(); t_draw(&command); t_rep(a); t_draw(&command); t_rep(b);
    u = t_run();
    CHECK(a[0] == FULL && b[0] == 2 * FULL, "report then report: the draw between counts into the later report");
    t_expect(u, 0, "report then report");

    // Reset, draw, reset: that draw is dropped by the second reset
    a = t_report(0);
    t_begin(); t_reset(); t_draw(&command); t_draw(&command); t_reset(); t_draw(&command); t_rep(a);
    u = t_run();
    CHECK(a[0] == FULL, "reset then reset: only the draw after the last reset is reported");
    t_expect(u, 2, "reset then reset");

    // A report with nothing after it in its submission: its draws count (the next submission could report them without a reset)
    a = t_report(0); b = t_report(1);
    t_begin(); t_reset(); t_draw(&command); t_rep(a); t_draw(&command);
    u = t_run();
    t_begin(); t_draw(&command); t_rep(b);
    u += t_run();
    CHECK(a[0] == FULL && b[0] == 3 * FULL, "a submission's draws after its last report reach a report of the next one");
    t_expect(u, 0, "submission end then report");

    // An indirect multi-draw between (a single record over MDI_INLINE draws; count 0) does not stop the scan
    uint32_t *cnt = (uint32_t *)(memory + INDIRECT); cnt[0] = 0;
    a = t_report(0); b = t_report(1);
    t_begin(); t_reset(); t_draw(&command); t_rep(a); t_draw(&command);
    P(nvnCommandBufferMultiDrawArraysIndirectCount)(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, (NVNbufferAddress)(uintptr_t)(memory + INDIRECT + 64),
                                                    (NVNbufferAddress)(uintptr_t)cnt, 1025, 16);
    t_draw(&command); t_reset(); t_draw(&command); t_rep(b);
    u = t_run();
    CHECK(a[0] == FULL && b[0] == FULL, "a multi-draw between a report and a reset: the same values");
    CHECK(need_on ? u >= 2 : u == 0, "the scan steps over the multi-draw: the draws on both sides of it are uncounted");

    // Draws of a called recording (CallCommands joins its records to the caller's)
    P(nvnCommandBufferBeginRecording)(&child); t_draw(&child); t_draw(&child); NVNcommandHandle ch = P(nvnCommandBufferEndRecording)(&child);
    a = t_report(0); b = t_report(1);
    t_begin(); t_reset(); t_draw(&command); t_rep(a); P(nvnCommandBufferCallCommands)(&command, 1, &ch); t_reset(); P(nvnCommandBufferCallCommands)(&command, 1, &ch); t_rep(b);
    u = t_run();
    CHECK(a[0] == FULL && b[0] == 2 * FULL, "called draws: the same values");
    t_expect(u, 2, "called draws between a report and a reset");

    // The scan itself: a reset next gives 0; a call stack deeper than the cursor holds keeps counting (it could not follow the replay's order)
    struct { op_hdr h; uint64_t t; op_hdr end; } ops = {{OP_RESET_COUNTER, 16}, NVN_COUNTER_TYPE_SAMPLES_PASSED, {OP_END, 8}};
    struct { op_hdr h; uint64_t t, addr; op_hdr end; } rops = {{OP_REPORT_COUNTER, 24}, NVN_COUNTER_TYPE_SAMPLES_PASSED, 0, {OP_END, 8}};
    const uint64_t *rest0 = g_rest; int nrest0 = g_nrest; mtl_exec_rest(NULL, 0);
    vis_need_scan((const uint8_t *)&ops); int direct = g_vis_need;
    g_ret_n = 9; vis_need_scan((const uint8_t *)&ops); int deep = g_vis_need; g_ret_n = 0;
    vis_need_scan((const uint8_t *)&rops); int to_report = g_vis_need;
    g_vis_need = 1; mtl_exec_rest(rest0, nrest0);
    CHECK(direct == !need_on && deep == 1 && to_report == 1, "the scan: a reset next stops counting, a report next or a deep call stack keeps it");

    P(nvnQueueFinish)(&queue); P(nvnProgramFinalize)(&red); P(nvnTextureFinalize)(&target);
    P(nvnCommandBufferFinalize)(&command); P(nvnCommandBufferFinalize)(&child); P(nvnMemoryPoolFinalize)(&pool);
    P(nvnQueueFinalize)(&queue); P(nvnDeviceFinalize)(&device); free(memory);
    printf("occlusion query need: %d checks, %d failed\n", checks, failures); return failures != 0;
} }
